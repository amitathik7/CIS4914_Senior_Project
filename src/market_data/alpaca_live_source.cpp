#include "trading_engine/market_data/alpaca_live_source.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

#include "trading_engine/common/errors.hpp"

namespace trading_engine::market_data {

namespace {

using SteadyClock = std::chrono::steady_clock;

// an error reconnecting cannot fix
class FatalStreamError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

std::string describe(const AlpacaStreamError& error) {
    return "Alpaca stream error " + std::to_string(error.code) + ": " + error.message;
}

std::string join(const std::vector<common::Symbol>& symbols) {
    std::string out;
    for (const auto& s : symbols) {
        out += (out.empty() ? "" : ",") + s;
    }
    return out.empty() ? "-" : out;
}

}  // namespace

const char* to_string(LiveStatus status) noexcept {
    switch (status) {
        case LiveStatus::Connecting:    return "connecting";
        case LiveStatus::Connected:     return "connected";
        case LiveStatus::Authenticated: return "authenticated";
        case LiveStatus::Subscribed:    return "subscribed";
        case LiveStatus::Disconnected:  return "disconnected";
        case LiveStatus::Failed:        return "failed";
        case LiveStatus::Stopped:       return "stopped";
    }
    return "invalid";
}

AlpacaLiveSource::AlpacaLiveSource(config::AlpacaConfig config, IMarketEventSink& sink,
                                   const common::IClock& clock, AlpacaLiveOptions options)
    : config_{std::move(config)},
      sink_{sink},
      clock_{clock},
      websockets_{[] { return make_default_websocket(); }},
      options_{std::move(options)} {}

AlpacaLiveSource::AlpacaLiveSource(config::AlpacaConfig config, IMarketEventSink& sink,
                                   const common::IClock& clock, WebSocketFactory websockets,
                                   AlpacaCredentials credentials, AlpacaLiveOptions options)
    : config_{std::move(config)},
      sink_{sink},
      clock_{clock},
      websockets_{std::move(websockets)},
      credentials_{std::move(credentials)},
      options_{std::move(options)} {}

AlpacaLiveSource::~AlpacaLiveSource() { stop(); }

void AlpacaLiveSource::start() {
    if (running_.load() || worker_.joinable()) {
        throw std::logic_error("AlpacaLiveSource::start: already started; stop() first");
    }
    AlpacaStreamChannels& c = options_.channels;
    if (c.bars.empty() && c.trades.empty() && c.quotes.empty()) {
        if (config_.symbols.empty()) {
            throw common::ConfigError("Alpaca stream: no symbols to subscribe to");
        }
        c.bars = config_.symbols;
    }
    AlpacaCredentials credentials = credentials_ ? *credentials_ : resolve_alpaca_credentials(config_);

    {
        const std::lock_guard lock{mutex_};
        failure_.reset();
        stats_ = {};
    }
    stop_requested_ = false;
    running_ = true;
    worker_ = std::thread([this, credentials = std::move(credentials)]() mutable { run(std::move(credentials)); });
}

void AlpacaLiveSource::stop() {
    {
        const std::lock_guard lock{mutex_};
        stop_requested_ = true;
    }
    wake_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

bool AlpacaLiveSource::is_running() const { return running_.load(); }

std::optional<std::string> AlpacaLiveSource::failure() const {
    const std::lock_guard lock{mutex_};
    return failure_;
}

AlpacaLiveSource::Stats AlpacaLiveSource::stats() const {
    const std::lock_guard lock{mutex_};
    return stats_;
}

void AlpacaLiveSource::status(LiveStatus s, const std::string& detail) {
    if (options_.on_status) {
        options_.on_status(s, detail);
    }
}

// false when stop() interrupted the wait
bool AlpacaLiveSource::wait_backoff(std::chrono::milliseconds delay) {
    std::unique_lock lock{mutex_};
    return !wake_.wait_for(lock, delay, [this] { return stop_requested_.load(); });
}

void AlpacaLiveSource::run(AlpacaCredentials credentials) {
    std::chrono::milliseconds backoff = options_.first_backoff;
    int failures = 0;
    bool failed = false;

    while (!stop_requested_) {
        status(LiveStatus::Connecting, config_.stream_url);
        std::unique_ptr<IWebSocket> socket;
        bool subscribed = false;
        try {
            socket = websockets_();
            {
                const std::lock_guard lock{mutex_};
                ++stats_.connections;
            }
            session(*socket, credentials, subscribed);
            socket->close();
            break;   // stop() was requested
        } catch (const FatalStreamError& e) {
            if (socket) {
                socket->close();
            }
            const std::lock_guard lock{mutex_};
            failure_ = e.what();
            failed = true;
        } catch (const std::exception& e) {
            if (socket) {
                socket->close();
            }
            if (stop_requested_) {
                break;
            }
            if (subscribed) {
                failures = 0;
                backoff = options_.first_backoff;
            }
            ++failures;
            if (options_.max_consecutive_failures >= 0 && failures > options_.max_consecutive_failures) {
                const std::lock_guard lock{mutex_};
                failure_ = "gave up after " + std::to_string(failures) + " failed connections; last: " + e.what();
                failed = true;
            } else {
                status(LiveStatus::Disconnected, std::string(e.what()) + "; reconnecting in " +
                                                     std::to_string(backoff.count()) + " ms");
                if (!wait_backoff(backoff)) {
                    break;
                }
                backoff = std::min(backoff * 2, options_.max_backoff);
                const std::lock_guard lock{mutex_};
                ++stats_.reconnects;
                continue;
            }
        }
        if (failed) {
            status(LiveStatus::Failed, *failure());
            break;
        }
    }

    if (!failed) {
        status(LiveStatus::Stopped, "");
    }
    running_ = false;
}

void AlpacaLiveSource::session(IWebSocket& socket, const AlpacaCredentials& credentials, bool& subscribed_out) {
    // Reads until `done` says the awaited acknowledgement arrived. Errors in a
    // frame end the session, fatally or not by their code.
    const auto handle = [this](const std::string& text) {
        AlpacaStreamBatch batch = parse_alpaca_stream_message(text);
        for (const auto& error : batch.errors) {
            if (alpaca_stream_error_is_fatal(error.code)) {
                throw FatalStreamError(describe(error));
            }
            throw std::runtime_error(describe(error));
        }
        for (const auto& event : batch.events) {
            sink_.on_market_event(event);
        }
        const std::lock_guard lock{mutex_};
        ++stats_.messages;
        stats_.events += batch.events.size();
        return batch;
    };
    const auto await = [&](const char* what, const std::function<bool(const AlpacaStreamBatch&)>& done) {
        const auto deadline = SteadyClock::now() + options_.handshake_timeout;
        while (!stop_requested_) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - SteadyClock::now());
            if (left.count() <= 0) {
                throw WebSocketError(std::string("timed out waiting for Alpaca to confirm ") + what);
            }
            if (const auto text = socket.receive(std::min(left, options_.poll_interval))) {
                if (done(handle(*text))) {
                    return true;
                }
            }
        }
        return false;
    };
    const auto success = [](const char* msg) {
        return [msg](const AlpacaStreamBatch& b) {
            return std::find(b.successes.begin(), b.successes.end(), msg) != b.successes.end();
        };
    };

    socket.connect(config_.stream_url, {});
    if (!await("the connection", success("connected"))) {
        return;
    }
    status(LiveStatus::Connected, config_.stream_url);

    socket.send_text(alpaca_stream_auth_message(credentials));
    if (!await("authentication", success("authenticated"))) {
        return;
    }
    status(LiveStatus::Authenticated, "");

    socket.send_text(alpaca_stream_subscribe_message(options_.channels));
    AlpacaStreamChannels subscribed;
    if (!await("the subscription", [&](const AlpacaStreamBatch& b) {
            subscribed = b.subscribed;
            return b.subscription;
        })) {
        return;
    }
    subscribed_out = true;
    status(LiveStatus::Subscribed, "bars " + join(subscribed.bars) + ", trades " + join(subscribed.trades) +
                                       ", quotes " + join(subscribed.quotes));

    while (!stop_requested_) {
        if (const auto text = socket.receive(options_.poll_interval)) {
            (void)handle(*text);
        }
    }
}

}  // namespace trading_engine::market_data
