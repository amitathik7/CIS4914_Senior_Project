#include "bar_csv.hpp"

#include <fstream>
#include <map>
#include <optional>
#include <string_view>

#include "trading_engine/common/decimal_text.hpp"
#include "trading_engine/common/timestamp_text.hpp"

namespace trading_engine::cli {

namespace {

std::vector<std::string> split(const std::string& line) {
    std::vector<std::string> cells;
    std::string cell;
    for (const char c : line) {
        if (c == ',') {
            cells.push_back(cell);
            cell.clear();
        } else if (c != '\r') {
            cell.push_back(c);
        }
    }
    cells.push_back(cell);
    return cells;
}

}  // namespace

std::vector<domain::MarketEvent> read_bar_csv(const std::string& path) {
    std::ifstream in{path};
    if (!in) {
        throw CsvError(path + ": cannot open");
    }

    std::string line;
    if (!std::getline(in, line)) {
        throw CsvError(path + ": empty file");
    }
    std::map<std::string, std::size_t> column;
    const std::vector<std::string> header = split(line);
    for (std::size_t i = 0; i < header.size(); ++i) {
        column.emplace(header[i], i);
    }
    for (const char* required : {"symbol", "exchange_time", "price"}) {
        if (column.count(required) == 0) {
            throw CsvError(path + ": header has no '" + required + "' column");
        }
    }

    std::vector<domain::MarketEvent> events;
    std::size_t line_number = 1;
    while (std::getline(in, line)) {
        ++line_number;
        if (line.empty() || line == "\r") {
            continue;
        }
        const std::vector<std::string> cells = split(line);
        const auto where = [&](std::string_view name) {
            return path + ":" + std::to_string(line_number) + ": column '" + std::string(name) + "'";
        };
        const auto cell = [&](const std::string& name) -> std::optional<std::string> {
            const auto it = column.find(name);
            if (it == column.end() || it->second >= cells.size() || cells[it->second].empty()) {
                return std::nullopt;
            }
            return cells[it->second];
        };
        const auto price = [&](const std::string& name) -> std::optional<common::Price> {
            const auto text = cell(name);
            if (!text) {
                return std::nullopt;
            }
            common::Price value{};
            if (common::parse_decimal(*text, value) != common::DecimalStatus::Ok) {
                throw CsvError(where(name) + ": '" + *text + "' is not a decimal");
            }
            return value;
        };

        domain::MarketEvent event;
        event.symbol = cell("symbol").value_or("");
        const auto time = cell("exchange_time");
        const auto parsed = time ? common::parse_timestamp(*time) : std::nullopt;
        if (!parsed) {
            throw CsvError(where("exchange_time") + ": not an RFC 3339 UTC time");
        }
        event.exchange_time = *parsed;

        const std::string type = cell("type").value_or("bar");
        if (type == "bar") {
            event.type = domain::MarketEventType::Bar;
        } else if (type == "trade") {
            event.type = domain::MarketEventType::Trade;
        } else if (type == "quote") {
            event.type = domain::MarketEventType::Quote;
        } else {
            throw CsvError(where("type") + ": '" + type + "' is not bar, trade or quote");
        }

        event.price = price("price");
        event.open  = price("open");
        event.high  = price("high");
        event.low   = price("low");
        if (const auto volume = cell("volume")) {
            common::Quantity q = 0;
            if (common::parse_quantity(*volume, q) != common::DecimalStatus::Ok) {
                throw CsvError(where("volume") + ": '" + *volume + "' is not a whole number");
            }
            event.volume = q;
        }
        events.push_back(std::move(event));
    }
    return events;
}

}  // namespace trading_engine::cli
