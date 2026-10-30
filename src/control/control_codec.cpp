#include "trading_engine/control/control_codec.hpp"

#include <charconv>
#include <cmath>
#include <optional>
#include <utility>

#include "../events/json.hpp"
#include "trading_engine/common/decimal_text.hpp"
#include "trading_engine/common/timestamp_text.hpp"

namespace trading_engine::control {

std::string_view to_string(CommandAction action) noexcept {
    switch (action) {
        case CommandAction::StartBacktest: return "start_backtest";
        case CommandAction::StopRun:       return "stop";
        case CommandAction::SetKillSwitch: return "set_kill_switch";
    }
    return "invalid";
}

std::string_view to_string(RunState state) noexcept {
    switch (state) {
        case RunState::Queued:    return "queued";
        case RunState::Running:   return "running";
        case RunState::Completed: return "completed";
        case RunState::Failed:    return "failed";
        case RunState::Stopped:   return "stopped";
    }
    return "invalid";
}

std::string command_topic(std::string_view control_prefix) { return std::string(control_prefix) + ".command"; }
std::string engine_topic(std::string_view control_prefix) { return std::string(control_prefix) + ".engine"; }
std::string run_topic(std::string_view run_prefix) { return std::string(run_prefix) + ".run"; }

std::string_view type_name(const ControlPayload& payload) noexcept {
    switch (payload.index()) {
        case 0:  return "command";
        case 1:  return "run";
        default: return "engine";
    }
}

namespace {

using events::DecodeError;
using events::json::ObjectWriter;
using events::json::Value;

// --- writing ------------------------------------------------------------------

std::string time_text(common::Timestamp t) { return common::format_timestamp(t); }

double millis(common::Duration d) { return std::chrono::duration<double, std::milli>(d).count(); }

std::string string_array(const std::vector<std::string>& items) {
    std::string out = "[";
    for (std::size_t i = 0; i < items.size(); ++i) {
        if (i != 0) {
            out += ',';
        }
        events::json::append_string(out, items[i]);
    }
    return out + "]";
}

std::string strategy_json(const StrategySettings& s) {
    std::string out;
    ObjectWriter w{out};
    w.string("kind", to_string(s.kind));
    w.string("strategy_id", s.strategy_id);
    w.raw("symbols", string_array(s.symbols));
    w.integer("requested_quantity", s.requested_quantity);
    if (s.kind == StrategyKind::SmaCrossover) {
        w.unsigned_integer("short_window", s.short_window);
        w.unsigned_integer("long_window", s.long_window);
    } else {
        w.unsigned_integer("lookback", s.lookback);
        w.number("entry_threshold", s.entry_threshold);
        w.number("rearm_threshold", s.rearm_threshold);
    }
    w.close();
    return out;
}

std::string kill_switch_json(const KillSwitchState& k) {
    std::string out;
    ObjectWriter w{out};
    w.raw("engaged", k.engaged ? "true" : "false");
    if (k.changed_at) {
        w.string("changed_at", time_text(*k.changed_at));
    }
    if (!k.reason.empty()) {
        w.string("reason", k.reason);
    }
    w.close();
    return out;
}

std::string payload_json(const Command& c) {
    std::string out;
    ObjectWriter w{out};
    w.string("command_id", c.command_id);
    w.string("action", to_string(c.action));
    w.string("run_id", c.run_id);
    w.string("issued_at", time_text(c.issued_at));
    if (c.settings) {
        w.raw("settings", settings_json(*c.settings));
    }
    if (c.action == CommandAction::SetKillSwitch) {
        w.raw("engaged", c.engaged ? "true" : "false");
        if (!c.reason.empty()) {
            w.string("reason", c.reason);
        }
    }
    w.close();
    return out;
}

std::string payload_json(const RunStatusReport& r) {
    std::string out;
    ObjectWriter w{out};
    w.string("run_id", r.run_id);
    if (!r.engine_id.empty()) {
        w.string("engine_id", r.engine_id);
    }
    w.string("status", to_string(r.status));
    w.string("updated_at", time_text(r.updated_at));
    w.raw("config", settings_json(r.settings));
    w.raw("kill_switch", kill_switch_json(r.kill_switch));
    if (r.progress) {
        w.number("progress", *r.progress);
    }
    if (r.clock) {
        w.string("clock", time_text(*r.clock));
    }
    if (r.error) {
        w.string("error", *r.error);
    }
    if (r.command_id) {
        w.string("command_id", *r.command_id);
    }
    w.close();
    return out;
}

std::string payload_json(const EngineAnnouncement& a) {
    std::string out;
    ObjectWriter w{out};
    w.string("engine_id", a.engine_id);
    w.string("version", a.version);
    w.string("started_at", time_text(a.started_at));
    w.string("sent_at", time_text(a.sent_at));
    w.unsigned_integer("heartbeat_ms", a.heartbeat_ms);
    std::string coverage;
    ObjectWriter m{coverage};
    m.raw("symbols", string_array(a.symbols));
    m.string("first_session", a.first_session);
    m.string("last_session", a.last_session);
    m.close();
    w.raw("market_data", coverage);
    w.raw("runs", string_array(a.runs));
    w.close();
    return out;
}

// --- reading ------------------------------------------------------------------
// Envelope problems are DecodeError; settings problems are SettingsError with
// the member's path, because a person typed them.

[[noreturn]] void bad(const std::string& path, const std::string& problem, bool setting) {
    if (setting) {
        throw SettingsError(path, problem);
    }
    throw DecodeError("control codec: " + path + " " + problem);
}

struct Reader {
    const Value& object;
    std::string  path;      // "" at the root
    bool         setting;   // errors are SettingsError

    [[nodiscard]] std::string where(std::string_view member) const {
        return path.empty() ? std::string(member) : path + "." + std::string(member);
    }
    [[nodiscard]] const Value* find(std::string_view member) const { return object.find(member); }
    [[nodiscard]] const Value& need(std::string_view member) const {
        const Value* v = object.find(member);
        if (v == nullptr) {
            bad(where(member), setting ? "is required." : "is missing", setting);
        }
        return *v;
    }
    [[nodiscard]] Reader child(std::string_view member) const {
        const Value& v = need(member);
        if (v.kind != Value::Kind::Object) {
            bad(where(member), setting ? "must be an object." : "must be an object", setting);
        }
        return Reader{v, where(member), setting};
    }
    [[nodiscard]] std::string str(std::string_view member) const { return text_of(need(member), where(member)); }
    [[nodiscard]] std::string text_of(const Value& v, const std::string& at) const {
        if (v.kind != Value::Kind::String) {
            bad(at, setting ? "must be text." : "must be a string", setting);
        }
        return v.text;
    }
    [[nodiscard]] bool boolean(std::string_view member) const {
        const Value& v = need(member);
        if (v.kind != Value::Kind::Bool) {
            bad(where(member), setting ? "must be true or false." : "must be a boolean", setting);
        }
        return v.boolean;
    }
    template <class Int>
    [[nodiscard]] Int integer(std::string_view member) const {
        const Value& v = need(member);
        Int out{};
        const char* begin = v.text.data();
        const char* end   = begin + v.text.size();
        if (v.kind != Value::Kind::Number || std::from_chars(begin, end, out).ptr != end) {
            bad(where(member), setting ? "must be a whole number." : "must be an integer", setting);
        }
        return out;
    }
    [[nodiscard]] double number(std::string_view member) const {
        const Value& v = need(member);
        double out = 0.0;
        const char* begin = v.text.data();
        const char* end   = begin + v.text.size();
        if (v.kind != Value::Kind::Number || std::from_chars(begin, end, out).ptr != end || !std::isfinite(out)) {
            bad(where(member), setting ? "must be a number." : "must be a number", setting);
        }
        return out;
    }
    [[nodiscard]] common::Decimal decimal(std::string_view member) const {
        const std::string owned = str(member);   // keep the text alive: str() returns by value
        std::string_view text = owned;
        const bool negative = !text.empty() && text.front() == '-';
        if (negative) {
            text.remove_prefix(1);
        }
        common::Decimal out{};
        if (common::parse_decimal(text, out) != common::DecimalStatus::Ok) {
            bad(where(member), setting ? "must be an amount like 2500 or 0.005." : "must be an exact decimal string",
                setting);
        }
        return negative ? -out : out;
    }
    [[nodiscard]] common::Timestamp time(std::string_view member) const {
        const auto parsed = common::parse_timestamp(str(member));
        if (!parsed) {
            bad(where(member), setting ? "must be a UTC time like 2026-01-05T14:30:00Z." : "must be an RFC 3339 UTC time",
                setting);
        }
        return *parsed;
    }
    [[nodiscard]] std::optional<common::Timestamp> opt_time(std::string_view member) const {
        return find(member) ? std::optional{time(member)} : std::nullopt;
    }
    [[nodiscard]] std::vector<std::string> strings(std::string_view member) const {
        const Value& v = need(member);
        if (v.kind != Value::Kind::Array) {
            bad(where(member), setting ? "must be a list." : "must be an array", setting);
        }
        std::vector<std::string> out;
        for (std::size_t i = 0; i < v.array.size(); ++i) {
            out.push_back(text_of(v.array[i], where(member) + "." + std::to_string(i)));
        }
        return out;
    }
};

std::size_t window(const Reader& r, std::string_view member) {
    const auto value = r.integer<std::int64_t>(member);
    if (value < 0) {
        bad(r.where(member), "cannot be negative.", r.setting);
    }
    return static_cast<std::size_t>(value);
}

StrategySettings strategy_from(const Reader& r) {
    StrategySettings s;
    const std::string kind = r.str("kind");
    if (kind == "sma_crossover") {
        s.kind = StrategyKind::SmaCrossover;
    } else if (kind == "mean_reversion") {
        s.kind = StrategyKind::MeanReversion;
    } else {
        bad(r.where("kind"), "'" + kind + "' is not a strategy this engine has.", r.setting);
    }
    s.strategy_id        = r.str("strategy_id");
    s.symbols            = r.strings("symbols");
    s.requested_quantity = r.integer<common::Quantity>("requested_quantity");
    if (s.kind == StrategyKind::SmaCrossover) {
        if (r.find("short_window")) s.short_window = window(r, "short_window");
        if (r.find("long_window"))  s.long_window  = window(r, "long_window");
    } else {
        if (r.find("lookback"))        s.lookback        = window(r, "lookback");
        if (r.find("entry_threshold")) s.entry_threshold = r.number("entry_threshold");
        if (r.find("rearm_threshold")) s.rearm_threshold = r.number("rearm_threshold");
    }
    return s;
}

common::Duration from_millis(double ms) {
    return std::chrono::duration_cast<common::Duration>(std::chrono::duration<double, std::milli>(ms));
}

RunSettings settings_from(const Reader& r) {
    RunSettings s;
    if (r.find("name")) s.name = r.str("name");
    if (r.find("mode")) {
        const std::string mode = r.str("mode");
        if (mode == "live") {
            s.mode = config::RunMode::Live;
        } else if (mode != "backtest") {
            bad(r.where("mode"), "must be backtest or live.", r.setting);
        }
    }
    s.period_start = r.opt_time("period_start");
    s.period_end   = r.opt_time("period_end");

    const Value& list = r.need("strategies");
    if (list.kind != Value::Kind::Array) {
        bad(r.where("strategies"), "must be a list.", r.setting);
    }
    for (std::size_t i = 0; i < list.array.size(); ++i) {
        const std::string at = r.where("strategies") + "." + std::to_string(i);
        if (list.array[i].kind != Value::Kind::Object) {
            bad(at, "must be an object.", r.setting);
        }
        s.strategies.push_back(strategy_from(Reader{list.array[i], at, r.setting}));
    }

    if (r.find("risk")) {
        const Reader risk = r.child("risk");
        s.risk.max_order_quantity    = risk.integer<common::Quantity>("max_order_quantity");
        s.risk.max_position_notional = risk.decimal("max_position_notional");
        s.risk.max_gross_exposure    = risk.decimal("max_gross_exposure");
        s.risk.max_daily_loss        = risk.decimal("max_daily_loss");
        s.risk.max_open_orders       = risk.integer<std::uint32_t>("max_open_orders");
        if (risk.find("allow_short") && risk.boolean("allow_short")) {
            bad(risk.where("allow_short"),
                "Short selling is not supported yet: the portfolio refuses to sell shares it does not hold.", r.setting);
        }
        if (risk.find("policies")) {
            s.risk_policies = risk.boolean("policies");
        }
    }
    if (r.find("simulation")) {
        const Reader sim = r.child("simulation");
        s.simulation.starting_cash = sim.decimal("starting_cash");
        s.simulation.slippage_bps  = sim.number("slippage_bps");
        if (sim.find("fees")) {
            const Reader fees = sim.child("fees");
            s.simulation.fees.per_share = fees.decimal("per_share");
            s.simulation.fees.per_trade = fees.decimal("per_trade");
            s.simulation.fees.bps       = fees.number("bps");
        }
        if (sim.find("latency_ms")) {
            const Reader latency = sim.child("latency_ms");
            s.simulation.latency.signal_to_order = from_millis(latency.number("signal_to_order"));
            s.simulation.latency.order_to_fill   = from_millis(latency.number("order_to_fill"));
        }
        if (sim.find("participation_cap")) {
            s.participation_cap = sim.number("participation_cap");
        }
    }
    return s;
}

Command command_from(const Reader& r) {
    Command c;
    c.command_id = r.str("command_id");
    c.run_id     = r.str("run_id");
    c.issued_at  = r.time("issued_at");
    const std::string action = r.str("action");
    if (action == "start_backtest") {
        c.action = CommandAction::StartBacktest;
        try {
            c.settings = settings_from(Reader{r.child("settings").object, "", true});
        } catch (const SettingsError& e) {
            c.settings_error_field = e.field();
            c.settings_error       = e.message();
        }
    } else if (action == "stop") {
        c.action = CommandAction::StopRun;
    } else if (action == "set_kill_switch") {
        c.action  = CommandAction::SetKillSwitch;
        c.engaged = r.boolean("engaged");
        if (r.find("reason")) c.reason = r.str("reason");
    } else {
        bad("action", "'" + action + "' is not a command", false);
    }
    return c;
}

KillSwitchState kill_switch_from(const Reader& r) {
    KillSwitchState k;
    k.engaged    = r.boolean("engaged");
    k.changed_at = r.opt_time("changed_at");
    if (r.find("reason")) k.reason = r.str("reason");
    return k;
}

RunStatusReport report_from(const Reader& r) {
    RunStatusReport out;
    out.run_id = r.str("run_id");
    if (r.find("engine_id")) out.engine_id = r.str("engine_id");
    const std::string status = r.str("status");
    bool known = false;
    for (const RunState s : {RunState::Queued, RunState::Running, RunState::Completed, RunState::Failed,
                             RunState::Stopped}) {
        if (to_string(s) == status) {
            out.status = s;
            known      = true;
        }
    }
    if (!known) {
        bad("status", "'" + status + "' is not a run status", false);
    }
    out.updated_at  = r.time("updated_at");
    out.settings    = settings_from(Reader{r.child("config").object, "config", false});
    out.kill_switch = kill_switch_from(r.child("kill_switch"));
    if (r.find("progress"))   out.progress   = r.number("progress");
    out.clock = r.opt_time("clock");
    if (r.find("error"))      out.error      = r.str("error");
    if (r.find("command_id")) out.command_id = r.str("command_id");
    return out;
}

EngineAnnouncement announcement_from(const Reader& r) {
    EngineAnnouncement a;
    a.engine_id    = r.str("engine_id");
    a.version      = r.str("version");
    a.started_at   = r.time("started_at");
    a.sent_at      = r.time("sent_at");
    a.heartbeat_ms = r.integer<std::uint32_t>("heartbeat_ms");
    const Reader md = r.child("market_data");
    a.symbols       = md.strings("symbols");
    a.first_session = md.str("first_session");
    a.last_session  = md.str("last_session");
    a.runs          = r.strings("runs");
    return a;
}

}  // namespace

std::string settings_json(const RunSettings& s) {
    std::string out;
    ObjectWriter w{out};
    w.string("name", s.name);
    w.string("mode", s.mode == config::RunMode::Live ? "live" : "backtest");
    if (s.period_start) w.string("period_start", time_text(*s.period_start));
    if (s.period_end)   w.string("period_end", time_text(*s.period_end));

    std::string strategies = "[";
    for (std::size_t i = 0; i < s.strategies.size(); ++i) {
        strategies += (i == 0 ? "" : ",") + strategy_json(s.strategies[i]);
    }
    w.raw("strategies", strategies + "]");

    std::string risk;
    ObjectWriter r{risk};
    r.integer("max_order_quantity", s.risk.max_order_quantity);
    r.string("max_position_notional", common::format_decimal(s.risk.max_position_notional));
    r.string("max_gross_exposure", common::format_decimal(s.risk.max_gross_exposure));
    r.string("max_daily_loss", common::format_decimal(s.risk.max_daily_loss));
    r.unsigned_integer("max_open_orders", s.risk.max_open_orders);
    r.raw("allow_short", "false");
    r.raw("policies", s.risk_policies ? "true" : "false");
    r.close();
    w.raw("risk", risk);

    std::string fees;
    ObjectWriter f{fees};
    f.string("per_share", common::format_decimal(s.simulation.fees.per_share));
    f.string("per_trade", common::format_decimal(s.simulation.fees.per_trade));
    f.number("bps", s.simulation.fees.bps);
    f.close();
    std::string latency;
    ObjectWriter l{latency};
    l.number("signal_to_order", millis(s.simulation.latency.signal_to_order));
    l.number("order_to_fill", millis(s.simulation.latency.order_to_fill));
    l.close();
    std::string sim;
    ObjectWriter m{sim};
    m.string("starting_cash", common::format_decimal(s.simulation.starting_cash));
    m.number("slippage_bps", s.simulation.slippage_bps);
    m.raw("fees", fees);
    m.raw("latency_ms", latency);
    m.number("participation_cap", s.participation_cap);
    m.close();
    w.raw("simulation", sim);
    w.close();
    return out;
}

std::string encode(const ControlEnvelope& envelope) {
    std::string out;
    ObjectWriter w{out};
    w.integer("schema", events::kWireSchemaVersion);
    w.string("origin", envelope.origin);
    w.unsigned_integer("sequence", envelope.sequence);
    w.string("type", type_name(envelope.payload));
    w.string("enqueued_at", time_text(envelope.enqueued_at));
    w.raw("payload", std::visit([](const auto& p) { return payload_json(p); }, envelope.payload));
    w.close();
    return out;
}

ControlEnvelope decode_control(std::string_view text) {
    const std::optional<Value> root = events::json::parse(text);
    if (!root || root->kind != Value::Kind::Object) {
        throw DecodeError("control codec: message is not a JSON object");
    }
    const Reader top{*root, "", false};
    const auto schema = top.integer<int>("schema");
    if (schema != events::kWireSchemaVersion) {
        throw DecodeError("control codec: unsupported schema " + std::to_string(schema));
    }
    ControlEnvelope envelope;
    envelope.origin      = top.str("origin");
    envelope.sequence    = top.integer<std::uint64_t>("sequence");
    envelope.enqueued_at = top.time("enqueued_at");
    const std::string type = top.str("type");
    const Reader payload   = top.child("payload");
    const Reader body{payload.object, "", false};
    if (type == "command") {
        envelope.payload = command_from(body);
    } else if (type == "run") {
        envelope.payload = report_from(body);
    } else if (type == "engine") {
        envelope.payload = announcement_from(body);
    } else {
        throw DecodeError("control codec: unknown message type '" + type + "'");
    }
    return envelope;
}

}  // namespace trading_engine::control
