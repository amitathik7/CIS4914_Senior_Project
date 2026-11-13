#include "trading_engine/app/metered_bus.hpp"

#include <chrono>
#include <utility>

namespace trading_engine::app {

common::SubscriptionId MeteredBus::subscribe(events::EventType type, events::EventHandler handler) {
    if (metrics_ == nullptr || !handler) {
        return bus_.subscribe(type, std::move(handler));
    }
    return bus_.subscribe(type, [handler = std::move(handler), metrics = metrics_,
                                 stage = stage_](const events::Event& event) {
        const auto started = std::chrono::steady_clock::now();
        try {
            handler(event);
        } catch (...) {
            metrics->increment_errors(stage);
            throw;
        }
        metrics->increment_events(stage);
        metrics->record_latency(stage, std::chrono::duration_cast<common::Duration>(
                                           std::chrono::steady_clock::now() - started));
    });
}

}  // namespace trading_engine::app
