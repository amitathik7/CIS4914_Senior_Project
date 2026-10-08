#pragma once

// Shared helpers for the Strategy Lab tests. Everything is inline: several test files
// include this header. STRATEGY_LAB_FIXTURE_DIR is defined by tests/unit/CMakeLists.txt.

#include <charconv>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "lab/catalog.hpp"
#include "lab/dataset.hpp"
#include "lab/errors.hpp"
#include "lab/session.hpp"
#include "lab/timestamp.hpp"
#include "trading_engine/domain/trade_signal.hpp"

namespace lab_test {

namespace lab    = trading_engine::lab;
namespace domain = trading_engine::domain;
namespace common = trading_engine::common;

inline std::filesystem::path fixture_path(const std::string& name) {
    return std::filesystem::path{STRATEGY_LAB_FIXTURE_DIR} / "datasets" / name;
}

using Params = std::vector<std::pair<std::string, std::string>>;

inline lab::StrategyRequest request(std::string kind, const Params& params = {}) {
    lab::StrategyRequest out;
    out.kind = std::move(kind);
    for (const auto& [name, text] : params) {
        out.params.push_back(lab::ParamAssignment{name, text});
    }
    return out;
}

inline lab::RunDocument run_fixture(const std::string& name, const std::vector<lab::StrategyRequest>& requests,
                                    const lab::SessionOptions& options = {}) {
    const lab::Dataset dataset = lab::load_dataset(fixture_path(name));
    return lab::run_session(dataset, requests, options);
}

// The reason code of each event for the strategy in `slot`, in replay order.
inline std::vector<std::string> reasons(const lab::RunDocument& doc, std::size_t slot) {
    std::vector<std::string> out;
    for (const lab::EventResult& event : doc.replay.events) {
        const auto& diagnostics = event.per_strategy[slot].diagnostics;
        out.push_back(diagnostics.has_value() ? diagnostics->reason : "<no diagnostics>");
    }
    return out;
}

// "<strategy_id> <symbol> <side> event=<index> id=<signal id>" per published signal.
inline std::vector<std::string> signal_lines(const lab::RunDocument& doc) {
    std::vector<std::string> out;
    for (const lab::SignalRecord& record : doc.replay.signals) {
        out.push_back(record.signal.strategy_id + " " + record.signal.symbol + " " +
                      std::string{domain::to_string(record.signal.side)} +
                      " event=" + std::to_string(record.event_index) +
                      " id=" + std::to_string(record.signal.id.value));
    }
    return out;
}

inline double parse_double(const std::string& text) {
    double value = 0.0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    EXPECT_EQ(result.ec, std::errc{}) << text;
    EXPECT_EQ(result.ptr, text.data() + text.size()) << text;
    return value;
}

// The LabError `action` throws, or a failure if it throws nothing else.
inline lab::LabError expect_lab_error(const std::function<void()>& action) {
    try {
        action();
    } catch (const lab::LabError& error) {
        return error;
    } catch (const std::exception& error) {
        ADD_FAILURE() << "expected a LabError, got std::exception: " << error.what();
        return lab::LabError(lab::ErrorCode::InternalError, "wrong exception type");
    }
    ADD_FAILURE() << "expected a LabError, but nothing was thrown";
    return lab::LabError(lab::ErrorCode::InternalError, "nothing thrown");
}

inline std::string timestamp_text(common::Timestamp time) { return lab::format_utc_timestamp(time); }

// The properties EVERY fault-free run must have, whatever the data or the strategies. They are
// statements about the relation between the engine's output and the strategies' diagnostics,
// not about what any strategy should decide (that is checked by hand-derived expectations).
inline void expect_run_invariants(const lab::RunDocument& doc) {
    const lab::ReplayResult& replay = doc.replay;

    // One result per event, one diagnostic per strategy per event, nothing lost.
    for (std::size_t i = 0; i < replay.events.size(); ++i) {
        ASSERT_EQ(replay.events[i].index, i);
        ASSERT_EQ(replay.events[i].per_strategy.size(), replay.strategies.size());
        for (std::size_t s = 0; s < replay.strategies.size(); ++s) {
            EXPECT_TRUE(replay.events[i].per_strategy[s].diagnostics.has_value()) << "event " << i << " strategy " << s;
        }
    }
    for (const lab::StrategyRunInfo& strategy : replay.strategies) {
        EXPECT_EQ(strategy.stats.events_delivered, replay.events.size()) << strategy.strategy_id;
        EXPECT_EQ(strategy.stats.errors, 0u);
        EXPECT_EQ(strategy.observer_failures, 0u);
    }
    EXPECT_EQ(replay.engine_stats.events_routed, replay.events.size());
    EXPECT_EQ(replay.engine_stats.events_dropped, 0u);
    EXPECT_EQ(replay.engine_stats.signals_published, replay.signals.size());

    // Signals: ids 1..N in publish order, bus sequence strictly increasing, stamped with the
    // clock reading at the triggering event, which the runner set to that event's time.
    std::uint64_t previous_sequence = 0;
    for (std::size_t n = 0; n < replay.signals.size(); ++n) {
        const lab::SignalRecord& record = replay.signals[n];
        EXPECT_EQ(record.signal.id.value, n + 1) << "ids count from 1 per run, in publish order";
        EXPECT_GT(record.bus_sequence, previous_sequence);
        previous_sequence = record.bus_sequence;
        ASSERT_LT(record.event_index, replay.events.size());
        EXPECT_EQ(record.signal.created_at, replay.events[record.event_index].input.event.exchange_time);
        EXPECT_EQ(record.signal.symbol, replay.events[record.event_index].input.event.symbol);
    }

    // Snapshot and signal agree: a call emitted a signal exactly when its action says so, with the
    // same side and the same numbers.
    std::size_t acting = 0;
    for (const lab::EventResult& event : replay.events) {
        for (std::size_t s = 0; s < event.per_strategy.size(); ++s) {
            const lab::StrategyEventResult& result = event.per_strategy[s];
            if (!result.diagnostics.has_value()) {
                continue;
            }
            const bool acted = result.diagnostics->action != "none";
            acting += acted ? 1U : 0U;
            EXPECT_EQ(result.signal_ids.size(), acted ? 1U : 0U) << "event " << event.index;
            EXPECT_EQ(result.diagnostics->verdict == "evaluated" || !acted, true);
            if (!acted) {
                continue;
            }
            const domain::TradeSignal& signal = replay.signals[result.signal_ids[0] - 1].signal;
            EXPECT_EQ(signal.strategy_id, replay.strategies[s].strategy_id);
            EXPECT_EQ(std::string{domain::to_string(signal.side)}, result.diagnostics->action);
            for (const auto& [name, value] : result.diagnostics->indicators) {
                const auto found = signal.metadata.find(name);
                if (found == signal.metadata.end()) {
                    continue;   // short_minus_long has no metadata twin
                }
                ASSERT_TRUE(value.has_value()) << name;
                EXPECT_EQ(*value, parse_double(found->second)) << name << ": one value, in the snapshot and in the signal";
            }
        }
    }
    EXPECT_EQ(acting, replay.signals.size());
}

}  // namespace lab_test
