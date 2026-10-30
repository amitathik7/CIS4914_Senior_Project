#include "lab/backtest_cli.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <type_traits>
#include <charconv>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <system_error>
#include <variant>

#include "lab/catalog.hpp"
#include "lab/dataset.hpp"
#include "lab/errors.hpp"
#include "lab/json_writer.hpp"
#include "lab/paths.hpp"
#include "lab/result_json.hpp"
#include "lab/timestamp.hpp"
#include "lab/utf8.hpp"
#include "trading_engine/app/backtest.hpp"
#include "trading_engine/common/decimal_text.hpp"

namespace trading_engine::lab {

namespace {

constexpr std::size_t    kMaxArguments     = 10'000;
constexpr std::uintmax_t kMaxResponseBytes = 1U << 20;

[[noreturn]] void usage_error(const std::string& message) {
    throw LabError(ErrorCode::UsageError, message, {Problem{0, "", "", message}}, 1);
}

// ---- @response files: the same rules as strategy_lab_replay (lab/cli.cpp) ----------------

void append_response_file(const std::string& token, std::vector<std::string>& out) {
    const std::string name = token.substr(1);
    const std::filesystem::path path = path_from_utf8(name);
    std::error_code ec;
    if (name.empty() || !std::filesystem::is_regular_file(path, ec)) {
        usage_error("cannot read response file '" + utf8_excerpt(name, 80) + "'");
    }
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec || size > kMaxResponseBytes) {
        usage_error("response file '" + utf8_excerpt(name, 80) + "' is unreadable or larger than " +
                    std::to_string(kMaxResponseBytes) + " bytes");
    }
    std::ifstream in(path, std::ios::binary);
    std::string text(static_cast<std::size_t>(size), '\0');
    in.read(text.data(), static_cast<std::streamsize>(text.size()));
    if (!in || static_cast<std::size_t>(in.gcount()) != text.size()) {
        usage_error("cannot read response file '" + utf8_excerpt(name, 80) + "'");
    }
    std::string_view view{text};
    if (view.substr(0, 3) == "\xEF\xBB\xBF") {
        view.remove_prefix(3);
    }
    std::size_t pos = 0;
    while (pos < view.size()) {
        const std::size_t newline = view.find('\n', pos);
        std::string_view line = view.substr(pos, (newline == std::string_view::npos ? view.size() : newline) - pos);
        pos = newline == std::string_view::npos ? view.size() : newline + 1;
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (line.empty() || line.front() == '#') {
            continue;
        }
        if (line.front() == '@') {
            usage_error("nested response files are not supported");
        }
        out.emplace_back(line);
    }
}

std::vector<std::string> expand_arguments(const std::vector<std::string>& args) {
    std::vector<std::string> out;
    for (const std::string& arg : args) {
        if (!is_valid_utf8(arg)) {
            usage_error("an argument is not valid UTF-8 (on Windows put non-ASCII values in an @file)");
        }
        if (!arg.empty() && arg.front() == '@') {
            append_response_file(arg, out);
        } else {
            out.push_back(arg);
        }
        if (out.size() > kMaxArguments) {
            usage_error("more than " + std::to_string(kMaxArguments) + " arguments");
        }
    }
    return out;
}

// ---- parsing ---------------------------------------------------------------------------

struct Command {
    std::string                  dataset{};
    std::vector<StrategyRequest> strategies{};
    app::BacktestConfig          config{};
    std::size_t                  max_rows{kDefaultMaxRows};
    bool                         pretty{false};
    bool                         periods_given{false};
};

// Annualisation from the bar spacing, when --periods-per-year is not given: the median
// gap between one symbol's consecutive bars, counted in a 252-day year of 6.5-hour
// sessions (daily or longer bars: 252).
unsigned infer_periods_per_year(const Dataset& dataset) {
    std::map<std::string, common::Timestamp> last;
    std::vector<std::int64_t> gaps;
    for (const auto& row : dataset.rows) {
        const auto it = last.find(row.event.symbol);
        if (it != last.end() && row.event.exchange_time > it->second) {
            gaps.push_back(std::chrono::duration_cast<std::chrono::seconds>(row.event.exchange_time - it->second).count());
        }
        last[row.event.symbol] = row.event.exchange_time;
    }
    if (gaps.empty()) {
        return 252;
    }
    std::nth_element(gaps.begin(), gaps.begin() + static_cast<std::ptrdiff_t>(gaps.size() / 2), gaps.end());
    const std::int64_t gap = gaps[gaps.size() / 2];
    constexpr std::int64_t kSession = 6 * 3600 + 1800;
    if (gap <= 0 || gap >= kSession) {
        return 252;
    }
    return static_cast<unsigned>(252 * kSession / gap);
}

common::Money parse_money(const std::string& flag, const std::string& text) {
    common::Money value{};
    if (common::parse_decimal(text, value) != common::DecimalStatus::Ok) {
        usage_error(flag + " needs a plain non-negative decimal such as 100000 or 0.005 (got '" +
                    utf8_excerpt(text, 40) + "')");
    }
    return value;
}

std::uint64_t parse_whole(const std::string& flag, const std::string& text, std::uint64_t max) {
    std::uint64_t value = 0;
    const auto r = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || r.ec != std::errc{} || r.ptr != text.data() + text.size() || value > max) {
        usage_error(flag + " needs a whole number up to " + std::to_string(max) + " (got '" +
                    utf8_excerpt(text, 40) + "')");
    }
    return value;
}

double parse_bps(const std::string& flag, const std::string& text) {
    // Exact text first, so "1.5" is read the same everywhere; a bps value is a rate, so a double is fine.
    return parse_money(flag, text).to_double();
}

Command parse(const std::vector<std::string>& tokens) {
    Command c;
    bool have_dataset = false;
    const auto value_of = [&](std::size_t& i, const std::string& flag) -> const std::string& {
        if (i + 1 >= tokens.size()) {
            usage_error(flag + " needs a value");
        }
        return tokens[++i];
    };
    auto& sim  = c.config.simulation;
    auto& risk = c.config.risk;
    for (std::size_t i = 0; i < tokens.size(); ++i) {
        const std::string& t = tokens[i];
        if (t == "--dataset") {
            if (have_dataset) {
                usage_error("--dataset was given more than once");
            }
            c.dataset    = value_of(i, t);
            have_dataset = true;
        } else if (t == "--strategy") {
            if (c.strategies.size() >= kMaxStrategies) {
                usage_error("at most " + std::to_string(kMaxStrategies) + " strategies per run");
            }
            c.strategies.push_back(StrategyRequest{value_of(i, t), {}});
        } else if (t == "--param") {
            const std::string& a = value_of(i, t);
            if (c.strategies.empty()) {
                usage_error("--param must come after the --strategy it configures");
            }
            const std::size_t eq = a.find('=');
            if (eq == std::string::npos || eq == 0) {
                usage_error("--param expects name=value (got '" + utf8_excerpt(a, 40) + "')");
            }
            c.strategies.back().params.push_back(ParamAssignment{a.substr(0, eq), a.substr(eq + 1)});
        } else if (t == "--cash") {
            sim.starting_cash = parse_money(t, value_of(i, t));
            if (sim.starting_cash <= common::Money{}) {
                usage_error("--cash must be more than 0");
            }
        } else if (t == "--slippage-bps") {
            sim.slippage_bps = parse_bps(t, value_of(i, t));
        } else if (t == "--fee-per-share") {
            sim.fees.per_share = parse_money(t, value_of(i, t));
        } else if (t == "--fee-per-trade") {
            sim.fees.per_trade = parse_money(t, value_of(i, t));
        } else if (t == "--fee-bps") {
            sim.fees.bps = parse_bps(t, value_of(i, t));
        } else if (t == "--max-order-quantity") {
            risk.max_order_quantity = static_cast<common::Quantity>(
                parse_whole(t, value_of(i, t), static_cast<std::uint64_t>(INT64_MAX)));
        } else if (t == "--max-position-notional") {
            risk.max_position_notional = parse_money(t, value_of(i, t));
        } else if (t == "--max-gross-exposure") {
            risk.max_gross_exposure = parse_money(t, value_of(i, t));
        } else if (t == "--max-daily-loss") {
            risk.max_daily_loss = parse_money(t, value_of(i, t));
        } else if (t == "--max-open-orders") {
            risk.max_open_orders = static_cast<std::uint32_t>(parse_whole(t, value_of(i, t), UINT32_MAX));
        } else if (t == "--no-risk-policies") {
            c.config.risk_policies = false;
        } else if (t == "--periods-per-year") {
            c.config.analysis.periods_per_year =
                static_cast<unsigned>(parse_whole(t, value_of(i, t), 10'000'000));
            if (c.config.analysis.periods_per_year == 0) {
                usage_error("--periods-per-year must be at least 1");
            }
            c.periods_given = true;
        } else if (t == "--max-rows") {
            c.max_rows = static_cast<std::size_t>(parse_whole(t, value_of(i, t), kHardMaxRows));
            if (c.max_rows == 0) {
                usage_error("--max-rows must be at least 1");
            }
        } else if (t == "--pretty") {
            c.pretty = true;
        } else {
            usage_error("unknown argument '" + utf8_excerpt(t, 40) + "' (see --help)");
        }
    }
    if (!have_dataset) {
        usage_error("--dataset is required");
    }
    if (c.strategies.empty()) {
        usage_error("at least one --strategy is required");
    }
    return c;
}

// ---- output ----------------------------------------------------------------------------

void money(JsonWriter& w, std::string_view name, common::Money m) {
    w.key(name).scaled_number(m.micros(), common::kDecimalPlaces);
}

void time_field(JsonWriter& w, std::string_view name, common::Timestamp t) {
    w.field(name, format_utc_timestamp(t));
}

void finite(JsonWriter& w, std::string_view name, double v) {
    if (std::isfinite(v)) {
        w.key(name).number(v);
    }
}

void param_value(JsonWriter& w, const ParamValue& value) {
    std::visit(
        [&](const auto& v) {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, std::string>) {
                w.string(v);
            } else if constexpr (std::is_same_v<T, std::uint64_t>) {
                w.unsigned_integer(v);
            } else if constexpr (std::is_same_v<T, double>) {
                if (std::isfinite(v)) {
                    w.number(v);
                } else {
                    w.null();
                }
            } else {
                w.begin_array();
                for (const auto& s : v) {
                    w.string(s);
                }
                w.end_array();
            }
        },
        value);
}

std::string side_text(common::Quantity signed_quantity) { return signed_quantity < 0 ? "sell" : "buy"; }

std::string document(const Command& c, const Dataset& dataset, const std::vector<BuiltStrategy>& built,
                     const app::BacktestResult& r) {
    const auto& sim  = c.config.simulation;
    const auto& risk = c.config.risk;
    JsonWriter w{c.pretty};
    w.begin_object();
    w.field("schema", kBacktestSchema);
    w.field("schema_version", kSchemaVersion);

    w.key("input").begin_object();
    w.field("dataset", dataset.info.name);
    w.field("sha256", dataset.info.sha256);
    w.field("rows", static_cast<std::uint64_t>(dataset.info.rows));
    time_field(w, "first_exchange_time", dataset.info.first_exchange_time);
    time_field(w, "last_exchange_time", dataset.info.last_exchange_time);
    w.key("symbols").begin_array();
    for (const auto& s : dataset.info.symbols) {
        w.string(s.symbol);
    }
    w.end_array();
    w.end_object();

    w.key("config").begin_object();
    money(w, "starting_cash", sim.starting_cash);
    w.field("slippage_bps", sim.slippage_bps);
    w.key("fees").begin_object();
    money(w, "per_share", sim.fees.per_share);
    money(w, "per_trade", sim.fees.per_trade);
    w.field("bps", sim.fees.bps);
    w.end_object();
    w.field("risk_policies", c.config.risk_policies);
    w.key("risk").begin_object();
    w.field("max_order_quantity", static_cast<std::int64_t>(risk.max_order_quantity));
    money(w, "max_position_notional", risk.max_position_notional);
    money(w, "max_gross_exposure", risk.max_gross_exposure);
    money(w, "max_daily_loss", risk.max_daily_loss);
    w.field("max_open_orders", static_cast<std::uint64_t>(risk.max_open_orders));
    w.end_object();
    w.field("periods_per_year", static_cast<std::uint64_t>(c.config.analysis.periods_per_year));
    w.field("periods_per_year_source", c.periods_given ? "given" : "inferred_from_bar_spacing");
    w.field("fill_rule", "next_bar_open");
    w.end_object();

    w.key("strategies").begin_array();
    for (const auto& b : built) {
        w.begin_object();
        w.field("kind", b.kind);
        w.field("strategy_id", b.strategy_id);
        w.key("parameters").begin_object();
        for (const auto& [name, value] : b.parameters) {
            w.key(name);
            param_value(w, value);
        }
        w.end_object();
        w.end_object();
    }
    w.end_array();

    const auto& rep = r.report;
    w.key("report").begin_object();
    time_field(w, "period_start", rep.period_start);
    time_field(w, "period_end", rep.period_end);
    finite(w, "total_return", rep.total_return);
    finite(w, "volatility", rep.volatility);
    finite(w, "max_drawdown", rep.max_drawdown);
    if (rep.losing_trades > 0) {
        finite(w, "profit_factor", rep.profit_factor);
    } else {
        w.field("profit_factor_status", rep.trade_count == 0 ? "no_trades" : "no_losing_trades");
    }
    if (rep.win_rate) {
        finite(w, "win_rate", *rep.win_rate);
    }
    if (rep.sharpe) {
        finite(w, "sharpe", *rep.sharpe);
    }
    w.field("trade_count", rep.trade_count);
    w.field("winning_trades", rep.winning_trades);
    w.field("losing_trades", rep.losing_trades);
    w.field("fill_count", rep.fill_count);
    money(w, "starting_equity", rep.starting_equity);
    money(w, "ending_equity", rep.ending_equity);
    money(w, "gross_profit", rep.gross_profit);
    money(w, "gross_loss", rep.gross_loss);
    money(w, "fees_paid", rep.fees_paid);
    w.end_object();

    w.key("counts").begin_object();
    w.field("bars_accepted", r.market_data.accepted);
    w.field("bars_rejected", r.market_data.rejected);
    w.field("signals", static_cast<std::uint64_t>(r.signals.size()));
    w.field("approved", r.risk.approved);
    w.field("resized", r.risk.resized);
    w.field("rejected", r.risk.rejected);
    w.field("orders", static_cast<std::uint64_t>(r.orders.size()));
    w.field("fills", static_cast<std::uint64_t>(r.fills.size()));
    w.field("open_orders_at_end", static_cast<std::uint64_t>(r.open_orders_at_end));
    w.field("strategy_errors", r.strategies.strategy_errors);
    w.field("portfolio_errors", r.portfolio.errors);
    w.end_object();

    w.key("final").begin_object();
    money(w, "cash", r.final_snapshot.cash);
    money(w, "equity", r.final_snapshot.total_equity);
    money(w, "gross_exposure", r.final_snapshot.gross_exposure);
    money(w, "reserved_cash", r.final_snapshot.reserved_cash);
    w.key("positions").begin_array();
    for (const auto& p : r.final_snapshot.positions) {
        w.begin_object();
        w.field("symbol", p.symbol);
        w.field("quantity", static_cast<std::int64_t>(p.quantity));
        money(w, "average_cost", p.average_cost);
        money(w, "realized_pnl", p.realized_pnl);
        money(w, "unrealized_pnl", p.unrealized_pnl);
        w.end_object();
    }
    w.end_array();
    w.end_object();

    // One point per bar timestamp. Drawdown is from the running peak, starting cash included.
    w.key("equity").begin_array();
    double peak = rep.starting_equity.to_double();
    for (const auto& s : r.equity_curve) {
        const double e = s.total_equity.to_double();
        peak = std::max(peak, e);
        w.begin_object();
        time_field(w, "time", s.as_of);
        money(w, "equity", s.total_equity);
        money(w, "cash", s.cash);
        money(w, "gross_exposure", s.gross_exposure);
        finite(w, "drawdown", peak > 0.0 ? e / peak - 1.0 : 0.0);
        w.end_object();
    }
    w.end_array();

    std::map<common::OrderId, const domain::Order*> by_id;
    for (const auto& o : r.orders) {
        by_id[o.id] = &o;
    }
    w.key("fills").begin_array();
    for (const auto& f : r.fills) {
        w.begin_object();
        w.field("fill_id", f.id.value);
        w.field("order_id", f.order_id.value);
        if (const auto it = by_id.find(f.order_id); it != by_id.end()) {
            w.field("strategy_id", it->second->strategy_id);
            w.field("signal_id", it->second->origin_signal.value);
        }
        w.field("symbol", f.symbol);
        w.field("side", side_text(f.filled_quantity));
        w.field("quantity", static_cast<std::int64_t>(f.filled_quantity < 0 ? -f.filled_quantity : f.filled_quantity));
        money(w, "price", f.fill_price);
        money(w, "fees", f.fees);
        money(w, "slippage", f.slippage);
        time_field(w, "time", f.filled_at);
        w.end_object();
    }
    w.end_array();

    w.key("orders").begin_array();
    for (const auto& o : r.orders) {
        w.begin_object();
        w.field("order_id", o.id.value);
        w.field("signal_id", o.origin_signal.value);
        w.field("strategy_id", o.strategy_id);
        w.field("symbol", o.symbol);
        w.field("side", o.side == domain::OrderSide::Sell ? "sell" : "buy");
        w.field("quantity", static_cast<std::int64_t>(o.quantity));
        w.field("filled_quantity", static_cast<std::int64_t>(o.filled_quantity));
        w.field("status", domain::to_string(o.status));
        if (o.average_fill_price) {
            money(w, "average_fill_price", *o.average_fill_price);
        }
        time_field(w, "created_at", o.created_at);
        if (o.reject_reason) {
            w.field("reject_reason", *o.reject_reason);
        }
        w.end_object();
    }
    w.end_array();

    w.key("decisions").begin_array();
    for (const auto& d : r.decisions) {
        w.begin_object();
        w.field("signal_id", d.signal.id.value);
        w.field("strategy_id", d.signal.strategy_id);
        w.field("symbol", d.signal.symbol);
        w.field("side", domain::to_string(d.signal.side));
        w.field("requested_quantity", static_cast<std::int64_t>(d.signal.requested_quantity.value_or(0)));
        time_field(w, "time", d.signal.created_at);
        w.field("approved", d.decision.approved);
        if (d.decision.adjusted_quantity) {
            w.field("adjusted_quantity", static_cast<std::int64_t>(*d.decision.adjusted_quantity));
        }
        w.field("reason", d.decision.reason);
        if (!d.policy.empty()) {
            w.field("policy", d.policy);
        }
        w.end_object();
    }
    w.end_array();

    w.end_object();
    std::string out = w.take();
    out += '\n';
    return out;
}

CliResult fail(const LabError& error, bool pretty) {
    CliResult result;
    result.exit_code = exit_status(error.code());
    result.err = "strategy_lab_backtest: error [" + std::string{code_name(error.code())} + "]: " + error.what() + "\n";
    for (const Problem& p : error.problems()) {
        result.err += "  " + (p.line ? "line " + std::to_string(p.line) + ": " : std::string{}) +
                      (p.where.empty() ? "" : p.where + ": ") + p.message + "\n";
    }
    try {
        result.out = error_json(error, pretty);
    } catch (const std::exception&) {
        result.exit_code = kExitInternal;
        result.out = "{\"schema\":\"strategy_lab.error\",\"schema_version\":\"1.0\",\"error\":{\"code\":"
                     "\"internal_error\",\"exit_status\":4,\"message\":\"could not write the error "
                     "document\",\"total_problems\":0,\"problems\":[]}}\n";
    }
    return result;
}

}  // namespace

std::string backtest_usage_text() {
    return "usage:\n"
           "  strategy_lab_backtest --dataset <csv> --strategy <kind> [--param name=value]...\n"
           "                        [--strategy <kind> [--param name=value]...]...\n"
           "                        [--cash D] [--slippage-bps X] [--fee-per-share D] [--fee-per-trade D]\n"
           "                        [--fee-bps X] [--max-order-quantity N] [--max-position-notional D]\n"
           "                        [--max-gross-exposure D] [--max-daily-loss D] [--max-open-orders N]\n"
           "                        [--no-risk-policies] [--periods-per-year N] [--max-rows N] [--pretty]\n"
           "  strategy_lab_backtest --help\n"
           "  @<file>   replaced by that file's lines, one argument per line\n"
           "\n"
           "Runs the strategies through risk, simulated execution (fills at the next bar's open) and the\n"
           "portfolio, and prints one strategy_lab.backtest JSON document. Exit status: 0 ok,\n"
           "2 usage/configuration, 3 dataset, 4 internal. See docs/BACKTESTING.md.\n";
}

CliResult run_backtest_cli(const std::vector<std::string>& args) {
    bool pretty = std::find(args.begin(), args.end(), "--pretty") != args.end();
    try {
        const std::vector<std::string> tokens = expand_arguments(args);
        pretty = std::find(tokens.begin(), tokens.end(), "--pretty") != tokens.end();
        if (!tokens.empty() && (tokens.front() == "--help" || tokens.front() == "-h")) {
            CliResult result;
            result.err = backtest_usage_text();
            return result;
        }
        Command command = parse(tokens);

        DatasetLimits limits;
        limits.max_rows = command.max_rows;
        const Dataset dataset = load_dataset(path_from_utf8(command.dataset), limits);
        if (dataset.rows.empty()) {
            throw LabError(ErrorCode::DatasetError, "the dataset has no rows to backtest");
        }

        if (!command.periods_given) {
            command.config.analysis.periods_per_year = infer_periods_per_year(dataset);
        }

        std::vector<BuiltStrategy> built;
        app::Backtest backtest{command.config};
        for (std::size_t i = 0; i < command.strategies.size(); ++i) {
            built.push_back(build_strategy(command.strategies[i], i));
            for (std::size_t j = 0; j < i; ++j) {
                if (built[j].strategy_id == built[i].strategy_id) {
                    const std::string m = "strategy_id '" + built[i].strategy_id + "' is used by more than one strategy";
                    throw LabError(ErrorCode::ConfigError, m,
                                   {Problem{0, "strategy_id", "strategies[" + std::to_string(i) + "]", m}});
                }
            }
            backtest.add_strategy(built.back().strategy);
        }

        std::vector<domain::MarketEvent> bars;
        bars.reserve(dataset.rows.size());
        for (const auto& row : dataset.rows) {
            bars.push_back(row.event);
        }
        const app::BacktestResult result = backtest.run(std::move(bars));

        CliResult out;
        out.out = document(command, dataset, built, result);
        if (result.open_orders_at_end > 0) {
            out.err += "strategy_lab_backtest: warning [open_orders_at_end]: " +
                       std::to_string(result.open_orders_at_end) +
                       " order(s) were still working when the data ended and never filled\n";
        }
        return out;
    } catch (const LabError& error) {
        return fail(error, pretty);
    } catch (const std::exception& error) {
        return fail(LabError(ErrorCode::InternalError, std::string{"unexpected failure: "} + to_valid_utf8(error.what())),
                    pretty);
    } catch (...) {
        return fail(LabError(ErrorCode::InternalError, "unexpected failure"), pretty);
    }
}

}  // namespace trading_engine::lab
