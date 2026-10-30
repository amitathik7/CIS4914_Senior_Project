#pragma once

// -----------------------------------------------------------------------------
//  Command channel messages (docs/adr/0007-engine-command-channel.md).
//
//  Three messages, each on its own topic, in the event bus envelope
//  (schema 1, docs/EVENT_BUS.md) so one decoder reads every topic:
//
//    <control>.command  Command             console gateway -> engine
//    <prefix>.run       RunStatusReport     engine -> anyone: a run's state,
//                                           settings and kill switch
//    <control>.engine   EngineAnnouncement  engine -> anyone: "I am here, and
//                                           this is the data I can replay"
//
//  <control> defaults to "trading_engine.control"; <prefix> is the run's own
//  topic prefix, which is also its run id. These are not bus EventTypes: they
//  are about runs, not inside one, and a run's bus never subscribes to them.
//
//  Plain value types.
// -----------------------------------------------------------------------------

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "trading_engine/common/types.hpp"
#include "trading_engine/control/run_settings.hpp"

namespace trading_engine::control {

inline constexpr std::string_view kDefaultControlPrefix = "trading_engine.control";

enum class CommandAction : std::uint8_t {
    StartBacktest = 0,   // run_id is the new run's topic prefix; settings required
    StopRun,             // finish the run early (status "stopped")
    SetKillSwitch        // engaged / reason
};

enum class RunState : std::uint8_t { Queued = 0, Running, Completed, Failed, Stopped };

[[nodiscard]] std::string_view to_string(CommandAction action) noexcept;   // "start_backtest", ...
[[nodiscard]] std::string_view to_string(RunState state) noexcept;         // "queued", ...

struct Command {
    std::string                command_id{};   // chosen by the sender; echoed on the status it causes
    CommandAction              action{CommandAction::StartBacktest};
    std::string                run_id{};
    std::optional<RunSettings> settings{};     // start_backtest
    // start_backtest whose settings could not be read: the field path and the
    // problem, so the engine can report the refusal on the run's topic.
    std::optional<std::string> settings_error_field{};
    std::string                settings_error{};
    bool                       engaged{false}; // set_kill_switch
    std::string                reason{};       // set_kill_switch
    common::Timestamp          issued_at{};
};

struct KillSwitchState {
    bool                             engaged{false};
    std::optional<common::Timestamp> changed_at{};
    std::string                      reason{};
};

// Published when a run is queued, starts, changes its kill switch, every few
// percent of progress, and when it ends. Each report is the whole state, so a
// reader only needs the latest one.
struct RunStatusReport {
    std::string                      run_id{};
    std::string                      engine_id{};     // empty: not controllable (trading_engine run)
    RunState                         status{RunState::Queued};
    common::Timestamp                updated_at{};    // wall clock
    RunSettings                      settings{};
    KillSwitchState                  kill_switch{};
    std::optional<double>            progress{};      // 0..1 while running a backtest
    std::optional<common::Timestamp> clock{};         // the run's market time
    std::optional<std::string>       error{};         // failed, or a refused command
    std::optional<std::string>       command_id{};    // the command this report answers
};

struct EngineAnnouncement {
    std::string                 engine_id{};
    std::string                 version{};
    common::Timestamp           started_at{};
    common::Timestamp           sent_at{};
    std::uint32_t               heartbeat_ms{0};   // the next one comes within this
    std::vector<common::Symbol> symbols{};         // recorded data it can replay
    std::string                 first_session{};   // YYYY-MM-DD, empty without data
    std::string                 last_session{};
    std::vector<std::string>    runs{};            // queued or running, in order
};

}  // namespace trading_engine::control
