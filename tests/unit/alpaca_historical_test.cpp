// AlpacaHistoricalSource and its free functions, driven by recorded responses
// (tests/fixtures/alpaca/) through a fake IHttpClient: no network.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "trading_engine/common/clock.hpp"
#include "trading_engine/common/decimal_text.hpp"
#include "trading_engine/common/errors.hpp"
#include "trading_engine/common/timestamp_text.hpp"
#include "trading_engine/market_data/alpaca_historical_source.hpp"

namespace {

namespace md  = trading_engine::market_data;
namespace tec = trading_engine::common;
namespace cfg = trading_engine::config;
namespace dom = trading_engine::domain;

using namespace std::chrono_literals;

std::string fixture(const std::string& name) {
    std::ifstream in{std::string(ALPACA_FIXTURE_DIR) + "/" + name};
    EXPECT_TRUE(in) << "missing fixture " << name;
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

tec::Timestamp at(const char* text) { return *tec::parse_timestamp(text); }

std::string price_text(const std::optional<tec::Price>& p) {
    return p ? tec::format_decimal(*p) : "none";
}

// Replays queued responses (or transport failures) and records every request.
class FakeHttp final : public md::IHttpClient {
public:
    struct Reply {
        long status{200};
        std::string body;
        bool transport_error{false};
    };

    std::deque<Reply> replies;
    std::vector<md::HttpRequest> requests;
    std::function<void()> on_get;

    md::HttpResponse get(const md::HttpRequest& request) override {
        requests.push_back(request);
        if (on_get) {
            on_get();
        }
        if (replies.empty()) {
            ADD_FAILURE() << "unexpected request " << request.url;
            return {500, "{}"};
        }
        Reply reply = replies.front();
        replies.pop_front();
        if (reply.transport_error) {
            throw md::HttpError("connection reset");
        }
        return {reply.status, reply.body};
    }
};

class CollectingSink final : public md::IMarketEventSink {
public:
    std::vector<dom::MarketEvent> events;
    void on_market_event(const dom::MarketEvent& event) override { events.push_back(event); }
};

cfg::AlpacaConfig two_symbols() {
    cfg::AlpacaConfig c;
    c.symbols = {"AAPL", "MSFT"};
    return c;
}

md::HistoricalQuery january_days() {
    md::HistoricalQuery q;
    q.start = at("2024-01-01T00:00:00Z");
    q.end = at("2024-01-04T00:00:00Z");
    q.timeframe = "1Day";
    return q;
}

const md::AlpacaCredentials kTestCredentials{"test-key-id", "test-secret"};

md::AlpacaFetchOptions no_sleep(std::vector<std::chrono::milliseconds>* slept = nullptr) {
    md::AlpacaFetchOptions o;
    o.sleep = [slept](std::chrono::milliseconds d) {
        if (slept != nullptr) {
            slept->push_back(d);
        }
    };
    return o;
}

std::string header(const md::HttpRequest& r, const std::string& name) {
    for (const auto& [k, v] : r.headers) {
        if (k == name) {
            return v;
        }
    }
    return "";
}

// --- parsing -----------------------------------------------------------------

TEST(AlpacaBarsParse, ReadsEveryFieldExactlyAndTheNextPageToken) {
    const auto page = md::parse_alpaca_bars_page(fixture("bars_page1.json"));

    ASSERT_EQ(page.bars.size(), 3u);
    ASSERT_TRUE(page.next_page_token.has_value());
    EXPECT_EQ(*page.next_page_token, "TVNGVHwyMDI0LTAxLTAyVDA1OjAwOjAwLjAwMDAwMDAwMFo=");

    const auto& first = page.bars[0];
    EXPECT_EQ(first.symbol, "AAPL");
    EXPECT_EQ(first.type, dom::MarketEventType::Bar);
    EXPECT_EQ(first.exchange_time, at("2024-01-02T05:00:00Z"));
    EXPECT_EQ(price_text(first.open), "187.15");
    EXPECT_EQ(price_text(first.high), "188.44");
    EXPECT_EQ(price_text(first.low), "183.885");
    EXPECT_EQ(price_text(first.price), "185.64");
    ASSERT_TRUE(first.volume.has_value());
    EXPECT_EQ(*first.volume, 82488674);

    EXPECT_EQ(page.bars[2].symbol, "MSFT");
}

TEST(AlpacaBarsParse, ALastPageHasNoTokenAndEmptyOrNullBarsAreNoBars) {
    EXPECT_FALSE(md::parse_alpaca_bars_page(fixture("bars_page2.json")).next_page_token.has_value());
    EXPECT_TRUE(md::parse_alpaca_bars_page(fixture("bars_empty.json")).bars.empty());
    EXPECT_TRUE(md::parse_alpaca_bars_page(R"({"bars":null,"next_page_token":null})").bars.empty());
}

TEST(AlpacaBarsParse, PricesBeyondSixDecimalsRoundAndFractionalVolumesRound) {
    const auto page = md::parse_alpaca_bars_page(
        R"({"bars":{"X":[{"t":"2024-01-02T05:00:00Z","o":1.0000004,"h":2,"l":0.5,"c":1.5,"v":10.6}]}})");
    ASSERT_EQ(page.bars.size(), 1u);
    EXPECT_EQ(price_text(page.bars[0].open), "1");
    EXPECT_EQ(*page.bars[0].volume, 11);
}

TEST(AlpacaBarsParse, MalformedBodiesAreAlpacaErrorsNotSilentGaps) {
    const std::vector<std::string> bad = {
        "not json",
        "[]",
        R"({"bars":[]})",
        R"({"bars":{"X":{}}})",
        R"({"bars":{"X":[1]}})",
        R"({"bars":{"X":[{"o":1,"h":1,"l":1,"c":1,"v":1}]}})",
        R"({"bars":{"X":[{"t":"2024-01-02 05:00","o":1,"h":1,"l":1,"c":1,"v":1}]}})",
        R"({"bars":{"X":[{"t":"2024-01-02T05:00:00Z","o":"1","h":1,"l":1,"c":1,"v":1}]}})",
        R"({"bars":{"X":[{"t":"2024-01-02T05:00:00Z","o":1,"h":1,"l":1,"v":1}]}})",
    };
    for (const auto& body : bad) {
        EXPECT_THROW((void)md::parse_alpaca_bars_page(body), md::AlpacaError) << body;
    }
}

// --- request building ----------------------------------------------------------

TEST(AlpacaBarsUrl, EncodesEveryParameterAndAppendsThePageToken) {
    auto config = two_symbols();
    config.market_data_base_url = "https://data.alpaca.markets/";
    auto query = january_days();
    query.end = at("2024-01-04T00:00:00.5Z");

    EXPECT_EQ(md::alpaca_bars_url(config, query, ""),
              "https://data.alpaca.markets/v2/stocks/bars?symbols=AAPL%2CMSFT&timeframe=1Day"
              "&start=2024-01-01T00%3A00%3A00Z&end=2024-01-04T00%3A00%3A00.500000000Z"
              "&limit=10000&adjustment=raw&feed=iex&sort=asc");
    EXPECT_NE(md::alpaca_bars_url(config, query, "a+b=").find("&page_token=a%2Bb%3D"), std::string::npos);
}

TEST(AlpacaBarsQuery, RejectsQueriesAlpacaWouldRefuse) {
    const auto config = two_symbols();
    const auto good = january_days();
    EXPECT_NO_THROW(md::validate_alpaca_query(config, good));

    cfg::AlpacaConfig no_symbols;
    EXPECT_THROW(md::validate_alpaca_query(no_symbols, good), tec::ConfigError);

    auto backwards = good;
    backwards.end = backwards.start;
    EXPECT_THROW(md::validate_alpaca_query(config, backwards), tec::ConfigError);

    for (const char* tf : {"", "Day", "0Min", "1min", "5Sec"}) {
        auto q = good;
        q.timeframe = tf;
        EXPECT_THROW(md::validate_alpaca_query(config, q), tec::ConfigError) << tf;
    }
    for (const char* tf : {"1Min", "15Min", "1Hour", "4H", "1Day", "1Week", "3Month"}) {
        auto q = good;
        q.timeframe = tf;
        EXPECT_NO_THROW(md::validate_alpaca_query(config, q)) << tf;
    }

    auto adjusted = good;
    adjusted.adjustment = "none";
    EXPECT_THROW(md::validate_alpaca_query(config, adjusted), tec::ConfigError);

    auto huge = good;
    huge.page_limit = 10001;
    EXPECT_THROW(md::validate_alpaca_query(config, huge), tec::ConfigError);
}

// --- fetching ------------------------------------------------------------------

TEST(AlpacaFetch, FollowsPagesSendsCredentialsAndReturnsBarsInTimeThenSymbolOrder) {
    FakeHttp http;
    http.replies = {{200, fixture("bars_page1.json")}, {200, fixture("bars_page2.json")}};

    std::vector<std::size_t> pages_seen;
    auto options = no_sleep();
    options.on_page = [&](std::size_t pages, std::size_t) { pages_seen.push_back(pages); };

    const auto bars = md::fetch_alpaca_bars(http, kTestCredentials, two_symbols(), january_days(), options);

    ASSERT_EQ(http.requests.size(), 2u);
    EXPECT_EQ(header(http.requests[0], "APCA-API-KEY-ID"), "test-key-id");
    EXPECT_EQ(header(http.requests[0], "APCA-API-SECRET-KEY"), "test-secret");
    EXPECT_EQ(http.requests[0].url.find("page_token"), std::string::npos);
    EXPECT_NE(http.requests[1].url.find("&page_token=TVNGVHwy"), std::string::npos);
    EXPECT_EQ(pages_seen, (std::vector<std::size_t>{1, 2}));

    ASSERT_EQ(bars.size(), 4u);
    const std::vector<std::pair<std::string, const char*>> want = {
        {"AAPL", "2024-01-02T05:00:00Z"},
        {"MSFT", "2024-01-02T05:00:00Z"},
        {"AAPL", "2024-01-03T05:00:00Z"},
        {"MSFT", "2024-01-03T05:00:00Z"},
    };
    for (std::size_t i = 0; i < want.size(); ++i) {
        EXPECT_EQ(bars[i].symbol, want[i].first) << i;
        EXPECT_EQ(bars[i].exchange_time, at(want[i].second)) << i;
    }
}

TEST(AlpacaFetch, RetriesRateLimitsServerErrorsAndResetsWithDoublingBackoff) {
    FakeHttp http;
    http.replies = {{429, R"({"message":"too many requests."})"},
                    {503, ""},
                    {0, "", true},
                    {200, fixture("bars_page2.json")}};

    std::vector<std::chrono::milliseconds> slept;
    const auto bars = md::fetch_alpaca_bars(http, kTestCredentials, two_symbols(), january_days(), no_sleep(&slept));

    EXPECT_EQ(bars.size(), 1u);
    EXPECT_EQ(slept, (std::vector<std::chrono::milliseconds>{1000ms, 2000ms, 4000ms}));
}

TEST(AlpacaFetch, GivesUpAfterMaxAttemptsWithTheLastStatus) {
    FakeHttp http;
    auto options = no_sleep();
    options.max_attempts = 3;
    http.replies = {{503, ""}, {503, ""}, {503, R"({"message":"down for maintenance"})"}};

    try {
        (void)md::fetch_alpaca_bars(http, kTestCredentials, two_symbols(), january_days(), options);
        FAIL() << "expected AlpacaError";
    } catch (const md::AlpacaError& e) {
        EXPECT_EQ(e.status(), 503);
        EXPECT_NE(std::string(e.what()).find("down for maintenance"), std::string::npos);
    }
    EXPECT_EQ(http.requests.size(), 3u);
}

TEST(AlpacaFetch, AuthAndPlanErrorsAreNotRetriedAndCarryAlpacasMessageAndAHint) {
    FakeHttp http;
    http.replies = {{403, fixture("error_forbidden.json")}};

    try {
        (void)md::fetch_alpaca_bars(http, kTestCredentials, two_symbols(), january_days(), no_sleep());
        FAIL() << "expected AlpacaError";
    } catch (const md::AlpacaError& e) {
        const std::string what = e.what();
        EXPECT_EQ(e.status(), 403);
        EXPECT_NE(what.find("does not permit querying recent SIP data"), std::string::npos);
        EXPECT_NE(what.find("feed 'iex'"), std::string::npos);
    }
    EXPECT_EQ(http.requests.size(), 1u);
}

TEST(AlpacaFetch, ARepeatedPageTokenIsAnErrorInsteadOfALoop) {
    const std::string looping = R"({"bars":{},"next_page_token":"same"})";
    FakeHttp http;
    http.replies = {{200, looping}, {200, looping}};
    EXPECT_THROW((void)md::fetch_alpaca_bars(http, kTestCredentials, two_symbols(), january_days(), no_sleep()),
                 md::AlpacaError);
    EXPECT_EQ(http.requests.size(), 2u);
}

TEST(AlpacaFetch, AnInvalidQueryFailsBeforeAnyRequest) {
    FakeHttp http;
    auto query = january_days();
    query.timeframe = "daily";
    EXPECT_THROW((void)md::fetch_alpaca_bars(http, kTestCredentials, two_symbols(), query, no_sleep()),
                 tec::ConfigError);
    EXPECT_TRUE(http.requests.empty());
}

// --- credentials ---------------------------------------------------------------

void set_env(const char* name, const char* value) {
#if defined(_WIN32)
    _putenv_s(name, value);
#else
    if (*value == '\0') {
        unsetenv(name);
    } else {
        setenv(name, value, 1);
    }
#endif
}

TEST(AlpacaCredentials, ComeFromTheNamedVariablesAndMissingOnesAreNamed) {
    cfg::AlpacaConfig config;
    config.api_key_env = "TE_TEST_ALPACA_KEY";
    config.api_secret_env = "TE_TEST_ALPACA_SECRET";

    set_env("TE_TEST_ALPACA_KEY", "");
    set_env("TE_TEST_ALPACA_SECRET", "");
    try {
        (void)md::resolve_alpaca_credentials(config);
        FAIL() << "expected ConfigError";
    } catch (const tec::ConfigError& e) {
        EXPECT_NE(std::string(e.what()).find("TE_TEST_ALPACA_KEY and TE_TEST_ALPACA_SECRET"), std::string::npos);
    }

    set_env("TE_TEST_ALPACA_KEY", "k");
    set_env("TE_TEST_ALPACA_SECRET", "s");
    const auto credentials = md::resolve_alpaca_credentials(config);
    EXPECT_EQ(credentials.key_id, "k");
    EXPECT_EQ(credentials.secret_key, "s");

    set_env("TE_TEST_ALPACA_KEY", "");
    set_env("TE_TEST_ALPACA_SECRET", "");
}

// --- the source ----------------------------------------------------------------

TEST(AlpacaHistoricalSource, PushesEveryBarInOrderAndIsNotRunningAfterwards) {
    FakeHttp http;
    http.replies = {{200, fixture("bars_page1.json")}, {200, fixture("bars_page2.json")}};
    CollectingSink sink;
    tec::ManualClock clock;

    md::AlpacaHistoricalSource source{two_symbols(), january_days(), sink, clock, http, kTestCredentials, no_sleep()};
    source.start();

    EXPECT_FALSE(source.is_running());
    EXPECT_EQ(source.events_pushed(), 4u);
    ASSERT_EQ(sink.events.size(), 4u);
    EXPECT_EQ(sink.events.front().symbol, "AAPL");
    EXPECT_EQ(sink.events.back().symbol, "MSFT");
    EXPECT_LE(sink.events.front().exchange_time, sink.events.back().exchange_time);
}

TEST(AlpacaHistoricalSource, StopDuringTheFetchEndsStartQuietlyWithNothingPushed) {
    FakeHttp http;
    http.replies = {{200, fixture("bars_page1.json")}, {200, fixture("bars_page2.json")}};
    CollectingSink sink;
    tec::ManualClock clock;

    md::AlpacaHistoricalSource source{two_symbols(), january_days(), sink, clock, http, kTestCredentials, no_sleep()};
    http.on_get = [&] { source.stop(); };

    EXPECT_NO_THROW(source.start());
    EXPECT_EQ(http.requests.size(), 1u);
    EXPECT_TRUE(sink.events.empty());
    EXPECT_FALSE(source.is_running());
}

TEST(AlpacaHistoricalSource, AFailedFetchPropagatesAndLeavesTheSourceRestartable) {
    FakeHttp http;
    http.replies = {{401, R"({"message":"unauthorized."})"}, {200, fixture("bars_page2.json")}};
    CollectingSink sink;
    tec::ManualClock clock;

    md::AlpacaHistoricalSource source{two_symbols(), january_days(), sink, clock, http, kTestCredentials, no_sleep()};
    EXPECT_THROW(source.start(), md::AlpacaError);
    EXPECT_FALSE(source.is_running());

    source.start();
    EXPECT_EQ(sink.events.size(), 1u);
}

}  // namespace
