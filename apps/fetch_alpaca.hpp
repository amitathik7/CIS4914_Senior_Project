#pragma once

// `trading_engine fetch-alpaca`: downloads Alpaca historical bars and writes
// them in the CSV layout read_bar_csv() reads, so real data feeds `run`,
// `serve` and the Strategy Lab without further changes.

namespace trading_engine::cli {

extern const char* const kFetchAlpacaUsage;

// argv[1] is "fetch-alpaca". Returns the process exit status.
int fetch_alpaca(int argc, char** argv);

}  // namespace trading_engine::cli
