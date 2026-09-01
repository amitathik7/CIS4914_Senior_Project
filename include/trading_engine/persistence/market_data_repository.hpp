#pragma once

// -----------------------------------------------------------------------------
//  IMarketDataRepository -- storage boundary for market data.
//
//  Responsibility: persist normalised MarketEvents and read them back for
//  backtests and analysis. This interface is intentionally free of any SQL,
//  driver, or connection types -- PostgreSQL knowledge lives only in the
//  concrete adapter (PostgresRepository).
//
//  Ownership / lifecycle: a repository is created by the composition root and
//  injected (as a pointer/reference) into MarketDataService and BacktestReplay.
//
//  Thread-safety: implementations must tolerate calls from the market-data
//  thread and the backtest thread; the scaffold makes no guarantees.
// -----------------------------------------------------------------------------

#include <cstdint>
#include <span>
#include <vector>

#include "trading_engine/common/types.hpp"
#include "trading_engine/domain/market_event.hpp"

namespace trading_engine::persistence {

// Half-open [start, end) time range, UTC.
struct TimeRange {
    common::Timestamp start{};
    common::Timestamp end{};
};

class IMarketDataRepository {
public:
    virtual ~IMarketDataRepository();

    virtual void store(const domain::MarketEvent& event) = 0;
    virtual void store_batch(std::span<const domain::MarketEvent> events) = 0;

    [[nodiscard]] virtual std::vector<domain::MarketEvent> load(
        const common::Symbol& symbol, const TimeRange& range) = 0;

    // Streaming read for large ranges, to avoid loading everything at once.
    // NOT IMPLEMENTED anywhere yet; signature is a placeholder.
    // TODO: replace std::vector return with a pull-based cursor / callback.
};

}  // namespace trading_engine::persistence
