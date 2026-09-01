#pragma once

// -----------------------------------------------------------------------------
//  ITradeRepository -- storage boundary for simulated trading activity.
//
//  Responsibility: persist the artefacts a run produces -- orders, fills,
//  portfolio snapshots, and the final performance report -- and read them back
//  for analytics and the Python visualization layer. No SQL/driver types here.
//
//  Ownership / lifecycle: created by the composition root; injected into the
//  ExecutionSimulator, PortfolioManager, and PerformanceAnalyzer wiring.
//
//  Thread-safety: implementation-defined; the scaffold makes no guarantees.
// -----------------------------------------------------------------------------

#include <vector>

#include "trading_engine/common/identifiers.hpp"
#include "trading_engine/domain/fill.hpp"
#include "trading_engine/domain/order.hpp"
#include "trading_engine/domain/performance_report.hpp"
#include "trading_engine/domain/portfolio_snapshot.hpp"
#include "trading_engine/persistence/market_data_repository.hpp"  // TimeRange

namespace trading_engine::persistence {

class ITradeRepository {
public:
    virtual ~ITradeRepository();

    // --- writes -----------------------------------------------------
    virtual void record_order(const domain::Order& order) = 0;
    virtual void record_fill(const domain::Fill& fill) = 0;
    virtual void record_snapshot(const domain::PortfolioSnapshot& snapshot) = 0;
    virtual void record_performance(const domain::PerformanceReport& report) = 0;

    // --- reads ------------------------------------------------------
    [[nodiscard]] virtual std::vector<domain::Order> orders_for_run(common::RunId run) = 0;
    [[nodiscard]] virtual std::vector<domain::Fill>  fills_for_run(common::RunId run) = 0;
    [[nodiscard]] virtual std::vector<domain::PortfolioSnapshot> snapshots_for_run(
        common::RunId run, const TimeRange& range) = 0;

    // TODO: a "create run" call returning a RunId and storing run metadata
    //       (mode, config hash, git commit, started/ended at); pagination.
};

}  // namespace trading_engine::persistence
