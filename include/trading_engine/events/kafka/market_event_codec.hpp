#pragma once

#include <string>
#include <string_view>

#include "trading_engine/domain/market_event.hpp"

namespace trading_engine::events::kafka {

[[nodiscard]] std::string serialize_market_event(
    const domain::MarketEvent& event);

[[nodiscard]] domain::MarketEvent deserialize_market_event(
    std::string_view json);

} // namespace trading_engine::events::kafka