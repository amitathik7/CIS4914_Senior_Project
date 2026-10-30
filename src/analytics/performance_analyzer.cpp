#include "trading_engine/analytics/performance_analyzer.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

#include "trading_engine/common/errors.hpp"

namespace trading_engine::analytics {

PerformanceAnalyzer::PerformanceAnalyzer(AnalysisOptions options)
    : options_{options} {}

namespace {

struct Lot {
    common::Quantity quantity{0};   // signed
    common::Price    average_cost{};
    common::Money    open_fees{};   // fees paid opening the shares still held
};

}  // namespace

domain::PerformanceReport PerformanceAnalyzer::analyze(
    std::span<const domain::PortfolioSnapshot> equity_curve,
    std::span<const domain::Fill> fills) const {
    if (equity_curve.empty()) {
        throw common::ValidationError("PerformanceAnalyzer::analyze: the equity curve is empty");
    }
    domain::PerformanceReport report;
    report.run_id          = equity_curve.front().run_id;
    report.period_start    = equity_curve.front().as_of;
    report.period_end      = equity_curve.back().as_of;
    report.generated_at    = report.period_end;   // deterministic: no wall clock here
    report.starting_equity = equity_curve.front().total_equity;
    report.ending_equity   = equity_curve.back().total_equity;

    // --- equity-curve metrics ------------------------------------------------
    const double start = report.starting_equity.to_double();
    if (start > 0.0) {
        report.total_return = report.ending_equity.to_double() / start - 1.0;
    }
    std::vector<double> returns;
    double peak = 0.0;
    for (std::size_t i = 0; i < equity_curve.size(); ++i) {
        const double e = equity_curve[i].total_equity.to_double();
        peak = std::max(peak, e);
        if (peak > 0.0) {
            report.max_drawdown = std::min(report.max_drawdown, e / peak - 1.0);
        }
        if (i > 0) {
            const double prev = equity_curve[i - 1].total_equity.to_double();
            if (prev > 0.0) {
                returns.push_back(e / prev - 1.0);
            }
        }
    }
    if (returns.size() >= 2) {
        double mean = 0.0;
        for (const double r : returns) {
            mean += r;
        }
        mean /= static_cast<double>(returns.size());
        double var = 0.0;
        for (const double r : returns) {
            var += (r - mean) * (r - mean);
        }
        var /= static_cast<double>(returns.size() - 1);
        const double sd    = std::sqrt(var);
        const double scale = std::sqrt(static_cast<double>(options_.periods_per_year));
        report.volatility  = sd * scale;
        if (returns.size() >= 3 && sd > 0.0) {
            const double rf = options_.risk_free_rate / static_cast<double>(options_.periods_per_year);
            report.sharpe   = (mean - rf) / sd * scale;
        }
    }

    // --- trade metrics: replay the fills with average cost -------------------
    std::map<common::Symbol, Lot> lots;
    for (const auto& fill : fills) {
        ++report.fill_count;
        report.fees_paid += fill.fees;
        Lot& lot = lots[fill.symbol];
        const common::Quantity q = fill.filled_quantity;
        if (q == 0) {
            continue;
        }
        const bool closing = lot.quantity != 0 && ((lot.quantity > 0) != (q > 0));
        if (!closing) {
            const common::Quantity total = lot.quantity + q;
            lot.average_cost = (lot.average_cost * lot.quantity + fill.fill_price * q) / total;
            lot.quantity     = total;
            lot.open_fees += fill.fees;
            continue;
        }
        const common::Quantity held   = lot.quantity < 0 ? -lot.quantity : lot.quantity;
        const common::Quantity moving = q < 0 ? -q : q;
        const common::Quantity closed = std::min(held, moving);
        const common::Quantity sign   = lot.quantity > 0 ? 1 : -1;
        // The closing fill's fees, plus the opening fees of the shares it closes.
        const common::Money opening = lot.open_fees * closed / held;
        lot.open_fees -= opening;
        const common::Money pnl = (fill.fill_price - lot.average_cost) * (closed * sign) - fill.fees - opening;
        ++report.trade_count;
        if (pnl > common::Money{}) {
            ++report.winning_trades;
            report.gross_profit += pnl;
        } else if (pnl < common::Money{}) {
            ++report.losing_trades;
            report.gross_loss += -pnl;
        }
        if (moving > held) {
            // Flipped through flat: the remainder opens a new lot at the fill price.
            lot.quantity     = (q > 0 ? 1 : -1) * (moving - held);
            lot.average_cost = fill.fill_price;
            lot.open_fees    = common::Money{};
        } else {
            lot.quantity += q;
            if (lot.quantity == 0) {
                lot.average_cost = common::Price{};
                lot.open_fees    = common::Money{};
            }
        }
    }
    if (report.trade_count > 0) {
        report.win_rate = static_cast<double>(report.winning_trades) /
                          static_cast<double>(report.trade_count);
    }
    if (report.losing_trades > 0 && report.gross_loss > common::Money{}) {
        report.profit_factor = report.gross_profit.to_double() / report.gross_loss.to_double();
    }
    return report;
}

}  // namespace trading_engine::analytics
