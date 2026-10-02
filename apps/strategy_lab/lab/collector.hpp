#pragma once

// The lab's diagnostics observer: copies each BarSnapshot a strategy reports into an
// owned record, tagged with the replay event it belongs to. Read-only by construction:
// it never touches a strategy, the engine or the bus (the observer contract in
// strategy_diagnostics.hpp), and it does not throw.

#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "trading_engine/strategy/strategy_diagnostics.hpp"

namespace trading_engine::lab {

// An owned copy of a BarSnapshot, safe to keep after the callback returns.
struct DiagnosticRecord {
    std::string                verdict{};
    std::string                reason{};
    std::string                action{};
    std::optional<std::size_t> window_fill{};
    std::size_t                window_size{0};
    std::vector<std::pair<std::string, std::optional<double>>> indicators{};   // unavailable = nullopt
    std::vector<std::pair<std::string, std::string>>           states{};
};

class SlotCollector final : public strategy::IStrategyObserver {
public:
    // `current_event` points at the runner's "event being replayed" counter.
    explicit SlotCollector(const std::size_t* current_event) noexcept : current_event_{current_event} {}

    void on_bar(const domain::MarketEvent&, const strategy::BarSnapshot& snapshot) override {
        DiagnosticRecord record;
        record.verdict     = std::string{strategy::to_string(snapshot.verdict)};
        record.reason      = std::string{strategy::to_string(snapshot.reason)};
        record.action      = std::string{strategy::to_string(snapshot.action)};
        record.window_fill = snapshot.window_fill;
        record.window_size = snapshot.window_size;
        for (const strategy::Indicator& each : snapshot.indicator_list()) {
            record.indicators.emplace_back(std::string{each.name}, each.value);
        }
        for (const strategy::StateLabel& each : snapshot.state_list()) {
            record.states.emplace_back(std::string{each.name}, std::string{each.value});
        }
        records.emplace_back(*current_event_, std::move(record));
    }

    // In the order reported: (event index, record).
    std::vector<std::pair<std::size_t, DiagnosticRecord>> records{};

private:
    const std::size_t* current_event_;
};

}  // namespace trading_engine::lab
