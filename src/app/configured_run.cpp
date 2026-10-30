#include "trading_engine/app/configured_run.hpp"

#include <algorithm>
#include <set>

#include "trading_engine/events/event_bus.hpp"

namespace trading_engine::app {

namespace {

PipelineConfig pipeline_config(const control::RunSettings& s, common::RunId run_id) {
    PipelineConfig c;
    c.run_id        = run_id;
    c.starting_cash = s.simulation.starting_cash;
    c.fees          = s.simulation.fees;
    return c;
}

OrderFlowConfig flow_config(const control::RunSettings& s, common::RunId run_id) {
    OrderFlowConfig c;
    c.run_id        = run_id;
    c.risk          = s.risk;
    c.simulation    = s.simulation;
    c.risk_policies = s.risk_policies;
    return c;
}

}  // namespace

ConfiguredRun::ConfiguredRun(events::IEventBus& bus, common::ManualClock& clock,
                             const control::RunSettings& settings, common::RunId run_id)
    : bus_{bus},
      clock_{clock},
      pipeline_{bus, clock, pipeline_config(settings, run_id)},
      flow_{pipeline_, bus, clock, flow_config(settings, run_id)} {
    for (auto& strategy : control::make_strategies(settings)) {
        pipeline_.add_strategy(std::move(strategy));
    }
}

std::vector<domain::MarketEvent> ConfiguredRun::select_bars(const std::vector<domain::MarketEvent>& bars,
                                                            const control::RunSettings& settings) {
    const auto list = settings.symbols();
    const std::set<common::Symbol> symbols(list.begin(), list.end());
    std::vector<domain::MarketEvent> out;
    for (const auto& bar : bars) {
        if (symbols.count(bar.symbol) == 0) {
            continue;
        }
        if ((settings.period_start && bar.exchange_time < *settings.period_start) ||
            (settings.period_end && bar.exchange_time > *settings.period_end)) {
            continue;
        }
        out.push_back(bar);
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const auto& a, const auto& b) { return a.exchange_time < b.exchange_time; });
    return out;
}

bool ConfiguredRun::replay(const std::vector<domain::MarketEvent>& bars, const InstantCallback& on_instant) {
    if (!bars.empty()) {
        clock_.set(bars.front().exchange_time);
    }
    flow_.start();   // before the pipeline: execution fills before the portfolio marks
    pipeline_.start();
    bool complete = true;
    for (std::size_t i = 0; i < bars.size(); ++i) {
        clock_.set(bars[i].exchange_time);
        pipeline_.input().on_market_event(bars[i]);
        if (i + 1 == bars.size() || bars[i + 1].exchange_time != bars[i].exchange_time) {
            bus_.wait_until_drained();   // one timestamp at a time
            if (on_instant && !on_instant(ReplayProgress{i + 1, bars.size(), bars[i].exchange_time}) &&
                i + 1 < bars.size()) {
                complete = false;
                break;
            }
        }
    }
    pipeline_.finish();
    flow_.stop();
    return complete;
}

}  // namespace trading_engine::app
