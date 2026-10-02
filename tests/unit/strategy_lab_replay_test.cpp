// Replays of the checked-in fixtures through the REAL StrategyEngine and strategies, against
// expectations derived by hand from the strategy documents and re-derived independently of
// the code under test (see the derivations beside each case). Nothing here is a snapshot
// of the implementation's own output.
//
// The CSV loader's rules are in strategy_lab_dataset_test.cpp; how a malformed event is handled
// is in strategy_lab_replay_events_test.cpp.
//
// Times: every fixture starts at 2026-01-05T14:30:00Z with one bar per minute, so bar k
// (counting from 1) is at 14:29 + k minutes and event index k-1 for a single-symbol file.

#include <cmath>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "lab/result_json.hpp"
#include "strategy_lab_test_support.hpp"

namespace {

using namespace lab_test;
using Lines = std::vector<std::string>;

lab::StrategyRequest sma(const std::string& id, const std::string& short_window, const std::string& long_window,
                         const std::string& symbols = "AAPL") {
    return request("sma_crossover", {{"strategy_id", id}, {"short_window", short_window},
                                     {"long_window", long_window}, {"symbols", symbols}});
}

lab::StrategyRequest mr(const std::string& id, const std::string& lookback, const std::string& entry,
                        const std::string& rearm, const std::string& symbols = "AAPL") {
    return request("mean_reversion", {{"strategy_id", id}, {"lookback", lookback}, {"entry_threshold", entry},
                                      {"rearm_threshold", rearm}, {"symbols", symbols}});
}

// ---- crossover ------------------------------------------------------------------------------------

TEST(LabReplayCrossover, TheDocumentedFixtureBuysAtBarFiveAndSellsAtBarEight) {
    // closes 3 2 1 2 3 4 3 2 1, 2/3 (docs/strategies/moving_average_crossover.md section 5; worked by hand:
    // bar 3 short 1.5 long 2 below = baseline; bar 5 short 2.5 long 2 above = Buy; bar 8 short 2.5 long 3 = Sell).
    const lab::RunDocument doc = run_fixture("sma_crossover.csv", {sma("sma_2_3", "2", "3")});
    expect_run_invariants(doc);

    EXPECT_EQ(signal_lines(doc), (Lines{"sma_2_3 AAPL buy event=4 id=1", "sma_2_3 AAPL sell event=7 id=2"}));
    EXPECT_EQ(reasons(doc, 0),
              (Lines{"warming_up", "warming_up", "baseline_established", "same_side", "crossover_buy", "same_side",
                     "same_side", "crossover_sell", "same_side"}));

    const domain::TradeSignal& buy  = doc.replay.signals[0].signal;
    const domain::TradeSignal& sell = doc.replay.signals[1].signal;
    EXPECT_EQ(timestamp_text(buy.created_at), "2026-01-05T14:34:00.000000000Z");
    EXPECT_EQ(timestamp_text(sell.created_at), "2026-01-05T14:37:00.000000000Z");
    EXPECT_EQ(buy.requested_quantity, std::optional<double>{1.0});
    EXPECT_EQ(buy.order_type, std::optional<domain::OrderType>{domain::OrderType::Market});
    EXPECT_FALSE(buy.confidence.has_value()) << "the strategies set none and the lab invents none";
    EXPECT_FALSE(buy.target_exposure.has_value());
    EXPECT_FALSE(buy.limit_price.has_value());
    EXPECT_EQ(buy.metadata, (std::map<std::string, std::string>{{"long_sma", "2"}, {"long_window", "3"},
                                                                 {"short_sma", "2.5"}, {"short_window", "2"},
                                                                 {"trigger", "short_crossed_above_long"}}));
    EXPECT_EQ(sell.metadata.at("short_sma"), "2.5");
    EXPECT_EQ(sell.metadata.at("long_sma"), "3");
    EXPECT_EQ(sell.metadata.at("trigger"), "short_crossed_below_long");

    // Warm-up boundary: the window fills at bar 3 (fill 3 of 3), the first bar with numbers.
    const auto& e1 = doc.replay.events[1].per_strategy[0].diagnostics;
    const auto& e2 = doc.replay.events[2].per_strategy[0].diagnostics;
    EXPECT_EQ(e1->window_fill, std::optional<std::size_t>{2});
    EXPECT_EQ(e1->verdict, "warming_up");
    EXPECT_FALSE(e1->indicators[0].second.has_value());
    EXPECT_EQ(e2->window_fill, std::optional<std::size_t>{3});
    EXPECT_EQ(e2->verdict, "evaluated");
    EXPECT_DOUBLE_EQ(*e2->indicators[0].second, 1.5);
    EXPECT_DOUBLE_EQ(*e2->indicators[1].second, 2.0);

    // The engine's own counters, and the signals each event produced.
    EXPECT_EQ(doc.replay.engine_stats.signals_published, 2u);
    EXPECT_EQ(doc.replay.engine_stats.signals_rejected, 0u);
    EXPECT_EQ(doc.replay.events[4].per_strategy[0].signal_ids, (std::vector<std::uint64_t>{1}));
    EXPECT_TRUE(doc.replay.events[3].per_strategy[0].signal_ids.empty()) << "no hold signal is ever manufactured";
}

TEST(LabReplayCrossover, AnAlternatingSeriesSignalsOnEveryBarFromTheThirdAndNeverBefore) {
    // closes 1 2 1 2 ..., short 1 / long 2: short = the close, long = mean of the last two.
    // Bar 2 short 2 > long 1.5 (the baseline); bar 3 short 1 < 1.5 Sell; bar 4 Buy; ... every bar flips.
    // The first signal is on accepted bar long_window + 1 = 3 (event index 2).
    const lab::RunDocument doc = run_fixture("sma_oscillation.csv", {sma("whipsaw", "1", "2")});
    expect_run_invariants(doc);
    Lines want;
    for (std::size_t event = 2; event <= 9; ++event) {
        want.push_back("whipsaw AAPL " + std::string{event % 2 == 0 ? "sell" : "buy"} + " event=" + std::to_string(event) +
                       " id=" + std::to_string(event - 1));
    }
    EXPECT_EQ(signal_lines(doc), want);
    EXPECT_EQ(reasons(doc, 0).front(), "warming_up");
    EXPECT_EQ(reasons(doc, 0)[1], "baseline_established");
    EXPECT_EQ(doc.replay.signals[0].signal.metadata.at("short_sma"), "1");
    EXPECT_EQ(doc.replay.signals[0].signal.metadata.at("long_sma"), "1.5");
}

TEST(LabReplayCrossover, ConstantPriceNeverEstablishesABaselineSoNothingEverSignals) {
    // 30 bars of 0.1 (a number that is not exact in binary), defaults 5/20: the window fills at bar 20
    // (event 19). Every full-window bar has equal averages, however the floating-point sums fell.
    const lab::RunDocument doc = run_fixture("constant_price.csv", {request("sma_crossover", {{"symbols", "AAPL"}})});
    expect_run_invariants(doc);
    EXPECT_TRUE(doc.replay.signals.empty());
    Lines want(19, "warming_up");
    want.insert(want.end(), 11, "averages_equal");
    EXPECT_EQ(reasons(doc, 0), want);
    const auto& last = doc.replay.events[29].per_strategy[0].diagnostics;
    EXPECT_EQ(last->states[0].second, "none") << "relation_before";
    EXPECT_EQ(last->states[2].second, "none") << "relation_after: a tie sets no baseline";
}

// ---- mean reversion ---------------------------------------------------------------------------------

TEST(LabReplayMeanReversion, TheDocumentedFixtureBuysRearmsBuysAndFlipsToSell) {
    // closes 10 10 10 6 9 2 30, lookback 4, entry 1.5, rearm 0.5 (docs/strategies/mean_reversion.md section 6):
    //   bar 4 window 10 10 10 6   mean 9     var 3        z = -3/sqrt(3)         = -1.7320508  Buy
    //   bar 5 window 10 10 6 9    mean 8.75  var 2.6875   z = 0.25/sqrt(2.6875)  =  0.1524986  rearm
    //   bar 6 window 10 6 9 2     mean 6.75  var 9.6875   z = -4.75/sqrt(9.6875) = -1.5261167  Buy
    //   bar 7 window 6 9 2 30     mean 11.75 var 117.1875 z = 18.25/sqrt(117.1875)=  1.6858628  Sell (flip)
    const lab::RunDocument doc = run_fixture("mr_rearm.csv", {mr("mr_4", "4", "1.5", "0.5")});
    expect_run_invariants(doc);

    EXPECT_EQ(signal_lines(doc), (Lines{"mr_4 AAPL buy event=3 id=1", "mr_4 AAPL buy event=5 id=2",
                                        "mr_4 AAPL sell event=6 id=3"}));
    EXPECT_EQ(reasons(doc, 0), (Lines{"warming_up", "warming_up", "warming_up", "entry_buy", "inside_rearm_band",
                                      "entry_buy", "entry_sell"}));

    const auto& first = doc.replay.signals[0].signal.metadata;
    EXPECT_EQ(first.at("mean"), "9");
    EXPECT_EQ(first.at("close"), "6");
    EXPECT_EQ(first.at("lookback"), "4");
    EXPECT_EQ(first.at("entry_threshold"), "1.5");
    EXPECT_EQ(first.at("rearm_threshold"), "0.5");
    EXPECT_EQ(first.at("trigger"), "z_score_at_or_below_lower_entry");
    EXPECT_NEAR(parse_double(first.at("z_score")), -std::sqrt(3.0), 1e-12);
    EXPECT_NEAR(parse_double(first.at("standard_deviation")), std::sqrt(3.0), 1e-12);
    EXPECT_NEAR(parse_double(doc.replay.signals[1].signal.metadata.at("z_score")), -4.75 / std::sqrt(9.6875), 1e-12);
    EXPECT_NEAR(parse_double(doc.replay.signals[2].signal.metadata.at("z_score")), 18.25 / std::sqrt(117.1875), 1e-12);
    EXPECT_EQ(doc.replay.signals[2].signal.metadata.at("trigger"), "z_score_at_or_above_upper_entry");

    // Warm-up boundary: bar 3 (fill 3 of 4) has no statistics, bar 4 (fill 4) is the first decision.
    EXPECT_EQ(doc.replay.events[2].per_strategy[0].diagnostics->verdict, "warming_up");
    EXPECT_FALSE(doc.replay.events[2].per_strategy[0].diagnostics->indicators[2].second.has_value());
    EXPECT_EQ(doc.replay.events[3].per_strategy[0].diagnostics->verdict, "evaluated");
    EXPECT_EQ(doc.replay.events[4].per_strategy[0].diagnostics->states[0].second, "lower_extreme") << "latch_before the rearm";
    EXPECT_EQ(doc.replay.events[4].per_strategy[0].diagnostics->states[1].second, "neutral") << "latch_after the rearm";
}

TEST(LabReplayMeanReversion, ARepeatedExcursionIsSuppressedUntilTheLatchRearms) {
    // closes 10 10 10 6 2 1 1 1 1 10, lookback 4, entry 1.5, rearm 0.5 (derived by hand, see the
    // diagnostics test for the windows): Buy at bar 4; bars 5-8 stay beyond or between the bands, so no
    // repeat; bar 9 is a constant window (1 1 1 1) which rearms; Sell at bar 10 (z = 6.75/sqrt(15.1875)).
    const lab::RunDocument doc = run_fixture("mr_suppression.csv", {mr("mr_4", "4", "1.5", "0.5")});
    expect_run_invariants(doc);
    EXPECT_EQ(signal_lines(doc), (Lines{"mr_4 AAPL buy event=3 id=1", "mr_4 AAPL sell event=9 id=2"}));
    EXPECT_EQ(reasons(doc, 0),
              (Lines{"warming_up", "warming_up", "warming_up", "entry_buy", "excursion_already_requested",
                     "between_bands", "between_bands", "between_bands", "constant_window", "entry_sell"}));
    const auto& constant = doc.replay.events[8].per_strategy[0].diagnostics;
    EXPECT_FALSE(constant->indicators[2].second.has_value()) << "z is unavailable for a constant window";
    EXPECT_TRUE(constant->indicators[0].second.has_value()) << "but its mean exists";
}

TEST(LabReplayMeanReversion, ConstantPriceIsAConstantWindowAndNeverSignals) {
    const lab::RunDocument doc = run_fixture("constant_price.csv", {request("mean_reversion", {{"symbols", "AAPL"}})});
    expect_run_invariants(doc);
    EXPECT_TRUE(doc.replay.signals.empty());
    Lines want(19, "warming_up");
    want.insert(want.end(), 11, "constant_window");
    EXPECT_EQ(reasons(doc, 0), want);
}

// ---- warm-up boundaries ------------------------------------------------------------------------------

TEST(LabReplayWarmup, TheFirstFullWindowMayAlreadySignalForMeanReversionButNeverBefore) {
    // closes 10 10 10 10 10 100, lookback 6: the window fills on bar 6. mean 25, population variance
    // (5*15^2 + 75^2)/6 = 1125, z = 75/sqrt(1125) = sqrt(5) = 2.236 >= 2.0: a Sell on the first full window.
    const lab::RunDocument filled = run_fixture("warmup_boundary.csv", {mr("mr_6", "6", "2", "0.5")});
    expect_run_invariants(filled);
    EXPECT_EQ(signal_lines(filled), (Lines{"mr_6 AAPL sell event=5 id=1"}));
    EXPECT_EQ(reasons(filled, 0), (Lines{"warming_up", "warming_up", "warming_up", "warming_up", "warming_up", "entry_sell"}));
    EXPECT_EQ(filled.replay.signals[0].signal.metadata.at("mean"), "25");
    EXPECT_NEAR(parse_double(filled.replay.signals[0].signal.metadata.at("z_score")), std::sqrt(5.0), 1e-12);

    // One bar short: the identical spike is invisible, because the window is not full yet.
    const lab::RunDocument short_one = run_fixture("warmup_boundary.csv", {mr("mr_7", "7", "2", "0.5")});
    EXPECT_TRUE(short_one.replay.signals.empty());
    EXPECT_EQ(reasons(short_one, 0), Lines(6, "warming_up"));
    EXPECT_EQ(short_one.replay.events[5].per_strategy[0].diagnostics->window_fill, std::optional<std::size_t>{6});
    EXPECT_EQ(short_one.replay.events[5].per_strategy[0].diagnostics->window_size, 7u);
    ASSERT_EQ(short_one.warnings.size(), 1u);
    EXPECT_EQ(short_one.warnings[0].code, "fewer_bars_than_warmup");
}

TEST(LabReplayWarmup, TheCrossoverSwallowsItsFirstComparisonEvenWhenItIsDramatic) {
    // 2/6 on the same data: bar 6 has short (10+100)/2 = 55 above long (50+100)/6 = 25, but it is only the
    // baseline. Unlike mean reversion there is no signal on the first full window.
    const lab::RunDocument doc = run_fixture("warmup_boundary.csv", {sma("sma_2_6", "2", "6")});
    expect_run_invariants(doc);
    EXPECT_TRUE(doc.replay.signals.empty());
    EXPECT_EQ(reasons(doc, 0), (Lines{"warming_up", "warming_up", "warming_up", "warming_up", "warming_up",
                                      "baseline_established"}));
    EXPECT_DOUBLE_EQ(*doc.replay.events[5].per_strategy[0].diagnostics->indicators[0].second, 55.0);
    EXPECT_DOUBLE_EQ(*doc.replay.events[5].per_strategy[0].diagnostics->indicators[1].second, 25.0);

    const lab::RunDocument too_few = run_fixture("warmup_boundary.csv", {sma("sma_2_7", "2", "7")});
    EXPECT_EQ(reasons(too_few, 0), Lines(6, "warming_up"));
}

// ---- trades are ignored, bars decide ------------------------------------------------------------------

TEST(LabReplayBarsAndTrades, TradeRowsAreIgnoredAndLeaveTheWindowsAlone) {
    // events: bar 10, trade, bar 11, trade, bar 10; crossover 1/2: bar 1 warms up, bar 2 (11) has short 11
    // above long 10.5 = baseline, bar 3 (10) short 10 below long 10.5 = Sell, on event index 4.
    const lab::RunDocument doc = run_fixture("bars_and_trades.csv", {sma("sma_1_2", "1", "2")});
    expect_run_invariants(doc);
    EXPECT_EQ(reasons(doc, 0), (Lines{"warming_up", "not_a_bar", "baseline_established", "not_a_bar", "crossover_sell"}));
    EXPECT_EQ(signal_lines(doc), (Lines{"sma_1_2 AAPL sell event=4 id=1"}));
    EXPECT_FALSE(doc.replay.events[1].per_strategy[0].diagnostics->window_fill.has_value()) << "never reached a symbol's state";
    EXPECT_EQ(doc.replay.events[2].per_strategy[0].diagnostics->window_fill, std::optional<std::size_t>{2})
        << "the trade added nothing to the window";
}

// ---- two symbols, several strategies --------------------------------------------------------------------------

lab::RunDocument interleaved(std::vector<lab::StrategyRequest> requests) {
    return run_fixture("two_symbols_interleaved.csv", requests);
}

TEST(LabReplayCombined, BothStrategiesOnTwoInterleavedSymbolsPublishEightSignalsInRegistrationOrder) {
    // AAPL 3 2 1 2 3 4 3 2 1 and MSFT 10 10 10 6 9 2 30 30 30, one row of each per minute (AAPL first), so
    // minute m has event indices 2(m-1) for AAPL and 2(m-1)+1 for MSFT. Derived by hand:
    //   sma_2_3: AAPL Buy minute 5, Sell minute 8; MSFT Buy minute 7.
    //   mr_4 (entry 1.2): AAPL Sell 5 (z 1.414), Buy 8; MSFT Buy 4 (-1.732), Buy 6 (-1.526), Sell 7 (1.686).
    // Within a minute the engine delivers to sma_2_3 first (registered first).
    const lab::RunDocument doc = interleaved({sma("sma_2_3", "2", "3", "AAPL,MSFT"), mr("mr_4", "4", "1.2", "0.5", "AAPL,MSFT")});
    expect_run_invariants(doc);
    EXPECT_EQ(signal_lines(doc), (Lines{
        "mr_4 MSFT buy event=7 id=1",
        "sma_2_3 AAPL buy event=8 id=2",   "mr_4 AAPL sell event=8 id=3",
        "mr_4 MSFT buy event=11 id=4",
        "sma_2_3 MSFT buy event=13 id=5",  "mr_4 MSFT sell event=13 id=6",
        "sma_2_3 AAPL sell event=14 id=7", "mr_4 AAPL buy event=14 id=8"}));
    EXPECT_EQ(doc.replay.engine_stats.signals_published, 8u);

    // Spot checks of per-bar reasons that only the hand calculation can know.
    EXPECT_EQ(doc.replay.events[5].per_strategy[0].diagnostics->reason, "averages_equal") << "MSFT 10 10 10: equal averages";
    EXPECT_EQ(doc.replay.events[7].per_strategy[1].diagnostics->reason, "entry_buy") << "MSFT bar 4, z -1.732";
}

TEST(LabReplayCombined, RegistrationOrderIsDeliveryOrderSoSwappingThemSwapsTheIdsWithinAnEvent) {
    const lab::RunDocument doc = interleaved({mr("mr_4", "4", "1.2", "0.5", "AAPL,MSFT"), sma("sma_2_3", "2", "3", "AAPL,MSFT")});
    expect_run_invariants(doc);
    const Lines lines = signal_lines(doc);
    ASSERT_EQ(lines.size(), 8u);
    EXPECT_EQ(lines[1], "mr_4 AAPL sell event=8 id=2") << "mr_4 now goes first at AAPL's minute 5";
    EXPECT_EQ(lines[2], "sma_2_3 AAPL buy event=8 id=3");
}

TEST(LabReplayCombined, EachSymbolIsDecidedOnItsOwnWhateverTheOtherOneDoes) {
    // Run each symbol alone (the same bars, in the same order, with the other symbol's rows removed) and
    // check that the combined run's signals for that symbol are exactly the lone run's.
    const lab::Dataset all = lab::load_dataset(fixture_path("two_symbols_interleaved.csv"));
    const std::vector<lab::StrategyRequest> requests{sma("sma_2_3", "2", "3", "AAPL,MSFT"),
                                                     mr("mr_4", "4", "1.2", "0.5", "AAPL,MSFT")};
    const lab::RunDocument combined = lab::run_session(all, requests);

    for (const std::string symbol : {"AAPL", "MSFT"}) {
        lab::Dataset only = all;
        only.rows.clear();
        for (const lab::DatasetRow& row : all.rows) {
            if (row.event.symbol == symbol) {
                only.rows.push_back(row);
            }
        }
        const lab::RunDocument alone = lab::run_session(only, requests);
        std::vector<std::string> want;
        for (const auto& record : alone.replay.signals) {
            want.push_back(record.signal.strategy_id + " " + std::string{domain::to_string(record.signal.side)} + " " +
                           timestamp_text(record.signal.created_at));
        }
        std::vector<std::string> got;
        for (const auto& record : combined.replay.signals) {
            if (record.signal.symbol == symbol) {
                got.push_back(record.signal.strategy_id + " " + std::string{domain::to_string(record.signal.side)} + " " +
                              timestamp_text(record.signal.created_at));
            }
        }
        EXPECT_FALSE(want.empty()) << symbol;
        EXPECT_EQ(got, want) << symbol << " must not be affected by the other symbol's rows";
    }
}

// ---- equal timestamps across symbols: the lab-only file-order policy ------------------------------------------------

TEST(LabReplayTies, SignalsOnTheSameTimestampFollowTheFilesOrderNotTheAlphabet) {
    // SMA 1/2 on closes 1 2 1 for both symbols, a row of each per minute. Bar 3 (event 4 and 5) makes both
    // sell on the SAME timestamp. Only the file's row order says who goes first.
    const auto sells = [](const std::string& fixture) {
        const lab::RunDocument doc = run_fixture(fixture, {sma("s", "1", "2", "AAPL,MSFT")});
        expect_run_invariants(doc);
        return doc;
    };
    const lab::RunDocument aapl_first = sells("tie_aapl_first.csv");
    const lab::RunDocument msft_first = sells("tie_msft_first.csv");

    EXPECT_EQ(signal_lines(aapl_first), (Lines{"s AAPL sell event=4 id=1", "s MSFT sell event=5 id=2"}));
    EXPECT_EQ(signal_lines(msft_first), (Lines{"s MSFT sell event=4 id=1", "s AAPL sell event=5 id=2"}));
    EXPECT_EQ(aapl_first.replay.signals[0].signal.created_at, aapl_first.replay.signals[1].signal.created_at)
        << "the two signals really do carry one timestamp";
    EXPECT_EQ(aapl_first.replay.events[1].input.event.exchange_time, aapl_first.replay.events[0].input.event.exchange_time);
}

// ---- reset and repeatability ------------------------------------------------------------------------------

TEST(LabReplayRepeatability, TheSameRequestGivesTheSameResultAndFreshStrategiesAreColdEveryRun) {
    const std::vector<lab::StrategyRequest> requests{sma("sma_2_3", "2", "3", "AAPL,MSFT"),
                                                     mr("mr_4", "4", "1.2", "0.5", "AAPL,MSFT")};
    const lab::RunDocument first  = run_fixture("two_symbols_interleaved.csv", requests);
    const lab::RunDocument second = run_fixture("two_symbols_interleaved.csv", requests);
    EXPECT_EQ(lab::result_json(first), lab::result_json(second)) << "byte-identical deterministic output";
    EXPECT_EQ(first.run_id, second.run_id);
    EXPECT_EQ(signal_lines(first), signal_lines(second));
    EXPECT_EQ(signal_lines(second).front(), "mr_4 MSFT buy event=7 id=1") << "ids restart at 1 in every run";
}

TEST(LabReplayRepeatability, StrategiesReusedAcrossTwoRunsAreResetByTheEnginesStart) {
    // The same BuiltStrategy objects (and so the same strategy state) run twice. on_start() clears the windows,
    // so the second run must equal the first: no baseline, window or latch carries over.
    const lab::Dataset dataset = lab::load_dataset(fixture_path("two_symbols_interleaved.csv"));
    std::vector<lab::ReplayEvent> events;
    for (const lab::DatasetRow& row : dataset.rows) {
        events.push_back(lab::ReplayEvent{row.event, row.line});
    }
    std::vector<lab::BuiltStrategy> built;
    built.push_back(lab::build_strategy(sma("sma_2_3", "2", "3", "AAPL,MSFT"), 0));
    built.push_back(lab::build_strategy(mr("mr_4", "4", "1.2", "0.5", "AAPL,MSFT"), 1));

    const lab::ReplayResult first  = lab::run_replay(built, events);
    const lab::ReplayResult second = lab::run_replay(built, events);

    ASSERT_EQ(first.signals.size(), 8u);
    ASSERT_EQ(second.signals.size(), first.signals.size());
    for (std::size_t i = 0; i < first.signals.size(); ++i) {
        EXPECT_EQ(first.signals[i].signal.id, second.signals[i].signal.id);
        EXPECT_EQ(first.signals[i].signal.metadata, second.signals[i].signal.metadata);
        EXPECT_EQ(first.signals[i].event_index, second.signals[i].event_index);
    }
    for (std::size_t e = 0; e < first.events.size(); ++e) {
        for (std::size_t s = 0; s < 2; ++s) {
            EXPECT_EQ(first.events[e].per_strategy[s].diagnostics->reason, second.events[e].per_strategy[s].diagnostics->reason);
        }
    }
}

// ---- everything at once --------------------------------------------------------------------------------------

TEST(LabReplayInvariants, EveryFixtureUnderBothStrategiesSatisfiesTheGenericInvariants) {
    for (const char* name : {"sma_crossover.csv", "sma_oscillation.csv", "mr_rearm.csv", "mr_suppression.csv",
                             "constant_price.csv", "warmup_boundary.csv", "two_symbols_interleaved.csv",
                             "tie_aapl_first.csv", "tie_msft_first.csv", "bars_and_trades.csv", "ohlcv_example.csv",
                             "precision_edge.csv"}) {
        SCOPED_TRACE(name);
        const lab::RunDocument doc = run_fixture(
            name, {request("sma_crossover", {{"strategy_id", "a"}, {"short_window", "1"}, {"long_window", "2"}, {"symbols", "AAPL,MSFT"}}),
                   request("mean_reversion", {{"strategy_id", "b"}, {"lookback", "3"}, {"entry_threshold", "1"}, {"rearm_threshold", "0.25"},
                                              {"symbols", "AAPL,MSFT"}})});
        expect_run_invariants(doc);
    }
}

}  // namespace
