#include "trading_engine/configuration/engine_config.hpp"

#include <charconv>
#include <chrono>
#include <cmath>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <set>
#include <sstream>
#include <string_view>

#include "../events/json.hpp"
#include "trading_engine/common/decimal_text.hpp"
#include "trading_engine/common/errors.hpp"

namespace trading_engine::config {

namespace {

using events::json::Value;

[[noreturn]] void fail(const std::string& field, const std::string& message) {
    throw common::ConfigError(field + ": " + message);
}

// One JSON object of the file. Every key is optional (the struct default
// stands); a key the struct does not have is an error, so a typo is not
// silently ignored. Keys starting with '$' are comments.
class Section {
public:
    Section(const Value& value, std::string path) : value_{value}, path_{std::move(path)} {
        if (value_.kind != Value::Kind::Object) {
            fail(path_.empty() ? "file" : path_, "must be an object");
        }
    }

    Section(const Section&)            = delete;
    Section& operator=(const Section&) = delete;

    void finish() const {
        for (const auto& [key, _] : value_.object) {
            if (!key.starts_with('$') && !seen_.contains(key)) {
                fail(where(key), "unknown setting");
            }
        }
    }

    [[nodiscard]] std::string where(std::string_view key) const {
        return path_.empty() ? std::string(key) : path_ + "." + std::string(key);
    }

    [[nodiscard]] const Value* get(std::string_view key) {
        seen_.emplace(key);
        return value_.find(key);
    }

    void text(std::string_view key, std::string& out) {
        if (const Value* v = get(key)) {
            if (v->kind != Value::Kind::String) {
                fail(where(key), "must be a string");
            }
            out = v->text;
        }
    }

    void flag(std::string_view key, bool& out) {
        if (const Value* v = get(key)) {
            if (v->kind != Value::Kind::Bool) {
                fail(where(key), "must be true or false");
            }
            out = v->boolean;
        }
    }

    void number(std::string_view key, double& out) {
        if (const Value* v = get(key)) {
            out = as_double(*v, where(key));
        }
    }

    void money(std::string_view key, common::Money& out) {
        if (const Value* v = get(key)) {
            out = as_decimal(*v, where(key));
        }
    }

    template <class Int>
    void integer(std::string_view key, Int& out) {
        if (const Value* v = get(key)) {
            const common::Decimal d = as_decimal(*v, where(key));
            const std::int64_t units = d.micros() / common::Decimal::kScale;
            if (d.micros() % common::Decimal::kScale != 0) {
                fail(where(key), "must be a whole number");
            }
            if (units > static_cast<std::int64_t>(std::numeric_limits<Int>::max())) {
                fail(where(key), "is too large");
            }
            out = static_cast<Int>(units);
        }
    }

    void milliseconds(std::string_view key, common::Duration& out) {
        if (const Value* v = get(key)) {
            const double ms = as_double(*v, where(key));
            if (ms < 0.0) {
                fail(where(key), "must not be negative");
            }
            out = std::chrono::duration_cast<common::Duration>(std::chrono::duration<double, std::milli>(ms));
        }
    }

    void strings(std::string_view key, std::vector<std::string>& out) {
        if (const Value* v = get(key)) {
            if (v->kind != Value::Kind::Array) {
                fail(where(key), "must be a list of strings");
            }
            out.clear();
            for (const Value& item : v->array) {
                if (item.kind != Value::Kind::String) {
                    fail(where(key), "must be a list of strings");
                }
                out.push_back(item.text);
            }
        }
    }

private:
    static double as_double(const Value& v, const std::string& field) {
        double out = 0.0;
        const char* begin = v.text.data();
        const char* end   = begin + v.text.size();
        if (v.kind != Value::Kind::Number || std::from_chars(begin, end, out).ptr != end || !std::isfinite(out)) {
            fail(field, "must be a number");
        }
        return out;
    }

    // Exact: the literal's text goes straight to fixed point, never through a double.
    static common::Decimal as_decimal(const Value& v, const std::string& field) {
        if (v.kind != Value::Kind::Number) {
            fail(field, "must be a number");
        }
        if (v.text.starts_with('-')) {
            fail(field, "must not be negative");
        }
        common::Decimal out{};
        switch (common::parse_decimal(v.text, out)) {
            case common::DecimalStatus::Ok:              return out;
            case common::DecimalStatus::TooManyDecimals: fail(field, "has more than 6 decimal places");
            case common::DecimalStatus::OutOfRange:      fail(field, "is too large");
            default:                                     break;
        }
        fail(field, "must be a plain decimal number (no exponent)");
    }

    const Value&          value_;
    std::string           path_;
    std::set<std::string, std::less<>> seen_;
};

void read_alpaca(Section& s, AlpacaConfig& a) {
    s.text("environment", a.environment);
    s.text("data_feed", a.data_feed);
    s.text("market_data_base_url", a.market_data_base_url);
    s.text("stream_url", a.stream_url);
    s.text("api_key_env", a.api_key_env);
    s.text("api_secret_env", a.api_secret_env);
    s.strings("symbols", a.symbols);
}

void read_postgres(Section& s, PostgresConfig& p) {
    s.text("url_env", p.url_env);
    s.text("host", p.host);
    s.integer("port", p.port);
    s.text("database", p.database);
    s.text("user", p.user);
    s.text("password_env", p.password_env);
    s.text("schema", p.schema);
    s.integer("pool_size", p.pool_size);
}

void read_risk(Section& s, RiskLimits& r) {
    s.money("max_gross_exposure", r.max_gross_exposure);
    s.money("max_position_notional", r.max_position_notional);
    s.integer("max_order_quantity", r.max_order_quantity);
    s.money("max_daily_loss", r.max_daily_loss);
    s.integer("max_open_orders", r.max_open_orders);
}

void read_simulation(Section& s, SimulationConfig& sim) {
    s.money("starting_cash", sim.starting_cash);
    s.number("replay_speed", sim.replay_speed);
    s.flag("deterministic", sim.deterministic);
    s.number("slippage_bps", sim.slippage_bps);
    if (const Value* fees = s.get("fees")) {
        Section f{*fees, s.where("fees")};
        f.money("per_share", sim.fees.per_share);
        f.money("per_trade", sim.fees.per_trade);
        f.number("bps", sim.fees.bps);
        f.finish();
    }
    if (const Value* latency = s.get("latency_ms")) {
        Section l{*latency, s.where("latency_ms")};
        l.milliseconds("signal_to_order", sim.latency.signal_to_order);
        l.milliseconds("order_to_fill", sim.latency.order_to_fill);
        l.finish();
    }
}

template <class Read, class Target>
void section(Section& top, std::string_view key, Read read, Target& target) {
    if (const Value* v = top.get(key)) {
        Section s{*v, std::string(key)};
        read(s, target);
        s.finish();
    }
}

bool one_of(const std::string& value, std::initializer_list<std::string_view> allowed) {
    for (const auto a : allowed) {
        if (value == a) {
            return true;
        }
    }
    return false;
}

void need_positive(const common::Decimal& value, const char* field) {
    if (value <= common::Decimal{}) {
        fail(field, "must be greater than zero");
    }
}

void need_non_negative(double value, const char* field) {
    if (!std::isfinite(value) || value < 0.0) {
        fail(field, "must not be negative");
    }
}

void need_url(const std::string& url, std::initializer_list<std::string_view> schemes, const char* field) {
    for (const auto scheme : schemes) {
        if (url.starts_with(scheme) && url.size() > scheme.size()) {
            return;
        }
    }
    std::string list;
    for (const auto scheme : schemes) {
        list += (list.empty() ? "" : " or ") + std::string(scheme);
    }
    fail(field, "must start with " + list);
}

}  // namespace

EngineConfig default_config() {
    // Every default lives in the struct member initialisers in the header.
    return EngineConfig{};
}

EngineConfig parse_config(std::string_view json_text) {
    const std::optional<Value> root = events::json::parse(json_text);
    if (!root) {
        fail("file", "is not valid JSON");
    }
    EngineConfig c = default_config();
    {
        Section top{*root, ""};
        std::string mode = c.mode == RunMode::Live ? "live" : "backtest";
        top.text("mode", mode);
        if (mode != "backtest" && mode != "live") {
            fail("mode", "must be backtest or live");
        }
        c.mode = mode == "live" ? RunMode::Live : RunMode::Backtest;
        section(top, "alpaca", read_alpaca, c.alpaca);
        section(top, "database", read_postgres, c.postgres);
        section(top, "risk", read_risk, c.risk);
        section(top, "simulation", read_simulation, c.simulation);
        top.finish();
    }
    validate(c);
    return c;
}

EngineConfig load_config(const std::filesystem::path& file) {
    std::ifstream in{file, std::ios::binary};
    if (!in) {
        throw common::ConfigError("cannot open " + file.string());
    }
    std::ostringstream text;
    text << in.rdbuf();
    try {
        return parse_config(text.str());
    } catch (const common::ConfigError& e) {
        constexpr std::string_view prefix = "configuration error: ";
        std::string_view what = e.what();
        if (what.starts_with(prefix)) {
            what.remove_prefix(prefix.size());
        }
        throw common::ConfigError(file.string() + ": " + std::string(what));
    }
}

void validate(const EngineConfig& c) {
    const auto& a = c.alpaca;
    if (!one_of(a.environment, {"paper", "live"})) {
        fail("alpaca.environment", "must be paper or live");
    }
    if (!one_of(a.data_feed, {"iex", "sip"})) {
        fail("alpaca.data_feed", "must be iex or sip");
    }
    need_url(a.market_data_base_url, {"https://", "http://"}, "alpaca.market_data_base_url");
    need_url(a.stream_url, {"wss://", "ws://"}, "alpaca.stream_url");
    if (a.api_key_env.empty() || a.api_secret_env.empty()) {
        fail(a.api_key_env.empty() ? "alpaca.api_key_env" : "alpaca.api_secret_env",
             "must name an environment variable");
    }
    for (const auto& symbol : a.symbols) {
        if (symbol.empty()) {
            fail("alpaca.symbols", "must not contain an empty symbol");
        }
    }
    if (c.mode == RunMode::Live && a.symbols.empty()) {
        fail("alpaca.symbols", "must list at least one symbol in live mode");
    }

    const auto& p = c.postgres;
    if (p.host.empty() || p.database.empty() || p.user.empty()) {
        fail(p.host.empty() ? "database.host" : p.database.empty() ? "database.database" : "database.user",
             "must not be empty");
    }
    if (p.port == 0) {
        fail("database.port", "must be between 1 and 65535");
    }
    if (p.pool_size == 0) {
        fail("database.pool_size", "must be at least 1");
    }

    const auto& r = c.risk;
    need_positive(r.max_gross_exposure, "risk.max_gross_exposure");
    need_positive(r.max_position_notional, "risk.max_position_notional");
    need_positive(r.max_daily_loss, "risk.max_daily_loss");
    if (r.max_order_quantity <= 0) {
        fail("risk.max_order_quantity", "must be greater than zero");
    }
    if (r.max_open_orders == 0) {
        fail("risk.max_open_orders", "must be at least 1");
    }
    if (r.max_position_notional > r.max_gross_exposure) {
        fail("risk.max_position_notional", "cannot exceed risk.max_gross_exposure");
    }

    const auto& s = c.simulation;
    need_positive(s.starting_cash, "simulation.starting_cash");
    need_non_negative(s.replay_speed, "simulation.replay_speed");
    need_non_negative(s.slippage_bps, "simulation.slippage_bps");
    need_non_negative(s.fees.bps, "simulation.fees.bps");
    if (s.fees.per_share < common::Money{} || s.fees.per_trade < common::Money{}) {
        fail(s.fees.per_share < common::Money{} ? "simulation.fees.per_share" : "simulation.fees.per_trade",
             "must not be negative");
    }
    if (s.latency.signal_to_order < common::Duration::zero() || s.latency.order_to_fill < common::Duration::zero()) {
        fail("simulation.latency_ms", "must not be negative");
    }
}

}  // namespace trading_engine::config
