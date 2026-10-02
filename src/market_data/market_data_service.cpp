#include "trading_engine/market_data/market_data_service.hpp"

#include <utility>
#include <optional>

#include "trading_engine/common/errors.hpp"

#include "trading_engine/events/event_bus.hpp"

namespace trading_engine::market_data
{
    namespace
    {
        bool invalid_value_check(std::optional<common::Price> value)
        {
            return value.has_value() && *value <= 0.00;
        }
    }

    MarketDataService::MarketDataService(events::IEventBus &bus,
                                         const common::IClock &clock,
                                         MarketDataPolicy policy,
                                         persistence::IMarketDataRepository *repository)
        : bus_{bus},
          clock_{clock},
          policy_{std::move(policy)},
          repository_{repository} {}

    MarketDataService::~MarketDataService() = default;

    void MarketDataService::on_market_event(const domain::MarketEvent &event)
    {
        // TODO: validate against policy_ (positive prices, ordering, spike filter),
        //       stamp ingest_time from clock_, assign per-symbol sequence, persist
        //       via repository_ when set, then publish an EventType::MarketData
        //       event on bus_. Update SystemMetrics counters.

        // Complete validation and then create a new MarketEvent copy copy of input.

        // Pre-validation section:
        // - Checking for valid pricing (> 0)
        // - Symbol is valid (event.symbol does exist)
        // - Valid event type (event.type != MarketEventType::Unknown)
        // If we fail any of these checks we skip the later sections by returning

        // TODO: Add the other full checks. This is the bare minimum for MVP and further testing/development of other components.

        // Policy check
        if (this->policy_.reject_non_positive_prices && (invalid_value_check(event.price) || invalid_value_check(event.bid) || invalid_value_check(event.ask) || invalid_value_check(event.open) || invalid_value_check(event.high) || invalid_value_check(event.low)))
        {
            this->running_counts_.rejected += 1;
            return;
        }

        // General checks (regardless of policy these checks are required)
        if (event.symbol.empty() || event.type == domain::MarketEventType::Unknown)
        {
            this->running_counts_.rejected += 1;
            return;
        }

        // Post-Validation section
        this->running_counts_.accepted += 1;

        domain::MarketEvent mutable_market_event = event;

        mutable_market_event.sequence = ++this->symbol_sequence_counters_[mutable_market_event.symbol];
        mutable_market_event.ingest_time = this->clock_.now();

        events::Event event_wrapper;

        event_wrapper.type = events::EventType::MarketData;
        event_wrapper.payload = mutable_market_event;

        this->bus_.publish(event_wrapper);
    }

    MarketDataService::Stats MarketDataService::stats() const
    {
        return this->running_counts_;
    }
} // namespace trading_engine::market_data
