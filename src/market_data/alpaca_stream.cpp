#include "trading_engine/market_data/alpaca_stream.hpp"

#include <utility>

#include "alpaca_json.hpp"
#include "trading_engine/common/timestamp_text.hpp"

namespace trading_engine::market_data {

namespace {

namespace json = events::json;
using Kind = json::Value::Kind;

[[noreturn]] void malformed(const std::string& what) {
    throw AlpacaError(0, "Alpaca stream message: " + what);
}

const json::Value& member(const json::Value& item, std::string_view key, const std::string& where) {
    const json::Value* v = item.find(key);
    if (v == nullptr) {
        malformed(where + " has no '" + std::string(key) + "'");
    }
    return *v;
}

common::Price price_member(const json::Value& item, std::string_view key, const std::string& where) {
    const auto price = alpaca_json::price(member(item, key, where));
    if (!price) {
        malformed(where + " '" + std::string(key) + "' is not a price");
    }
    return *price;
}

common::Quantity quantity_member(const json::Value& item, std::string_view key, const std::string& where) {
    const auto quantity = alpaca_json::quantity(member(item, key, where));
    if (!quantity) {
        malformed(where + " '" + std::string(key) + "' is not a quantity");
    }
    return *quantity;
}

domain::MarketEvent base_event(const json::Value& item, domain::MarketEventType type, const std::string& where) {
    const json::Value& symbol = member(item, "S", where);
    const json::Value& t = member(item, "t", where);
    if (symbol.kind != Kind::String || symbol.text.empty()) {
        malformed(where + " 'S' is not a symbol");
    }
    const auto time = t.kind == Kind::String ? common::parse_timestamp(t.text) : std::nullopt;
    if (!time) {
        malformed(where + " 't' is not an RFC 3339 UTC time");
    }
    domain::MarketEvent event;
    event.symbol = symbol.text;
    event.exchange_time = *time;
    event.type = type;
    return event;
}

std::vector<common::Symbol> symbol_list(const json::Value* list) {
    std::vector<common::Symbol> out;
    if (list != nullptr && list->kind == Kind::Array) {
        for (const auto& s : list->array) {
            if (s.kind == Kind::String) {
                out.push_back(s.text);
            }
        }
    }
    return out;
}

void append_list(std::string& out, std::string_view name, const std::vector<common::Symbol>& symbols) {
    out += ',';
    json::append_string(out, name);
    out += ":[";
    for (std::size_t i = 0; i < symbols.size(); ++i) {
        if (i > 0) {
            out += ',';
        }
        json::append_string(out, symbols[i]);
    }
    out += ']';
}

}  // namespace

std::string alpaca_stream_auth_message(const AlpacaCredentials& credentials) {
    std::string out;
    json::ObjectWriter w{out};
    w.string("action", "auth");
    w.string("key", credentials.key_id);
    w.string("secret", credentials.secret_key);
    w.close();
    return out;
}

std::string alpaca_stream_subscribe_message(const AlpacaStreamChannels& channels) {
    std::string out = R"({"action":"subscribe")";
    append_list(out, "bars", channels.bars);
    append_list(out, "trades", channels.trades);
    append_list(out, "quotes", channels.quotes);
    out += '}';
    return out;
}

AlpacaStreamBatch parse_alpaca_stream_message(std::string_view text) {
    const auto doc = json::parse(text);
    if (!doc || doc->kind != Kind::Array) {
        malformed("not a JSON array");
    }

    AlpacaStreamBatch batch;
    for (std::size_t i = 0; i < doc->array.size(); ++i) {
        const json::Value& item = doc->array[i];
        const std::string where = "item " + std::to_string(i);
        if (item.kind != Kind::Object) {
            malformed(where + " is not an object");
        }
        const json::Value* tag = item.find("T");
        if (tag == nullptr || tag->kind != Kind::String) {
            malformed(where + " has no 'T'");
        }
        const std::string& T = tag->text;

        if (T == "success") {
            const json::Value* msg = item.find("msg");
            batch.successes.push_back(msg != nullptr && msg->kind == Kind::String ? msg->text : "");
        } else if (T == "error") {
            AlpacaStreamError error;
            if (const json::Value* code = item.find("code"); code != nullptr && code->kind == Kind::Number) {
                error.code = static_cast<int>(alpaca_json::quantity(*code).value_or(0));
            }
            if (const json::Value* msg = item.find("msg"); msg != nullptr && msg->kind == Kind::String) {
                error.message = msg->text;
            }
            batch.errors.push_back(std::move(error));
        } else if (T == "subscription") {
            batch.subscription = true;
            batch.subscribed.bars = symbol_list(item.find("bars"));
            batch.subscribed.trades = symbol_list(item.find("trades"));
            batch.subscribed.quotes = symbol_list(item.find("quotes"));
        } else if (T == "b") {
            domain::MarketEvent bar = base_event(item, domain::MarketEventType::Bar, where);
            bar.open = price_member(item, "o", where);
            bar.high = price_member(item, "h", where);
            bar.low = price_member(item, "l", where);
            bar.price = price_member(item, "c", where);
            bar.volume = quantity_member(item, "v", where);
            batch.events.push_back(std::move(bar));
        } else if (T == "t") {
            domain::MarketEvent trade = base_event(item, domain::MarketEventType::Trade, where);
            trade.price = price_member(item, "p", where);
            trade.size = quantity_member(item, "s", where);
            batch.events.push_back(std::move(trade));
        } else if (T == "q") {
            domain::MarketEvent quote = base_event(item, domain::MarketEventType::Quote, where);
            quote.bid = price_member(item, "bp", where);
            quote.ask = price_member(item, "ap", where);
            batch.events.push_back(std::move(quote));
        } else {
            ++batch.ignored;
        }
    }
    return batch;
}

bool alpaca_stream_error_is_fatal(int code) noexcept {
    switch (code) {
        case 400:   // invalid syntax
        case 401:   // not authenticated
        case 402:   // auth failed
        case 405:   // symbol limit exceeded
        case 406:   // connection limit exceeded
        case 409:   // insufficient subscription
        case 410:   // invalid subscribe action
            return true;
        default:
            return false;
    }
}

}  // namespace trading_engine::market_data
