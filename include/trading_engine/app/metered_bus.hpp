#pragma once

// -----------------------------------------------------------------------------
//  MeteredBus -- an IEventBus view that times one stage's handlers.
//
//  Pipeline and OrderFlow hand each component its own view, so every event a
//  component handles is counted under that component's stage in
//  analytics::SystemMetrics, with how long the handler took. Everything else
//  passes straight through to the real bus. With no metrics it adds nothing.
// -----------------------------------------------------------------------------

#include "trading_engine/analytics/system_metrics.hpp"
#include "trading_engine/events/event_bus.hpp"

namespace trading_engine::app {

class MeteredBus final : public events::IEventBus {
public:
    MeteredBus(events::IEventBus& bus, analytics::SystemMetrics* metrics, analytics::Stage stage) noexcept
        : bus_{bus}, metrics_{metrics}, stage_{stage} {}

    bool publish(events::Event event) override { return bus_.publish(std::move(event)); }
    common::SubscriptionId subscribe(events::EventType type, events::EventHandler handler) override;
    void unsubscribe(common::SubscriptionId id) override { bus_.unsubscribe(id); }
    void start() override { bus_.start(); }
    void request_shutdown() override { bus_.request_shutdown(); }
    void wait_until_drained() override { bus_.wait_until_drained(); }
    [[nodiscard]] std::size_t depth() const override { return bus_.depth(); }

private:
    events::IEventBus&        bus_;
    analytics::SystemMetrics* metrics_;
    analytics::Stage          stage_;
};

}  // namespace trading_engine::app
