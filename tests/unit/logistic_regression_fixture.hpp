#pragma once

// Shared scaffolding for the logistic-regression tests: bar and config builders,
// deterministic price series, and a Harness that drives the strategy by hand and
// records its signals and per-bar diagnostics. Everything is inline: several test
// files include this header.

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "support/price_literals.hpp"
#include "trading_engine/common/types.hpp"
#include "trading_engine/domain/market_event.hpp"
#include "trading_engine/domain/trade_signal.hpp"
#include "trading_engine/strategy/logistic_regression_strategy.hpp"
#include "trading_engine/strategy/strategy_diagnostics.hpp"

namespace lr_test {

namespace common   = trading_engine::common;
namespace domain   = trading_engine::domain;
namespace strategy = trading_engine::strategy;
namespace ml       = trading_engine::strategy::ml;

using Closes = std::vector<common::Price>;

using trading_engine::test_support::micros;
using trading_engine::test_support::units;

inline common::Timestamp at_minute(long long minute) {
    return common::Timestamp{} + std::chrono::minutes{minute};
}

inline domain::MarketEvent bar(const std::string& symbol, common::Price close, long long minute) {
    domain::MarketEvent event;
    event.symbol        = symbol;
    event.type          = domain::MarketEventType::Bar;
    event.exchange_time = at_minute(minute);
    event.price         = close;
    return event;
}

// Horizon 1, emits as soon as one sample has trained, so short series reach decisions.
inline strategy::LogisticRegressionConfig make_config(std::vector<std::string> symbols = {"AAPL"}) {
    strategy::LogisticRegressionConfig result;
    result.horizon              = 1;
    result.min_training_samples = 1;
    result.symbols              = std::move(symbols);
    return result;
}

// Regimes of `run` bars rising 1% per bar, then `run` bars falling 1% per bar, and so
// on: the last move predicts the next one except at the turns. Rounded to micros.
inline Closes trending(std::size_t bars, std::size_t run = 30) {
    Closes out;
    double price = 100.0;
    for (std::size_t i = 0; i < bars; ++i) {
        const bool rising = (i / run) % 2 == 0;
        price *= rising ? 1.01 : 0.99;
        out.push_back(micros(static_cast<std::int64_t>(std::llround(price * 1e6))));
    }
    return out;
}

// A random walk with +-1% steps from a fixed-seed linear congruential generator, so
// it is the same on every platform: there is nothing to learn.
inline Closes random_walk(std::size_t bars, std::uint64_t seed = 12345) {
    Closes out;
    double price = 100.0;
    for (std::size_t i = 0; i < bars; ++i) {
        seed  = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        price *= ((seed >> 33) & 1U) != 0 ? 1.01 : 0.99;
        out.push_back(micros(static_cast<std::int64_t>(std::llround(price * 1e6))));
    }
    return out;
}

// One record per on_market_event() call, as the observer saw it.
struct Seen {
    strategy::BarVerdict  verdict{};
    strategy::BarReason   reason{};
    strategy::BarAction   action{};
    std::optional<double> probability{};
    std::string           latch_after{};
};

class Harness final : public strategy::ISignalSink, public strategy::IStrategyObserver {
public:
    explicit Harness(strategy::LogisticRegressionConfig cfg) : lr{std::move(cfg)} {
        lr.set_observer(this);
    }
    ~Harness() override { lr.set_observer(nullptr); }

    Harness(const Harness&)            = delete;
    Harness& operator=(const Harness&) = delete;

    void emit(const domain::TradeSignal& signal) override { signals.push_back(signal); }

    void on_bar(const domain::MarketEvent&, const strategy::BarSnapshot& snapshot) override {
        Seen record{snapshot.verdict, snapshot.reason, snapshot.action, std::nullopt, {}};
        for (const strategy::Indicator& indicator : snapshot.indicator_list()) {
            if (indicator.name == "probability_up") {
                record.probability = indicator.value;
            }
        }
        for (const strategy::StateLabel& state : snapshot.state_list()) {
            if (state.name == "latch_after") {
                record.latch_after = std::string{state.value};
            }
        }
        seen.push_back(record);
    }

    void feed(const domain::MarketEvent& event) { lr.on_market_event(event, *this); }

    void feed_closes(const Closes& closes, const std::string& symbol = "AAPL",
                     long long first_minute = 1) {
        long long minute = first_minute;
        for (const common::Price close : closes) {
            feed(bar(symbol, close, minute++));
        }
    }

    strategy::LogisticRegressionStrategy lr;
    std::vector<domain::TradeSignal>     signals;
    std::vector<Seen>                    seen;
};

}  // namespace lr_test
