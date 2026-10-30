#pragma once

// -----------------------------------------------------------------------------
//  EngineService -- the engine as a long-running process that takes commands
//  (`trading_engine serve`; docs/adr/0007-engine-command-channel.md).
//
//      console ─▶ gateway ─▶ <control>.command ─▶ EngineService
//                                                     │ start_backtest: queue a run
//                                                     │ stop / set_kill_switch
//                                                     ▼
//              <run>.market_data/.signal/...  ◀── ConfiguredRun on its own TopicEventBus
//              <run>.run (status + settings)  ◀── this class
//              <control>.engine (heartbeat)   ◀── this class
//
//  Runs replay the recorded bars it was given (EngineServiceConfig::bars),
//  one at a time, in the order they were asked for: a queued run waits for
//  the one before it. Each run gets its own topic prefix -- the run id the
//  command chose -- so it appears in the console exactly like a
//  `trading_engine run`.
//
//  Commands:
//   * start_backtest -- validates the settings; refused (a "failed" report)
//     when they are invalid, the run id is taken or not a valid topic
//     prefix, the queue is full, or no recorded bar matches. Otherwise a
//     "queued" report, then "running" with progress, then "completed",
//     "stopped" or "failed".
//   * stop -- a queued run ends "stopped" without starting; a running one
//     stops after the current timestamp.
//   * set_kill_switch -- engages or releases the run's RiskManager kill
//     switch (every new signal rejected). Answered by a report with the new
//     state. Allowed while queued, so a run can start with it engaged.
//  Commands for a run this engine does not own are ignored: another engine
//  on the same cluster may own it.
//
//  Threads: one reads commands and sends the heartbeat; one runs the queue.
//  Every public method may be called from any thread.
// -----------------------------------------------------------------------------

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "trading_engine/common/clock.hpp"
#include "trading_engine/control/control_codec.hpp"
#include "trading_engine/control/control_messages.hpp"
#include "trading_engine/domain/market_event.hpp"
#include "trading_engine/events/message_transport.hpp"

namespace trading_engine::risk { class RiskManager; }

namespace trading_engine::app {

// Makes the transport for one bus: the command reader (prefix = the control
// prefix) or one run (prefix = its run id). A Kafka factory may create the
// prefix's topics first.
using TransportFactory = std::function<std::unique_ptr<events::IMessageTransport>(std::string_view prefix)>;

struct EngineServiceConfig {
    std::string                      control_prefix{control::kDefaultControlPrefix};
    std::string                      engine_id{};         // empty: "engine-" + random hex
    std::string                      version{};           // shown in the announcement
    std::vector<domain::MarketEvent> bars{};              // the recorded data runs replay
    std::chrono::milliseconds        heartbeat{std::chrono::seconds{2}};
    std::chrono::milliseconds        instant_delay{0};    // pause after each timestamp, to watch a run
    std::size_t                      max_queued{8};       // queued + running
    std::chrono::milliseconds        poll_interval{std::chrono::milliseconds{50}};
};

class EngineService {
public:
    struct Stats {
        std::uint64_t commands{0};        // read and decoded
        std::uint64_t decode_errors{0};
        std::uint64_t ignored{0};         // for a run this engine does not own
        std::uint64_t refused{0};         // answered with an error
        std::uint64_t completed{0};
        std::uint64_t stopped{0};
        std::uint64_t failed{0};
        std::uint64_t reports{0};         // status reports sent
    };

    // `wall` stamps reports and heartbeats; runs use their own market clock.
    EngineService(TransportFactory transports, const common::IClock& wall, EngineServiceConfig config);
    ~EngineService();   // stop()

    EngineService(const EngineService&)            = delete;
    EngineService& operator=(const EngineService&) = delete;

    void start();           // subscribes to the command topic; idempotent
    void stop() noexcept;   // stops the running run, drops the queue, joins

    // What the command reader does with each command. Public for tests.
    void handle(const control::Command& command);

    // Blocks until nothing is queued or running (or the timeout passes).
    bool wait_until_idle(std::chrono::milliseconds timeout);

    [[nodiscard]] const std::string& engine_id() const noexcept { return config_.engine_id; }
    [[nodiscard]] Stats stats() const;
    [[nodiscard]] control::EngineAnnouncement announcement() const;

private:
    struct Run {
        std::string                         run_id;
        control::RunSettings                settings;
        control::KillSwitchState            kill;
        control::RunState                   state{control::RunState::Queued};
        std::string                         start_command;   // the command that created it
        std::string                         stop_command;    // answered when the replay ends
        bool                                stop{false};
        risk::RiskManager*                  risk{nullptr};   // while running
        std::optional<double>               progress{};
        std::optional<common::Timestamp>    clock{};
    };

    void read_commands();
    void work();
    void execute(const std::shared_ptr<Run>& run);
    // Callers hold mutex_. Sends the run's whole state on <run>.run.
    void report(const Run& run, const std::string& command_id = {}, std::optional<std::string> error = {});
    void send(const std::string& topic, const std::string& key, control::ControlPayload payload);
    void announce();

    TransportFactory               transports_;
    const common::IClock&          wall_;
    EngineServiceConfig            config_;
    common::Timestamp              started_at_{};
    std::vector<common::Symbol>    symbols_{};
    std::string                    first_session_{};
    std::string                    last_session_{};

    std::unique_ptr<events::IMessageTransport> control_;
    std::mutex                     send_mutex_;    // sequence_ and control_->send()
    std::uint64_t                  sequence_{0};

    mutable std::mutex             mutex_;         // everything below
    std::condition_variable        changed_;
    bool                           started_{false};
    bool                           stopping_{false};
    std::deque<std::shared_ptr<Run>> queue_{};
    std::shared_ptr<Run>           current_{};
    std::map<std::string, std::shared_ptr<Run>> runs_{};   // every run this engine has owned
    Stats                          stats_{};

    std::thread                    reader_;
    std::thread                    worker_;
};

}  // namespace trading_engine::app
