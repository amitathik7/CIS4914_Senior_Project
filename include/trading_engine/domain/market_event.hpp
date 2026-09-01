#pragma once

// -----------------------------------------------------------------------------
//  MarketEvent -- the canonical, source-agnostic unit of market data.
//
//  Responsibility: represent one observation about an instrument (a trade
//  print, a quote update, a bar, a status change) after it has been validated
//  and normalised by MarketDataService. Strategies and the rest of the
//  pipeline only ever see this type -- never an Alpaca payload.
//
//  This is a plain value type: copyable, movable, no invariants enforced in
//  code yet. Not thread-safe; treat instances as immutable once published.
// -----------------------------------------------------------------------------

#include <cstdint>
#include <optional>
#include <string_view>

#include "trading_engine/common/types.hpp"

namespace trading_engine::domain {

// PROVISIONAL taxonomy. The exact set of event types is an open question
// (see docs/OPEN_QUESTIONS.md -- "exact market event types"). Keep this list
// small until strategies tell us what they actually need.
enum class MarketEventType : std::uint8_t {
    Unknown = 0,
    Trade,        // a print: last price + size
    Quote,        // top-of-book bid/ask update
    Bar,          // aggregated OHLCV over a fixed interval
    Status        // trading halt / resume / auction, etc.
    // TODO: OrderBookDelta, Imbalance, CorporateAction, Reference/Snapshot ...
};

[[nodiscard]] std::string_view to_string(MarketEventType) noexcept;

struct MarketEvent {
    common::Symbol    symbol{};
    common::Timestamp exchange_time{};   // when the venue says it happened (UTC)
    common::Timestamp ingest_time{};     // when the engine received it (UTC)
    MarketEventType   type{MarketEventType::Unknown};

    // Price-related fields. Which ones are populated depends on `type`; all are
    // optional so an absent value is never confused with zero.
    std::optional<common::Price> price{};   // last / close
    std::optional<common::Price> bid{};
    std::optional<common::Price> ask{};
    std::optional<common::Price> open{};
    std::optional<common::Price> high{};
    std::optional<common::Price> low{};

    std::optional<common::Quantity> size{};     // size of this trade/quote level
    std::optional<common::Quantity> volume{};   // cumulative or bar volume

    std::uint64_t sequence{0};   // per-symbol monotonic sequence, gap-detectable

    // TODO: venue/feed id, bid_size/ask_size, bar interval, condition flags,
    //       tick direction, and a "revision" marker for corrected prints.
};

}  // namespace trading_engine::domain
