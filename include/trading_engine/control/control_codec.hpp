#pragma once

// -----------------------------------------------------------------------------
//  Wire format of the command channel messages (control_messages.hpp).
//
//  The event bus envelope (events/event_codec.hpp, schema 1) with a control
//  type, so the gateway reads every topic with one decoder:
//
//    {"schema":1,"origin":"engine-3f2a","sequence":7,"type":"run",
//     "enqueued_at":"...Z","payload":{...}}
//
//  type is "command", "run" or "engine". Payloads follow ADR 0006 section 3
//  (money as exact decimal text, times RFC 3339 UTC, absent optionals
//  omitted); a RunSettings is the console's RunConfig. Field reference:
//  docs/adr/0007-engine-command-channel.md.
//
//  Decoding is strict about what it uses and ignores unknown members, like
//  the event codec. A bad message throws events::DecodeError; a bad setting
//  inside a well-formed command throws SettingsError with its field path, so
//  the engine can say exactly what it refused.
// -----------------------------------------------------------------------------

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>

#include "trading_engine/control/control_messages.hpp"
#include "trading_engine/events/event_codec.hpp"

namespace trading_engine::control {

using ControlPayload = std::variant<Command, RunStatusReport, EngineAnnouncement>;

struct ControlEnvelope {
    std::string       origin{};
    std::uint64_t     sequence{0};
    common::Timestamp enqueued_at{};
    ControlPayload    payload{};
};

[[nodiscard]] std::string encode(const ControlEnvelope& envelope);
[[nodiscard]] ControlEnvelope decode_control(std::string_view text);   // DecodeError / SettingsError

// "command", "run" or "engine": the envelope type, and the topic suffix.
[[nodiscard]] std::string_view type_name(const ControlPayload& payload) noexcept;

// The RunConfig JSON object alone (what the gateway shows as RunDetail.config).
[[nodiscard]] std::string settings_json(const RunSettings& settings);

// Topic names.
[[nodiscard]] std::string command_topic(std::string_view control_prefix);
[[nodiscard]] std::string engine_topic(std::string_view control_prefix);
[[nodiscard]] std::string run_topic(std::string_view run_prefix);

}  // namespace trading_engine::control
