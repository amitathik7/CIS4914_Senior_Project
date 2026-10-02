// strategy_lab_json_probe -- TEST-ONLY. Writes the Strategy Lab's JSON serializers' output for
// documents that real runs cannot produce: values the CSV loader would refuse (NaN and infinite
// prices), diagnostics with non-finite or unavailable values, hostile text (quotes, backslashes,
// control characters, line separators, emoji), numbers at the edges of the double range, and
// counters beyond 2^53. tests/strategy_lab/test_json_robustness.py parses the output with a real
// JSON parser and checks every value.
//
//   strategy_lab_json_probe document [--pretty]    a strategy_lab.replay document
//   strategy_lab_json_probe locale                 the SAME document, written under a hostile locale
//   strategy_lab_json_probe error                  a strategy_lab.error document
//
// Not built by default and not part of the tool.

#include <clocale>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <locale>
#include <string>
#include <vector>

#include "lab/io.hpp"
#include "lab/result_json.hpp"
#include "lab/timestamp.hpp"

namespace {

using namespace trading_engine;
using namespace trading_engine::lab;

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kInf = std::numeric_limits<double>::infinity();

// A locale that would corrupt any number written through iostreams or printf-style formatting.
struct CommaDecimal final : std::numpunct<char> {
    char do_decimal_point() const override { return ','; }
    char do_thousands_sep() const override { return '.'; }
    std::string do_grouping() const override { return "\3"; }
};

RunDocument hostile_document() {
    RunDocument doc;
    doc.run_id           = "0123456789abcdef";
    doc.max_rows         = 50000;
    doc.input.name       = "n\"a\\me\t\x01\xE2\x80\xA8 \xF0\x9F\x98\x80.csv";   // quote, backslash, tab, SOH, U+2028, emoji
    doc.input.sha256     = std::string(64, 'a');
    doc.input.bytes      = 123456789012ULL;
    doc.input.rows       = 3;
    doc.input.first_exchange_time = *parse_utc_timestamp("2026-01-05T14:30:00.000000001Z");
    doc.input.last_exchange_time  = *parse_utc_timestamp("2261-12-31T23:59:59.999999999Z");
    SymbolSummary symbol;
    symbol.symbol     = "A\\B";
    symbol.bar_rows   = 3;
    symbol.first      = doc.input.first_exchange_time;
    symbol.last       = doc.input.last_exchange_time;
    doc.input.symbols = {symbol};

    doc.warnings.push_back(Warning{"w\"code", "message with \\ and \"quotes\" and \n newline", "s\"1"});

    ReplayResult& replay = doc.replay;
    replay.bus.fault     = "none";

    StrategyRunInfo info;
    info.kind        = "sma_crossover";
    info.strategy_id = "s\"1";
    info.parameters  = {{"short_window", std::uint64_t{2}},
                        {"requested_quantity", 1.7976931348623157e308},
                        {"tiny", 5e-324},
                        {"third", 1.0 / 3.0},
                        {"symbols", std::vector<std::string>{"A\\B", "caf\xC3\xA9"}},
                        {"strategy_id", std::string{"s\"1"}}};
    info.derived     = {{"flag", true}, {"count", std::uint64_t{18446744073709551615ULL}}, {"ratio", 0.1 + 0.2}};
    info.symbols     = {"A\\B"};
    info.window_size = 3;
    info.stats.id               = "s\"1";
    info.stats.events_delivered = 3;
    info.stats.last_error       = "error with \"quotes\" \x01";
    replay.strategies.push_back(info);

    const auto event = [](const char* sym, std::optional<double> price, long long ns) {
        EventResult row;
        row.input.source_line               = 2;
        row.input.event.symbol              = sym;
        row.input.event.type                = domain::MarketEventType::Bar;
        row.input.event.exchange_time       = common::Timestamp{common::Duration{ns}};
        row.input.event.price               = price;
        row.per_strategy.resize(1);
        return row;
    };

    EventResult first = event("A\\B", 0.1, 1'767'623'400'000'000'001LL);
    first.input.event.open   = kNaN;           // non-finite: omitted, with a status
    first.input.event.high   = kInf;
    first.input.event.low    = -kInf;
    first.input.event.volume = 1e-320;         // subnormal
    first.bus_sequence       = 1;
    DiagnosticRecord record;
    record.verdict     = "evaluated";
    record.reason      = "r\"x";
    record.action      = "none";
    record.window_fill = 3;
    record.window_size = 3;
    record.indicators  = {{"finite", 0.30000000000000004}, {"nan", kNaN}, {"inf", kInf}, {"absent", std::nullopt},
                          {"neg\"zero", -0.0}};
    record.states      = {{"st\"ate", "va\\lue"}};
    first.per_strategy[0].diagnostics = record;
    first.per_strategy[0].signal_ids  = {1};
    replay.events.push_back(first);

    EventResult second = event("A\\B", kNaN, 1'767'623'460'000'000'000LL);   // a NaN price: never written as a number
    second.bus_sequence = 3;
    DiagnosticRecord ignored;
    ignored.verdict = "ignored";
    ignored.reason  = "price_invalid";
    ignored.action  = "none";
    ignored.indicators = {{"short_sma", std::nullopt}};
    second.per_strategy[0].diagnostics = ignored;
    replay.events.push_back(second);

    EventResult third = event("A\\B", std::nullopt, 1'767'623'520'000'000'000LL);   // no price at all
    third.bus_sequence = 4;
    replay.events.push_back(third);   // and no diagnostics for it: shown as unavailable

    SignalRecord signal;
    signal.signal.id           = common::SignalId{1};
    signal.signal.strategy_id  = "s\"1";
    signal.signal.symbol       = "A\\B";
    signal.signal.side         = domain::SignalSide::Buy;
    signal.signal.created_at   = first.input.event.exchange_time;
    signal.signal.requested_quantity = 1.0 / 3.0;
    signal.signal.target_exposure    = kNaN;     // non-finite: omitted, with a status
    signal.signal.confidence         = 0.1 + 0.2;
    signal.signal.order_type         = domain::OrderType::Market;
    signal.signal.metadata           = {{"k\"ey", "v\\al\nue \xF0\x9F\x98\x80"}, {"\xC3\xBCn\xC3\xAF", "\x7F"}};
    signal.event_index  = 0;
    signal.bus_sequence = 2;
    replay.signals.push_back(signal);

    PublicationFailure failure;
    failure.signal      = signal.signal;
    failure.event_index = 0;
    failure.kind        = "rejected";
    failure.message     = "refused \"on purpose\"";
    replay.failures.push_back(failure);

    replay.engine_stats.events_routed     = 18446744073709551615ULL;   // 2^64 - 1: beyond what a double holds
    replay.engine_stats.signals_published = 9007199254740993ULL;       // 2^53 + 1
    replay.bus.market_data_published      = 3;
    return doc;
}

int emit(const std::string& text) {
    set_binary_mode(stdout);
    return write_all(stdout, text) ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "";
    const bool pretty      = argc > 2 && std::string{argv[2]} == "--pretty";

    if (mode == "document" || mode == "locale") {
        if (mode == "locale") {
            std::locale::global(std::locale(std::locale::classic(), new CommaDecimal));
            for (const char* name : {"de_DE.UTF-8", "German_Germany.1252", "fr_FR.UTF-8", "French_France.1252"}) {
                if (std::setlocale(LC_ALL, name) != nullptr) {
                    break;
                }
            }
        }
        RunDocument doc = hostile_document();
        Provenance provenance = current_provenance(std::nullopt);
        return emit(replay_json(doc, provenance, pretty));
    }
    if (mode == "error") {
        const LabError error(ErrorCode::DatasetError, std::string{"bad byte \xFF and a tab\t and a quote \" and "} + "\xC3\xA9",
                             {Problem{7, "pri\xFF" "ce", "strategies[0]", "line\nbreak \x01 and \xE2\x82"}}, 99);
        return emit(error_json(error, pretty));
    }
    std::fputs("usage: strategy_lab_json_probe document|locale|error [--pretty]\n", stderr);
    return 2;
}
