#include "trading_engine/app/engine_service.hpp"

#include <algorithm>
#include <cstdio>
#include <exception>
#include <random>
#include <set>
#include <utility>

#include "trading_engine/app/configured_run.hpp"
#include "trading_engine/common/timestamp_text.hpp"
#include "trading_engine/events/topic_event_bus.hpp"
#include "trading_engine/risk/risk_manager.hpp"

namespace trading_engine::app {

namespace {

using control::RunState;

std::string random_hex() {
    std::random_device device;
    char buffer[9];
    std::snprintf(buffer, sizeof buffer, "%08x", static_cast<unsigned>(device()));
    return buffer;
}

bool valid_prefix(const std::string& prefix) {
    if (prefix.empty() || prefix.size() > 200) {
        return false;
    }
    return std::all_of(prefix.begin(), prefix.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
               c == '_' || c == '-';
    });
}

bool finished(RunState state) {
    return state == RunState::Completed || state == RunState::Failed || state == RunState::Stopped;
}

std::string session_of(common::Timestamp t) { return common::format_timestamp(t).substr(0, 10); }

}  // namespace

EngineService::EngineService(TransportFactory transports, const common::IClock& wall, EngineServiceConfig config)
    : transports_{std::move(transports)}, wall_{wall}, config_{std::move(config)} {
    if (config_.engine_id.empty()) {
        config_.engine_id = "engine-" + random_hex();
    }
    std::set<common::Symbol> symbols;
    for (const auto& bar : config_.bars) {
        symbols.insert(bar.symbol);
        const std::string session = session_of(bar.exchange_time);
        if (first_session_.empty() || session < first_session_) first_session_ = session;
        if (last_session_.empty() || session > last_session_)   last_session_  = session;
    }
    symbols_.assign(symbols.begin(), symbols.end());
}

EngineService::~EngineService() { stop(); }

void EngineService::start() {
    {
        const std::lock_guard lock{mutex_};
        if (started_) {
            return;
        }
        started_    = true;
        started_at_ = wall_.now();
    }
    control_ = transports_(config_.control_prefix);
    control_->subscribe({control::command_topic(config_.control_prefix)});
    announce();
    reader_ = std::thread{[this] { read_commands(); }};
    worker_ = std::thread{[this] { work(); }};
}

void EngineService::stop() noexcept {
    {
        const std::lock_guard lock{mutex_};
        if (!started_ || stopping_) {
            return;
        }
        stopping_ = true;
        if (current_) {
            current_->stop = true;
        }
    }
    changed_.notify_all();
    if (reader_.joinable()) reader_.join();
    if (worker_.joinable()) worker_.join();
    {
        const std::lock_guard lock{mutex_};
        for (auto& run : queue_) {   // never started
            run->state = RunState::Stopped;
            report(*run, {}, std::string{"The engine shut down before the run started."});
        }
        queue_.clear();
    }
    if (control_) {
        (void)control_->flush(std::chrono::seconds{5});
    }
}

bool EngineService::wait_until_idle(std::chrono::milliseconds timeout) {
    std::unique_lock lock{mutex_};
    return changed_.wait_for(lock, timeout, [&] { return queue_.empty() && !current_; });
}

EngineService::Stats EngineService::stats() const {
    const std::lock_guard lock{mutex_};
    return stats_;
}

control::EngineAnnouncement EngineService::announcement() const {
    control::EngineAnnouncement a;
    a.engine_id     = config_.engine_id;
    a.version       = config_.version;
    a.sent_at       = wall_.now();
    a.heartbeat_ms  = static_cast<std::uint32_t>(config_.heartbeat.count());
    a.symbols       = symbols_;
    a.first_session = first_session_;
    a.last_session  = last_session_;
    const std::lock_guard lock{mutex_};
    a.started_at = started_at_;
    if (current_) {
        a.runs.push_back(current_->run_id);
    }
    for (const auto& run : queue_) {
        a.runs.push_back(run->run_id);
    }
    return a;
}

void EngineService::send(const std::string& topic, const std::string& key, control::ControlPayload payload) {
    const std::lock_guard lock{send_mutex_};
    control::ControlEnvelope envelope{config_.engine_id, sequence_ + 1, wall_.now(), std::move(payload)};
    if (control_ && control_->send(events::TransportMessage{topic, key, control::encode(envelope)})) {
        ++sequence_;
    }
}

void EngineService::announce() {
    send(control::engine_topic(config_.control_prefix), config_.engine_id, announcement());
}

void EngineService::report(const Run& run, const std::string& command_id, std::optional<std::string> error) {
    control::RunStatusReport r;
    r.run_id      = run.run_id;
    r.engine_id   = config_.engine_id;
    r.status      = run.state;
    r.updated_at  = wall_.now();
    r.settings    = run.settings;
    r.kill_switch = run.kill;
    r.progress    = run.progress;
    r.clock       = run.clock;
    r.error       = std::move(error);
    if (!command_id.empty()) {
        r.command_id = command_id;
    }
    ++stats_.reports;
    send(control::run_topic(run.run_id), run.run_id, std::move(r));
}

void EngineService::read_commands() {
    const std::string topic = control::command_topic(config_.control_prefix);
    auto next_beat = std::chrono::steady_clock::now() + config_.heartbeat;
    while (true) {
        {
            const std::lock_guard lock{mutex_};
            if (stopping_) {
                break;
            }
        }
        if (std::chrono::steady_clock::now() >= next_beat) {
            announce();
            next_beat = std::chrono::steady_clock::now() + config_.heartbeat;
        }
        std::optional<events::TransportMessage> message;
        try {
            message = control_->poll(config_.poll_interval);
        } catch (...) {
            message.reset();
        }
        if (!message || message->topic != topic) {
            continue;
        }
        try {
            auto envelope = control::decode_control(message->payload);
            if (const auto* command = std::get_if<control::Command>(&envelope.payload)) {
                handle(*command);
            }
        } catch (const std::exception&) {
            const std::lock_guard lock{mutex_};
            ++stats_.decode_errors;
        }
    }
}

void EngineService::handle(const control::Command& command) {
    {
        const std::lock_guard lock{mutex_};
        ++stats_.commands;
        const auto found = runs_.find(command.run_id);

        if (command.action == control::CommandAction::StartBacktest) {
            if (found != runs_.end()) {
                // A redelivered command is a no-op; anything else must not
                // overwrite another run's status topic.
                if (found->second->start_command != command.command_id) {
                    ++stats_.refused;
                }
                return;
            }
            const std::string reserved = config_.control_prefix + ".";
            if (!valid_prefix(command.run_id) || command.run_id == config_.control_prefix ||
                command.run_id.rfind(reserved, 0) == 0) {
                ++stats_.refused;   // no topic to answer on
                return;
            }
            auto run           = std::make_shared<Run>();
            run->run_id        = command.run_id;
            run->start_command = command.command_id;
            run->settings      = command.settings.value_or(control::RunSettings{});
            run->state         = RunState::Failed;
            std::optional<std::string> problem;
            if (!command.settings) {
                problem = command.settings_error_field.value_or("settings") + ": " +
                          (command.settings_error.empty() ? "missing" : command.settings_error);
            } else {
                try {
                    control::validate(run->settings);
                } catch (const control::SettingsError& e) {
                    problem = e.field() + ": " + e.message();
                }
            }
            if (!problem) {
                const auto bars = ConfiguredRun::select_bars(config_.bars, run->settings);
                if (bars.empty()) {
                    problem = "No recorded bars match the strategies' symbols in that period.";
                } else {
                    run->settings.period_start = run->settings.period_start.value_or(bars.front().exchange_time);
                    run->settings.period_end   = run->settings.period_end.value_or(bars.back().exchange_time);
                }
            }
            if (!problem && queue_.size() + (current_ ? 1 : 0) >= config_.max_queued) {
                problem = "The engine already has " + std::to_string(config_.max_queued) +
                          " runs queued; try again when one finishes.";
            }
            if (stopping_) {
                problem = "The engine is shutting down.";
            }
            runs_[run->run_id] = run;
            if (problem) {
                ++stats_.refused;
                ++stats_.failed;
                report(*run, command.command_id, problem);
                return;
            }
            run->state = RunState::Queued;
            queue_.push_back(run);
            report(*run, command.command_id);
        } else {
            if (found == runs_.end()) {
                ++stats_.ignored;
                return;
            }
            Run& run = *found->second;
            if (finished(run.state)) {
                ++stats_.refused;
                report(run, command.command_id, std::string{"The run has already finished."});
                return;
            }
            if (command.action == control::CommandAction::StopRun) {
                if (run.state == RunState::Queued) {
                    queue_.erase(std::remove(queue_.begin(), queue_.end(), found->second), queue_.end());
                    run.state = RunState::Stopped;
                    ++stats_.stopped;
                    report(run, command.command_id);
                } else {
                    run.stop         = true;   // the worker reports "stopped" when the replay ends
                    run.stop_command = command.command_id;
                }
            } else {
                if (run.risk != nullptr) {
                    const auto k = run.risk->set_kill_switch(command.engaged, command.reason);
                    run.kill     = {k.engaged, k.changed_at, k.reason};
                } else {
                    run.kill = {command.engaged, std::nullopt, command.engaged ? command.reason : std::string{}};
                }
                report(run, command.command_id);
            }
        }
    }
    changed_.notify_all();
}

void EngineService::work() {
    while (true) {
        std::shared_ptr<Run> run;
        {
            std::unique_lock lock{mutex_};
            changed_.wait(lock, [&] { return stopping_ || !queue_.empty(); });
            if (stopping_) {
                return;
            }
            run = queue_.front();
            queue_.pop_front();
            current_ = run;
        }
        execute(run);
        {
            const std::lock_guard lock{mutex_};
            current_.reset();
        }
        changed_.notify_all();
        announce();
    }
}

void EngineService::execute(const std::shared_ptr<Run>& run) {
    std::optional<std::string> error;
    bool complete = false;
    try {
        const auto bars = ConfiguredRun::select_bars(config_.bars, run->settings);
        common::ManualClock clock{bars.empty() ? common::Timestamp{} : bars.front().exchange_time};
        auto transport = transports_(run->run_id);
        events::TopicEventBusOptions options;
        options.topic_prefix  = run->run_id;
        options.poll_interval = std::chrono::milliseconds{5};
        events::TopicEventBus bus{*transport, clock, options};
        ConfiguredRun configured{bus, clock, run->settings};

        // Hands the RiskManager to handle() for the kill switch, and takes it
        // back before the run is destroyed, whichever way this block exits.
        struct Lend {
            EngineService& self;
            Run&           run;
            ~Lend() {
                const std::lock_guard lock{self.mutex_};
                run.risk = nullptr;
            }
        } lend{*this, *run};
        {
            const std::lock_guard lock{mutex_};
            run->risk  = &configured.flow().risk();
            run->state = RunState::Running;
            run->progress = 0.0;
            if (run->kill.engaged) {
                const auto k = run->risk->set_kill_switch(true, run->kill.reason);
                run->kill    = {k.engaged, k.changed_at, k.reason};
            }
            report(*run);
        }

        double reported = 0.0;
        auto last_report = std::chrono::steady_clock::now();
        complete = configured.replay(bars, [&](const ReplayProgress& p) {
            std::unique_lock lock{mutex_};
            run->progress = static_cast<double>(p.done) / static_cast<double>(p.total);
            run->clock    = p.clock;
            const auto now = std::chrono::steady_clock::now();
            if (*run->progress - reported >= 0.02 || now - last_report >= std::chrono::milliseconds{500}) {
                reported    = *run->progress;
                last_report = now;
                report(*run);
            }
            if (config_.instant_delay.count() > 0) {
                changed_.wait_for(lock, config_.instant_delay, [&] { return run->stop || stopping_; });
            }
            return !(run->stop || stopping_);
        });
    } catch (const std::exception& e) {
        error = e.what();
    }

    const std::lock_guard lock{mutex_};
    if (error) {
        run->state = RunState::Failed;
        ++stats_.failed;
    } else if (complete) {
        run->state    = RunState::Completed;
        run->progress = 1.0;
        ++stats_.completed;
    } else {
        run->state = RunState::Stopped;
        ++stats_.stopped;
    }
    report(*run, run->stop_command, error);
}

}  // namespace trading_engine::app
