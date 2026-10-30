#pragma once

// -----------------------------------------------------------------------------
//  ConfiguredRun -- one run built from control::RunSettings, replayed in
//  market time on a bus.
//
//      bars ──▶ Pipeline (market data, the settings' strategies, portfolio)
//                   └──▶ OrderFlow (risk with the settings' limits, execution)
//
//  What `trading_engine run` and `trading_engine serve` share, so a run
//  started from the command line and one started from the console are the
//  same composition: starting cash, fees and slippage from
//  settings.simulation, risk limits from settings.risk, one strategy per
//  settings.strategies entry.
//
//  replay() drives the run the way docs/EVENT_BUS.md describes: the clock is
//  set to each bar's time and the bus is drained before the next timestamp,
//  so a market order fills at the next bar's open on every bus. After each
//  timestamp it calls `on_instant`, which can stop the run early.
//
//  Lifecycle: construct (subscribes nothing), replay() once (start, feed,
//  finish), read the results. The kill switch is flow().risk().
//  Ownership: borrows the bus and the clock, which must outlive it.
// -----------------------------------------------------------------------------

#include <cstddef>
#include <functional>
#include <vector>

#include "trading_engine/app/order_flow.hpp"
#include "trading_engine/app/pipeline.hpp"
#include "trading_engine/common/clock.hpp"
#include "trading_engine/control/run_settings.hpp"
#include "trading_engine/domain/market_event.hpp"

namespace trading_engine::app {

struct ReplayProgress {
    std::size_t       done{0};    // bars fed so far
    std::size_t       total{0};
    common::Timestamp clock{};    // the bar time just drained
};

class ConfiguredRun {
public:
    // Returns false to stop the replay after this timestamp.
    using InstantCallback = std::function<bool(const ReplayProgress&)>;

    // Throws control::SettingsError if a strategy refuses its settings.
    ConfiguredRun(events::IEventBus& bus, common::ManualClock& clock, const control::RunSettings& settings,
                  common::RunId run_id = common::RunId{1});

    ConfiguredRun(const ConfiguredRun&)            = delete;
    ConfiguredRun& operator=(const ConfiguredRun&) = delete;

    // The bars this run replays: the settings' symbols, inside the period,
    // sorted by exchange time (stable).
    [[nodiscard]] static std::vector<domain::MarketEvent> select_bars(const std::vector<domain::MarketEvent>& bars,
                                                                      const control::RunSettings& settings);

    // Starts the components, feeds `bars` in market time, finishes. Returns
    // true when every bar was fed, false when on_instant stopped it.
    bool replay(const std::vector<domain::MarketEvent>& bars, const InstantCallback& on_instant = {});

    [[nodiscard]] Pipeline& pipeline() noexcept { return pipeline_; }
    [[nodiscard]] OrderFlow& flow() noexcept { return flow_; }

private:
    events::IEventBus&   bus_;
    common::ManualClock& clock_;
    Pipeline             pipeline_;
    OrderFlow            flow_;
};

}  // namespace trading_engine::app
