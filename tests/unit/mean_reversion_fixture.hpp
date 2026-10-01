#pragma once

// Shared scaffolding for the mean-reversion tests: the fixture series, builders
// for bars and configs, and a Harness that drives the strategy by hand and records
// what it emits. Everything is inline (no unnamed namespace, so no unused-function
// warning in a file that skips some of it): several test files include this header.

#include <chrono>
#include <cstddef>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "trading_engine/common/types.hpp"
#include "trading_engine/domain/market_event.hpp"
#include "trading_engine/domain/trade_signal.hpp"
#include "trading_engine/strategy/mean_reversion_strategy.hpp"

namespace mr_test {

namespace common   = trading_engine::common;
namespace domain   = trading_engine::domain;
namespace strategy = trading_engine::strategy;

using Closes = std::vector<double>;
using Labels = std::vector<std::string>;

inline constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
inline constexpr double kInf = std::numeric_limits<double>::infinity();
inline constexpr double kMax = std::numeric_limits<double>::max();

// The fixture from the task: lookback 4, entry 1.5, rearm 0.5. Counting bars from
// 1 it must give: nothing on bars 1-3, Buy at 4 (z -1.7320508), a rearm at 5 (z
// 0.1524986), Buy at 6 (z -1.5261167) and Sell at 7 (z 1.6858628).
inline const Closes kFixture{10, 10, 10, 6, 9, 2, 30};

inline common::Timestamp at_minute(long long minute) {
    return common::Timestamp{} + std::chrono::minutes{minute};
}

// A finalized bar: the only fields the strategy reads.
inline domain::MarketEvent bar(const std::string& symbol, double close, long long minute) {
    domain::MarketEvent event;
    event.symbol        = symbol;
    event.type          = domain::MarketEventType::Bar;
    event.exchange_time = at_minute(minute);
    event.price         = close;
    return event;
}

// Lookback, entry and rearm thresholds as given; one share; the symbols given.
inline strategy::MeanReversionConfig make_config(
    std::size_t lookback = 4, double entry = 1.5, double rearm = 0.5,
    std::vector<std::string> symbols = {"AAPL"}) {
    strategy::MeanReversionConfig result;
    result.lookback        = lookback;
    result.entry_threshold = entry;
    result.rearm_threshold = rearm;
    result.symbols         = std::move(symbols);
    return result;
}

inline double number(const domain::TradeSignal& signal, const std::string& key) {
    return std::stod(signal.metadata.at(key));
}

// The strategy under test, driven by hand, and the sink that records its signals.
class Harness final : public strategy::ISignalSink {
public:
    explicit Harness(strategy::MeanReversionConfig cfg) : mr{std::move(cfg)} {}

    void emit(const domain::TradeSignal& signal) override { signals.push_back(signal); }

    void feed(const domain::MarketEvent& event) {
        const std::size_t before = signals.size();
        mr.on_market_event(event, *this);
        const long long minute =
            std::chrono::duration_cast<std::chrono::minutes>(event.exchange_time.time_since_epoch())
                .count();
        for (std::size_t i = before; i < signals.size(); ++i) {
            const char* side = signals[i].side == domain::SignalSide::Buy ? "buy@" : "sell@";
            labelled.emplace_back(signals[i].symbol, side + std::to_string(minute));
        }
    }

    // One bar per close at minutes first_minute, +1, +2, ..., so a label's number
    // is the 1-based bar when first_minute is 1.
    void feed_closes(const Closes& closes, const std::string& symbol = "AAPL",
                     long long first_minute = 1) {
        long long minute = first_minute;
        for (const double close : closes) {
            feed(bar(symbol, close, minute++));
        }
    }

    // "<side>@<minute of the bar that triggered it>" for one symbol, in emission order.
    [[nodiscard]] Labels labels(const std::string& symbol = "AAPL") const {
        Labels out;
        for (const auto& [signalled_symbol, label] : labelled) {
            if (signalled_symbol == symbol) {
                out.push_back(label);
            }
        }
        return out;
    }

    strategy::MeanReversionStrategy mr;
    std::vector<domain::TradeSignal> signals;

private:
    std::vector<std::pair<std::string, std::string>> labelled;
};

}  // namespace mr_test
