#pragma once

// -----------------------------------------------------------------------------
//  FakeStrategy -- a scriptable, observable IStrategy for exercising the real
//  StrategyEngine.
//
//  Script it by setting the public fields before the engine drives it; observe
//  it through the other public fields afterwards. If it is given a CallLog it
//  also appends one line per callback ("<id>:start", "<id>:event:<symbol>",
//  "<id>:stop"), so a test can pin the order of calls ACROSS strategies with a
//  single comparison.
//
//  Per market event it first emits every signal in `emits`, then runs
//  `on_event_hook`, then throws if `throw_on_event` is set: a throwing
//  strategy has therefore always emitted before it fails. The start / stop
//  hooks run before the matching throw flag is honoured.
//
//  It applies no symbol or event-type filtering: like every real strategy, it
//  receives whatever the engine routes to it.
// -----------------------------------------------------------------------------

#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "trading_engine/domain/market_event.hpp"
#include "trading_engine/domain/trade_signal.hpp"
#include "trading_engine/strategy/strategy.hpp"

namespace trading_engine::test_support {

// Shared by the strategies of one test; the test owns it and must keep it alive
// for as long as any strategy that was given it can still be called.
using CallLog = std::vector<std::string>;

class FakeStrategy final : public strategy::IStrategy {
public:
    explicit FakeStrategy(std::string strategy_id, CallLog* call_log = nullptr)
        : id_{std::move(strategy_id)}, log_{call_log} {}

    // --- Script --------------------------------------------------------------
    std::vector<domain::TradeSignal> emits{};   // emitted, in order, on every event
    std::function<void(const domain::MarketEvent&, strategy::ISignalSink&)> on_event_hook{};
    std::function<void()> on_start_hook{};
    std::function<void()> on_stop_hook{};
    bool throw_on_start{false};
    bool throw_on_stop{false};
    bool throw_on_event{false};

    // --- Observations --------------------------------------------------------
    int starts{0};
    int stops{0};
    std::vector<domain::MarketEvent> received{};
    strategy::ISignalSink* last_sink{nullptr};   // the sink of the latest on_market_event()

    // --- strategy::IStrategy -------------------------------------------------
    [[nodiscard]] std::string_view id() const override { return id_; }

    void on_start() override {
        ++starts;
        note("start");
        if (on_start_hook) {
            on_start_hook();
        }
        if (throw_on_start) {
            throw std::runtime_error(id_ + ": on_start failed");
        }
    }

    void on_stop() override {
        ++stops;
        note("stop");
        if (on_stop_hook) {
            on_stop_hook();
        }
        if (throw_on_stop) {
            throw std::runtime_error(id_ + ": on_stop failed");
        }
    }

    void on_market_event(const domain::MarketEvent& event, strategy::ISignalSink& out) override {
        received.push_back(event);
        last_sink = &out;
        note("event:" + event.symbol);

        for (const domain::TradeSignal& signal : emits) {
            out.emit(signal);
        }
        if (on_event_hook) {
            on_event_hook(event, out);
        }
        if (throw_on_event) {
            throw std::runtime_error(id_ + ": on_market_event failed");
        }
    }

private:
    void note(const std::string& what) {
        if (log_ != nullptr) {
            log_->push_back(id_ + ":" + what);
        }
    }

    std::string id_;
    CallLog*    log_;
};

}  // namespace trading_engine::test_support
