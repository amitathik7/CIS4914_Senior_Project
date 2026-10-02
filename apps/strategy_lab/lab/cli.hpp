#pragma once

// -----------------------------------------------------------------------------
//  The command line of strategy_lab_replay, as a pure function: arguments in, the text for
//  stdout, the text for stderr and an exit status out. main() only writes them.
//
//    strategy_lab_replay describe [--pretty]
//    strategy_lab_replay run --dataset <csv> --strategy <kind> [--param name=value]...
//                            [--strategy <kind> [--param ...]]...
//                            [--bus-fault none|reject-signals|throw-on-signal]
//                            [--max-rows N] [--pretty] [--no-wall-clock]
//    strategy_lab_replay --help
//    @<file>   anywhere: replaced by that file's lines, one argument per line (UTF-8,
//              blank lines and lines starting with '#' skipped, no nesting)
//
//  stdout: exactly one JSON document (a result, or a strategy_lab.error) and nothing else,
//  except --help, which prints its text on stderr and leaves stdout empty. stderr:
//  human-readable text (the error, any warnings). Exit status: see lab/errors.hpp.
//
//  Every argument is treated as UTF-8. On Windows the C runtime hands a program its
//  arguments in the ANSI code page, so put non-ASCII values in an @file. --param applies to
//  the most recent --strategy; strategies keep command-line order, which is registration and
//  delivery order. Relative paths resolve against the current directory.
//
//  Stateless: nothing is read from the environment, the clock, or files except the dataset
//  and @files; the wall-clock time for provenance is supplied by the caller.
// -----------------------------------------------------------------------------

#include <functional>
#include <string>
#include <vector>

namespace trading_engine::lab {

struct CliEnvironment {
    // RFC 3339 UTC time for provenance.generated_at; empty means "no wall clock available".
    std::function<std::string()> now_utc{};
};

struct CliResult {
    int         exit_code{0};
    std::string out{};   // for stdout
    std::string err{};   // for stderr
};

// `args` excludes the program name. Never throws.
[[nodiscard]] CliResult run_cli(const std::vector<std::string>& args, const CliEnvironment& env);

[[nodiscard]] std::string usage_text();

}  // namespace trading_engine::lab
