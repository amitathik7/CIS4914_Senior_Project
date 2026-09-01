#include "trading_engine/events/event_bus.hpp"

#include "trading_engine/common/errors.hpp"

namespace trading_engine::events {

IEventBus::~IEventBus() = default;

InProcessEventBus::InProcessEventBus() = default;
InProcessEventBus::~InProcessEventBus() = default;

bool InProcessEventBus::publish(Event /*event*/) {
    throw common::NotImplemented("InProcessEventBus::publish");
}

common::SubscriptionId InProcessEventBus::subscribe(EventType /*type*/,
                                                    EventHandler /*handler*/) {
    throw common::NotImplemented("InProcessEventBus::subscribe");
}

void InProcessEventBus::unsubscribe(common::SubscriptionId /*id*/) {
    throw common::NotImplemented("InProcessEventBus::unsubscribe");
}

void InProcessEventBus::start() {
    throw common::NotImplemented("InProcessEventBus::start");
}

void InProcessEventBus::request_shutdown() {
    throw common::NotImplemented("InProcessEventBus::request_shutdown");
}

void InProcessEventBus::wait_until_drained() {
    throw common::NotImplemented("InProcessEventBus::wait_until_drained");
}

std::size_t InProcessEventBus::depth() const {
    throw common::NotImplemented("InProcessEventBus::depth");
}

}  // namespace trading_engine::events
