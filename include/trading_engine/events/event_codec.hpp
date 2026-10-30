#pragma once

// -----------------------------------------------------------------------------
//  Event wire format v1 -- how an events::Event crosses a process boundary.
//
//  Used by TopicEventBus for every message it puts on a topic (Kafka in
//  production, InMemoryBroker in tests). One UTF-8 JSON object per message:
//
//    {"schema":1,"origin":"9f2c...","sequence":42,"type":"market_data",
//     "enqueued_at":"2026-01-05T14:30:00.000000000Z","payload":{...}}
//
//  Encoding rules follow docs/adr/0006-web-frontend-api-contract.md section 3,
//  so the read-model gateway can consume topics without a translation layer:
//   * Money and prices are exact decimal strings ("150.02"), never numbers.
//   * Quantities and ids are JSON integers.
//   * Times are RFC 3339 UTC with nine fractional digits and a 'Z'.
//   * Absent optional members are omitted, never null.
//   * Enums use the to_string() spellings in src/domain/domain.cpp.
//  Field-by-field reference: docs/EVENT_BUS.md, "Wire format".
//
//  decode() is strict: a missing required member, a wrong JSON type, an
//  unknown enum spelling, an unparseable decimal/time or a schema other than
//  kWireSchemaVersion throws DecodeError. Unknown extra members are ignored so
//  a newer producer can add optional fields without breaking older consumers.
// -----------------------------------------------------------------------------

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

#include "trading_engine/events/event_bus.hpp"

namespace trading_engine::events {

inline constexpr int kWireSchemaVersion = 1;

class DecodeError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// What travels on the wire: the Event plus the id of the bus instance that
// published it, so a bus can tell its own messages from other processes'.
struct WireEnvelope {
    std::string origin{};
    Event event{};
};

[[nodiscard]] std::string encode(const WireEnvelope& envelope);
[[nodiscard]] WireEnvelope decode(std::string_view text);   // throws DecodeError

// Stable lower_snake_case name of an EventType ("market_data", "signal",
// "order", "fill"); also the topic suffix TopicEventBus uses.
[[nodiscard]] std::string_view to_string(EventType type) noexcept;

// The key a transport should partition by: the payload's symbol, so all
// events for one symbol stay in order on one partition.
[[nodiscard]] std::string partition_key(const Event& event);

}  // namespace trading_engine::events
