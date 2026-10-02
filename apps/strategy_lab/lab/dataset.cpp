#include "lab/dataset.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>

#include "lab/errors.hpp"
#include "lab/numbers.hpp"
#include "lab/paths.hpp"
#include "lab/sha256.hpp"
#include "lab/timestamp.hpp"
#include "lab/utf8.hpp"

namespace trading_engine::lab {

namespace {

// Column positions in the authoritative table below.
enum Col : std::size_t { kSymbol, kTime, kType, kPrice, kOpen, kHigh, kLow, kVolume, kColumnCount };

constexpr std::size_t kMaxProblems  = 50;
constexpr std::size_t kMaxSymbolLen = 32;

// Collects problems up to a cap, counting them all.
class Problems {
public:
    void add(std::size_t line, std::string column, std::string message) {
        ++total_;
        if (list_.size() < kMaxProblems) {
            list_.push_back(Problem{line, std::move(column), {}, std::move(message)});
        }
    }
    [[nodiscard]] bool any() const noexcept { return total_ > 0; }
    [[nodiscard]] std::size_t total() const noexcept { return total_; }

    [[noreturn]] void raise(const std::string& name) {
        std::string text = "dataset '" + name + "' is not valid: " + std::to_string(total_) + " problem" +
                           (total_ == 1 ? "" : "s");
        if (!list_.empty()) {
            const Problem& first = list_.front();
            text += "; first: ";
            if (first.line != 0) {
                text += "line " + std::to_string(first.line);
                text += first.column.empty() ? "" : ", column " + first.column;
                text += ": ";
            }
            text += first.message;
        }
        throw LabError(ErrorCode::DatasetError, std::move(text), list_, total_);
    }

private:
    std::vector<Problem> list_{};
    std::size_t          total_{0};
};

std::vector<std::string_view> split_commas(std::string_view line) {
    std::vector<std::string_view> fields;
    std::size_t start = 0;
    while (true) {
        const std::size_t comma = line.find(',', start);
        if (comma == std::string_view::npos) {
            fields.push_back(line.substr(start));
            return fields;
        }
        fields.push_back(line.substr(start, comma - start));
        start = comma + 1;
    }
}

std::string quote(std::string_view text) {
    return "'" + utf8_excerpt(text, 40) + "'";
}

bool valid_symbol(std::string_view symbol) {
    if (symbol.empty() || symbol.size() > kMaxSymbolLen) {
        return false;
    }
    return std::all_of(symbol.begin(), symbol.end(), [](char c) {
        const auto u = static_cast<unsigned char>(c);
        return u >= 0x21U && u <= 0x7EU && c != ',' && c != '"';
    });
}

struct OrderState {
    std::optional<common::Timestamp> any_time{};
    std::size_t                      any_line{0};
    std::optional<common::Timestamp> bar_time{};
    std::size_t                      bar_line{0};
    std::uint64_t                    rows{0};
};

class Parser {
public:
    Parser(std::string_view text, std::string name, const DatasetLimits& limits)
        : text_{text}, name_{std::move(name)}, limits_{limits} {}

    Dataset run() {
        if (text_.empty()) {
            problems_.add(0, "", "the file is empty");
            problems_.raise(name_);
        }
        std::size_t line_no = 0;
        std::size_t pos     = 0;
        while (pos <= text_.size()) {
            const std::size_t newline = text_.find('\n', pos);
            const bool last_segment   = newline == std::string_view::npos;
            std::string_view line     = text_.substr(pos, (last_segment ? text_.size() : newline) - pos);
            pos                       = last_segment ? text_.size() + 1 : newline + 1;
            if (last_segment && line.empty() && line_no > 0) {
                break;   // the file ends with a newline: not a blank line
            }
            ++line_no;
            if (line_no == 1 && line.size() >= 3 && line.substr(0, 3) == "\xEF\xBB\xBF") {
                line.remove_prefix(3);
            }
            if (!line.empty() && line.back() == '\r') {
                line.remove_suffix(1);
            }
            handle_line(line_no, line);
        }

        if (!header_ok_ && !problems_.any()) {
            problems_.add(0, "", "the file has no header line");
        }
        if (!problems_.any() && data_rows_ == 0) {
            problems_.add(1, "", "the file has a header but no data rows");
        }
        if (problems_.any()) {
            problems_.raise(name_);
        }
        return finish();
    }

private:
    void handle_line(std::size_t line_no, std::string_view line) {
        if (line.size() > limits_.max_line_bytes) {
            problems_.add(line_no, "", "line is longer than " + std::to_string(limits_.max_line_bytes) + " bytes");
        } else if (!is_valid_utf8(line)) {
            problems_.add(line_no, "", "line is not valid UTF-8");
        } else if (line.empty()) {
            problems_.add(line_no, "", "blank line");
        } else if (line.find('"') != std::string_view::npos) {
            problems_.add(line_no, "", "quote characters are not supported (quoted CSV fields are not parsed)");
        } else if (line_no == 1) {
            parse_header(line);
            return;
        } else if (header_ok_) {
            ++data_rows_;
            if (data_rows_ > limits_.max_rows) {
                throw LabError(ErrorCode::LimitExceeded,
                               "dataset '" + name_ + "' has more than " + std::to_string(limits_.max_rows) +
                                   " data rows (the limit); the tool never truncates input, so split the file or "
                                   "raise --max-rows (at most " + std::to_string(kHardMaxRows) + ")",
                               {Problem{line_no, "", "", "row limit exceeded"}}, 1);
            }
            parse_row(line_no, line);
            return;
        }
        if (line_no == 1) {
            problems_.raise(name_);   // nothing can be checked without a usable header
        }
    }

    void parse_header(std::string_view line) {
        const std::vector<DatasetColumn>& known = dataset_columns();
        std::array<bool, kColumnCount> seen{};
        for (const std::string_view cell : split_commas(line)) {
            const auto found = std::find_if(known.begin(), known.end(),
                                            [&](const DatasetColumn& c) { return c.name == cell; });
            if (found == known.end()) {
                problems_.add(1, std::string{cell}, "unknown column " + quote(cell));
                continue;
            }
            const auto id = static_cast<std::size_t>(found - known.begin());
            if (seen[id]) {
                problems_.add(1, std::string{cell}, "column " + quote(cell) + " appears more than once");
                continue;
            }
            seen[id] = true;
            header_.push_back(id);
        }
        for (std::size_t id = 0; id < known.size(); ++id) {
            if (known[id].required && !seen[id]) {
                problems_.add(1, std::string{known[id].name}, "required column is missing");
            }
        }
        if (problems_.any()) {
            problems_.raise(name_);
        }
        header_ok_ = true;
    }

    // ---- one data row -------------------------------------------------------------
    void parse_row(std::size_t line_no, std::string_view line) {
        const std::vector<std::string_view> fields = split_commas(line);
        if (fields.size() != header_.size()) {
            problems_.add(line_no, "",
                          "has " + std::to_string(fields.size()) + " fields; the header has " +
                              std::to_string(header_.size()));
            return;
        }
        std::array<std::string_view, kColumnCount> cell{};
        for (std::size_t i = 0; i < header_.size(); ++i) {
            cell[header_[i]] = fields[i];
        }

        bool ok = true;
        const auto bad = [&](Col column, const std::string& message) {
            problems_.add(line_no, std::string{dataset_columns()[column].name}, message);
            ok = false;
        };

        // symbol
        if (!valid_symbol(cell[kSymbol])) {
            bad(kSymbol, "symbol " + quote(cell[kSymbol]) +
                             " must be 1-" + std::to_string(kMaxSymbolLen) +
                             " printable ASCII characters without a comma or double quote");
        }

        // exchange_time
        common::Timestamp time{};
        {
            std::string why;
            if (const auto parsed = parse_utc_timestamp(cell[kTime], &why)) {
                time = *parsed;
            } else {
                bad(kTime, "exchange_time " + quote(cell[kTime]) + ": " + why);
            }
        }

        // type
        std::optional<domain::MarketEventType> type;
        if (cell[kType] == domain::to_string(domain::MarketEventType::Bar)) {
            type = domain::MarketEventType::Bar;
        } else if (cell[kType] == domain::to_string(domain::MarketEventType::Trade)) {
            type = domain::MarketEventType::Trade;
        } else {
            bad(kType, "type " + quote(cell[kType]) + " is not supported; this tool accepts 'bar' and 'trade'");
        }

        // price: finite and strictly positive, never repaired
        const auto read_number = [&](Col column, double minimum, bool strictly_greater,
                                     std::optional<double>& out) {
            if (cell[column].empty()) {
                return;
            }
            double value = 0.0;
            std::string why;
            const std::string name{dataset_columns()[column].name};
            if (!parse_double_text(cell[column], value, why)) {
                bad(column, name + " " + quote(cell[column]) + " " + why);
            } else if (!std::isfinite(value)) {
                bad(column, name + " " + quote(cell[column]) + " must be a finite number");
            } else if (strictly_greater ? !(value > minimum) : !(value >= minimum)) {
                bad(column, name + " " + quote(cell[column]) + " must be " +
                                (strictly_greater ? "greater than " : "at least ") +
                                (minimum == 0.0 ? "0" : std::to_string(minimum)));
            } else {
                out = value;
            }
        };

        std::optional<double> price, open, high, low, volume;
        if (cell[kPrice].empty()) {
            bad(kPrice, "price is required (a finite number greater than 0)");
        } else {
            read_number(kPrice, 0.0, true, price);
        }
        const bool is_bar = type.has_value() && *type == domain::MarketEventType::Bar;
        for (const Col column : {kOpen, kHigh, kLow, kVolume}) {
            if (cell[column].empty()) {
                continue;
            }
            if (type.has_value() && !is_bar) {
                bad(column, std::string{dataset_columns()[column].name} + " is only allowed on bar rows");
                continue;
            }
            if (column == kOpen) read_number(kOpen, 0.0, true, open);
            if (column == kHigh) read_number(kHigh, 0.0, true, high);
            if (column == kLow) read_number(kLow, 0.0, true, low);
            if (column == kVolume) read_number(kVolume, 0.0, false, volume);
        }
        if (price.has_value()) {
            if (low.has_value() && *low > *price) {
                bad(kLow, "low " + quote(cell[kLow]) + " is above the close " + quote(cell[kPrice]));
            }
            if (high.has_value() && *high < *price) {
                bad(kHigh, "high " + quote(cell[kHigh]) + " is below the close " + quote(cell[kPrice]));
            }
        }
        if (open.has_value()) {
            if (low.has_value() && *low > *open) {
                bad(kLow, "low " + quote(cell[kLow]) + " is above the open " + quote(cell[kOpen]));
            }
            if (high.has_value() && *high < *open) {
                bad(kHigh, "high " + quote(cell[kHigh]) + " is below the open " + quote(cell[kOpen]));
            }
        }
        if (!ok) {
            return;
        }

        // Ordering, checked only for a row that is otherwise valid.
        const std::string symbol{cell[kSymbol]};
        OrderState& order = order_[symbol];
        if (is_bar && order.bar_time.has_value() && time <= *order.bar_time) {
            problems_.add(line_no, "exchange_time",
                          "bar for " + symbol + " at " + format_utc_timestamp(time) +
                              " is not later than the previous " + symbol + " bar at line " +
                              std::to_string(order.bar_line) + " (" + format_utc_timestamp(*order.bar_time) +
                              "); bars of one symbol must strictly increase (a duplicate or older bar is an error)");
            return;
        }
        if (order.any_time.has_value() && time < *order.any_time) {
            problems_.add(line_no, "exchange_time",
                          "row for " + symbol + " at " + format_utc_timestamp(time) +
                              " is earlier than the previous " + symbol + " row at line " +
                              std::to_string(order.any_line) + " (" + format_utc_timestamp(*order.any_time) + ")");
            return;
        }
        if (previous_time_.has_value() && time < *previous_time_) {
            problems_.add(line_no, "exchange_time",
                          "exchange_time " + format_utc_timestamp(time) + " is earlier than line " +
                              std::to_string(previous_line_) + " (" + format_utc_timestamp(*previous_time_) +
                              "); rows must be in non-decreasing time order across the file (the lab replays in "
                              "file order and never sorts)");
            return;
        }

        order.any_time = time;
        order.any_line = line_no;
        if (is_bar) {
            order.bar_time = time;
            order.bar_line = line_no;
        }
        previous_time_ = time;
        previous_line_ = line_no;

        DatasetRow row;
        row.line                  = line_no;
        row.event.symbol          = symbol;
        row.event.exchange_time   = time;
        row.event.ingest_time     = time;
        row.event.type            = *type;
        row.event.price           = price;
        row.event.open            = open;
        row.event.high            = high;
        row.event.low             = low;
        row.event.volume          = volume;
        row.event.sequence        = ++order.rows;
        rows_.push_back(std::move(row));
    }

    Dataset finish() {
        Dataset dataset;
        dataset.info.name   = name_;
        dataset.info.sha256 = sha256_hex(text_);
        dataset.info.bytes  = text_.size();
        dataset.info.rows   = rows_.size();
        dataset.info.first_exchange_time = rows_.front().event.exchange_time;
        dataset.info.last_exchange_time  = rows_.back().event.exchange_time;

        std::map<std::string, SymbolSummary> summaries;   // sorted by symbol
        for (const DatasetRow& row : rows_) {
            auto [slot, inserted] = summaries.try_emplace(row.event.symbol);
            SymbolSummary& summary = slot->second;
            if (inserted) {
                summary.symbol = row.event.symbol;
                summary.first  = row.event.exchange_time;
            }
            summary.last = row.event.exchange_time;
            (row.event.type == domain::MarketEventType::Bar ? summary.bar_rows : summary.trade_rows) += 1;
        }
        for (auto& entry : summaries) {
            dataset.info.symbols.push_back(std::move(entry.second));
        }
        dataset.rows = std::move(rows_);
        return dataset;
    }

    std::string_view text_;
    std::string      name_;
    DatasetLimits    limits_;
    Problems         problems_{};

    bool                     header_ok_{false};
    std::vector<std::size_t> header_{};   // column id of each header position
    std::size_t              data_rows_{0};

    std::unordered_map<std::string, OrderState> order_{};
    std::optional<common::Timestamp>            previous_time_{};
    std::size_t                                 previous_line_{0};
    std::vector<DatasetRow>                     rows_{};
};

}  // namespace

const std::vector<DatasetColumn>& dataset_columns() {
    static const std::vector<DatasetColumn> columns{
        {"symbol", true, "Instrument symbol, 1-32 printable ASCII characters; matched exactly and case-sensitively."},
        {"exchange_time", true, "UTC, YYYY-MM-DDTHH:MM:SS[.f{1,9}]Z, 1970-2261; no offsets."},
        {"type", true, "'bar' or 'trade'. Only bars feed the reference strategies; trades show them being ignored."},
        {"price", true, "Finite and greater than 0. For a bar this is the close."},
        {"open", false, "Bars only: finite, greater than 0, within [low, high] with the close."},
        {"high", false, "Bars only: finite, greater than 0, at least the open and the close."},
        {"low", false, "Bars only: finite, greater than 0, at most the open and the close."},
        {"volume", false, "Bars only: finite, at least 0."},
    };
    return columns;
}

Dataset parse_dataset(std::string_view text, std::string name, const DatasetLimits& limits) {
    if (!is_valid_utf8(name)) {
        name = "dataset";
    }
    return Parser{text, std::move(name), limits}.run();
}

Dataset load_dataset(const std::filesystem::path& path, const DatasetLimits& limits) {
    const std::string shown = path_to_utf8(path);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec)) {
        throw LabError(ErrorCode::DatasetError, "cannot read dataset '" + shown + "': not a readable regular file",
                       {Problem{0, "", "", "not a readable regular file"}}, 1);
    }
    const std::uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec) {
        throw LabError(ErrorCode::DatasetError, "cannot read dataset '" + shown + "': " + ec.message());
    }
    if (size > limits.max_file_bytes) {
        throw LabError(ErrorCode::LimitExceeded,
                       "dataset '" + shown + "' is " + std::to_string(size) + " bytes; the limit is " +
                           std::to_string(limits.max_file_bytes));
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw LabError(ErrorCode::DatasetError, "cannot open dataset '" + shown + "'");
    }
    std::string text(static_cast<std::size_t>(size), '\0');
    in.read(text.data(), static_cast<std::streamsize>(text.size()));
    if (static_cast<std::size_t>(in.gcount()) != text.size()) {
        throw LabError(ErrorCode::DatasetError, "cannot read all of dataset '" + shown + "'");
    }
    return parse_dataset(text, path_to_utf8(path.filename()), limits);
}

}  // namespace trading_engine::lab
