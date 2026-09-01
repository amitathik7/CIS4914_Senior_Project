#pragma once

// -----------------------------------------------------------------------------
//  PostgresRepository -- the ONLY place PostgreSQL knowledge is allowed to live.
//
//  Responsibility (future): implement IMarketDataRepository and ITradeRepository
//  on top of PostgreSQL -- connection pooling, prepared statements, batched
//  COPY inserts for market data, transactional writes for trade artefacts.
//
//  STATUS: no database work. Every method throws common::NotImplemented.
//  This class is compiled into the separate `trading_engine_postgres_adapter`
//  target; the eventual libpq / libpqxx dependency must be added THERE only,
//  never in the core library or these headers (which stay driver-free).
//
//  Ownership / lifecycle: created by the composition root from PostgresConfig;
//  owns its connection pool; must be destroyed after every component that holds
//  a repository reference.
//
//  Thread-safety (future): safe for concurrent use via an internal pool.
// -----------------------------------------------------------------------------

#include <span>
#include <vector>

#include "trading_engine/configuration/engine_config.hpp"
#include "trading_engine/persistence/market_data_repository.hpp"
#include "trading_engine/persistence/trade_repository.hpp"

namespace trading_engine::persistence {

class PostgresRepository final : public IMarketDataRepository,
                                 public ITradeRepository {
public:
    explicit PostgresRepository(config::PostgresConfig config);
    ~PostgresRepository() override;

    // Open the pool / verify connectivity / check schema version.
    void connect();      // NOT IMPLEMENTED
    void disconnect();   // NOT IMPLEMENTED
    [[nodiscard]] bool is_connected() const noexcept;

    // --- IMarketDataRepository ------------------------------------
    void store(const domain::MarketEvent& event) override;                    // NOT IMPLEMENTED
    void store_batch(std::span<const domain::MarketEvent> events) override;   // NOT IMPLEMENTED
    [[nodiscard]] std::vector<domain::MarketEvent> load(
        const common::Symbol& symbol, const TimeRange& range) override;       // NOT IMPLEMENTED

    // --- ITradeRepository ---------------------------------------
    void record_order(const domain::Order& order) override;                   // NOT IMPLEMENTED
    void record_fill(const domain::Fill& fill) override;                      // NOT IMPLEMENTED
    void record_snapshot(const domain::PortfolioSnapshot& snapshot) override; // NOT IMPLEMENTED
    void record_performance(const domain::PerformanceReport& report) override;// NOT IMPLEMENTED
    [[nodiscard]] std::vector<domain::Order> orders_for_run(common::RunId run) override;
    [[nodiscard]] std::vector<domain::Fill>  fills_for_run(common::RunId run) override;
    [[nodiscard]] std::vector<domain::PortfolioSnapshot> snapshots_for_run(
        common::RunId run, const TimeRange& range) override;

private:
    [[maybe_unused]] config::PostgresConfig config_;
    bool                                    connected_{false};

    // TODO: connection pool handle, prepared-statement registry, schema-version
    //       check against database/migrations, retry policy, COPY-based bulk
    //       insert path for market data.
};

}  // namespace trading_engine::persistence
