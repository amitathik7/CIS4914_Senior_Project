#pragma once

// Shared scaffolding for the strategy-diagnostics tests: an observer that keeps an
// owned copy of every snapshot, a plain signal sink, builders, a seeded pseudo-random
// event stream (bad prices, duplicate and older times, unlisted symbols and non-bar
// events included) and a field-by-field TradeSignal comparison. Everything is inline:
// two test files include this header.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "support/price_literals.hpp"
#include "trading_engine/common/types.hpp"
#include "trading_engine/domain/market_event.hpp"
#include "trading_engine/domain/trade_signal.hpp"
#include "trading_engine/strategy/mean_reversion_strategy.hpp"
#include "trading_engine/strategy/moving_average_crossover_strategy.hpp"
#include "trading_engine/strategy/strategy.hpp"
#include "trading_engine/strategy/strategy_diagnostics.hpp"

namespace diag_test {

namespace common   = trading_engine::common;
namespace domain   = trading_engine::domain;
namespace strategy = trading_engine::strategy;

using trading_engine::test_support::kMaxPrice;
using trading_engine::test_support::kMinPrice;
using trading_engine::test_support::px;
using trading_engine::test_support::micros;
using trading_engine::test_support::units;
using namespace trading_engine::test_support::literals;

// One snapshot, copied out of the callback.
struct Seen {
    domain::MarketEvent                                      event{};
    std::string                                              strategy_id{};
    strategy::BarVerdict                                     verdict{};
    strategy::BarReason                                      reason{};
    strategy::BarAction                                      action{};
    std::optional<std::size_t>                               window_fill{};
    std::size_t                                              window_size{};
    std::vector<std::pair<std::string, std::optional<double>>> indicators{};
    std::vector<std::pair<std::string, std::string>>          states{};

    [[nodiscard]] std::optional<double> number(const std::string& name) const {
        for (const auto& [key, value] : indicators) {
            if (key == name) {
                return value;
            }
        }
        ADD_FAILURE() << "no indicator named " << name;
        return std::nullopt;
    }
    [[nodiscard]] std::string state(const std::string& name) const {
        for (const auto& [key, value] : states) {
            if (key == name) {
                return value;
            }
        }
        return "<absent>";
    }
    [[nodiscard]] std::string reason_text() const { return std::string{strategy::to_string(reason)}; }
};

class Collector final : public strategy::IStrategyObserver {
public:
    void on_bar(const domain::MarketEvent& event, const strategy::BarSnapshot& snapshot) override {
        Seen seen;
        seen.event       = event;
        seen.strategy_id = std::string{snapshot.strategy_id};
        seen.verdict     = snapshot.verdict;
        seen.reason      = snapshot.reason;
        seen.action      = snapshot.action;
        seen.window_fill = snapshot.window_fill;
        seen.window_size = snapshot.window_size;
        for (const strategy::Indicator& each : snapshot.indicator_list()) {
            seen.indicators.emplace_back(std::string{each.name}, each.value);
        }
        for (const strategy::StateLabel& each : snapshot.state_list()) {
            seen.states.emplace_back(std::string{each.name}, std::string{each.value});
        }
        all.push_back(std::move(seen));
    }

    std::vector<Seen> all;
};

class Sink final : public strategy::ISignalSink {
public:
    void emit(const domain::TradeSignal& signal) override { signals.push_back(signal); }
    std::vector<domain::TradeSignal> signals;
};

inline common::Timestamp at_minute(long long minute) {
    return common::Timestamp{} + std::chrono::minutes{minute};
}

inline domain::MarketEvent event_at(const std::string& symbol, std::optional<common::Price> price,
                                    long long minute,
                                    domain::MarketEventType type = domain::MarketEventType::Bar) {
    domain::MarketEvent event;
    event.symbol        = symbol;
    event.type          = type;
    event.exchange_time = at_minute(minute);
    event.price         = price;
    return event;
}

inline strategy::MovingAverageCrossoverConfig sma_config(
    std::size_t short_window, std::size_t long_window,
    std::vector<std::string> symbols = {"AAPL"}) {
    strategy::MovingAverageCrossoverConfig config;
    config.short_window = short_window;
    config.long_window  = long_window;
    config.symbols      = std::move(symbols);
    return config;
}

inline strategy::MeanReversionConfig mr_config(std::size_t lookback, double entry, double rearm,
                                               std::vector<std::string> symbols = {"AAPL"}) {
    strategy::MeanReversionConfig config;
    config.lookback        = lookback;
    config.entry_threshold = entry;
    config.rearm_threshold = rearm;
    config.symbols         = std::move(symbols);
    return config;
}

// A tiny deterministic generator: the sequence does not depend on the standard library.
class Lcg {
public:
    explicit Lcg(std::uint64_t seed) : state_{seed} {}
    std::uint64_t next() {
        state_ = state_ * 6364136223846793005ULL + 1442695040888963407ULL;
        return state_ >> 33;
    }
    std::size_t below(std::size_t n) { return static_cast<std::size_t>(next() % n); }

private:
    std::uint64_t state_;
};

// Roughly 15% non-bar events, 30% unlisted or wrongly cased symbols, a tenth bad
// prices (zero, negative, absent, the int64 extremes, one micro) and about one
// timestamp in ten repeated or older.
// Prices walk in whole cents (10'000 millionths) from 100 currency units.
inline std::vector<domain::MarketEvent> random_stream(std::uint64_t seed, std::size_t count) {
    Lcg rng{seed};
    const std::vector<std::string> symbols{"AAPL", "AAPL", "AAPL", "MSFT", "MSFT", "ZZZ", "aapl"};
    std::map<std::string, long long> next_minute;
    common::Price last = common::Price::from_units(100);

    std::vector<domain::MarketEvent> out;
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const std::string& symbol = symbols[rng.below(symbols.size())];
        const std::size_t kind    = rng.below(100);
        const auto type = kind < 85 ? domain::MarketEventType::Bar
                        : kind < 95 ? domain::MarketEventType::Trade
                                    : domain::MarketEventType::Quote;

        long long& fresh = next_minute.try_emplace(symbol, 1000).first->second;
        long long minute = fresh;
        switch (rng.below(20)) {
            case 0:  minute = fresh - 1; break;           // repeats the previous bar's time
            case 1:  minute = fresh - 3; break;           // older
            case 2:  minute = fresh + 4; fresh = minute + 1; break;   // a gap
            default: ++fresh; break;
        }

        std::optional<common::Price> price;
        const std::size_t p = rng.below(100);
        if (p < 60 || p >= 92) {
            const auto cents = static_cast<std::int64_t>(rng.below(2001)) - 1000;   // -10.00 .. +10.00
            last += micros(cents * 10'000);                                          // a cent is 10'000 millionths
            last  = last < common::Price::from_units(1) ? common::Price::from_units(1) : last;
            price = last;
        } else if (p < 70) {
            price = last;                                 // a plateau
        } else if (p < 74) {
            price = kMinPrice;
        } else if (p < 77) {
            price = kMaxPrice;
        } else if (p < 79) {
            price = -kMaxPrice;
        } else if (p < 82) {
            price = common::Price{};
        } else if (p < 85) {
            price = -common::Price::from_units(3);
        } else if (p < 88) {
            price = std::nullopt;
        } else if (p < 90) {
            price = kMaxPrice / 3;
        } else {
            price = micros(1);                             // the smallest positive price
        }
        out.push_back(event_at(symbol, price, minute, type));
    }
    return out;
}

// Every field of two signals, including the free-form metadata.
inline bool same_signal(const domain::TradeSignal& a, const domain::TradeSignal& b) {
    return a.id == b.id && a.strategy_id == b.strategy_id && a.symbol == b.symbol &&
           a.side == b.side && a.created_at == b.created_at &&
           a.requested_quantity == b.requested_quantity && a.target_exposure == b.target_exposure &&
           a.confidence == b.confidence && a.metadata == b.metadata &&
           a.order_type == b.order_type && a.limit_price == b.limit_price;
}

inline bool same_signals(const std::vector<domain::TradeSignal>& a,
                         const std::vector<domain::TradeSignal>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (!same_signal(a[i], b[i])) {
            return false;
        }
    }
    return true;
}

}  // namespace diag_test
