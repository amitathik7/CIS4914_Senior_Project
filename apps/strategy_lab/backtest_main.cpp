// strategy_lab_backtest: a full simulated backtest (strategies -> risk -> execution ->
// portfolio -> analytics) over one Strategy Lab dataset, written as one JSON document on
// stdout. All behaviour is in lab/backtest_cli.cpp; this file only moves bytes.

#include <cstdio>
#include <string>
#include <vector>

#include "lab/backtest_cli.hpp"
#include "lab/errors.hpp"
#include "lab/io.hpp"

int main(int argc, char** argv) {
    using namespace trading_engine;

    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        args.emplace_back(argv[i]);
    }
    const lab::CliResult result = lab::run_backtest_cli(args);

    lab::set_binary_mode(stdout);
    int exit_code = result.exit_code;
    if (!lab::write_all(stdout, result.out)) {
        exit_code = lab::kExitInternal;
        (void)std::fputs("strategy_lab_backtest: error [internal_error]: could not write the result to stdout\n", stderr);
    }
    if (!result.err.empty()) {
        (void)lab::write_all(stderr, result.err);
    }
    return exit_code;
}
