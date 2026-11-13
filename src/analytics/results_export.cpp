#include "trading_engine/analytics/results_export.hpp"

#include <cmath>
#include <cstdio>
#include <string_view>

#include "trading_engine/common/decimal_text.hpp"
#include "trading_engine/common/timestamp_text.hpp"

namespace trading_engine::analytics {
namespace {

std::string json_text(std::string_view text) {
    std::string s{"\""};
    for (const char c : text) {
        switch (c) {
            case '"':  s += "\\\""; break;
            case '\\': s += "\\\\"; break;
            case '\n': s += "\\n"; break;
            case '\r': s += "\\r"; break;
            case '\t': s += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", static_cast<unsigned>(c));
                    s += buf;
                } else {
                    s += c;
                }
        }
    }
    return s + '"';
}

std::string number(double value) {
    if (!std::isfinite(value)) {
        return "null";
    }
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.10g", value);
    return buf;
}

std::string number(std::optional<double> value) { return value ? number(*value) : "null"; }

std::string money(common::Decimal value) { return common::format_decimal(value); }

std::string json_time(common::Timestamp t) { return json_text(common::format_timestamp(t)); }

std::string nanos(common::Duration d) { return std::to_string(d.count()); }

}  // namespace

void write_results_json(std::ostream& out, const RunResults& r) {
    const auto& p = r.report;
    out << "{\n"
        << "  \"schema\": " << json_text(kResultsSchema) << ",\n"
        << "  \"name\": " << json_text(r.name) << ",\n"
        << "  \"mode\": " << json_text(r.mode) << ",\n"
        << "  \"source\": " << json_text(r.source) << ",\n"
        << "  \"symbols\": [";
    for (std::size_t i = 0; i < r.symbols.size(); ++i) {
        out << (i == 0 ? "" : ", ") << json_text(r.symbols[i]);
    }
    out << "],\n"
        << "  \"summary\": {\n"
        << "    \"period_start\": " << json_time(p.period_start) << ",\n"
        << "    \"period_end\": " << json_time(p.period_end) << ",\n"
        << "    \"generated_at\": " << json_time(p.generated_at) << ",\n"
        << "    \"starting_equity\": " << money(p.starting_equity) << ",\n"
        << "    \"ending_equity\": " << money(p.ending_equity) << ",\n"
        << "    \"total_return\": " << number(p.total_return) << ",\n"
        << "    \"volatility\": " << number(p.volatility) << ",\n"
        << "    \"max_drawdown\": " << number(p.max_drawdown) << ",\n"
        << "    \"sharpe\": " << number(p.sharpe) << ",\n"
        << "    \"profit_factor\": " << number(p.profit_factor) << ",\n"
        << "    \"win_rate\": " << number(p.win_rate) << ",\n"
        << "    \"trade_count\": " << p.trade_count << ",\n"
        << "    \"fill_count\": " << p.fill_count << ",\n"
        << "    \"winning_trades\": " << p.winning_trades << ",\n"
        << "    \"losing_trades\": " << p.losing_trades << ",\n"
        << "    \"gross_profit\": " << money(p.gross_profit) << ",\n"
        << "    \"gross_loss\": " << money(p.gross_loss) << ",\n"
        << "    \"fees_paid\": " << money(p.fees_paid) << "\n"
        << "  },\n"
        << "  \"equity_curve\": [";
    for (std::size_t i = 0; i < r.equity_curve.size(); ++i) {
        const auto& s = r.equity_curve[i];
        out << (i == 0 ? "\n" : ",\n") << "    {\"time\": " << json_time(s.as_of) << ", \"equity\": "
            << money(s.total_equity) << ", \"cash\": " << money(s.cash) << '}';
    }
    out << (r.equity_curve.empty() ? "" : "\n  ") << "],\n"
        << "  \"fills\": [";
    for (std::size_t i = 0; i < r.fills.size(); ++i) {
        const auto& f = r.fills[i];
        out << (i == 0 ? "\n" : ",\n") << "    {\"time\": " << json_time(f.filled_at) << ", \"order_id\": "
            << f.order_id.value << ", \"symbol\": " << json_text(f.symbol) << ", \"quantity\": " << f.filled_quantity
            << ", \"price\": " << money(f.fill_price) << ", \"fees\": " << money(f.fees) << '}';
    }
    out << (r.fills.empty() ? "" : "\n  ") << "],\n"
        << "  \"stages\": [";
    bool first = true;
    if (r.metrics) {
        for (const auto& s : r.metrics->stages) {
            if (s.events == 0 && s.errors == 0) {
                continue;
            }
            out << (first ? "\n" : ",\n") << "    {\"stage\": " << json_text(to_string(s.stage))
                << ", \"events\": " << s.events << ", \"errors\": " << s.errors
                << ", \"p50_ns\": " << nanos(s.latency.p50) << ", \"p99_ns\": " << nanos(s.latency.p99)
                << ", \"max_ns\": " << nanos(s.latency.max) << '}';
            first = false;
        }
    }
    out << (first ? "" : "\n  ") << "]\n}\n";
}

}  // namespace trading_engine::analytics
