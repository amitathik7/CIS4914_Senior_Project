// strategy_lab_replay: replays a CSV of bars through the real StrategyEngine and the
// reference strategies and writes one JSON document on stdout. A developer/demo tool, not
// the trading engine (apps/trading_engine_main.cpp is that). All behaviour is in lab/;
// this file reads the wall clock for provenance and moves bytes.
//
// See lab/cli.hpp for the command line and lab/errors.hpp for the exit status.

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "lab/cli.hpp"
#include "lab/errors.hpp"
#include "lab/io.hpp"
#include "lab/timestamp.hpp"

int main(int argc, char** argv) {
    using namespace trading_engine;

    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }

    lab::CliEnvironment environment;
    environment.now_utc = [] {
        // The one wall-clock read in the tool, for provenance only: nothing under `result`
        // depends on it.
        const auto now = std::chrono::time_point_cast<common::Duration>(common::Clock::now());
        return lab::format_utc_timestamp(now);
    };

    const lab::CliResult result = lab::run_cli(args, environment);

    lab::set_binary_mode(stdout);
    int exit_code = result.exit_code;
    if (!lab::write_all(stdout, result.out)) {
        // Success must not be reported if the document did not get out.
        exit_code = lab::kExitInternal;
        (void)std::fputs("strategy_lab_replay: error [internal_error]: could not write the result to stdout\n", stderr);
    }
    if (!result.err.empty()) {
        (void)lab::write_all(stderr, result.err);
    }
    return exit_code;
}
