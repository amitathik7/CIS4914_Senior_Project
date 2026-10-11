#include "trading_engine/events/kafka/market_event_codec.hpp"

#include <cstdint>
#include <string>

#include <nlohmann/json.hpp>

#include "trading_engine/common/errors.hpp"

namespace trading_engine::events::kafka {

namespace {

using json = nlohmann::json;

constexpr int kSchemaVersion = 1;

domain::MarketEventType parse_event_type(std::string_view type)
{
    if (type == "trade") {
        return domain::MarketEventType::Trade;
    }
    if (type == "quote") {
        return domain::MarketEventType::Quote;
    }
    if (type == "bar") {
        return domain::MarketEventType::Bar;
    }
    if (type == "status") {
        return domain::MarketEventType::Status;
    }
    if (type == "unknown") {
        return domain::MarketEventType::Unknown;
    }

    throw common::ValidationError("unknown MarketEvent type");
}

void add_price(
    json& output,
    const char* name,
    const std::optional<common::Price>& value)
{
    if (value.has_value()) {
        output[name] = value->micros();
    }
}

void add_quantity(
    json& output,
    const char* name,
    const std::optional<common::Quantity>& value)
{
    if (value.has_value()) {
        output[name] = *value;
    }
}

std::optional<common::Price> read_price(
    const json& input,
    const char* name)
{
    if (!input.contains(name)) {
        return std::nullopt;
    }

    return common::Price::from_micros(
        input.at(name).get<std::int64_t>());
}

std::optional<common::Quantity> read_quantity(
    const json& input,
    const char* name)
{
    if (!input.contains(name)) {
        return std::nullopt;
    }

    return input.at(name).get<common::Quantity>();
}

} // namespace

std::string serialize_market_event(const domain::MarketEvent& event)
{
    json output{
        {"schema_version", kSchemaVersion},
        {"type", domain::to_string(event.type)},
        {"symbol", event.symbol},
        {"exchange_time_ns", event.exchange_time.time_since_epoch().count()},
        {"ingest_time_ns", event.ingest_time.time_since_epoch().count()},
        {"sequence", event.sequence}
    };

    add_price(output, "price_micros", event.price);
    add_price(output, "bid_micros", event.bid);
    add_price(output, "ask_micros", event.ask);
    add_price(output, "open_micros", event.open);
    add_price(output, "high_micros", event.high);
    add_price(output, "low_micros", event.low);

    add_quantity(output, "size", event.size);
    add_quantity(output, "bid_size", event.bid_size);
    add_quantity(output, "ask_size", event.ask_size);
    add_quantity(output, "volume", event.volume);

    if (event.bar_interval.has_value()) {
        output["bar_interval_ns"] = event.bar_interval->count();
    }

    return output.dump();
}

domain::MarketEvent deserialize_market_event(std::string_view json_text)
{
    try {
        const json input = json::parse(json_text.begin(), json_text.end());

        if (!input.is_object()) {
            throw common::ValidationError(
                "MarketEvent JSON must be an object");
        }

        if (input.at("schema_version").get<int>() != kSchemaVersion) {
            throw common::ValidationError(
                "unsupported MarketEvent schema version");
        }

        domain::MarketEvent event{};

        event.type = parse_event_type(
            input.at("type").get<std::string>());

        event.symbol = input.at("symbol").get<std::string>();

        event.exchange_time = common::Timestamp{
            common::Duration{
                input.at("exchange_time_ns").get<std::int64_t>()}};

        event.ingest_time = common::Timestamp{
            common::Duration{
                input.at("ingest_time_ns").get<std::int64_t>()}};

        event.sequence = input.at("sequence").get<std::uint64_t>();

        event.price = read_price(input, "price_micros");
        event.bid = read_price(input, "bid_micros");
        event.ask = read_price(input, "ask_micros");
        event.open = read_price(input, "open_micros");
        event.high = read_price(input, "high_micros");
        event.low = read_price(input, "low_micros");

        event.size = read_quantity(input, "size");
        event.bid_size = read_quantity(input, "bid_size");
        event.ask_size = read_quantity(input, "ask_size");
        event.volume = read_quantity(input, "volume");

        if (input.contains("bar_interval_ns")) {
            event.bar_interval = common::Duration{
                input.at("bar_interval_ns").get<std::int64_t>()};
        }

        return event;
    }
    catch (const nlohmann::json::exception& error) {
        throw common::ValidationError(
            std::string{"invalid MarketEvent JSON: "} + error.what());
    }
}

} // namespace trading_engine::events::kafka