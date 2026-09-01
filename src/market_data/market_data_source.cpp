#include "trading_engine/market_data/market_data_source.hpp"

// Anchors the vtables for the two market-data interfaces in one translation
// unit. No behaviour -- concrete sources/sinks live elsewhere.

namespace trading_engine::market_data {

IMarketEventSink::~IMarketEventSink() = default;
IMarketDataSource::~IMarketDataSource() = default;

}  // namespace trading_engine::market_data
