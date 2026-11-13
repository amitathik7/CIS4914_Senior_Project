#pragma once

// -----------------------------------------------------------------------------
//  Live feed plumbing shared by `trading_engine run --alpaca-live` and live
//  runs started from the console through `trading_engine serve`.
//
//  A live source pushes events from its own I/O thread; LiveEventQueue hands
//  them to the run's thread, and feed_live() feeds each one through
//  ConfiguredRun::feed() until told to stop or the source gives up.
// -----------------------------------------------------------------------------

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "trading_engine/common/clock.hpp"
#include "trading_engine/common/types.hpp"
#include "trading_engine/domain/market_event.hpp"
#include "trading_engine/market_data/market_data_source.hpp"

namespace trading_engine::app {

class ConfiguredRun;

class LiveEventQueue final : public market_data::IMarketEventSink {
public:
    void on_market_event(const domain::MarketEvent& event) override;

    // The oldest event, or nullopt after `timeout` with none.
    [[nodiscard]] std::optional<domain::MarketEvent> pop(std::chrono::milliseconds timeout);

private:
    std::mutex                      mutex_;
    std::condition_variable         ready_;
    std::deque<domain::MarketEvent> queue_;
};

// A started-or-not live source and how to ask it why it stopped.
struct LiveFeed {
    std::unique_ptr<market_data::IMarketDataSource> source;
    std::function<std::optional<std::string>()>     failure;   // nullopt: no unrecoverable error
};

// Opens a live feed of `symbols` on `feed` (iex, sip or test) that pushes to
// `sink`. Throws if it cannot be built (no credentials, no WebSocket support).
using LiveFeedFactory = std::function<LiveFeed(const std::vector<common::Symbol>& symbols, const std::string& feed,
                                               market_data::IMarketEventSink& sink, const common::IClock& clock)>;

struct LiveHooks {
    std::function<bool()>                           keep_going;   // checked between events; unset: forever
    std::function<void(const domain::MarketEvent&)> before_feed;  // printing, recording
    std::function<void(const domain::MarketEvent&)> after_feed;   // equity samples, progress reports
};

// Starts `feed.source`, begins `run` at `start`, feeds every event until
// keep_going() is false or the source stops, then stops the source, feeds what
// was already queued, and ends the run. Returns the source's failure, if any.
std::optional<std::string> feed_live(ConfiguredRun& run, LiveFeed& feed, LiveEventQueue& queue,
                                     common::Timestamp start, const LiveHooks& hooks = {});

}  // namespace trading_engine::app
