#include "trading_engine/events/event_codec.hpp"

#include <charconv>
#include <cmath>
#include <optional>
#include <type_traits>
#include <utility>

#include "json.hpp"
#include "trading_engine/common/decimal_text.hpp"
#include "trading_engine/common/timestamp_text.hpp"

namespace trading_engine::events {

std::string_view to_string(EventType type) noexcept {
    switch (type) {
        case EventType::MarketData: return "market_data";
        case EventType::Signal:     return "signal";
        case EventType::Order:      return "order";
        case EventType::Fill:       return "fill";
        case EventType::RiskDecision: return "risk_decision";
    }
    return "invalid";
}

std::string partition_key(const Event& event) {
    return std::visit([](const auto& payload) { return payload.symbol; }, event.payload);
}

namespace {

using json::ObjectWriter;
using json::Value;

// --- enum spellings -----------------------------------------------------------
// The forward direction is domain::to_string(); the reverse is a linear scan
// over the enumerators, so the two can never disagree.

template <class Enum, std::size_t N>
Enum enum_from(std::string_view text, const Enum (&all)[N], std::string_view field) {
    for (const Enum value : all) {
        if (domain::to_string(value) == text) {
            return value;
        }
    }
    throw DecodeError("event codec: unknown " + std::string(field) + " '" + std::string(text) + "'");
}

constexpr domain::MarketEventType kMarketEventTypes[] = {
    domain::MarketEventType::Unknown, domain::MarketEventType::Trade,
    domain::MarketEventType::Quote, domain::MarketEventType::Bar,
    domain::MarketEventType::Status};
constexpr domain::SignalSide kSignalSides[] = {domain::SignalSide::Buy, domain::SignalSide::Sell,
                                               domain::SignalSide::Flat};
constexpr domain::OrderSide kOrderSides[] = {domain::OrderSide::Buy, domain::OrderSide::Sell};
constexpr domain::OrderType kOrderTypes[] = {domain::OrderType::Market, domain::OrderType::Limit};
constexpr domain::OrderStatus kOrderStatuses[] = {
    domain::OrderStatus::New,       domain::OrderStatus::PendingRisk,
    domain::OrderStatus::Rejected,  domain::OrderStatus::Approved,
    domain::OrderStatus::Working,   domain::OrderStatus::PartiallyFilled,
    domain::OrderStatus::Filled,    domain::OrderStatus::Cancelled,
    domain::OrderStatus::Expired};
constexpr domain::FillStatus kFillStatuses[] = {domain::FillStatus::Filled,
                                                domain::FillStatus::PartiallyFilled};
constexpr domain::RiskOutcome kRiskOutcomes[] = {domain::RiskOutcome::Approved,
                                                 domain::RiskOutcome::Resized,
                                                 domain::RiskOutcome::Rejected};

EventType event_type_from(std::string_view text) {
    for (const EventType type : {EventType::MarketData, EventType::Signal, EventType::Order,
                                 EventType::Fill, EventType::RiskDecision}) {
        if (to_string(type) == text) {
            return type;
        }
    }
    throw DecodeError("event codec: unknown event type '" + std::string(text) + "'");
}

// --- writing ------------------------------------------------------------------

std::string decimal_text(common::Decimal value) { return common::format_decimal(value); }

void write_time(ObjectWriter& w, std::string_view name, common::Timestamp t) {
    w.string(name, common::format_timestamp(t));
}

void write_price(ObjectWriter& w, std::string_view name, const std::optional<common::Price>& p) {
    if (p) {
        w.string(name, decimal_text(*p));
    }
}

void write_quantity(ObjectWriter& w, std::string_view name,
                    const std::optional<common::Quantity>& q) {
    if (q) {
        w.integer(name, *q);
    }
}

std::string payload_json(const domain::MarketEvent& e) {
    std::string out;
    ObjectWriter w{out};
    w.string("symbol", e.symbol);
    write_time(w, "exchange_time", e.exchange_time);
    write_time(w, "ingest_time", e.ingest_time);
    w.string("type", domain::to_string(e.type));
    write_price(w, "price", e.price);
    write_price(w, "bid", e.bid);
    write_price(w, "ask", e.ask);
    write_price(w, "open", e.open);
    write_price(w, "high", e.high);
    write_price(w, "low", e.low);
    write_quantity(w, "size", e.size);
    write_quantity(w, "volume", e.volume);
    w.unsigned_integer("sequence", e.sequence);
    w.close();
    return out;
}

std::string payload_json(const domain::TradeSignal& s) {
    std::string out;
    ObjectWriter w{out};
    w.unsigned_integer("id", s.id.value);
    w.string("strategy_id", s.strategy_id);
    w.string("symbol", s.symbol);
    w.string("side", domain::to_string(s.side));
    write_time(w, "created_at", s.created_at);
    write_quantity(w, "requested_quantity", s.requested_quantity);
    if (s.target_exposure) {
        w.number("target_exposure", *s.target_exposure);
    }
    if (s.confidence) {
        w.number("confidence", *s.confidence);
    }
    std::string metadata;
    ObjectWriter m{metadata};
    for (const auto& [key, value] : s.metadata) {
        m.string(key, value);
    }
    m.close();
    w.raw("metadata", metadata);
    if (s.order_type) {
        w.string("order_type", domain::to_string(*s.order_type));
    }
    write_price(w, "limit_price", s.limit_price);
    w.close();
    return out;
}

std::string payload_json(const domain::Order& o) {
    std::string out;
    ObjectWriter w{out};
    w.unsigned_integer("id", o.id.value);
    w.unsigned_integer("run_id", o.run_id.value);
    w.unsigned_integer("origin_signal", o.origin_signal.value);
    w.string("strategy_id", o.strategy_id);
    w.string("symbol", o.symbol);
    w.string("side", domain::to_string(o.side));
    w.string("type", domain::to_string(o.type));
    w.integer("quantity", o.quantity);
    write_price(w, "limit_price", o.limit_price);
    w.string("status", domain::to_string(o.status));
    write_time(w, "created_at", o.created_at);
    write_time(w, "updated_at", o.updated_at);
    w.integer("filled_quantity", o.filled_quantity);
    write_price(w, "average_fill_price", o.average_fill_price);
    if (o.reject_reason) {
        w.string("reject_reason", *o.reject_reason);
    }
    w.close();
    return out;
}

std::string payload_json(const domain::Fill& f) {
    std::string out;
    ObjectWriter w{out};
    w.unsigned_integer("id", f.id.value);
    w.unsigned_integer("order_id", f.order_id.value);
    w.unsigned_integer("run_id", f.run_id.value);
    w.string("symbol", f.symbol);
    w.integer("filled_quantity", f.filled_quantity);
    w.string("fill_price", decimal_text(f.fill_price));
    w.string("fees", decimal_text(f.fees));
    w.string("slippage", decimal_text(f.slippage));
    write_time(w, "filled_at", f.filled_at);
    w.string("status", domain::to_string(f.status));
    w.unsigned_integer("sequence", f.sequence);
    w.close();
    return out;
}

std::string payload_json(const domain::RiskDecisionReport& r) {
    std::string checks = "[";
    for (const auto& c : r.checks) {
        if (checks.size() > 1) {
            checks += ',';
        }
        ObjectWriter w{checks};
        w.string("policy", c.policy);
        w.raw("passed", c.passed ? "true" : "false");
        w.string("detail", c.detail);
        if (c.limit) {
            w.string("limit", *c.limit);
        }
        if (c.observed) {
            w.string("observed", *c.observed);
        }
        w.close();
    }
    checks += ']';

    std::string out;
    ObjectWriter w{out};
    w.unsigned_integer("id", r.id);
    w.unsigned_integer("run_id", r.run_id.value);
    w.unsigned_integer("signal_id", r.signal_id.value);
    write_time(w, "decided_at", r.decided_at);
    w.string("symbol", r.symbol);
    w.string("side", domain::to_string(r.side));
    w.string("strategy_id", r.strategy_id);
    w.string("outcome", domain::to_string(r.outcome));
    w.integer("requested_quantity", r.requested_quantity);
    w.integer("approved_quantity", r.approved_quantity);
    if (r.reason) {
        w.string("reason", *r.reason);
    }
    w.raw("checks", checks);
    if (r.order_id) {
        w.unsigned_integer("order_id", r.order_id->value);
    }
    w.close();
    return out;
}

// --- reading ------------------------------------------------------------------

[[noreturn]] void fail(std::string_view field, std::string_view problem) {
    throw DecodeError("event codec: '" + std::string(field) + "' " + std::string(problem));
}

const Value& require(const Value& object, std::string_view field) {
    const Value* v = object.find(field);
    if (v == nullptr) {
        fail(field, "is missing");
    }
    return *v;
}

const std::string& string_of(const Value& v, std::string_view field) {
    if (v.kind != Value::Kind::String) {
        fail(field, "must be a string");
    }
    return v.text;
}

template <class Int>
Int integer_of(const Value& v, std::string_view field) {
    if (v.kind != Value::Kind::Number) {
        fail(field, "must be an integer");
    }
    Int out{};
    const char* begin = v.text.data();
    const char* end   = begin + v.text.size();
    const auto result = std::from_chars(begin, end, out);
    if (result.ec != std::errc{} || result.ptr != end) {
        fail(field, "must be an integer in range");
    }
    return out;
}

double number_of(const Value& v, std::string_view field) {
    if (v.kind != Value::Kind::Number) {
        fail(field, "must be a number");
    }
    double out = 0.0;
    const char* begin = v.text.data();
    const char* end   = begin + v.text.size();
    const auto result = std::from_chars(begin, end, out);
    if (result.ec != std::errc{} || result.ptr != end || !std::isfinite(out)) {
        fail(field, "must be a finite number");
    }
    return out;
}

common::Decimal decimal_of(const Value& v, std::string_view field) {
    std::string_view text = string_of(v, field);
    const bool negative = !text.empty() && text.front() == '-';
    if (negative) {
        text.remove_prefix(1);
    }
    common::Decimal out{};
    if (common::parse_decimal(text, out) != common::DecimalStatus::Ok) {
        fail(field, "must be an exact decimal string");
    }
    return negative ? -out : out;
}

common::Timestamp time_of(const Value& v, std::string_view field) {
    const auto parsed = common::parse_timestamp(string_of(v, field));
    if (!parsed) {
        fail(field, "must be an RFC 3339 UTC time");
    }
    return *parsed;
}

std::string str(const Value& o, std::string_view f) { return string_of(require(o, f), f); }
common::Timestamp time_field(const Value& o, std::string_view f) { return time_of(require(o, f), f); }
common::Decimal dec(const Value& o, std::string_view f) { return decimal_of(require(o, f), f); }
template <class Int>
Int integer(const Value& o, std::string_view f) {
    return integer_of<Int>(require(o, f), f);
}
template <class Id>
Id id(const Value& o, std::string_view f) {
    return Id{integer<std::uint64_t>(o, f)};
}

std::optional<common::Price> opt_price(const Value& o, std::string_view f) {
    const Value* v = o.find(f);
    return v ? std::optional<common::Price>{decimal_of(*v, f)} : std::nullopt;
}
std::optional<common::Quantity> opt_quantity(const Value& o, std::string_view f) {
    const Value* v = o.find(f);
    return v ? std::optional<common::Quantity>{integer_of<common::Quantity>(*v, f)} : std::nullopt;
}
std::optional<double> opt_number(const Value& o, std::string_view f) {
    const Value* v = o.find(f);
    return v ? std::optional<double>{number_of(*v, f)} : std::nullopt;
}

domain::MarketEvent market_event_from(const Value& p) {
    domain::MarketEvent e;
    e.symbol        = str(p, "symbol");
    e.exchange_time = time_field(p, "exchange_time");
    e.ingest_time   = time_field(p, "ingest_time");
    e.type          = enum_from(str(p, "type"), kMarketEventTypes, "market event type");
    e.price         = opt_price(p, "price");
    e.bid           = opt_price(p, "bid");
    e.ask           = opt_price(p, "ask");
    e.open          = opt_price(p, "open");
    e.high          = opt_price(p, "high");
    e.low           = opt_price(p, "low");
    e.size          = opt_quantity(p, "size");
    e.volume        = opt_quantity(p, "volume");
    e.sequence      = integer<std::uint64_t>(p, "sequence");
    return e;
}

domain::TradeSignal signal_from(const Value& p) {
    domain::TradeSignal s;
    s.id                 = id<common::SignalId>(p, "id");
    s.strategy_id        = str(p, "strategy_id");
    s.symbol             = str(p, "symbol");
    s.side               = enum_from(str(p, "side"), kSignalSides, "signal side");
    s.created_at         = time_field(p, "created_at");
    s.requested_quantity = opt_quantity(p, "requested_quantity");
    s.target_exposure    = opt_number(p, "target_exposure");
    s.confidence         = opt_number(p, "confidence");
    if (const Value* metadata = p.find("metadata")) {
        if (metadata->kind != Value::Kind::Object) {
            fail("metadata", "must be an object");
        }
        for (const auto& [key, value] : metadata->object) {
            s.metadata.emplace(key, string_of(value, "metadata." + key));
        }
    }
    if (const Value* order_type = p.find("order_type")) {
        s.order_type = enum_from(string_of(*order_type, "order_type"), kOrderTypes, "order type");
    }
    s.limit_price = opt_price(p, "limit_price");
    return s;
}

domain::Order order_from(const Value& p) {
    domain::Order o;
    o.id                 = id<common::OrderId>(p, "id");
    o.run_id             = id<common::RunId>(p, "run_id");
    o.origin_signal      = id<common::SignalId>(p, "origin_signal");
    o.strategy_id        = str(p, "strategy_id");
    o.symbol             = str(p, "symbol");
    o.side               = enum_from(str(p, "side"), kOrderSides, "order side");
    o.type               = enum_from(str(p, "type"), kOrderTypes, "order type");
    o.quantity           = integer<common::Quantity>(p, "quantity");
    o.limit_price        = opt_price(p, "limit_price");
    o.status             = enum_from(str(p, "status"), kOrderStatuses, "order status");
    o.created_at         = time_field(p, "created_at");
    o.updated_at         = time_field(p, "updated_at");
    o.filled_quantity    = integer<common::Quantity>(p, "filled_quantity");
    o.average_fill_price = opt_price(p, "average_fill_price");
    if (const Value* reason = p.find("reject_reason")) {
        o.reject_reason = string_of(*reason, "reject_reason");
    }
    return o;
}

domain::Fill fill_from(const Value& p) {
    domain::Fill f;
    f.id              = id<common::FillId>(p, "id");
    f.order_id        = id<common::OrderId>(p, "order_id");
    f.run_id          = id<common::RunId>(p, "run_id");
    f.symbol          = str(p, "symbol");
    f.filled_quantity = integer<common::Quantity>(p, "filled_quantity");
    f.fill_price      = dec(p, "fill_price");
    f.fees            = dec(p, "fees");
    f.slippage        = dec(p, "slippage");
    f.filled_at       = time_field(p, "filled_at");
    f.status          = enum_from(str(p, "status"), kFillStatuses, "fill status");
    f.sequence        = integer<std::uint32_t>(p, "sequence");
    return f;
}

domain::RiskDecisionReport risk_decision_from(const Value& p) {
    domain::RiskDecisionReport r;
    r.id                 = integer<std::uint64_t>(p, "id");
    r.run_id             = id<common::RunId>(p, "run_id");
    r.signal_id          = id<common::SignalId>(p, "signal_id");
    r.decided_at         = time_field(p, "decided_at");
    r.symbol             = str(p, "symbol");
    r.side               = enum_from(str(p, "side"), kSignalSides, "signal side");
    r.strategy_id        = str(p, "strategy_id");
    r.outcome            = enum_from(str(p, "outcome"), kRiskOutcomes, "risk outcome");
    r.requested_quantity = integer<common::Quantity>(p, "requested_quantity");
    r.approved_quantity  = integer<common::Quantity>(p, "approved_quantity");
    if (const Value* reason = p.find("reason")) {
        r.reason = string_of(*reason, "reason");
    }
    const Value& checks = require(p, "checks");
    if (checks.kind != Value::Kind::Array) {
        fail("checks", "must be an array");
    }
    for (const Value& c : checks.array) {
        if (c.kind != Value::Kind::Object) {
            fail("checks", "must hold objects");
        }
        domain::RiskCheck check;
        check.policy = str(c, "policy");
        const Value& passed = require(c, "passed");
        if (passed.kind != Value::Kind::Bool) {
            fail("passed", "must be true or false");
        }
        check.passed = passed.boolean;
        check.detail = str(c, "detail");
        if (const Value* limit = c.find("limit")) {
            check.limit = string_of(*limit, "limit");
        }
        if (const Value* observed = c.find("observed")) {
            check.observed = string_of(*observed, "observed");
        }
        r.checks.push_back(std::move(check));
    }
    if (p.find("order_id") != nullptr) {
        r.order_id = id<common::OrderId>(p, "order_id");
    }
    return r;
}

// The payload alternative that belongs to each EventType. Event::type and the
// variant are set independently by publishers, so encode() checks they agree.
bool payload_matches(const Event& event) {
    switch (event.type) {
        case EventType::MarketData: return std::holds_alternative<domain::MarketEvent>(event.payload);
        case EventType::Signal:     return std::holds_alternative<domain::TradeSignal>(event.payload);
        case EventType::Order:      return std::holds_alternative<domain::Order>(event.payload);
        case EventType::Fill:       return std::holds_alternative<domain::Fill>(event.payload);
        case EventType::RiskDecision:
            return std::holds_alternative<domain::RiskDecisionReport>(event.payload);
    }
    return false;
}

}  // namespace

std::string encode(const WireEnvelope& envelope) {
    const Event& event = envelope.event;
    if (!payload_matches(event)) {
        throw std::invalid_argument("event codec: Event::type does not match its payload");
    }
    std::string out;
    ObjectWriter w{out};
    w.integer("schema", kWireSchemaVersion);
    w.string("origin", envelope.origin);
    w.unsigned_integer("sequence", event.sequence);
    w.string("type", to_string(event.type));
    write_time(w, "enqueued_at", event.enqueued_at);
    w.raw("payload", std::visit([](const auto& p) { return payload_json(p); }, event.payload));
    w.close();
    return out;
}

WireEnvelope decode(std::string_view text) {
    const std::optional<Value> root = json::parse(text);
    if (!root || root->kind != Value::Kind::Object) {
        throw DecodeError("event codec: message is not a JSON object");
    }
    const auto schema = integer<int>(*root, "schema");
    if (schema != kWireSchemaVersion) {
        throw DecodeError("event codec: unsupported schema " + std::to_string(schema));
    }

    WireEnvelope envelope;
    envelope.origin               = str(*root, "origin");
    envelope.event.sequence       = integer<std::uint64_t>(*root, "sequence");
    envelope.event.type           = event_type_from(str(*root, "type"));
    envelope.event.enqueued_at    = time_field(*root, "enqueued_at");

    const Value& payload = require(*root, "payload");
    if (payload.kind != Value::Kind::Object) {
        fail("payload", "must be an object");
    }
    switch (envelope.event.type) {
        case EventType::MarketData: envelope.event.payload = market_event_from(payload); break;
        case EventType::Signal:     envelope.event.payload = signal_from(payload); break;
        case EventType::Order:      envelope.event.payload = order_from(payload); break;
        case EventType::Fill:       envelope.event.payload = fill_from(payload); break;
        case EventType::RiskDecision: envelope.event.payload = risk_decision_from(payload); break;
    }
    return envelope;
}

}  // namespace trading_engine::events
