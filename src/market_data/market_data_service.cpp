#include "trading_engine/market_data/market_data_service.hpp"

#include <utility>

#include "trading_engine/common/errors.hpp"

namespace trading_engine::market_data {

MarketDataService::MarketDataService(events::IEventBus& bus,
                                     const common::IClock& clock,
                                     MarketDataPolicy policy,
                                     persistence::IMarketDataRepository* repository)
    : bus_{bus},
      clock_{clock},
      policy_{std::move(policy)},
      repository_{repository} {}

MarketDataService::~MarketDataService() = default;

void MarketDataService::on_market_event(const domain::MarketEvent& /*event*/) {
    // TODO: validate against policy_ (positive prices, ordering, spike filter),
    //       stamp ingest_time from clock_, assign per-symbol sequence, persist
    //       via repository_ when set, then publish an EventType::MarketData
    //       event on bus_. Update SystemMetrics counters.
    throw common::NotImplemented("MarketDataService::on_market_event");
}

MarketDataService::Stats MarketDataService::stats() const {
    throw common::NotImplemented("MarketDataService::stats");
}

}  // namespace trading_engine::market_data
