#include "trading_engine/market_data/alpaca_historical_source.hpp"

#include <algorithm>
#include <cstdlib>
#include <thread>
#include <utility>

#include "alpaca_json.hpp"
#include "trading_engine/common/errors.hpp"
#include "trading_engine/common/timestamp_text.hpp"

namespace trading_engine::market_data
{
    namespace
    {
        namespace json = events::json;

        std::string url_encode(std::string_view text)
        {
            static constexpr char kHex[] = "0123456789ABCDEF";
            std::string out;
            out.reserve(text.size());
            for (const char c : text)
            {
                const auto u = static_cast<unsigned char>(c);
                const bool unreserved = (u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') ||
                                        (u >= '0' && u <= '9') || u == '-' || u == '_' || u == '.' || u == '~';
                if (unreserved)
                {
                    out.push_back(c);
                }
                else
                {
                    out.push_back('%');
                    out.push_back(kHex[u >> 4U]);
                    out.push_back(kHex[u & 0x0FU]);
                }
            }
            return out;
        }

        // RFC 3339 without the fraction when it is zero, the form Alpaca documents
        std::string query_time(common::Timestamp t)
        {
            std::string text = common::format_timestamp(t);
            const auto dot = text.find('.');
            if (dot != std::string::npos && text.find_first_not_of('0', dot + 1) == text.size() - 1)
            {
                text.erase(dot, text.size() - 1 - dot);
            }
            return text;
        }

        bool valid_timeframe(std::string_view tf)
        {
            std::size_t digits = 0;
            while (digits < tf.size() && tf[digits] >= '0' && tf[digits] <= '9')
            {
                ++digits;
            }
            if (digits == 0 || tf[0] == '0')
            {
                return false;
            }
            const std::string_view unit = tf.substr(digits);
            return unit == "Min" || unit == "T" || unit == "Hour" || unit == "H" || unit == "Day" ||
                   unit == "D" || unit == "Week" || unit == "W" || unit == "Month" || unit == "M";
        }

        [[noreturn]] void malformed(const std::string &what)
        {
            throw AlpacaError(200, "Alpaca bars response: " + what);
        }

        const json::Value &member(const json::Value &bar, std::string_view key, const std::string &where)
        {
            const json::Value *v = bar.find(key);
            if (v == nullptr)
            {
                malformed(where + " has no '" + std::string(key) + "'");
            }
            return *v;
        }

        common::Price price_field(const json::Value &bar, std::string_view key, const std::string &where)
        {
            const auto price = alpaca_json::price(member(bar, key, where));
            if (!price)
            {
                malformed(where + " '" + std::string(key) + "' is not a price");
            }
            return *price;
        }

        common::Quantity volume_field(const json::Value &bar, const std::string &where)
        {
            const auto volume = alpaca_json::quantity(member(bar, "v", where));
            if (!volume)
            {
                malformed(where + " 'v' is not a volume");
            }
            return *volume;
        }

        std::string error_message(const HttpResponse &response)
        {
            if (const auto doc = json::parse(response.body))
            {
                if (const json::Value *m = doc->find("message"); m != nullptr && m->kind == json::Value::Kind::String)
                {
                    return m->text;
                }
            }
            std::string body = response.body.substr(0, 200);
            return body.empty() ? "(empty body)" : body;
        }

        bool retryable(long status) { return status == 429 || (status >= 500 && status <= 599); }

        HttpResponse get_with_retries(IHttpClient &http, const HttpRequest &request, const AlpacaFetchOptions &options)
        {
            const auto sleep = options.sleep ? options.sleep
                                             : [](std::chrono::milliseconds d) { std::this_thread::sleep_for(d); };
            std::chrono::milliseconds backoff = options.first_backoff;
            const int attempts = std::max(1, options.max_attempts);

            for (int attempt = 1;; ++attempt)
            {
                std::optional<HttpResponse> response;
                std::string failure;
                try
                {
                    response = http.get(request);
                }
                catch (const HttpError &e)
                {
                    failure = e.what();
                }

                if (response && !retryable(response->status))
                {
                    return std::move(*response);
                }
                if (attempt >= attempts)
                {
                    if (response)
                    {
                        throw AlpacaError(response->status, "Alpaca returned HTTP " + std::to_string(response->status) +
                                                                " after " + std::to_string(attempts) +
                                                                " attempts: " + error_message(*response));
                    }
                    throw HttpError(failure + " (after " + std::to_string(attempts) + " attempts)");
                }
                if (options.should_stop && options.should_stop())
                {
                    throw AlpacaError(0, "Alpaca fetch stopped");
                }
                sleep(backoff);
                backoff = std::min(backoff * 2, std::chrono::milliseconds{30'000});
            }
        }
    } // namespace

    AlpacaCredentials resolve_alpaca_credentials(const config::AlpacaConfig &config)
    {
        const auto read = [](const std::string &name) -> std::string {
            if (name.empty())
            {
                return {};
            }
#if defined(_WIN32)
            char *buffer = nullptr;
            std::size_t length = 0;
            if (_dupenv_s(&buffer, &length, name.c_str()) != 0 || buffer == nullptr)
            {
                return {};
            }
            std::string value{buffer};
            std::free(buffer);
            return value;
#else
            const char *value = std::getenv(name.c_str());
            return value == nullptr ? std::string{} : std::string{value};
#endif
        };
        AlpacaCredentials credentials{read(config.api_key_env), read(config.api_secret_env)};

        std::string missing;
        if (credentials.key_id.empty())
        {
            missing = config.api_key_env;
        }
        if (credentials.secret_key.empty())
        {
            missing += (missing.empty() ? "" : " and ") + config.api_secret_env;
        }
        if (!missing.empty())
        {
            throw common::ConfigError("Alpaca keys missing: set " + missing + " in the environment (python3 start.py --set-keys saves them for the console)");
        }
        return credentials;
    }

    void validate_alpaca_query(const config::AlpacaConfig &config, const HistoricalQuery &query)
    {
        if (config.symbols.empty())
        {
            throw common::ConfigError("Alpaca bars: no symbols");
        }
        for (const auto &symbol : config.symbols)
        {
            if (symbol.empty())
            {
                throw common::ConfigError("Alpaca bars: empty symbol");
            }
        }
        if (query.end <= query.start)
        {
            throw common::ConfigError("Alpaca bars: end must be after start");
        }
        if (!valid_timeframe(query.timeframe))
        {
            throw common::ConfigError("Alpaca bars: timeframe '" + query.timeframe +
                                      "' is not like 1Min, 15Min, 1Hour, 1Day, 1Week or 1Month");
        }
        const auto &adj = query.adjustment;
        if (adj != "raw" && adj != "split" && adj != "dividend" && adj != "all")
        {
            throw common::ConfigError("Alpaca bars: adjustment '" + adj + "' is not raw, split, dividend or all");
        }
        if (query.page_limit == 0 || query.page_limit > 10000)
        {
            throw common::ConfigError("Alpaca bars: page limit must be 1..10000");
        }
    }

    std::string alpaca_bars_url(const config::AlpacaConfig &config, const HistoricalQuery &query,
                                std::string_view page_token)
    {
        std::string symbols;
        for (const auto &symbol : config.symbols)
        {
            symbols += (symbols.empty() ? "" : ",") + symbol;
        }

        std::string base = config.market_data_base_url;
        while (!base.empty() && base.back() == '/')
        {
            base.pop_back();
        }

        std::string url = base + "/v2/stocks/bars?symbols=" + url_encode(symbols) +
                          "&timeframe=" + url_encode(query.timeframe) +
                          "&start=" + url_encode(query_time(query.start)) +
                          "&end=" + url_encode(query_time(query.end)) +
                          "&limit=" + std::to_string(query.page_limit) +
                          "&adjustment=" + url_encode(query.adjustment) +
                          "&feed=" + url_encode(config.data_feed) +
                          "&sort=asc";
        if (!page_token.empty())
        {
            url += "&page_token=" + url_encode(page_token);
        }
        return url;
    }

    AlpacaBarsPage parse_alpaca_bars_page(std::string_view body)
    {
        const auto doc = json::parse(body);
        if (!doc || doc->kind != json::Value::Kind::Object)
        {
            malformed("not a JSON object");
        }

        AlpacaBarsPage page;

        if (const json::Value *token = doc->find("next_page_token");
            token != nullptr && token->kind == json::Value::Kind::String && !token->text.empty())
        {
            page.next_page_token = token->text;
        }

        const json::Value *bars = doc->find("bars");
        if (bars == nullptr || bars->kind == json::Value::Kind::Null)
        {
            return page;
        }
        if (bars->kind != json::Value::Kind::Object)
        {
            malformed("'bars' is not an object keyed by symbol");
        }

        for (const auto &[symbol, list] : bars->object)
        {
            if (list.kind != json::Value::Kind::Array)
            {
                malformed("bars for " + symbol + " are not an array");
            }
            for (std::size_t i = 0; i < list.array.size(); ++i)
            {
                const json::Value &bar = list.array[i];
                const std::string where = symbol + " bar " + std::to_string(i);
                if (bar.kind != json::Value::Kind::Object)
                {
                    malformed(where + " is not an object");
                }

                const json::Value &t = member(bar, "t", where);
                const auto time = t.kind == json::Value::Kind::String ? common::parse_timestamp(t.text) : std::nullopt;
                if (!time)
                {
                    malformed(where + " 't' is not an RFC 3339 UTC time");
                }

                domain::MarketEvent event;
                event.symbol = symbol;
                event.exchange_time = *time;
                event.type = domain::MarketEventType::Bar;
                event.open = price_field(bar, "o", where);
                event.high = price_field(bar, "h", where);
                event.low = price_field(bar, "l", where);
                event.price = price_field(bar, "c", where);
                event.volume = volume_field(bar, where);
                page.bars.push_back(std::move(event));
            }
        }
        return page;
    }

    std::vector<domain::MarketEvent> fetch_alpaca_bars(IHttpClient &http, const AlpacaCredentials &credentials,
                                                       const config::AlpacaConfig &config, const HistoricalQuery &query,
                                                       const AlpacaFetchOptions &options)
    {
        validate_alpaca_query(config, query);

        HttpRequest request;
        request.headers = {{"APCA-API-KEY-ID", credentials.key_id},
                           {"APCA-API-SECRET-KEY", credentials.secret_key},
                           {"Accept", "application/json"}};

        std::vector<domain::MarketEvent> all;
        std::string token;
        std::size_t pages = 0;

        do
        {
            if (options.should_stop && options.should_stop())
            {
                throw AlpacaError(0, "Alpaca fetch stopped");
            }
            request.url = alpaca_bars_url(config, query, token);
            const HttpResponse response = get_with_retries(http, request, options);
            if (response.status != 200)
            {
                std::string hint;
                if (response.status == 401 || response.status == 403)
                {
                    hint = " (check the API key and secret, and that the feed '" + config.data_feed +
                           "' is included in your Alpaca data plan)";
                }
                throw AlpacaError(response.status, "Alpaca returned HTTP " + std::to_string(response.status) + ": " +
                                                       error_message(response) + hint);
            }

            AlpacaBarsPage page = parse_alpaca_bars_page(response.body);
            all.insert(all.end(), std::make_move_iterator(page.bars.begin()),
                       std::make_move_iterator(page.bars.end()));
            ++pages;
            if (options.on_page)
            {
                options.on_page(pages, all.size());
            }

            if (page.next_page_token && *page.next_page_token == token)
            {
                throw AlpacaError(200, "Alpaca bars response repeated its page token");
            }
            token = page.next_page_token.value_or("");
        } while (!token.empty());

        std::stable_sort(all.begin(), all.end(), [](const domain::MarketEvent &a, const domain::MarketEvent &b) {
            return a.exchange_time != b.exchange_time ? a.exchange_time < b.exchange_time : a.symbol < b.symbol;
        });
        return all;
    }

    AlpacaHistoricalSource::AlpacaHistoricalSource(config::AlpacaConfig config,
                                                   HistoricalQuery query,
                                                   IMarketEventSink &sink,
                                                   const common::IClock &clock)
        : config_{std::move(config)},
          query_{std::move(query)},
          sink_{sink},
          clock_{clock},
          owned_http_{make_default_http_client()},
          http_{owned_http_.get()} {}

    AlpacaHistoricalSource::AlpacaHistoricalSource(config::AlpacaConfig config,
                                                   HistoricalQuery query,
                                                   IMarketEventSink &sink,
                                                   const common::IClock &clock,
                                                   IHttpClient &http,
                                                   AlpacaCredentials credentials,
                                                   AlpacaFetchOptions options)
        : config_{std::move(config)},
          query_{std::move(query)},
          sink_{sink},
          clock_{clock},
          http_{&http},
          credentials_{std::move(credentials)},
          options_{std::move(options)} {}

    AlpacaHistoricalSource::~AlpacaHistoricalSource() = default;

    void AlpacaHistoricalSource::start()
    {
        if (running_.exchange(true))
        {
            throw std::logic_error("AlpacaHistoricalSource::start: already running");
        }
        stop_requested_ = false;
        pushed_ = 0;

        struct Reset
        {
            std::atomic<bool> &flag;
            ~Reset() { flag = false; }
        } reset{running_};

        const AlpacaCredentials credentials = credentials_ ? *credentials_ : resolve_alpaca_credentials(config_);

        AlpacaFetchOptions options = options_;
        const auto caller_stop = options.should_stop;
        options.should_stop = [this, caller_stop] {
            return stop_requested_.load() || (caller_stop && caller_stop());
        };

        std::vector<domain::MarketEvent> bars;
        try
        {
            bars = fetch_alpaca_bars(*http_, credentials, config_, query_, options);
        }
        catch (const AlpacaError &)
        {
            if (stop_requested_)
            {
                return;
            }
            throw;
        }
        for (const auto &bar : bars)
        {
            if (stop_requested_)
            {
                break;
            }
            sink_.on_market_event(bar);
            ++pushed_;
        }
    }

    void AlpacaHistoricalSource::stop()
    {
        stop_requested_ = true;
    }

    bool AlpacaHistoricalSource::is_running() const
    {
        return running_.load(std::memory_order_relaxed);
    }

} // namespace trading_engine::market_data
