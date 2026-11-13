#pragma once

// -----------------------------------------------------------------------------
//  AlpacaLiveSource -- WebSocket adapter for the Alpaca live market data feed.
//
//  start() resolves credentials (throwing common::ConfigError if they are
//  missing) and spawns one I/O thread that connects to config.stream_url,
//  authenticates, subscribes, and pushes each bar / trade / quote to the sink
//  from that thread. A dropped connection or a retryable server error
//  reconnects with a doubling backoff; an error reconnecting cannot fix (bad
//  credentials, a plan without the feed, the one-connection limit) ends the
//  thread, and failure() says why. stop() joins the thread.
//
//  Thread-safety: the sink's on_market_event() runs on the I/O thread.
//  start/stop/failure/stats may be called from any thread.
// -----------------------------------------------------------------------------

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "trading_engine/common/clock.hpp"
#include "trading_engine/configuration/engine_config.hpp"
#include "trading_engine/market_data/alpaca_historical_source.hpp"
#include "trading_engine/market_data/alpaca_stream.hpp"
#include "trading_engine/market_data/market_data_source.hpp"
#include "trading_engine/market_data/websocket.hpp"

namespace trading_engine::market_data {

// Alpaca's always-on test stream: symbol FAKEPACA, any time of day.
inline constexpr const char* kAlpacaTestStreamUrl = "wss://stream.data.alpaca.markets/v2/test";

enum class LiveStatus : std::uint8_t {
    Connecting,
    Connected,
    Authenticated,
    Subscribed,
    Disconnected,   // will reconnect
    Failed,         // will not reconnect; see failure()
    Stopped,
};

[[nodiscard]] const char* to_string(LiveStatus status) noexcept;

struct AlpacaLiveOptions {
    // Empty: minute bars for every symbol in AlpacaConfig::symbols.
    AlpacaStreamChannels channels{};
    std::chrono::milliseconds first_backoff{1000};
    std::chrono::milliseconds max_backoff{30'000};
    int max_consecutive_failures{-1};                   // -1: keep reconnecting
    std::chrono::milliseconds handshake_timeout{10'000};   // per connect/auth/subscribe step
    std::chrono::milliseconds poll_interval{250};          // how often stop() is noticed
    std::function<void(LiveStatus, const std::string&)> on_status{};   // called on the I/O thread
};

using WebSocketFactory = std::function<std::unique_ptr<IWebSocket>()>;

class AlpacaLiveSource final : public IMarketDataSource {
public:
    struct Stats {
        std::uint64_t connections{0};
        std::uint64_t messages{0};
        std::uint64_t events{0};
        std::uint64_t reconnects{0};
    };

    // Default WebSocket client; credentials from the environment at start().
    AlpacaLiveSource(config::AlpacaConfig config, IMarketEventSink& sink, const common::IClock& clock,
                     AlpacaLiveOptions options = {});

    AlpacaLiveSource(config::AlpacaConfig config, IMarketEventSink& sink, const common::IClock& clock,
                     WebSocketFactory websockets, AlpacaCredentials credentials, AlpacaLiveOptions options = {});

    ~AlpacaLiveSource() override;   // stop()

    AlpacaLiveSource(const AlpacaLiveSource&) = delete;
    AlpacaLiveSource& operator=(const AlpacaLiveSource&) = delete;

    void start() override;
    void stop() override;
    [[nodiscard]] bool is_running() const override;

    [[nodiscard]] std::optional<std::string> failure() const;
    [[nodiscard]] Stats stats() const;

private:
    void run(AlpacaCredentials credentials);
    void session(IWebSocket& socket, const AlpacaCredentials& credentials, bool& subscribed);
    void status(LiveStatus status, const std::string& detail);
    bool wait_backoff(std::chrono::milliseconds delay);

    config::AlpacaConfig config_;
    IMarketEventSink& sink_;
    [[maybe_unused]] const common::IClock& clock_;
    WebSocketFactory websockets_;
    std::optional<AlpacaCredentials> credentials_;
    AlpacaLiveOptions options_;

    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    mutable std::mutex mutex_;   // guards failure_, stats_ and the backoff wait
    std::condition_variable wake_;
    std::optional<std::string> failure_;
    Stats stats_;
    std::thread worker_;
};

}  // namespace trading_engine::market_data
