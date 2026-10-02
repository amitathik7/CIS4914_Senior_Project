#pragma once

// -----------------------------------------------------------------------------
//  Dataset: local market bars from a CSV file (format id "strategy_lab_bars_csv/1").
//
//  This is a LAB-LOCAL schema. It is not the project's market-data fixture schema
//  (tests/fixtures/market_data/, still planned) and not a persistence format.
//
//  The loader VALIDATES; it never repairs. A row that breaks a rule is reported with
//  its line number and column and the whole load fails (exit status 3): nothing is
//  skipped, defaulted, re-sorted, clamped or "fixed up". In particular no invalid price
//  is turned into a valid one.
//
//  File
//   * UTF-8, "\n" or "\r\n" line ends, an optional UTF-8 byte-order mark, no blank
//     lines, no quote characters (quoted CSV is not supported), at most max_line_bytes
//     per line and max_rows data rows. A header line is required.
//   * Columns (any order, none repeated, no others): symbol, exchange_time, type, price
//     are required; open, high, low, volume are optional (an empty cell means absent).
//
//  Fields
//   * symbol    1..32 printable ASCII characters, no comma or double quote. Case is
//               preserved. Symbols are matched EXACTLY and case-sensitively by the
//               strategies against their `symbols` list: "aapl" is not "AAPL".
//   * exchange_time  RFC 3339 UTC, YYYY-MM-DDTHH:MM:SS[.f{1,9}]Z, 1970..2261 (see
//               timestamp.hpp). There is no time-zone conversion: a value with an offset
//               or no zone is an error. The lab sets ingest_time equal to it.
//   * type      "bar" or "trade". A trade row is accepted so a dataset can show that
//               the strategies ignore it; no other type is accepted.
//   * price     required: a finite number > 0, in plain C-locale decimal or scientific
//               syntax ("101.25", "1e-7"; no sign, no spaces, no hex, no "nan"/"inf"). A
//               bar's price is its CLOSE. A value that overflows a double is an error.
//   * open high low  optional, bars only: finite > 0, and consistent with the close:
//               low <= open, close <= high wherever both are present.
//   * volume    optional, bars only: finite >= 0.
//
//  Interval. The file carries none. The strategies expect FINALIZED bars of ONE
//  consistent interval per symbol (MarketEvent has no interval or "final" field,
//  docs/OPEN_QUESTIONS.md OQ#1), and the lab neither checks nor infers that: it is the
//  data author's responsibility. The strategies' windows count accepted bars, whatever
//  the spacing.
//
//  Ordering. Rows are replayed in FILE ORDER; nothing is sorted. Required of the file:
//   * across the whole file, exchange_time never decreases (so the replay clock only
//     moves forward);
//   * within one symbol, bar times strictly increase (a duplicate or older bar is an
//     error, never dropped), and no row is earlier than the symbol's previous row;
//   * rows of DIFFERENT symbols with the SAME exchange_time are fine, and replay in the
//     order they appear in the file. That tie rule is a LAB-ONLY policy so a run is
//     reproducible. It is not the production replay's merge or tie-break (plan M3 says
//     ties break by `sequence`, undecided and not implemented) and decides nothing
//     about it.
//   * a trade row may share a timestamp with a bar of the same symbol.
// -----------------------------------------------------------------------------

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "trading_engine/common/types.hpp"
#include "trading_engine/domain/market_event.hpp"

namespace trading_engine::lab {

inline constexpr std::string_view kDatasetFormat = "strategy_lab_bars_csv/1";
inline constexpr std::size_t kDefaultMaxRows = 50'000;
inline constexpr std::size_t kHardMaxRows    = 200'000;

// The authoritative column list: the parser validates the header against it and
// `describe` publishes it.
struct DatasetColumn {
    std::string_view name;
    bool             required;
    std::string_view description;
};
[[nodiscard]] const std::vector<DatasetColumn>& dataset_columns();

struct DatasetLimits {
    std::size_t max_rows{kDefaultMaxRows};
    std::size_t max_line_bytes{4096};
    std::size_t max_file_bytes{64U * 1024U * 1024U};
};

struct DatasetRow {
    std::size_t         line{0};   // 1-based CSV line
    domain::MarketEvent event{};
};

struct SymbolSummary {
    std::string       symbol{};
    std::size_t       bar_rows{0};
    std::size_t       trade_rows{0};
    common::Timestamp first{};
    common::Timestamp last{};
};

struct DatasetInfo {
    std::string                name{};     // the file name, never a path
    std::string                sha256{};   // of the file's bytes
    std::size_t                bytes{0};
    std::size_t                rows{0};
    std::vector<SymbolSummary> symbols{};  // sorted by symbol
    common::Timestamp          first_exchange_time{};
    common::Timestamp          last_exchange_time{};
};

struct Dataset {
    DatasetInfo             info{};
    std::vector<DatasetRow> rows{};
};

// Throws LabError(DatasetError, ...) listing up to 50 problems with their line and
// column, or LabError(LimitExceeded, ...) when a limit is exceeded. A file with no
// data rows is an error.
[[nodiscard]] Dataset parse_dataset(std::string_view text, std::string name,
                                    const DatasetLimits& limits = {});

[[nodiscard]] Dataset load_dataset(const std::filesystem::path& path,
                                   const DatasetLimits& limits = {});

}  // namespace trading_engine::lab
