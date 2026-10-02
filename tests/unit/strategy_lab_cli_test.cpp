// The runner's OUTPUT AND ERROR CONTRACT, in process: the exit status of each failure class,
// that stdout carries exactly one JSON document and nothing else (stderr is for humans), that the
// wall clock lives only in provenance, and the @file argument mechanism. That the documents
// really are valid JSON is checked with a real parser in tests/strategy_lab (Python).

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "lab/cli.hpp"
#include "lab/dataset.hpp"
#include "strategy_lab_test_support.hpp"

namespace {

using namespace lab_test;
using Args = std::vector<std::string>;

lab::CliResult run(const Args& args, const std::string& now = "2030-01-01T00:00:00.000000000Z") {
    lab::CliEnvironment env;
    env.now_utc = [now] { return now; };
    return lab::run_cli(args, env);
}

Args crossover_args(const std::string& fixture = "sma_crossover.csv") {
    return {"run", "--dataset", fixture_path(fixture).string(), "--strategy", "sma_crossover",
            "--param", "short_window=2", "--param", "long_window=3", "--param", "symbols=AAPL"};
}

Args with(Args base, const Args& extra) {
    base.insert(base.end(), extra.begin(), extra.end());
    return base;
}

// stdout is ONE line: a JSON document and its final newline, nothing before or after, no CR.
void expect_one_json_line(const std::string& out) {
    ASSERT_FALSE(out.empty());
    EXPECT_EQ(out.front(), '{');
    EXPECT_EQ(out.back(), '\n');
    EXPECT_EQ(out.find('\n'), out.size() - 1) << "compact JSON has exactly one newline: the last character";
    EXPECT_EQ(out.find('\r'), std::string::npos);
    EXPECT_EQ(out[out.size() - 2], '}');
}

bool contains(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }

// ---- success -------------------------------------------------------------------------------------

TEST(LabCli, ASuccessfulRunWritesOneReplayDocumentToStdoutAndNothingToStderr) {
    const lab::CliResult result = run(crossover_args());
    EXPECT_EQ(result.exit_code, 0);
    expect_one_json_line(result.out);
    EXPECT_TRUE(result.err.empty()) << "no warnings, so stderr stays silent: " << result.err;
    EXPECT_TRUE(contains(result.out, "{\"schema\":\"strategy_lab.replay\",\"schema_version\":\"1.0\",\"provenance\":{"));
    EXPECT_TRUE(contains(result.out, "\"side\":\"buy\""));
    EXPECT_TRUE(contains(result.out, "\"side\":\"sell\""));
}

TEST(LabCli, TheWallClockAppearsOnlyInProvenanceAndOnlyWhenRequested) {
    const lab::CliResult stamped  = run(crossover_args(), "2031-02-03T04:05:06.000000000Z");
    const lab::CliResult plain    = run(with(crossover_args(), {"--no-wall-clock"}), "2031-02-03T04:05:06.000000000Z");
    const lab::CliResult other_day = run(crossover_args(), "2032-09-09T09:09:09.000000000Z");

    EXPECT_TRUE(contains(stamped.out, "\"generated_at\":\"2031-02-03T04:05:06.000000000Z\""));
    EXPECT_FALSE(contains(plain.out, "generated_at"));
    EXPECT_FALSE(contains(plain.out, "2031-02-03")) << "no trace of the supplied time anywhere in the document";

    // Only the provenance differs between two days: everything after it is byte-identical.
    const auto result_of = [](const std::string& out) { return out.substr(out.find("\"result\":")); };
    EXPECT_EQ(result_of(stamped.out), result_of(other_day.out));
    EXPECT_EQ(result_of(stamped.out), result_of(plain.out));
    EXPECT_NE(stamped.out, other_day.out);
}

TEST(LabCli, WithoutAWallClockTheWholeDocumentIsReproducibleByteForByte) {
    const Args args = with(crossover_args(), {"--no-wall-clock"});
    EXPECT_EQ(run(args).out, run(args, "1999-01-01T00:00:00.000000000Z").out);
}

TEST(LabCli, PrettyOutputIsIndentedAndTheSameDocument) {
    const lab::CliResult compact = run(with(crossover_args(), {"--no-wall-clock"}));
    const lab::CliResult pretty  = run(with(crossover_args(), {"--no-wall-clock", "--pretty"}));
    EXPECT_EQ(pretty.exit_code, 0);
    EXPECT_TRUE(contains(pretty.out, "\n  \"schema\": \"strategy_lab.replay\""));
    EXPECT_EQ(pretty.out.back(), '\n');
    EXPECT_EQ(pretty.out.find('\r'), std::string::npos);
    // Same content: strip all whitespace outside strings is not trivial, so compare sizes of the
    // non-whitespace characters instead (strings in this document contain no spaces except the notice).
    const auto significant = [](const std::string& text) {
        std::size_t count = 0;
        bool in_string = false;
        for (std::size_t i = 0; i < text.size(); ++i) {
            if (text[i] == '"' && (i == 0 || text[i - 1] != '\\')) in_string = !in_string;
            if (in_string || (text[i] != ' ' && text[i] != '\n')) ++count;
        }
        return count;
    };
    EXPECT_EQ(significant(compact.out), significant(pretty.out));
}

TEST(LabCli, WarningsGoToStderrAndNeverIntoStdoutsFraming) {
    Args args = crossover_args();
    args.back() = "symbols=aapl";   // the data says AAPL: a case mismatch
    const lab::CliResult result = run(args);
    EXPECT_EQ(result.exit_code, 0) << "a warning is not a failure";
    expect_one_json_line(result.out);
    EXPECT_TRUE(contains(result.err, "warning [dataset_symbol_not_allowlisted]"));
    EXPECT_TRUE(contains(result.err, "warning [allowlisted_symbol_not_in_dataset]"));
    EXPECT_TRUE(contains(result.out, "\"code\":\"dataset_symbol_not_allowlisted\"")) << "and the document carries them too";
}

TEST(LabCli, StrategiesKeepCommandLineOrderAndEachGetsItsOwnParameters) {
    const Args args{"run", "--dataset", fixture_path("two_symbols_interleaved.csv").string(),
                    "--strategy", "mean_reversion", "--param", "strategy_id=mr", "--param", "lookback=4",
                    "--param", "entry_threshold=1.2", "--param", "symbols=AAPL,MSFT",
                    "--strategy", "sma_crossover", "--param", "strategy_id=sma", "--param", "short_window=2",
                    "--param", "long_window=3", "--param", "symbols=AAPL,MSFT", "--no-wall-clock"};
    const lab::CliResult result = run(args);
    EXPECT_EQ(result.exit_code, 0) << result.err;
    // mr is registered first, so it publishes first at minute 5: ids 2 (mr) then 3 (sma) at AAPL.
    const std::size_t mr_first  = result.out.find("\"strategy_id\":\"mr\",\"event_index\":8");
    const std::size_t sma_first = result.out.find("\"strategy_id\":\"sma\",\"event_index\":8");
    ASSERT_NE(mr_first, std::string::npos);
    ASSERT_NE(sma_first, std::string::npos);
    EXPECT_LT(mr_first, sma_first);
}

TEST(LabCli, ABusFaultIsOptInAndReportedInTheDocument) {
    const lab::CliResult result = run(with(crossover_args(), {"--bus-fault", "reject-signals", "--no-wall-clock"}));
    EXPECT_EQ(result.exit_code, 0) << "a refused signal is a result, not an error";
    expect_one_json_line(result.out);
    EXPECT_TRUE(contains(result.out, "\"bus_fault\":\"reject-signals\""));
    EXPECT_TRUE(contains(result.out, "\"publication_failures\":[{\"signal_id\":1"));
    EXPECT_TRUE(contains(result.out, "\"signals_rejected\":2"));
    EXPECT_TRUE(contains(result.out, "\"signals\":[],\"publication_failures\""));
}

// ---- describe and help ------------------------------------------------------------------------------

TEST(LabCli, DescribeWritesTheCatalogAndIsDeterministic) {
    const lab::CliResult result = run({"describe"});
    EXPECT_EQ(result.exit_code, 0);
    expect_one_json_line(result.out);
    EXPECT_TRUE(result.err.empty());
    EXPECT_TRUE(contains(result.out, "\"schema\":\"strategy_lab.catalog\""));
    EXPECT_EQ(result.out, run({"describe"}, "1999-01-01T00:00:00.000000000Z").out);
    EXPECT_EQ(run({"describe", "--extra"}).exit_code, 2);
}

TEST(LabCli, HelpGoesToStderrAndLeavesStdoutEmpty) {
    for (const char* flag : {"--help", "-h"}) {
        const lab::CliResult result = run({flag});
        EXPECT_EQ(result.exit_code, 0);
        EXPECT_TRUE(result.out.empty()) << "stdout is reserved for the JSON document";
        EXPECT_TRUE(contains(result.err, "usage:"));
    }
}

// ---- exit statuses ------------------------------------------------------------------------------------

struct ErrorExpectation {
    Args        args;
    int         exit_code;
    std::string code;
};

TEST(LabCliExitStatus, EveryFailureClassHasItsDocumentedStatusAndAMatchingErrorDocument) {
    const std::string path = fixture_path("sma_crossover.csv").string();
    const std::vector<ErrorExpectation> cases{
        {{}, 2, "usage_error"},
        {{"frobnicate"}, 2, "usage_error"},
        {{"run"}, 2, "usage_error"},
        {{"run", "--dataset", path}, 2, "usage_error"},
        {{"run", "--strategy", "sma_crossover", "--param", "symbols=A"}, 2, "usage_error"},
        {{"run", "--dataset", path, "--param", "symbols=A", "--strategy", "sma_crossover"}, 2, "usage_error"},
        {{"run", "--dataset", path, "--strategy", "sma_crossover", "--param", "symbols"}, 2, "usage_error"},
        {{"run", "--dataset"}, 2, "usage_error"},
        {{"run", "--dataset", path, "--strategy", "sma_crossover", "--bogus"}, 2, "usage_error"},
        {{"run", "--dataset", path, "--strategy", "sma_crossover", "--bus-fault", "explode"}, 2, "usage_error"},
        {{"run", "--dataset", path, "--strategy", "sma_crossover", "--max-rows", "0"}, 2, "usage_error"},
        {{"run", "--dataset", path, "--strategy", "sma_crossover", "--max-rows", "abc"}, 2, "usage_error"},
        {{"run", "--dataset", path, "--strategy", "sma_crossover", "--max-rows", "200001"}, 2, "usage_error"},
        {{"run", "--dataset", path, "--dataset", path, "--strategy", "sma_crossover"}, 2, "usage_error"},
        {{"run", "--dataset", path, "--strategy", "nope", "--param", "symbols=A"}, 2, "usage_error"},
        {{"run", "--dataset", path, "--strategy", "sma_crossover", "--param", "symbols=A", "--param", "bogus=1"}, 2, "invalid_parameter"},
        {{"run", "--dataset", path, "--strategy", "sma_crossover", "--param", "symbols=A", "--param", "short_window=-1"}, 2, "invalid_parameter"},
        {{"run", "--dataset", path, "--strategy", "ml", "--param", "symbols=A"}, 2, "unavailable_strategy"},
        {{"run", "--dataset", path, "--strategy", "sma_crossover", "--param", "symbols=A", "--param", "short_window=0"}, 2, "config_error"},
        {{"run", "--dataset", fixture_path("missing.csv").string(), "--strategy", "sma_crossover", "--param", "symbols=A"}, 3, "dataset_error"},
        {{"run", "--dataset", path, "--max-rows", "5", "--strategy", "sma_crossover", "--param", "symbols=AAPL"}, 3, "limit_exceeded"},
    };
    for (std::size_t i = 0; i < cases.size(); ++i) {
        SCOPED_TRACE("case " + std::to_string(i));
        const lab::CliResult result = run(cases[i].args);
        EXPECT_EQ(result.exit_code, cases[i].exit_code) << result.err;
        EXPECT_FALSE(result.err.empty()) << "a failure is explained for a human on stderr";
        expect_one_json_line(result.out);
        EXPECT_TRUE(contains(result.out, "\"schema\":\"strategy_lab.error\""));
        EXPECT_TRUE(contains(result.out, "\"code\":\"" + cases[i].code + "\"")) << result.out;
        EXPECT_TRUE(contains(result.out, "\"exit_status\":" + std::to_string(cases[i].exit_code)));
    }
    EXPECT_EQ(run({"run", "--dataset", path, "--strategy", "sma_crossover", "--param", "symbols=AAPL"}).exit_code, 0)
        << "5/20 on nine bars never leaves warm-up, which is a valid (silent) run";
}

TEST(LabCliExitStatus, AStrategyConfigurationErrorCarriesTheStrategysOwnWordsInBothStreams) {
    const lab::CliResult result =
        run({"run", "--dataset", fixture_path("sma_crossover.csv").string(), "--strategy", "sma_crossover",
             "--param", "symbols=AAPL", "--param", "requested_quantity=1.5"});
    EXPECT_EQ(result.exit_code, 2);
    const std::string words = "MovingAverageCrossoverStrategy: requested_quantity must be a finite, positive whole number of shares (got 1.5)";
    EXPECT_TRUE(contains(result.out, words));
    EXPECT_TRUE(contains(result.err, words));
    EXPECT_TRUE(contains(result.err, "strategies[0]")) << "and says which strategy";
}

TEST(LabCliExitStatus, ADatasetErrorListsEachProblemWithItsLineAndColumn) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "strategy_lab_cli_test_dataset";
    std::filesystem::create_directories(dir);
    const std::filesystem::path bad = dir / "bad.csv";
    {
        std::ofstream out(bad, std::ios::binary);
        out << "symbol,exchange_time,type,price\n"
               "AAPL,2026-01-05T14:30:00Z,bar,10\n"
               "AAPL,2026-01-05T14:31:00Z,bar,nan\n"
               "AAPL,2026-01-05T14:30:00Z,bar,12\n";
    }
    const lab::CliResult result = run({"run", "--dataset", bad.string(), "--strategy", "sma_crossover", "--param", "symbols=AAPL"});
    EXPECT_EQ(result.exit_code, 3);
    EXPECT_TRUE(contains(result.out, "\"code\":\"dataset_error\""));
    EXPECT_TRUE(contains(result.out, "\"total_problems\":2"));
    EXPECT_TRUE(contains(result.out, "\"line\":3,\"column\":\"price\""));
    EXPECT_TRUE(contains(result.out, "\"line\":4,\"column\":\"exchange_time\""));
    EXPECT_TRUE(contains(result.err, "line 3: [price]"));
    EXPECT_TRUE(contains(result.err, "line 4: [exchange_time]"));
    std::filesystem::remove_all(dir);
}

TEST(LabCliExitStatus, AnUnexpectedFailureIsAnInternalErrorWithStatusFour) {
    lab::CliEnvironment env;
    env.now_utc = []() -> std::string { throw std::runtime_error("the clock exploded"); };
    const lab::CliResult result = lab::run_cli(crossover_args(), env);
    EXPECT_EQ(result.exit_code, 4);
    expect_one_json_line(result.out);
    EXPECT_TRUE(contains(result.out, "\"code\":\"internal_error\""));
    EXPECT_TRUE(contains(result.out, "the clock exploded"));
    EXPECT_TRUE(contains(result.err, "internal_error"));
}

TEST(LabCliExitStatus, AnArgumentThatIsNotValidUtf8IsAUsageErrorWhoseDocumentIsStillValid) {
    const lab::CliResult result = run({"run", "--dataset", "bad\xFF.csv", "--strategy", "sma_crossover"});
    EXPECT_EQ(result.exit_code, 2);
    expect_one_json_line(result.out);
    EXPECT_FALSE(contains(result.out, "\xFF")) << "the invalid byte is never echoed into the JSON";

    // Echoed text of unknown origin is sanitized, not trusted.
    const lab::CliResult unknown = run({"run", "--dataset", fixture_path("sma_crossover.csv").string(), "--strategy", "sma_crossover",
                                        "--param", "symbols=A", "--param", "caf\xC3\xA9=1"});
    EXPECT_EQ(unknown.exit_code, 2);
    EXPECT_TRUE(contains(unknown.out, "caf\xC3\xA9")) << "valid UTF-8 is kept";
}

// ---- @file ---------------------------------------------------------------------------------------------

class ResponseFiles : public ::testing::Test {
protected:
    void SetUp() override {
        dir_ = std::filesystem::temp_directory_path() / "strategy_lab_cli_test_args";
        std::filesystem::create_directories(dir_);
    }
    void TearDown() override { std::filesystem::remove_all(dir_); }
    std::string write(const std::string& name, const std::string& text) const {
        const std::filesystem::path path = dir_ / name;
        std::ofstream out(path, std::ios::binary);
        out << text;
        return path.string();
    }
    std::filesystem::path dir_;
};

TEST_F(ResponseFiles, AnArgsFileIsExpandedLineByLineWithCommentsAndBlankLinesSkipped) {
    const std::string file = write("a.args",
        "# a comment\n"
        "run\n"
        "\n"
        "--dataset\r\n" + fixture_path("sma_crossover.csv").string() + "\r\n"
        "--strategy\nsma_crossover\n--param\nshort_window=2\n--param\nlong_window=3\n--param\nsymbols=AAPL\n"
        "--no-wall-clock\n");
    const lab::CliResult from_file = run({"@" + file});
    const lab::CliResult direct    = run(with(crossover_args(), {"--no-wall-clock"}));
    EXPECT_EQ(from_file.exit_code, 0) << from_file.err;
    EXPECT_EQ(from_file.out, direct.out);

    // Mixed with ordinary arguments, in position.
    const std::string tail = write("tail.args", "--param\nsymbols=AAPL\n--no-wall-clock\n");
    const lab::CliResult mixed = run({"run", "--dataset", fixture_path("sma_crossover.csv").string(), "--strategy", "sma_crossover",
                                      "--param", "short_window=2", "--param", "long_window=3", "@" + tail});
    EXPECT_EQ(mixed.out, direct.out);
}

TEST_F(ResponseFiles, ANestedOrMissingResponseFileIsAUsageError) {
    const std::string inner = write("inner.args", "--no-wall-clock\n");
    const std::string outer = write("outer.args", "run\n@" + inner + "\n");
    const lab::CliResult nested = run({"@" + outer});
    EXPECT_EQ(nested.exit_code, 2);
    EXPECT_TRUE(contains(nested.out, "nested"));

    const lab::CliResult missing = run({"@" + (dir_ / "nope.args").string()});
    EXPECT_EQ(missing.exit_code, 2);
    EXPECT_TRUE(contains(missing.out, "cannot read response file"));
    EXPECT_EQ(run({"@"}).exit_code, 2);
}

TEST_F(ResponseFiles, NonAsciiValuesTravelIntactThroughAnArgsFile) {
    // A strategy id with accents, an emoji, a quote and a backslash: all must come back exactly.
    const std::string id = "caf\xC3\xA9 \xF0\x9F\x98\x80 \"q\" back\\slash";
    const std::string file = write("unicode.args",
        "run\n--dataset\n" + fixture_path("sma_crossover.csv").string() + "\n--strategy\nsma_crossover\n"
        "--param\nstrategy_id=" + id + "\n--param\nshort_window=2\n--param\nlong_window=3\n--param\nsymbols=AAPL\n"
        "--no-wall-clock\n");
    const lab::CliResult result = run({"@" + file});
    ASSERT_EQ(result.exit_code, 0) << result.err;
    EXPECT_TRUE(contains(result.out, "\"strategy_id\":\"caf\xC3\xA9 \xF0\x9F\x98\x80 \\\"q\\\" back\\\\slash\""));
}

}  // namespace
