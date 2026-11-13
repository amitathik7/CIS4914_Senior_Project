#pragma once

// Field readers shared by the Alpaca REST and stream parsers. Private to the
// Alpaca adapter.

#include <charconv>
#include <cmath>
#include <optional>
#include <string>

#include "../events/json.hpp"
#include "trading_engine/common/decimal_text.hpp"
#include "trading_engine/common/types.hpp"

namespace trading_engine::market_data::alpaca_json {

namespace json = events::json;

inline std::optional<double> number_as_double(const std::string& text) {
    double value = 0.0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || end != text.data() + text.size() || !std::isfinite(value)) {
        return std::nullopt;
    }
    return value;
}

// exact for prices with up to six decimals, nearest micro otherwise; nullopt if not a usable number
inline std::optional<common::Price> price(const json::Value& v) {
    if (v.kind != json::Value::Kind::Number) {
        return std::nullopt;
    }
    common::Price out{};
    if (common::parse_decimal(v.text, out) == common::DecimalStatus::Ok) {
        return out;
    }
    if (const auto d = number_as_double(v.text); d && std::fabs(*d) < 9.0e12) {
        return common::Price::from_double(*d);
    }
    return std::nullopt;
}

// whole numbers exactly, fractional ones rounded; nullopt if negative or not a number
inline std::optional<common::Quantity> quantity(const json::Value& v) {
    if (v.kind != json::Value::Kind::Number) {
        return std::nullopt;
    }
    common::Quantity out = 0;
    if (common::parse_quantity(v.text, out) == common::DecimalStatus::Ok) {
        return out;
    }
    if (const auto d = number_as_double(v.text); d && *d >= 0 && *d < 9.0e18) {
        return static_cast<common::Quantity>(std::llround(*d));
    }
    return std::nullopt;
}

}  // namespace trading_engine::market_data::alpaca_json
