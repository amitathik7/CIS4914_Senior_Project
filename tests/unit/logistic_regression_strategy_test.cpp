// LogisticRegressionStrategy and its building blocks (logistic_model.hpp), driven
// directly: the features, the standardizer and one SGD step against hand-computed
// numbers; then the strategy's config checks, input policy, warm-up, latch, signals,
// learning, and the two properties a reviewer will ask about: no look-ahead and
// determinism. The engine-level tests are in logistic_regression_engine_test.cpp.

#include <cmath>
#include <cstddef>
#include <limits>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "logistic_regression_fixture.hpp"
#include "trading_engine/common/errors.hpp"
#include "trading_engine/strategy/logistic_model.hpp"

namespace {

using lr_test::bar;
using lr_test::Closes;
using lr_test::Harness;
using lr_test::make_config;
using lr_test::micros;
using lr_test::units;
namespace common   = trading_engine::common;
namespace domain   = trading_engine::domain;
namespace strategy = trading_engine::strategy;
namespace ml       = trading_engine::strategy::ml;

constexpr double kTight = 1e-12;

// --- Features ----------------------------------------------------------------------

TEST(LogisticFeatures, NeedTwentyOneCloses) {
    const Closes twenty = units({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20});
    EXPECT_FALSE(ml::compute_features(twenty).has_value());
}

TEST(LogisticFeatures, MatchHandComputedValuesOnALinearSeries) {
    // Closes 1..21: the newest is 21.
    const Closes closes = units({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21});
    const auto f = ml::compute_features(closes);
    ASSERT_TRUE(f.has_value());
    EXPECT_NEAR((*f)[0], std::log(21.0 / 20.0), kTight);
    EXPECT_NEAR((*f)[1], std::log(21.0 / 16.0), kTight);
    EXPECT_NEAR((*f)[2], std::log(21.0 / 1.0), kTight);
    // Closes 2..21: mean 11.5, population variance (20^2 - 1) / 12.
    EXPECT_NEAR((*f)[3], (21.0 - 11.5) / std::sqrt(399.0 / 12.0), 1e-9);
    // Mean of 17..21 is 19.
    EXPECT_NEAR((*f)[4], 19.0 / 11.5 - 1.0, 1e-12);
}

TEST(LogisticFeatures, ADoublingSeriesHasEqualReturnsAndNoVolatility) {
    Closes closes;
    for (int i = 0; i <= 20; ++i) {
        closes.push_back(common::Price::from_units(std::int64_t{1} << i));
    }
    const auto f = ml::compute_features(closes);
    ASSERT_TRUE(f.has_value());
    EXPECT_NEAR((*f)[0], std::log(2.0), kTight);
    EXPECT_NEAR((*f)[1], 5.0 * std::log(2.0), kTight);
    EXPECT_NEAR((*f)[2], 20.0 * std::log(2.0), kTight);
    EXPECT_NEAR((*f)[5], 0.0, 1e-12);
}

TEST(LogisticFeatures, AConstantSeriesIsAllZero) {
    const Closes closes(21, common::Price::from_units(50));
    const auto f = ml::compute_features(closes);
    ASSERT_TRUE(f.has_value());
    for (const double value : *f) {
        EXPECT_EQ(value, 0.0);
    }
}

TEST(LogisticFeatures, UseOnlyTheLastTwentyOneCloses) {
    Closes longer = units({999, 1});
    const Closes tail = units({1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21});
    longer.insert(longer.end(), tail.begin() + 1, tail.end());
    EXPECT_EQ(ml::compute_features(longer), ml::compute_features(tail));
}

// --- Standardizer and model --------------------------------------------------------

TEST(LogisticStandardizer, ScalesWithWhatItSawBeforeAndClips) {
    ml::RunningStandardizer s;
    const ml::FeatureVector one{1, 1, 1, 1, 1, 1};
    const ml::FeatureVector three{3, 3, 3, 3, 3, 3};
    EXPECT_EQ(s.scale(three), ml::FeatureVector{}) << "nothing observed yet";
    s.observe(one);
    EXPECT_EQ(s.scale(three), ml::FeatureVector{}) << "one observation has no spread";
    s.observe(three);   // mean 2, population stddev 1
    const ml::FeatureVector scaled = s.scale(ml::FeatureVector{4, 0, 2, 100, -100, 2.5});
    EXPECT_DOUBLE_EQ(scaled[0], 2.0);
    EXPECT_DOUBLE_EQ(scaled[1], -2.0);
    EXPECT_DOUBLE_EQ(scaled[2], 0.0);
    EXPECT_DOUBLE_EQ(scaled[3], ml::RunningStandardizer::kClip);
    EXPECT_DOUBLE_EQ(scaled[4], -ml::RunningStandardizer::kClip);
    EXPECT_DOUBLE_EQ(scaled[5], 0.5);
    s.reset();
    EXPECT_EQ(s.count(), 0u);
}

TEST(LogisticModel, SigmoidIsStableAtTheExtremes) {
    EXPECT_EQ(ml::sigmoid(0.0), 0.5);
    EXPECT_EQ(ml::sigmoid(1000.0), 1.0);
    EXPECT_EQ(ml::sigmoid(-1000.0), 0.0);
    EXPECT_NEAR(ml::sigmoid(2.0), 1.0 / (1.0 + std::exp(-2.0)), kTight);
}

TEST(LogisticModel, OneStepMatchesTheHandComputedUpdate) {
    // Zero weights: p = 0.5. Label up: error = -0.5.
    //   w0 <- 0 - 0.1 * (-0.5 * 2 + 0) = 0.1;  w1 <- 0 - 0.1 * (-0.5 * -1) = -0.05;  b <- 0.05
    ml::LogisticModel model{0.1, 0.0, ml::Parameters{}};
    const double before = model.train(ml::FeatureVector{2, -1, 0, 0, 0, 0}, true);
    EXPECT_EQ(before, 0.5);
    EXPECT_NEAR(model.parameters()[0], 0.1, kTight);
    EXPECT_NEAR(model.parameters()[1], -0.05, kTight);
    EXPECT_NEAR(model.parameters()[2], 0.0, kTight);
    EXPECT_NEAR(model.parameters()[ml::kFeatureCount], 0.05, kTight);
    EXPECT_NEAR(model.predict(ml::FeatureVector{2, -1, 0, 0, 0, 0}), ml::sigmoid(0.2 + 0.05 + 0.05), kTight);
}

TEST(LogisticModel, L2ShrinksTheWeightsButNotTheBias) {
    ml::Parameters start{};
    start[0]                 = 1.0;
    start[ml::kFeatureCount] = 1.0;
    ml::LogisticModel model{0.5, 0.1, start};
    // x is zero, so only the penalty moves w0; p = sigmoid(1), label up.
    model.train(ml::FeatureVector{}, true);
    EXPECT_NEAR(model.parameters()[0], 1.0 - 0.5 * 0.1 * 1.0, kTight);
    EXPECT_NEAR(model.parameters()[ml::kFeatureCount], 1.0 - 0.5 * (ml::sigmoid(1.0) - 1.0), kTight);
    model.reset();
    EXPECT_EQ(model.parameters(), start);
}

// --- Config ------------------------------------------------------------------------

template <class Mutate>
std::string rejection(Mutate mutate) {
    strategy::LogisticRegressionConfig cfg = make_config();
    mutate(cfg);
    try {
        strategy::LogisticRegressionStrategy s{cfg};
    } catch (const common::ConfigError& e) {
        return e.what();
    }
    return {};
}

TEST(LogisticRegressionConfigTest, RejectsEachInvalidFieldByName) {
    using Cfg = strategy::LogisticRegressionConfig;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    EXPECT_NE(rejection([](Cfg& c) { c.strategy_id.clear(); }).find("strategy_id"), std::string::npos);
    EXPECT_NE(rejection([](Cfg& c) { c.horizon = 0; }).find("horizon"), std::string::npos);
    EXPECT_NE(rejection([](Cfg& c) { c.label_dead_band = -0.1; }).find("label_dead_band"), std::string::npos);
    EXPECT_NE(rejection([](Cfg& c) { c.learning_rate = 0.0; }).find("learning_rate"), std::string::npos);
    EXPECT_NE(rejection([&](Cfg& c) { c.learning_rate = nan; }).find("learning_rate"), std::string::npos);
    EXPECT_NE(rejection([](Cfg& c) { c.l2 = -1.0; }).find("l2"), std::string::npos);
    EXPECT_NE(rejection([](Cfg& c) { c.entry_probability = 0.5; }).find("entry_probability"), std::string::npos);
    EXPECT_NE(rejection([](Cfg& c) { c.entry_probability = 1.0; }).find("entry_probability"), std::string::npos);
    EXPECT_NE(rejection([](Cfg& c) { c.rearm_band = 0.1; }).find("rearm_band"), std::string::npos)
        << "0.1 is not below 0.60 - 0.5";
    EXPECT_NE(rejection([](Cfg& c) { c.initial_parameters = {1.0}; }).find("initial_parameters"), std::string::npos);
    EXPECT_NE(rejection([&](Cfg& c) { c.initial_parameters = {0, 0, 0, 0, 0, 0, nan}; })
                  .find("initial_parameters"), std::string::npos);
    EXPECT_NE(rejection([](Cfg& c) { c.requested_quantity = 0; }).find("requested_quantity"), std::string::npos);
    EXPECT_NE(rejection([](Cfg& c) { c.symbols.clear(); }).find("symbols"), std::string::npos);
    EXPECT_EQ(rejection([](Cfg&) {}), "") << "the test config itself is valid";
    EXPECT_EQ(rejection([](Cfg& c) { c = Cfg{}; c.symbols = {"AAPL"}; }), "") << "the defaults are valid";
}

// --- Strategy ----------------------------------------------------------------------

TEST(LogisticRegressionStrategyTest, WarmsUpForTwentyBarsThenPredictsExactlyOneHalf) {
    Harness h{make_config()};
    h.feed_closes(lr_test::trending(21));
    ASSERT_EQ(h.seen.size(), 21u);
    for (std::size_t i = 0; i < 20; ++i) {
        EXPECT_EQ(h.seen[i].verdict, strategy::BarVerdict::WarmingUp);
        EXPECT_EQ(h.seen[i].reason, strategy::BarReason::WarmingUp);
        EXPECT_FALSE(h.seen[i].probability.has_value());
    }
    EXPECT_EQ(h.seen[20].reason, strategy::BarReason::Training) << "nothing has trained yet";
    EXPECT_EQ(h.seen[20].probability, std::optional<double>{0.5}) << "zero weights";
    EXPECT_TRUE(h.signals.empty());
}

TEST(LogisticRegressionStrategyTest, WaitsForMinTrainingSamples) {
    auto cfg                 = make_config();
    cfg.min_training_samples = 10;
    Harness h{cfg};
    h.feed_closes(lr_test::trending(30));
    // Bar 21 queues the first sample; bar 22 trains it; bar 31 would be the 10th.
    EXPECT_EQ(h.lr.training_samples("AAPL"), std::optional<std::size_t>{9});
    for (std::size_t i = 20; i < 30; ++i) {
        EXPECT_EQ(h.seen[i].reason, strategy::BarReason::Training);
    }
    EXPECT_TRUE(h.signals.empty());
}

TEST(LogisticRegressionStrategyTest, LearnsAPlantedTrendFarAboveChance) {
    auto cfg                 = make_config();
    cfg.min_training_samples = 50;
    Harness h{cfg};
    h.feed_closes(lr_test::trending(1500));
    const auto accuracy = h.lr.rolling_accuracy("AAPL");
    ASSERT_TRUE(accuracy.has_value());
    EXPECT_GT(*accuracy, 0.85) << "the last move predicts the next except at a turn";
    const auto parameters = h.lr.parameters("AAPL");
    ASSERT_TRUE(parameters.has_value());
    EXPECT_GT((*parameters)[0], 0.0) << "a positive weight on the one-bar return";
    EXPECT_FALSE(h.signals.empty());
}

TEST(LogisticRegressionStrategyTest, FindsNothingInARandomWalk) {
    auto cfg = make_config();
    Harness h{cfg};
    h.feed_closes(lr_test::random_walk(3000));
    const auto accuracy = h.lr.rolling_accuracy("AAPL");
    ASSERT_TRUE(accuracy.has_value());
    EXPECT_GT(*accuracy, 0.3);
    EXPECT_LT(*accuracy, 0.7);
}

TEST(LogisticRegressionStrategyTest, SignalsCarryConfidenceAndTheModel) {
    auto cfg               = make_config();
    cfg.requested_quantity = 7;
    Harness h{cfg};
    h.feed_closes(lr_test::trending(400));
    ASSERT_FALSE(h.signals.empty());
    for (const domain::TradeSignal& s : h.signals) {
        EXPECT_EQ(s.symbol, "AAPL");
        EXPECT_EQ(s.requested_quantity, std::optional<common::Quantity>{7});
        EXPECT_EQ(s.order_type, std::optional<domain::OrderType>{domain::OrderType::Market});
        ASSERT_TRUE(s.confidence.has_value());
        const double p = std::stod(s.metadata.at("probability_up"));
        if (s.side == domain::SignalSide::Buy) {
            EXPECT_GE(p, 0.6);
            EXPECT_DOUBLE_EQ(*s.confidence, p);
            EXPECT_EQ(s.metadata.at("trigger"), "probability_at_or_above_buy_entry");
        } else {
            ASSERT_EQ(s.side, domain::SignalSide::Sell);
            EXPECT_LE(p, 0.4);
            EXPECT_DOUBLE_EQ(*s.confidence, 1.0 - p);
            EXPECT_EQ(s.metadata.at("trigger"), "probability_at_or_below_sell_entry");
        }
        EXPECT_GE(*s.confidence, 0.6);
        EXPECT_EQ(s.metadata.at("horizon"), "1");
        for (const std::string_view name : ml::feature_names()) {
            EXPECT_TRUE(s.metadata.contains("feature_" + std::string{name}));
            EXPECT_TRUE(s.metadata.contains("weight_" + std::string{name}));
        }
        EXPECT_TRUE(s.metadata.contains("bias"));
        EXPECT_TRUE(s.metadata.contains("rolling_accuracy"));
    }
}

TEST(LogisticRegressionStrategyTest, TheLatchNeverRepeatsASideWithoutARearmOrAFlip) {
    Harness h{make_config()};
    h.feed_closes(lr_test::trending(600));
    std::string latch = "neutral";
    for (const lr_test::Seen& seen : h.seen) {
        if (seen.action == strategy::BarAction::Buy) {
            EXPECT_NE(latch, "up") << "a second Buy needs a rearm or a Sell between";
        }
        if (seen.action == strategy::BarAction::Sell) {
            EXPECT_NE(latch, "down");
        }
        if (!seen.latch_after.empty()) {
            latch = seen.latch_after;
        }
    }
}

TEST(LogisticRegressionStrategyTest, AWarmStartActsOnTheFirstFullWindow) {
    auto cfg                 = make_config();
    cfg.min_training_samples = 0;
    cfg.initial_parameters   = {0, 0, 0, 0, 0, 0, 2.0};   // bias only: p = sigmoid(2) everywhere
    Harness h{cfg};
    h.feed_closes(lr_test::trending(21));
    ASSERT_EQ(h.signals.size(), 1u);
    EXPECT_EQ(h.signals[0].side, domain::SignalSide::Buy);
    EXPECT_DOUBLE_EQ(*h.signals[0].confidence, ml::sigmoid(2.0));
}

TEST(LogisticRegressionStrategyTest, TheDeadBandDropsSmallMovesFromTraining) {
    auto cfg            = make_config();
    cfg.label_dead_band = 0.02;   // every move in the series is about 1%
    Harness h{cfg};
    h.feed_closes(lr_test::trending(100));
    EXPECT_EQ(h.lr.training_samples("AAPL"), std::optional<std::size_t>{0});
    EXPECT_EQ(h.lr.parameters("AAPL"), std::optional<ml::Parameters>{ml::Parameters{}});
    EXPECT_TRUE(h.signals.empty());
}

TEST(LogisticRegressionStrategyTest, IgnoredEventsChangeNothing) {
    const Closes closes = lr_test::trending(200);
    Harness clean{make_config()};
    clean.feed_closes(closes);

    Harness noisy{make_config()};
    long long minute = 1;
    for (const common::Price close : closes) {
        noisy.feed(bar("TSLA", close, minute));                      // not allowlisted
        domain::MarketEvent print = bar("AAPL", close, minute);
        print.type                = domain::MarketEventType::Trade;   // not a bar
        noisy.feed(print);
        domain::MarketEvent empty = bar("AAPL", close, minute);
        empty.price.reset();                                          // no price
        noisy.feed(empty);
        noisy.feed(bar("AAPL", micros(0), minute));                   // not positive
        noisy.feed(bar("AAPL", close, minute));                       // the real bar
        noisy.feed(bar("AAPL", close, minute));                       // a duplicate
        ++minute;
    }
    EXPECT_EQ(noisy.lr.parameters("AAPL"), clean.lr.parameters("AAPL"));
    ASSERT_EQ(noisy.signals.size(), clean.signals.size());
    for (std::size_t i = 0; i < clean.signals.size(); ++i) {
        EXPECT_EQ(noisy.signals[i].metadata, clean.signals[i].metadata);
    }
}

TEST(LogisticRegressionStrategyTest, ChangingLaterBarsNeverChangesAnEarlierDecision) {
    const Closes original = lr_test::trending(400);
    Closes changed        = original;
    constexpr std::size_t kCut = 250;
    for (std::size_t i = kCut; i < changed.size(); ++i) {
        changed[i] = micros(changed[i].micros() * 3 / 2 + static_cast<std::int64_t>(i) * 1000);
    }
    Harness a{make_config()};
    Harness b{make_config()};
    a.feed_closes(original);
    b.feed_closes(changed);
    ASSERT_EQ(a.seen.size(), b.seen.size());
    for (std::size_t i = 0; i < kCut; ++i) {
        EXPECT_EQ(a.seen[i].probability, b.seen[i].probability) << "bar " << i + 1;
        EXPECT_EQ(a.seen[i].action, b.seen[i].action) << "bar " << i + 1;
    }
    bool differs_later = false;
    for (std::size_t i = kCut; i < a.seen.size(); ++i) {
        differs_later = differs_later || a.seen[i].probability != b.seen[i].probability;
    }
    EXPECT_TRUE(differs_later) << "the change itself must be visible, or the test proves nothing";
}

TEST(LogisticRegressionStrategyTest, ARestartReplaysBitForBit) {
    const Closes closes = lr_test::trending(300);
    Harness h{make_config()};
    h.feed_closes(closes);
    const std::vector<domain::TradeSignal> first = h.signals;
    const auto first_parameters                  = h.lr.parameters("AAPL");

    h.lr.on_start();
    h.signals.clear();
    h.feed_closes(closes);   // the same minutes: on_start() cleared the last timestamp too
    EXPECT_EQ(h.lr.parameters("AAPL"), first_parameters);
    ASSERT_EQ(h.signals.size(), first.size());
    for (std::size_t i = 0; i < first.size(); ++i) {
        EXPECT_EQ(h.signals[i].side, first[i].side);
        EXPECT_EQ(h.signals[i].confidence, first[i].confidence);
        EXPECT_EQ(h.signals[i].metadata, first[i].metadata);
    }
}

TEST(LogisticRegressionStrategyTest, SymbolsLearnIndependently) {
    const Closes up = lr_test::trending(300);
    Harness both{make_config({"AAPL", "MSFT"})};
    Harness alone{make_config({"AAPL", "MSFT"})};
    long long minute = 1;
    for (const common::Price close : up) {
        both.feed(bar("AAPL", close, minute));
        both.feed(bar("MSFT", micros(close.micros() / 2), minute));
        alone.feed(bar("AAPL", close, minute));
        ++minute;
    }
    EXPECT_EQ(both.lr.parameters("AAPL"), alone.lr.parameters("AAPL"));
    EXPECT_FALSE(both.lr.parameters("TSLA").has_value());
}

}  // namespace
