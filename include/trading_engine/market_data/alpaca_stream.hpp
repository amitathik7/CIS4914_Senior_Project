#pragma once

// Alpaca market data stream protocol (v2): the messages the client sends and
// a parser for what the server sends. Every server frame is a JSON array of
// objects tagged by "T": "success", "error", "subscription", "b" (minute
// bar), "t" (trade), "q" (quote), and others this engine ignores.

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "trading_engine/common/types.hpp"
#include "trading_engine/domain/market_event.hpp"
#include "trading_engine/market_data/alpaca_historical_source.hpp"

namespace trading_engine::market_data {

struct AlpacaStreamChannels {
    std::vector<common::Symbol> bars;
    std::vector<common::Symbol> trades;
    std::vector<common::Symbol> quotes;
};

struct AlpacaStreamError {
    int code{0};
    std::string message;
};

struct AlpacaStreamBatch {
    std::vector<domain::MarketEvent> events;   // bars, trades and quotes, in frame order
    std::vector<std::string> successes;        // "connected", "authenticated"
    std::vector<AlpacaStreamError> errors;
    bool subscription{false};                  // a subscription acknowledgement arrived
    AlpacaStreamChannels subscribed;           // ... and what it lists
    std::size_t ignored{0};                    // updated/daily bars, statuses, LULD, corrections
};

[[nodiscard]] std::string alpaca_stream_auth_message(const AlpacaCredentials& credentials);
[[nodiscard]] std::string alpaca_stream_subscribe_message(const AlpacaStreamChannels& channels);

// Throws AlpacaError when the frame is not a JSON array of objects, or a
// bar, trade or quote is missing a field it needs.
[[nodiscard]] AlpacaStreamBatch parse_alpaca_stream_message(std::string_view text);

// Errors reconnecting cannot fix: bad request or subscription, bad or missing
// credentials, symbol or connection limits, a plan without the feed.
[[nodiscard]] bool alpaca_stream_error_is_fatal(int code) noexcept;

}  // namespace trading_engine::market_data
