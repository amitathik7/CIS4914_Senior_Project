// The runner with EVENTS built in memory, bypassing the CSV loader. Two things need that:
//  * MALFORMED events (zero, negative, extreme and absent prices; other symbols; non-bars; repeated
//    and older timestamps). A Price is an int64, so it cannot be NaN or infinite. The loader refuses these in a file (strategy_lab_dataset_test.cpp); here
//    they reach the strategies anyway, so the strategies' OWN handling can be observed.
//  * Bus faults, which the runner reports as results (publication failures), not as errors.
// Also run identity, warnings and the errors the session raises.

#include <chrono>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "lab/result_json.hpp"
#include "lab/sha256.hpp"
#include "strategy_lab_test_support.hpp"
#include "support/price_literals.hpp"

namespace {

using namespace lab_test;
using namespace trading_engine::test_support::literals;
using trading_engine::test_support::units;
using Lines = std::vector<std::string>;

using trading_engine::test_support::kMaxPrice;
using trading_engine::test_support::kMinPrice;
using trading_engine::test_support::micros;

const common::Timestamp kBase = common::Timestamp{} + std::chrono::hours{20};

lab::ReplayEvent make_event(const std::string& symbol, std::optional<common::Price> price, long long minute,
                            domain::MarketEventType type = domain::MarketEventType::Bar) {
    lab::ReplayEvent out;
    out.event.symbol        = symbol;
    out.event.type          = type;
    out.event.exchange_time = kBase + std::chrono::minutes{minute};
    out.event.ingest_time   = out.event.exchange_time;
    out.event.price         = price;
    return out;
}

std::vector<lab::BuiltStrategy> sma_and_mr() {
    std::vector<lab::BuiltStrategy> built;
    built.push_back(lab::build_strategy(request("sma_crossover", {{"short_window", "2"}, {"long_window", "3"}, {"symbols", "AAPL"}}), 0));
    built.push_back(lab::build_strategy(request("mean_reversion", {{"lookback", "4"}, {"symbols", "AAPL"}}), 1));
    return built;
}

// ---- malformed events: the strategies' own handling ----------------------------------------------------

TEST(LabReplayMalformed, EveryKindOfBadEventIsIgnoredWithItsOwnReasonAndTheNextGoodBarStillCounts) {
    const std::vector<lab::ReplayEvent> events{
        make_event("AAPL", 10_px, 0),                                             //  0 accepted by both
        make_event("AAPL", 10_px, 1, domain::MarketEventType::Trade),             //  1 not a bar
        make_event("MSFT", 10_px, 1),                                             //  2 not on the allowlist
        make_event("AAPL", std::nullopt, 1),                                      //  3 no price
        make_event("AAPL", kMinPrice, 1),                                         //  4 INT64_MIN
        make_event("AAPL", -kMaxPrice, 1),                                        //  5 -INT64_MAX
        make_event("AAPL", micros(-1), 1),                                         //  6 minus one millionth
        make_event("AAPL", common::Price{}, 1),                                   //  7 zero
        make_event("AAPL", -1_px, 1),                                             //  8 negative
        make_event("AAPL", kMaxPrice, 1),                                         //  9 positive, but above the crossover's ceiling
        make_event("AAPL", 10_px, 0),                                             // 10 repeats event 0's time
        make_event("AAPL", 10_px, 1),                                             // 11 time 1: after the crossover's last bar (0), equal to the reversion's (event 9)
    };
    const lab::ReplayResult result = lab::run_replay(sma_and_mr(), events);

    // Hand-derived. Crossover (2/3): accepts event 0 (fill 1); rejects 9 for its ceiling; event 10 duplicates
    // time 0; event 11 (time 1) is later than 0, so accepted (fill 2). Reversion (lookback 4): accepts event 0
    // and event 9 (it has no ceiling: INT64_MAX is a positive close, time 1); event 10 is older; event 11
    // repeats time 1.
    const Lines want_sma{"warming_up",  "not_a_bar",     "symbol_not_allowlisted", "price_absent",  "price_invalid",
                         "price_invalid", "price_invalid", "price_invalid",         "price_invalid", "price_above_max_close",
                         "time_not_after_last_accepted", "warming_up"};
    const Lines want_mr{"warming_up",  "not_a_bar",     "symbol_not_allowlisted", "price_absent",  "price_invalid",
                        "price_invalid", "price_invalid", "price_invalid",         "price_invalid", "warming_up",
                        "time_not_after_last_accepted", "time_not_after_last_accepted"};
    ASSERT_EQ(result.events.size(), events.size());
    for (std::size_t i = 0; i < events.size(); ++i) {
        ASSERT_TRUE(result.events[i].per_strategy[0].diagnostics.has_value()) << i;
        EXPECT_EQ(result.events[i].per_strategy[0].diagnostics->reason, want_sma[i]) << "crossover, event " << i;
        EXPECT_EQ(result.events[i].per_strategy[1].diagnostics->reason, want_mr[i]) << "reversion, event " << i;
    }
    EXPECT_EQ(result.events[11].per_strategy[0].diagnostics->window_fill, std::optional<std::size_t>{2});
    EXPECT_EQ(result.events[9].per_strategy[1].diagnostics->window_fill, std::optional<std::size_t>{2});
    EXPECT_TRUE(result.signals.empty());

    // The engine routed every one of them and counted nothing as a failure.
    EXPECT_EQ(result.engine_stats.events_routed, events.size());
    EXPECT_EQ(result.engine_stats.strategy_errors, 0u);
    EXPECT_EQ(result.engine_stats.events_dropped, 0u);
}

TEST(LabReplayMalformed, AMalformedEventNeverPoisonsALaterWindow) {
    // After a burst of garbage, the documented crossover (3 2 1 2 3 on a 2/3 pair) still buys on its fifth GOOD bar.
    std::vector<lab::ReplayEvent> events;
    long long minute = 0;
    for (const common::Price close : units({3, 2})) {
        events.push_back(make_event("AAPL", close, ++minute));
        events.push_back(make_event("AAPL", kMinPrice, minute));    // same time: ignored
        events.push_back(make_event("AAPL", -kMaxPrice, ++minute));
        events.push_back(make_event("AAPL", -5_px, minute));
    }
    for (const common::Price close : units({1, 2, 3})) {
        events.push_back(make_event("AAPL", close, ++minute));
    }
    std::vector<lab::BuiltStrategy> built;
    built.push_back(lab::build_strategy(request("sma_crossover", {{"short_window", "2"}, {"long_window", "3"}, {"symbols", "AAPL"}}), 0));
    const lab::ReplayResult result = lab::run_replay(std::move(built), events);

    // The accepted bars are 3 (minute 1), 2 (minute 3), 1 (5), 2 (6), 3 (7): the documented 3 2 1 2 3.
    // Every garbage event has an invalid price, so it is ignored before the time check and cannot
    // advance the strategy's last-seen time or touch its windows.
    ASSERT_EQ(result.signals.size(), 1u);
    EXPECT_EQ(result.signals[0].signal.side, domain::SignalSide::Buy);
    EXPECT_EQ(result.signals[0].signal.metadata.at("short_sma"), "2.5");
    EXPECT_EQ(result.signals[0].signal.metadata.at("long_sma"), "2");
}

TEST(LabReplayMalformed, ExtremePricesSerializeAsTheirExactDecimalTextNeverThroughADouble) {
    // Every price is an int64 count of 1e-6, written digit for digit: the extremes keep every digit
    // (a double would round 9223372036854.775807), and nothing is ever NaN, infinite or an exponent.
    const std::vector<lab::ReplayEvent> events{make_event("AAPL", kMinPrice, 0), make_event("AAPL", kMaxPrice, 1),
                                               make_event("AAPL", -kMaxPrice, 2), make_event("AAPL", micros(1), 3),
                                               make_event("AAPL", common::Price{}, 4), make_event("AAPL", 150.02_px, 5),
                                               make_event("AAPL", std::nullopt, 6)};
    lab::RunDocument doc;
    doc.input.name   = "in-memory";
    doc.input.sha256 = std::string(64, '0');
    doc.replay       = lab::run_replay(sma_and_mr(), events);
    doc.run_id       = lab::make_run_id(doc);

    const std::string text = lab::result_json(doc);
    for (const char* exact : {"\"price\":-9223372036854.775808", "\"price\":9223372036854.775807",
                              "\"price\":-9223372036854.775807", "\"price\":0.000001", "\"price\":0,",
                              "\"price\":150.02,"}) {
        EXPECT_NE(text.find(exact), std::string::npos) << exact;
    }
    EXPECT_EQ(text.find("price_status"), std::string::npos) << "an integer price is never non-finite";
    for (const char* forbidden : {"NaN", "nan,", "Infinity", ":inf", ":-inf", "null"}) {
        EXPECT_EQ(text.find(forbidden), std::string::npos) << forbidden;
    }
}

// ---- publication failures are results -----------------------------------------------------------------------

lab::RunDocument crossover_with_fault(lab::SynchronousDemoBus::Fault fault) {
    lab::SessionOptions options;
    options.fault = fault;
    return run_fixture("sma_crossover.csv", {request("sma_crossover", {{"strategy_id", "sma_2_3"}, {"short_window", "2"},
                                                                       {"long_window", "3"}, {"symbols", "AAPL"}})},
                       options);
}

TEST(LabReplayFaults, ARefusedSignalIsReportedLostWithItsConsumedIdAndNeverResent) {
    const lab::RunDocument doc = crossover_with_fault(lab::SynchronousDemoBus::Fault::RejectSignals);
    const lab::RunDocument clean = crossover_with_fault(lab::SynchronousDemoBus::Fault::None);

    EXPECT_TRUE(doc.replay.signals.empty()) << "nothing was published";
    ASSERT_EQ(doc.replay.failures.size(), 2u) << "the two crossovers, each offered exactly once";
    EXPECT_EQ(doc.replay.failures.size(), clean.replay.signals.size()) << "state was committed: no re-offer on later bars";
    EXPECT_EQ(doc.replay.failures[0].kind, "rejected");
    EXPECT_EQ(doc.replay.failures[0].signal.id.value, 1u) << "the id the engine stamped is consumed";
    EXPECT_EQ(doc.replay.failures[1].signal.id.value, 2u);
    EXPECT_EQ(doc.replay.failures[0].event_index, 4u);
    EXPECT_EQ(doc.replay.failures[0].signal.side, domain::SignalSide::Buy);
    EXPECT_EQ(doc.replay.failures[0].signal.metadata, clean.replay.signals[0].signal.metadata);

    EXPECT_EQ(doc.replay.engine_stats.signals_rejected, 2u);
    EXPECT_EQ(doc.replay.engine_stats.signals_published, 0u);
    EXPECT_EQ(doc.replay.engine_stats.publish_errors, 0u);
    EXPECT_EQ(doc.replay.engine_stats.strategy_errors, 0u) << "a bus fault is not the strategy's";
    EXPECT_EQ(doc.replay.bus.fault, "reject-signals");
    EXPECT_EQ(doc.replay.bus.signals_refused_by_fault, 2u);
    EXPECT_EQ(doc.replay.bus.market_data_published, 9u) << "market data is never faulted";
    EXPECT_TRUE(doc.replay.events[4].per_strategy[0].signal_ids.empty()) << "no published signal on that event";
    // The decisions are unaffected by what the bus did with the signals.
    EXPECT_EQ(reasons(doc, 0), reasons(clean, 0));
}

TEST(LabReplayFaults, APublishThatThrowsIsCountedAsAPublishErrorAndTheRunContinues) {
    const lab::RunDocument doc = crossover_with_fault(lab::SynchronousDemoBus::Fault::ThrowOnSignal);
    ASSERT_EQ(doc.replay.failures.size(), 2u);
    EXPECT_EQ(doc.replay.failures[0].kind, "publish_threw");
    EXPECT_EQ(doc.replay.engine_stats.publish_errors, 2u);
    EXPECT_EQ(doc.replay.engine_stats.signals_rejected, 0u);
    EXPECT_EQ(doc.replay.engine_stats.strategy_errors, 0u);
    EXPECT_EQ(doc.replay.strategies[0].stats.last_error.rfind("publish: ", 0), 0u) << doc.replay.strategies[0].stats.last_error;
    EXPECT_EQ(doc.replay.events.size(), 9u) << "every bar was still replayed";
}

// ---- run identity -----------------------------------------------------------------------------------

TEST(LabReplayIdentity, TheRunIdIsContentAddressedAndSeparatesDifferentInputs) {
    const auto id_of = [](const std::string& fixture, const std::string& long_window, const std::string& symbol = "AAPL") {
        return run_fixture(fixture, {request("sma_crossover", {{"short_window", "2"}, {"long_window", long_window},
                                                              {"symbols", symbol}})}).run_id;
    };
    const std::string base = id_of("sma_crossover.csv", "3");
    EXPECT_EQ(base.size(), 16u);
    EXPECT_EQ(base.find_first_not_of("0123456789abcdef"), std::string::npos);
    EXPECT_EQ(base, id_of("sma_crossover.csv", "3")) << "reproducible";
    EXPECT_NE(base, id_of("sma_crossover.csv", "4")) << "a different parameter";
    EXPECT_NE(base, id_of("sma_oscillation.csv", "3")) << "a different dataset";
    EXPECT_NE(base, id_of("sma_crossover.csv", "3", "AAPL,MSFT")) << "a different allowlist";

    lab::SessionOptions faulty;
    faulty.fault = lab::SynchronousDemoBus::Fault::RejectSignals;
    const lab::RunDocument with_fault = run_fixture(
        "sma_crossover.csv", {request("sma_crossover", {{"short_window", "2"}, {"long_window", "3"}, {"symbols", "AAPL"}})}, faulty);
    EXPECT_NE(base, with_fault.run_id) << "a bus fault is part of the run's identity";
}

TEST(LabReplayIdentity, ResultsCarryNoWallClockAndProvenanceIsSeparate) {
    const lab::RunDocument doc = run_fixture("sma_crossover.csv", {request("sma_crossover", {{"short_window", "2"}, {"long_window", "3"}, {"symbols", "AAPL"}})});
    const std::string result = lab::result_json(doc);
    EXPECT_EQ(result.find("generated_at"), std::string::npos);
    EXPECT_EQ(result, lab::result_json(doc));

    const lab::Provenance with_time = lab::current_provenance(std::string{"2030-01-01T00:00:00.000000000Z"});
    const lab::Provenance without   = lab::current_provenance(std::nullopt);
    const std::string a = lab::replay_json(doc, with_time, false);
    const std::string b = lab::replay_json(doc, without, false);
    EXPECT_NE(a, b);
    EXPECT_NE(a.find("\"generated_at\":\"2030-01-01T00:00:00.000000000Z\""), std::string::npos);
    EXPECT_EQ(b.find("generated_at"), std::string::npos);
    // ... yet both embed the same result and the same hash of it.
    const std::string hash = "\"result_sha256\":\"" + lab::sha256_hex(result) + "\"";
    EXPECT_NE(a.find(hash), std::string::npos);
    EXPECT_NE(b.find(hash), std::string::npos);
    EXPECT_EQ(a.substr(a.find("\"result\":") + 9), b.substr(b.find("\"result\":") + 9));
}

TEST(LabReplayIdentity, EverySignalIsScopedToItsRun) {
    const lab::RunDocument doc = run_fixture("sma_crossover.csv", {request("sma_crossover", {{"short_window", "2"}, {"long_window", "3"}, {"symbols", "AAPL"}})});
    const std::string text = lab::result_json(doc);
    EXPECT_NE(text.find("\"signal_ref\":\"" + doc.run_id + ":1\""), std::string::npos);
    EXPECT_NE(text.find("\"signal_ref\":\"" + doc.run_id + ":2\""), std::string::npos);
}

// ---- warnings and session errors ----------------------------------------------------------------------

std::vector<std::string> warning_codes(const lab::RunDocument& doc) {
    std::vector<std::string> out;
    for (const lab::Warning& w : doc.warnings) {
        out.push_back(w.code);
    }
    return out;
}

TEST(LabReplayWarnings, FactsAboutTheMatchBetweenDataAndConfigurationAreReportedNotRepaired) {
    // The data has AAPL and MSFT; the strategy trades 'aapl' (wrong case) and TSLA.
    const lab::RunDocument doc = run_fixture(
        "two_symbols_interleaved.csv", {request("sma_crossover", {{"short_window", "2"}, {"long_window", "3"}, {"symbols", "aapl,TSLA"}})});
    EXPECT_EQ(warning_codes(doc), (std::vector<std::string>{"dataset_symbol_not_allowlisted", "dataset_symbol_not_allowlisted",
                                                           "allowlisted_symbol_not_in_dataset", "allowlisted_symbol_not_in_dataset"}));
    EXPECT_TRUE(doc.replay.signals.empty()) << "nothing is traded: the case mismatch is not silently fixed";
    EXPECT_EQ(reasons(doc, 0).front(), "symbol_not_allowlisted");

    const lab::RunDocument unreachable = run_fixture(
        "sma_crossover.csv", {request("mean_reversion", {{"lookback", "4"}, {"entry_threshold", "5"}, {"symbols", "AAPL"}})});
    EXPECT_EQ(warning_codes(unreachable), (std::vector<std::string>{"entry_threshold_unreachable"}));
}

TEST(LabReplayErrors, ADuplicateStrategyIdIsAConfigErrorFromTheEngineItself) {
    const lab::LabError error = expect_lab_error([] {
        (void)run_fixture("sma_crossover.csv", {request("sma_crossover", {{"symbols", "AAPL"}}), request("sma_crossover", {{"symbols", "AAPL"}})});
    });
    EXPECT_EQ(error.code(), lab::ErrorCode::ConfigError);
    EXPECT_NE(std::string{error.what()}.find("duplicate strategy id 'sma_crossover'"), std::string::npos) << error.what();
    ASSERT_EQ(error.problems().size(), 1u);
    EXPECT_EQ(error.problems()[0].where, "strategies[1]");
}

TEST(LabReplayErrors, AnEmptyEventListIsAValidReplayOfNothing) {
    const lab::ReplayResult result = lab::run_replay(sma_and_mr(), {});
    EXPECT_TRUE(result.events.empty());
    EXPECT_TRUE(result.signals.empty());
    EXPECT_EQ(result.engine_stats.events_routed, 0u);
    EXPECT_EQ(result.strategies.size(), 2u);
}

}  // namespace
