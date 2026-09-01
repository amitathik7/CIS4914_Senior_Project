#include "trading_engine/persistence/postgres_repository.hpp"

#include <utility>

#include "trading_engine/common/errors.hpp"

// Part of the `trading_engine_postgres_adapter` target. The PostgreSQL client
// library (libpq / libpqxx) must be linked to THAT target only, in
// src/CMakeLists.txt -- never to the core library.

namespace trading_engine::persistence {

PostgresRepository::PostgresRepository(config::PostgresConfig config)
    : config_{std::move(config)} {}

PostgresRepository::~PostgresRepository() = default;

void PostgresRepository::connect() {
    // TODO: build a libpq connection string from config_ (resolving the
    //       password from config_.password_env / url_env), open the pool, and
    //       verify the schema version against database/migrations.
    throw common::NotImplemented(
        "PostgresRepository::connect -- no PostgreSQL client library is linked");
}

void PostgresRepository::disconnect() {
    throw common::NotImplemented("PostgresRepository::disconnect");
}

bool PostgresRepository::is_connected() const noexcept {
    return connected_;
}

void PostgresRepository::store(const domain::MarketEvent& /*event*/) {
    throw common::NotImplemented("PostgresRepository::store(MarketEvent)");
}

void PostgresRepository::store_batch(
    std::span<const domain::MarketEvent> /*events*/) {
    throw common::NotImplemented("PostgresRepository::store_batch");
}

std::vector<domain::MarketEvent> PostgresRepository::load(
    const common::Symbol& /*symbol*/, const TimeRange& /*range*/) {
    throw common::NotImplemented("PostgresRepository::load");
}

void PostgresRepository::record_order(const domain::Order& /*order*/) {
    throw common::NotImplemented("PostgresRepository::record_order");
}

void PostgresRepository::record_fill(const domain::Fill& /*fill*/) {
    throw common::NotImplemented("PostgresRepository::record_fill");
}

void PostgresRepository::record_snapshot(
    const domain::PortfolioSnapshot& /*snapshot*/) {
    throw common::NotImplemented("PostgresRepository::record_snapshot");
}

void PostgresRepository::record_performance(
    const domain::PerformanceReport& /*report*/) {
    throw common::NotImplemented("PostgresRepository::record_performance");
}

std::vector<domain::Order> PostgresRepository::orders_for_run(
    common::RunId /*run*/) {
    throw common::NotImplemented("PostgresRepository::orders_for_run");
}

std::vector<domain::Fill> PostgresRepository::fills_for_run(
    common::RunId /*run*/) {
    throw common::NotImplemented("PostgresRepository::fills_for_run");
}

std::vector<domain::PortfolioSnapshot> PostgresRepository::snapshots_for_run(
    common::RunId /*run*/, const TimeRange& /*range*/) {
    throw common::NotImplemented("PostgresRepository::snapshots_for_run");
}

}  // namespace trading_engine::persistence
