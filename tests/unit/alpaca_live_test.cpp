// Alpaca stream protocol and AlpacaLiveSource, driven by a scripted fake
// WebSocket: no network, no credentials. Messages follow Alpaca's documented
// v2 stream format.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "trading_engine/common/clock.hpp"
#include "trading_engine/common/decimal_text.hpp"
#include "trading_engine/common/errors.hpp"
#include "trading_engine/common/timestamp_text.hpp"
#include "trading_engine/market_data/alpaca_live_source.hpp"
#include "trading_engine/market_data/alpaca_stream.hpp"

namespace {

namespace md  = trading_engine::market_data;
namespace tec = trading_engine::common;
namespace cfg = trading_engine::config;
namespace dom = trading_engine::domain;

using namespace std::chrono_literals;

const std::string kConnected = R"([{"T":"success","msg":"connected"}])";
const std::string kAuthenticated = R"([{"T":"success","msg":"authenticated"}])";
const std::string kAuthFailed = R"([{"T":"error","code":402,"msg":"auth failed"}])";
const std::string kSubscribed =
    R"([{"T":"subscription","trades":[],"quotes":[],"bars":["AAPL","MSFT"],"updatedBars":[],"dailyBars":[]}])";

std::string bar(const char* symbol, const char* time, const char* close) {
    return std::string(R"([{"T":"b","S":")") + symbol + R"(","o":1.5,"h":2.5,"l":1.25,"c":)" + close +
           R"(,"v":100,"t":")" + time + R"(","n":3,"vw":2}])";
}

std::string price_text(const std::optional<tec::Price>& p) { return p ? tec::format_decimal(*p) : "none"; }

// --- protocol --------------------------------------------------------------------

TEST(AlpacaStreamProtocol, ClientMessagesAreTheDocumentedJson) {
    // Outside the macro: MSVC mis-lexes escapes in raw strings passed as macro arguments.
    const md::AlpacaCredentials credentials{"id", "se\"cret"};
    const std::string auth = R"({"action":"auth","key":"id","secret":"se\"cret"})";
    const std::string subscribe =
        R"({"action":"subscribe","bars":["AAPL","MSFT"],"trades":[],"quotes":["SPY"]})";
    EXPECT_EQ(md::alpaca_stream_auth_message(credentials), auth);
    EXPECT_EQ(md::alpaca_stream_subscribe_message({{"AAPL", "MSFT"}, {}, {"SPY"}}), subscribe);
}

TEST(AlpacaStreamProtocol, ParsesControlMessagesBarsTradesAndQuotes) {
    const auto control = md::parse_alpaca_stream_message(
        R"([{"T":"success","msg":"connected"},{"T":"error","code":406,"msg":"connection limit exceeded"}])");
    EXPECT_EQ(control.successes, (std::vector<std::string>{"connected"}));
    ASSERT_EQ(control.errors.size(), 1u);
    EXPECT_EQ(control.errors[0].code, 406);
    EXPECT_EQ(control.errors[0].message, "connection limit exceeded");

    const auto sub = md::parse_alpaca_stream_message(kSubscribed);
    EXPECT_TRUE(sub.subscription);
    EXPECT_EQ(sub.subscribed.bars, (std::vector<std::string>{"AAPL", "MSFT"}));

    const auto data = md::parse_alpaca_stream_message(
        R"([{"T":"b","S":"AAPL","o":187.15,"h":188.44,"l":183.885,"c":185.64,"v":8248,"t":"2026-11-13T15:31:00Z"},)"
        R"({"T":"t","S":"AAPL","i":96921,"x":"D","p":126.55,"s":1,"t":"2026-11-13T15:31:44.208123456Z","c":["@","I"],"z":"C"},)"
        R"({"T":"q","S":"AAPL","bx":"U","bp":126.5,"bs":1,"ax":"Q","ap":126.6,"as":4,"t":"2026-11-13T15:31:44.3Z","c":["R"],"z":"C"},)"
        R"({"T":"u","S":"AAPL","o":1,"h":1,"l":1,"c":1,"v":1,"t":"2026-11-13T15:31:00Z"},)"
        R"({"T":"s","S":"AAPL","sc":"H","sm":"Trading Halt"}])");
    ASSERT_EQ(data.events.size(), 3u);
    EXPECT_EQ(data.ignored, 2u);

    const auto& b = data.events[0];
    EXPECT_EQ(b.type, dom::MarketEventType::Bar);
    EXPECT_EQ(b.symbol, "AAPL");
    EXPECT_EQ(price_text(b.open), "187.15");
    EXPECT_EQ(price_text(b.low), "183.885");
    EXPECT_EQ(price_text(b.price), "185.64");
    EXPECT_EQ(*b.volume, 8248);
    EXPECT_EQ(b.exchange_time, *tec::parse_timestamp("2026-11-13T15:31:00Z"));

    const auto& t = data.events[1];
    EXPECT_EQ(t.type, dom::MarketEventType::Trade);
    EXPECT_EQ(price_text(t.price), "126.55");
    EXPECT_EQ(*t.size, 1);
    EXPECT_EQ(t.exchange_time, *tec::parse_timestamp("2026-11-13T15:31:44.208123456Z"));

    const auto& q = data.events[2];
    EXPECT_EQ(q.type, dom::MarketEventType::Quote);
    EXPECT_EQ(price_text(q.bid), "126.5");
    EXPECT_EQ(price_text(q.ask), "126.6");
}

TEST(AlpacaStreamProtocol, MalformedFramesAreErrors) {
    for (const char* bad : {"{}", "nope", "[1]", R"([{"msg":"x"}])", R"([{"T":"b","S":"X","t":"2026-11-13T15:31:00Z"}])",
                            R"([{"T":"t","S":"","p":1,"s":1,"t":"2026-11-13T15:31:00Z"}])",
                            R"([{"T":"t","S":"X","p":1,"s":1,"t":"yesterday"}])"}) {
        EXPECT_THROW((void)md::parse_alpaca_stream_message(bad), md::AlpacaError) << bad;
    }
}

TEST(AlpacaStreamProtocol, OnlyErrorsReconnectingCannotFixAreFatal) {
    for (const int code : {400, 401, 402, 405, 406, 409, 410}) {
        EXPECT_TRUE(md::alpaca_stream_error_is_fatal(code)) << code;
    }
    for (const int code : {404, 407, 500, 0}) {
        EXPECT_FALSE(md::alpaca_stream_error_is_fatal(code)) << code;
    }
}

// --- the source --------------------------------------------------------------------

// One scripted connection: replies to connect, auth and subscribe, then
// `after` in order. An empty string in `after` makes receive() throw, as a
// dropped connection does.
struct Script {
    bool connect_fails{false};
    std::string on_connect{kConnected};
    std::string on_auth{kAuthenticated};
    std::string on_subscribe{kSubscribed};
    std::vector<std::string> after;
};

Script refused() {
    Script s;
    s.connect_fails = true;
    return s;
}

struct Server {
    std::mutex mutex;
    std::deque<Script> scripts;
    std::vector<std::string> sent;
    std::vector<std::string> urls;
    int connections{0};
};

class FakeSocket final : public md::IWebSocket {
public:
    FakeSocket(Server& server, Script script) : server_{server}, script_{std::move(script)} {}

    void connect(const std::string& url, const std::vector<std::pair<std::string, std::string>>&) override {
        {
            const std::lock_guard lock{server_.mutex};
            server_.urls.push_back(url);
        }
        if (script_.connect_fails) {
            throw md::WebSocketError("connection refused");
        }
        push(script_.on_connect);
    }

    void send_text(std::string_view message) override {
        {
            const std::lock_guard lock{server_.mutex};
            server_.sent.emplace_back(message);
        }
        if (message.find(R"("action":"auth")") != std::string_view::npos) {
            push(script_.on_auth);
        } else if (message.find(R"("action":"subscribe")") != std::string_view::npos) {
            push(script_.on_subscribe);
            for (const auto& m : script_.after) {
                inbound_.push_back(m);
            }
        }
    }

    std::optional<std::string> receive(std::chrono::milliseconds timeout) override {
        if (inbound_.empty()) {
            std::this_thread::sleep_for(std::min(timeout, std::chrono::milliseconds{2}));
            return std::nullopt;
        }
        std::string m = std::move(inbound_.front());
        inbound_.pop_front();
        if (m.empty()) {
            throw md::WebSocketError("connection reset");
        }
        return m;
    }

    void close() noexcept override {}

private:
    void push(const std::string& m) {
        if (!m.empty()) {
            inbound_.push_back(m);
        }
    }

    Server& server_;
    Script script_;
    std::deque<std::string> inbound_;
};

class Sink final : public md::IMarketEventSink {
public:
    void on_market_event(const dom::MarketEvent& event) override {
        const std::lock_guard lock{mutex_};
        events_.push_back(event);
    }
    std::vector<dom::MarketEvent> events() {
        const std::lock_guard lock{mutex_};
        return events_;
    }

private:
    std::mutex mutex_;
    std::vector<dom::MarketEvent> events_;
};

struct Harness {
    Server server;
    Sink sink;
    tec::ManualClock clock;
    std::mutex status_mutex;
    std::vector<md::LiveStatus> statuses;
    std::unique_ptr<md::AlpacaLiveSource> source;

    explicit Harness(std::vector<Script> scripts, md::AlpacaLiveOptions options = {}) {
        server.scripts.assign(scripts.begin(), scripts.end());
        cfg::AlpacaConfig config;
        config.symbols = {"AAPL", "MSFT"};
        options.first_backoff = 1ms;
        options.max_backoff = 4ms;
        options.poll_interval = 5ms;
        if (options.handshake_timeout == 10'000ms) {
            options.handshake_timeout = 500ms;
        }
        options.on_status = [this](md::LiveStatus s, const std::string&) {
            const std::lock_guard lock{status_mutex};
            statuses.push_back(s);
        };
        source = std::make_unique<md::AlpacaLiveSource>(
            config, sink, clock,
            [this]() -> std::unique_ptr<md::IWebSocket> {
                const std::lock_guard lock{server.mutex};
                ++server.connections;
                Script script = server.scripts.empty() ? refused() : server.scripts.front();
                if (!server.scripts.empty()) {
                    server.scripts.pop_front();
                }
                return std::make_unique<FakeSocket>(server, std::move(script));
            },
            md::AlpacaCredentials{"test-key", "test-secret"}, options);
    }

    bool wait_for(const std::function<bool()>& done, std::chrono::milliseconds limit = 3s) {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        while (std::chrono::steady_clock::now() < deadline) {
            if (done()) {
                return true;
            }
            std::this_thread::sleep_for(2ms);
        }
        return done();
    }

    bool saw(md::LiveStatus s) {
        const std::lock_guard lock{status_mutex};
        return std::find(statuses.begin(), statuses.end(), s) != statuses.end();
    }
};

TEST(AlpacaLiveSource, AuthenticatesSubscribesToBarsAndPushesThemInArrivalOrder) {
    Script s;
    s.after = {bar("AAPL", "2026-11-13T15:31:00Z", "10.5"), bar("MSFT", "2026-11-13T15:31:00Z", "20.25")};
    Harness h{{s}};

    h.source->start();
    ASSERT_TRUE(h.wait_for([&] { return h.sink.events().size() == 2; }));
    EXPECT_TRUE(h.source->is_running());
    h.source->stop();
    EXPECT_FALSE(h.source->is_running());

    const auto events = h.sink.events();
    EXPECT_EQ(events[0].symbol, "AAPL");
    EXPECT_EQ(price_text(events[0].price), "10.5");
    EXPECT_EQ(events[1].symbol, "MSFT");

    ASSERT_EQ(h.server.sent.size(), 2u);
    EXPECT_EQ(h.server.sent[0], R"({"action":"auth","key":"test-key","secret":"test-secret"})");
    EXPECT_EQ(h.server.sent[1], R"({"action":"subscribe","bars":["AAPL","MSFT"],"trades":[],"quotes":[]})");
    EXPECT_EQ(h.server.urls[0], "wss://stream.data.alpaca.markets/v2/iex");

    const std::vector<md::LiveStatus> want = {md::LiveStatus::Connecting, md::LiveStatus::Connected,
                                              md::LiveStatus::Authenticated, md::LiveStatus::Subscribed,
                                              md::LiveStatus::Stopped};
    EXPECT_EQ(h.statuses, want);
    EXPECT_FALSE(h.source->failure().has_value());
    EXPECT_EQ(h.source->stats().events, 2u);
}

TEST(AlpacaLiveSource, BadCredentialsFailOnceWithoutReconnecting) {
    Script s;
    s.on_auth = kAuthFailed;
    Harness h{{s, Script{}}};

    h.source->start();
    ASSERT_TRUE(h.wait_for([&] { return !h.source->is_running(); }));
    h.source->stop();

    ASSERT_TRUE(h.source->failure().has_value());
    EXPECT_NE(h.source->failure()->find("402"), std::string::npos);
    EXPECT_EQ(h.server.connections, 1);
    EXPECT_TRUE(h.saw(md::LiveStatus::Failed));
    EXPECT_FALSE(h.saw(md::LiveStatus::Stopped));
}

TEST(AlpacaLiveSource, ADroppedConnectionReconnectsAndKeepsStreaming) {
    Script first;
    first.after = {bar("AAPL", "2026-11-13T15:31:00Z", "1"), ""};
    Script second;
    second.after = {bar("AAPL", "2026-11-13T15:32:00Z", "2")};
    Harness h{{first, second}};

    h.source->start();
    ASSERT_TRUE(h.wait_for([&] { return h.sink.events().size() == 2; }));
    h.source->stop();

    EXPECT_EQ(h.server.connections, 2);
    EXPECT_EQ(h.source->stats().reconnects, 1u);
    EXPECT_TRUE(h.saw(md::LiveStatus::Disconnected));
    EXPECT_FALSE(h.source->failure().has_value());
    EXPECT_EQ(price_text(h.sink.events()[1].price), "2");
}

TEST(AlpacaLiveSource, RetryableServerErrorsReconnect) {
    Script slow;
    slow.after = {R"([{"T":"error","code":407,"msg":"slow client"}])"};
    Harness h{{slow, Script{}}};

    h.source->start();
    ASSERT_TRUE(h.wait_for([&] { return h.server.connections == 2 && h.saw(md::LiveStatus::Subscribed); }));
    h.source->stop();
    EXPECT_FALSE(h.source->failure().has_value());
}

TEST(AlpacaLiveSource, GivesUpAfterTheConfiguredNumberOfFailedConnections) {
    md::AlpacaLiveOptions options;
    options.max_consecutive_failures = 2;
    Harness h{{refused(), refused(), refused(), Script{}}, options};

    h.source->start();
    ASSERT_TRUE(h.wait_for([&] { return !h.source->is_running(); }));
    h.source->stop();

    EXPECT_EQ(h.server.connections, 3);
    ASSERT_TRUE(h.source->failure().has_value());
    EXPECT_NE(h.source->failure()->find("gave up after 3"), std::string::npos);
}

TEST(AlpacaLiveSource, AMissingAcknowledgementTimesOutAndReconnects) {
    md::AlpacaLiveOptions options;
    options.handshake_timeout = 30ms;
    Script silent;
    silent.on_connect = "";
    Harness h{{silent, Script{}}, options};

    h.source->start();
    ASSERT_TRUE(h.wait_for([&] { return h.saw(md::LiveStatus::Subscribed); }));
    h.source->stop();
    EXPECT_EQ(h.server.connections, 2);
}

TEST(AlpacaLiveSource, StopInterruptsABackoffAndTheSourceCanStartAgain) {
    md::AlpacaLiveOptions options;
    cfg::AlpacaConfig config;
    config.symbols = {"AAPL"};
    options.first_backoff = 60s;
    options.poll_interval = 5ms;
    Server server;
    server.scripts = {refused(), Script{}};
    Sink sink;
    tec::ManualClock clock;
    md::AlpacaLiveSource source{config, sink, clock,
                                [&]() -> std::unique_ptr<md::IWebSocket> {
                                    const std::lock_guard lock{server.mutex};
                                    ++server.connections;
                                    Script script = server.scripts.front();
                                    server.scripts.pop_front();
                                    return std::make_unique<FakeSocket>(server, std::move(script));
                                },
                                md::AlpacaCredentials{"k", "s"}, options};

    source.start();
    std::this_thread::sleep_for(20ms);
    const auto before = std::chrono::steady_clock::now();
    source.stop();
    EXPECT_LT(std::chrono::steady_clock::now() - before, 2s);
    EXPECT_FALSE(source.is_running());

    source.start();
    std::this_thread::sleep_for(50ms);
    source.stop();
    EXPECT_EQ(server.connections, 2);
}

TEST(AlpacaLiveSource, NoSymbolsOrNoCredentialsFailAtStart) {
    Sink sink;
    tec::ManualClock clock;
    cfg::AlpacaConfig none;
    md::AlpacaLiveSource empty{none, sink, clock, [] { return std::unique_ptr<md::IWebSocket>{}; },
                               md::AlpacaCredentials{"k", "s"}};
    EXPECT_THROW(empty.start(), tec::ConfigError);
    EXPECT_FALSE(empty.is_running());

    cfg::AlpacaConfig config;
    config.symbols = {"AAPL"};
    config.api_key_env = "TE_TEST_UNSET_ALPACA_KEY";
    config.api_secret_env = "TE_TEST_UNSET_ALPACA_SECRET";
    md::AlpacaLiveSource from_env{config, sink, clock};
    EXPECT_THROW(from_env.start(), tec::ConfigError);
    EXPECT_FALSE(from_env.is_running());
}

}  // namespace
