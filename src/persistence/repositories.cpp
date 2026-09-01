#include "trading_engine/persistence/market_data_repository.hpp"
#include "trading_engine/persistence/trade_repository.hpp"

// Anchors the vtables for the storage interfaces. Concrete implementations
// (PostgresRepository, and any in-memory test double) live elsewhere. Keeping
// these interfaces here -- in the core library, with no driver headers -- is
// what keeps PostgreSQL out of the core.

namespace trading_engine::persistence {

IMarketDataRepository::~IMarketDataRepository() = default;
ITradeRepository::~ITradeRepository() = default;

}  // namespace trading_engine::persistence
