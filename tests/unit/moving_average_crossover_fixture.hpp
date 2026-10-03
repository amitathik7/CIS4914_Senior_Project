#pragma once

// Shared scaffolding for the moving-average crossover tests: the fixture series,
// builders for bars and configs, and a Harness that drives the strategy by hand
// and records what it emits. Everything is inline (no unnamed namespace, so no
// unused-function warning in a file that skips some of it): several test files
// include this header.

#include <chrono>
#include <cstddef>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include "support/price_literals.hpp"
#include "trading_engine/common/types.hpp"
#include "trading_engine/domain/market_event.hpp"
#include "trading_engine/domain/trade_signal.hpp"
#include "trading_engine/strategy/moving_average_crossover_strategy.hpp"

namespace sma_test {

namespace common   = trading_engine::common;
namespace domain   = trading_engine::domain;
namespace strategy = trading_engine::strategy;

// Closes are exact int64 Prices. Write them with units({3, 2}) (whole currency units) or
// 0.25_px (exact decimal text); never as a bare number, which would mean that many micros.
using Closes = std::vector<common::Price>;
using Labels = std::vector<std::string>;

using trading_engine::test_support::kMaxPrice;
using trading_engine::test_support::px;
using trading_engine::test_support::micros;
using trading_engine::test_support::units;
using namespace trading_engine::test_support::literals;

// The fixture from the task, for a 2/3 pair. Counting bars from 1 it must give
// exactly: Buy at bar 5 (short 2.5, long 2) and Sell at bar 8 (short 2.5, long 3).
inline const Closes kFixture = units({3, 2, 1, 2, 3, 4, 3, 2, 1});

inline common::Timestamp at_minute(long long minute) {
    return common::Timestamp{} + std::chrono::minutes{minute};
}

// A finalized bar: the only fields the strategy reads.
inline domain::MarketEvent bar(const std::string& symbol, common::Price close, long long minute) {
    domain::MarketEvent event;
    event.symbol        = symbol;
    event.type          = domain::MarketEventType::Bar;
    event.exchange_time = at_minute(minute);
    event.price         = close;
    return event;
}

inline strategy::MovingAverageCrossoverConfig make_config(
    std::size_t short_window, std::size_t long_window,
    std::vector<std::string> symbols = {"AAPL"}) {
    strategy::MovingAverageCrossoverConfig result;
    result.short_window = short_window;
    result.long_window  = long_window;
    result.symbols      = std::move(symbols);
    return result;
}

inline double number(const domain::TradeSignal& signal, const std::string& key) {
    return std::stod(signal.metadata.at(key));
}

// The strategy under test, driven by hand, and the sink that records its signals.
class Harness final : public strategy::ISignalSink {
public:
    explicit Harness(strategy::MovingAverageCrossoverConfig cfg) : sma{std::move(cfg)} {}

    void emit(const domain::TradeSignal& signal) override { signals.push_back(signal); }

    void feed(const domain::MarketEvent& event) {
        const std::size_t before = signals.size();
        sma.on_market_event(event, *this);
        const long long minute =
            std::chrono::duration_cast<std::chrono::minutes>(event.exchange_time.time_since_epoch())
                .count();
        for (std::size_t i = before; i < signals.size(); ++i) {
            const char* side = signals[i].side == domain::SignalSide::Buy ? "buy@" : "sell@";
            labelled.emplace_back(signals[i].symbol, side + std::to_string(minute));
        }
    }

    // One bar per close at minutes 1, 2, 3, ..., so a label's number is the 1-based bar.
    void feed_closes(const Closes& closes, const std::string& symbol = "AAPL") {
        long long minute = 0;
        for (const common::Price close : closes) {
            feed(bar(symbol, close, ++minute));
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

    strategy::MovingAverageCrossoverStrategy sma;
    std::vector<domain::TradeSignal> signals;

private:
    std::vector<std::pair<std::string, std::string>> labelled;
};

}  // namespace sma_test
