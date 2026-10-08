// The strategy catalog: the single authoritative mapping from kinds and parameters to the
// real configuration structs. Defaults must be the structs' own; a bad configuration must be
// rejected in the STRATEGY's words (the lab only parses text into a type); ML must stay
// unavailable.

#include <cmath>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "lab/result_json.hpp"
#include "strategy_lab_test_support.hpp"
#include "trading_engine/strategy/mean_reversion_strategy.hpp"
#include "trading_engine/strategy/moving_average_crossover_strategy.hpp"
#include "trading_engine/strategy/strategy_diagnostics.hpp"

namespace {

using namespace lab_test;
using lab::ErrorCode;
using lab::ParamValue;
using strategy_cfg_sma = trading_engine::strategy::MovingAverageCrossoverConfig;
using strategy_cfg_mr  = trading_engine::strategy::MeanReversionConfig;
namespace strategy     = trading_engine::strategy;

const lab::StrategyKind& kind(const std::string& name) {
    const lab::StrategyKind* found = lab::find_kind(name);
    EXPECT_NE(found, nullptr) << name;
    static const lab::StrategyKind empty{};
    return found != nullptr ? *found : empty;
}

std::vector<std::string> names_of(const lab::StrategyKind& k) {
    std::vector<std::string> out;
    for (const lab::ParamSpec& p : k.params) {
        out.push_back(p.name);
    }
    return out;
}

const lab::ParamSpec& param(const lab::StrategyKind& k, const std::string& name) {
    for (const lab::ParamSpec& p : k.params) {
        if (p.name == name) {
            return p;
        }
    }
    ADD_FAILURE() << "no parameter " << name;
    static const lab::ParamSpec none{};
    return none;
}

// ---- what exists --------------------------------------------------------------------------------

TEST(LabCatalog, ListsTheTwoReferenceStrategiesAndAnUnavailableMlEntry) {
    const auto& catalog = lab::strategy_catalog();
    ASSERT_EQ(catalog.size(), 3u);
    EXPECT_EQ(catalog[0].kind, "sma_crossover");
    EXPECT_EQ(catalog[1].kind, "mean_reversion");
    EXPECT_EQ(catalog[2].kind, "ml");
    EXPECT_TRUE(catalog[0].available);
    EXPECT_TRUE(catalog[1].available);
    EXPECT_FALSE(catalog[2].available) << "the custom ML strategy has not been started";
    EXPECT_FALSE(catalog[2].unavailable_reason.empty());
    EXPECT_TRUE(catalog[2].params.empty());
    EXPECT_EQ(lab::find_kind("nope"), nullptr);
}

TEST(LabCatalog, AnMlStrategyCannotBeBuiltAndTheErrorSaysWhy) {
    const lab::LabError error = expect_lab_error([] { (void)lab::build_strategy(request("ml", {{"symbols", "AAPL"}})); });
    EXPECT_EQ(error.code(), ErrorCode::UnavailableStrategy);
    EXPECT_EQ(lab::exit_status(error.code()), 2);
    EXPECT_NE(std::string{error.what()}.find("not available"), std::string::npos);
    EXPECT_NE(std::string{error.what()}.find("Not implemented"), std::string::npos) << error.what();
}

TEST(LabCatalog, AnUnknownKindIsAUsageErrorListingTheKnownOnes) {
    const lab::LabError error = expect_lab_error([] { (void)lab::build_strategy(request("sma")); });
    EXPECT_EQ(error.code(), ErrorCode::UsageError);
    EXPECT_NE(std::string{error.what()}.find("sma_crossover, mean_reversion, ml"), std::string::npos) << error.what();
}

// ---- parameter names and defaults ---------------------------------------------------------------------------

TEST(LabCatalog, ParameterNamesAreTheConfigFieldNamesInTheirDeclaredOrder) {
    EXPECT_EQ(names_of(kind("sma_crossover")),
              (std::vector<std::string>{"strategy_id", "short_window", "long_window", "requested_quantity", "symbols"}));
    EXPECT_EQ(names_of(kind("mean_reversion")),
              (std::vector<std::string>{"strategy_id", "lookback", "entry_threshold", "rearm_threshold",
                                        "requested_quantity", "symbols"}));
}

TEST(LabCatalog, CrossoverDefaultsAreTheDocumentedOnesAndComeFromTheConfigStruct) {
    const lab::StrategyKind& k = kind("sma_crossover");
    // The literals are docs/strategies/moving_average_crossover.md section 2 ...
    EXPECT_EQ(std::get<std::string>(*param(k, "strategy_id").default_value), "sma_crossover");
    EXPECT_EQ(std::get<std::uint64_t>(*param(k, "short_window").default_value), 5u);
    EXPECT_EQ(std::get<std::uint64_t>(*param(k, "long_window").default_value), 20u);
    EXPECT_EQ(std::get<std::uint64_t>(*param(k, "requested_quantity").default_value), 1u)
        << "a whole number of shares, never a double";
    EXPECT_EQ(param(k, "requested_quantity").type, lab::ParamType::Quantity);
    EXPECT_TRUE(param(k, "symbols").required);
    EXPECT_FALSE(param(k, "symbols").default_value.has_value()) << "no default: an allowlist must be chosen";
    // ... and they equal the default-constructed struct, the single source of truth.
    const strategy_cfg_sma defaults{};
    EXPECT_EQ(std::get<std::string>(*param(k, "strategy_id").default_value), defaults.strategy_id);
    EXPECT_EQ(std::get<std::uint64_t>(*param(k, "short_window").default_value), defaults.short_window);
    EXPECT_EQ(std::get<std::uint64_t>(*param(k, "long_window").default_value), defaults.long_window);
    EXPECT_EQ(std::get<std::uint64_t>(*param(k, "requested_quantity").default_value),
              static_cast<std::uint64_t>(defaults.requested_quantity));
}

TEST(LabCatalog, MeanReversionDefaultsAreTheDocumentedOnesAndComeFromTheConfigStruct) {
    const lab::StrategyKind& k = kind("mean_reversion");
    EXPECT_EQ(std::get<std::string>(*param(k, "strategy_id").default_value), "mean_reversion");
    EXPECT_EQ(std::get<std::uint64_t>(*param(k, "lookback").default_value), 20u);
    EXPECT_EQ(std::get<double>(*param(k, "entry_threshold").default_value), 2.0);
    EXPECT_EQ(std::get<double>(*param(k, "rearm_threshold").default_value), 0.5);
    EXPECT_EQ(std::get<std::uint64_t>(*param(k, "requested_quantity").default_value), 1u);
    EXPECT_TRUE(param(k, "symbols").required);

    const strategy_cfg_mr defaults{};
    EXPECT_EQ(std::get<std::uint64_t>(*param(k, "lookback").default_value), defaults.lookback);
    EXPECT_EQ(std::get<double>(*param(k, "entry_threshold").default_value), defaults.entry_threshold);
    EXPECT_EQ(std::get<double>(*param(k, "rearm_threshold").default_value), defaults.rearm_threshold);
}

TEST(LabCatalog, ABuiltStrategyEchoesEveryParameterWithDefaultsFilledIn) {
    const lab::BuiltStrategy built =
        lab::build_strategy(request("sma_crossover", {{"symbols", "AAPL,MSFT"}, {"long_window", "30"}}));
    EXPECT_EQ(built.kind, "sma_crossover");
    EXPECT_EQ(built.strategy_id, "sma_crossover");
    ASSERT_EQ(built.parameters.size(), 5u);
    EXPECT_EQ(built.parameters[0].first, "strategy_id");
    EXPECT_EQ(std::get<std::uint64_t>(built.parameters[1].second), 5u) << "short_window kept its default";
    EXPECT_EQ(std::get<std::uint64_t>(built.parameters[2].second), 30u);
    EXPECT_EQ(std::get<std::vector<std::string>>(built.parameters[4].second), (std::vector<std::string>{"AAPL", "MSFT"}));
    EXPECT_EQ(built.symbols, (std::vector<std::string>{"AAPL", "MSFT"}));
    EXPECT_EQ(built.window_size, 30u);
    ASSERT_NE(built.strategy, nullptr);
    EXPECT_EQ(built.strategy->id(), "sma_crossover");
}

TEST(LabCatalog, DerivedFactsFollowTheDocumentedFormulas) {
    const auto derived = [](const lab::BuiltStrategy& b, const std::string& name) {
        for (const auto& [key, value] : b.derived) {
            if (key == name) {
                return value;
            }
        }
        ADD_FAILURE() << name;
        return lab::DerivedValue{};
    };
    const lab::BuiltStrategy sma =
        lab::build_strategy(request("sma_crossover", {{"symbols", "A"}, {"short_window", "2"}, {"long_window", "7"}}));
    EXPECT_EQ(std::get<std::uint64_t>(derived(sma, "earliest_signal_accepted_bar")), 8u) << "long_window + 1";

    // |z| <= sqrt(N - 1): lookback 5 reaches the default 2.0; lookback 4 (sqrt 3 = 1.73) does not.
    const lab::BuiltStrategy reachable =
        lab::build_strategy(request("mean_reversion", {{"symbols", "A"}, {"lookback", "5"}}));
    EXPECT_DOUBLE_EQ(std::get<double>(derived(reachable, "max_abs_z")), 2.0);
    EXPECT_TRUE(std::get<bool>(derived(reachable, "entry_threshold_reachable")));
    const lab::BuiltStrategy unreachable =
        lab::build_strategy(request("mean_reversion", {{"symbols", "A"}, {"lookback", "4"}}));
    EXPECT_DOUBLE_EQ(std::get<double>(derived(unreachable, "max_abs_z")), std::sqrt(3.0));
    EXPECT_FALSE(std::get<bool>(derived(unreachable, "entry_threshold_reachable")))
        << "accepted by the strategy, but it can never fire";
}

// ---- text that is not the right TYPE (a lab concern) ---------------------------------------------------------------

lab::LabError param_error(const std::string& kind_name, const Params& params) {
    return expect_lab_error([&] { (void)lab::build_strategy(request(kind_name, params)); });
}

TEST(LabCatalogParameters, UnknownRepeatedAndUnparsableParametersAreInvalidParameterErrors) {
    const lab::LabError unknown = param_error("sma_crossover", {{"symbols", "A"}, {"window", "3"}});
    EXPECT_EQ(unknown.code(), ErrorCode::InvalidParameter);
    EXPECT_NE(std::string{unknown.what()}.find("unknown parameter 'window'"), std::string::npos);
    EXPECT_NE(std::string{unknown.what()}.find("short_window"), std::string::npos) << "lists what is known";

    const lab::LabError twice = param_error("sma_crossover", {{"symbols", "A"}, {"short_window", "2"}, {"short_window", "3"}});
    EXPECT_EQ(twice.code(), ErrorCode::InvalidParameter);
    EXPECT_NE(std::string{twice.what()}.find("more than once"), std::string::npos);

    for (const char* text : {"-1", "1.5", "", "abc", "+3", " 3", "3 ", "1e2", "0x10", "99999999999999999999"}) {
        const lab::LabError error = param_error("sma_crossover", {{"symbols", "A"}, {"short_window", text}});
        EXPECT_EQ(error.code(), ErrorCode::InvalidParameter) << "'" << text << "'";
        EXPECT_NE(std::string{error.what()}.find("short_window"), std::string::npos);
    }
    for (const char* text : {"", "abc", "1.5x", "1e999", "--1"}) {
        const lab::LabError error = param_error("mean_reversion", {{"symbols", "A"}, {"entry_threshold", text}});
        EXPECT_EQ(error.code(), ErrorCode::InvalidParameter) << "'" << text << "'";
    }
}

TEST(LabCatalogParameters, ANegativeNumberNeverWrapsToAHugeUnsignedValue) {
    // If "-1" were parsed as an unsigned integer it would become 2^64 - 1 and sail past a ">= 1" check.
    const lab::LabError error = param_error("mean_reversion", {{"symbols", "A"}, {"lookback", "-1"}});
    EXPECT_EQ(error.code(), ErrorCode::InvalidParameter);
    EXPECT_NE(std::string{error.what()}.find("unsigned whole number"), std::string::npos);
}

TEST(LabCatalogParameters, TextThatIsNotValidUtf8IsRefusedBeforeItCanReachAMessage) {
    EXPECT_EQ(param_error("sma_crossover", {{"symbols", "A"}, {"strategy_id", "bad\xFF"}}).code(), ErrorCode::InvalidParameter);
    EXPECT_EQ(param_error("sma_crossover", {{"symbols", "A\xC3"}}).code(), ErrorCode::InvalidParameter);
}

// ---- configurations the STRATEGY rejects: its words, unchanged ----------------------------------------------------------

// What constructing the strategy directly from `config` throws.
template <class Strategy, class Config>
std::string direct_message(const Config& config) {
    try {
        Strategy built{config};
        ADD_FAILURE() << "the direct construction was expected to fail";
    } catch (const std::exception& error) {
        return error.what();
    }
    return {};
}

void expect_verbatim(const std::string& kind_name, const Params& params, const std::string& want) {
    const lab::LabError error = param_error(kind_name, params);
    EXPECT_EQ(error.code(), ErrorCode::ConfigError) << want;
    EXPECT_EQ(lab::exit_status(error.code()), 2);
    EXPECT_EQ(std::string{error.what()}, want) << "the strategy's own message, not a paraphrase";
    ASSERT_EQ(error.problems().size(), 1u);
    EXPECT_EQ(error.problems()[0].where, "strategies[0]");
}

TEST(LabCatalogConfig, CrossoverRejectionsAreTheStrategysOwnWords) {
    const auto direct = [](auto mutate) {
        strategy_cfg_sma config;
        config.symbols = {"AAPL"};
        mutate(config);
        return direct_message<strategy::MovingAverageCrossoverStrategy>(config);
    };
    expect_verbatim("sma_crossover", {{"symbols", "AAPL"}, {"short_window", "0"}},
                    direct([](auto& c) { c.short_window = 0; }));
    expect_verbatim("sma_crossover", {{"symbols", "AAPL"}, {"short_window", "20"}},
                    direct([](auto& c) { c.short_window = 20; }));
    expect_verbatim("sma_crossover", {{"symbols", "AAPL"}, {"long_window", "3"}},
                    direct([](auto& c) { c.long_window = 3; }));
    // A quantity is a whole number of shares held as an int64: zero is the strategy's refusal ...
    expect_verbatim("sma_crossover", {{"symbols", "AAPL"}, {"requested_quantity", "0"}},
                    direct([](auto& c) { c.requested_quantity = 0; }));
    // ... and text that is not an unsigned whole number (a fraction, a sign, nan, inf, an exponent) or that
    // does not fit an int64 never reaches the strategy: it is refused at parse time, never rounded or wrapped.
    for (const char* quantity : {"1.5", "-1", "nan", "inf", "1e2", "", "9223372036854775808", "18446744073709551615"}) {
        const lab::LabError error = param_error("sma_crossover", {{"symbols", "AAPL"}, {"requested_quantity", quantity}});
        EXPECT_EQ(error.code(), ErrorCode::InvalidParameter) << "'" << quantity << "'";
        EXPECT_NE(std::string{error.what()}.find("requested_quantity"), std::string::npos) << error.what();
    }
    EXPECT_NO_THROW((void)lab::build_strategy(
        request("sma_crossover", {{"symbols", "AAPL"}, {"requested_quantity", "9223372036854775807"}}), 0))
        << "INT64_MAX shares is exact and accepted";
    expect_verbatim("sma_crossover", {{"symbols", "AAPL"}, {"strategy_id", ""}},
                    direct([](auto& c) { c.strategy_id = ""; }));

    // Hand-written spot checks of the words themselves.
    EXPECT_EQ(param_error("sma_crossover", {{"symbols", "AAPL"}, {"short_window", "0"}}).what(),
              std::string{"configuration error: MovingAverageCrossoverStrategy: short_window must be at least 1 (got 0)"});
    EXPECT_EQ(param_error("sma_crossover", {{"symbols", "AAPL"}, {"requested_quantity", "0"}}).what(),
              std::string{"configuration error: MovingAverageCrossoverStrategy: requested_quantity must be a positive "
                          "whole number of shares (got 0)"});
}

TEST(LabCatalogConfig, SymbolAllowlistRejectionsAreTheStrategysOwnWords) {
    const auto direct = [](std::vector<std::string> symbols) {
        strategy_cfg_sma config;
        config.symbols = std::move(symbols);
        return direct_message<strategy::MovingAverageCrossoverStrategy>(config);
    };
    expect_verbatim("sma_crossover", {}, direct({}));                                            // none given
    expect_verbatim("sma_crossover", {{"symbols", ""}}, direct({}));                             // empty list
    expect_verbatim("sma_crossover", {{"symbols", "A,,B"}}, direct({"A", "", "B"}));             // an empty entry survives
    expect_verbatim("sma_crossover", {{"symbols", "A,B,A"}}, direct({"A", "B", "A"}));           // a repeat
}

TEST(LabCatalogConfig, MeanReversionRejectionsAreTheStrategysOwnWords) {
    const auto direct = [](auto mutate) {
        strategy_cfg_mr config;
        config.symbols = {"AAPL"};
        mutate(config);
        return direct_message<strategy::MeanReversionStrategy>(config);
    };
    expect_verbatim("mean_reversion", {{"symbols", "AAPL"}, {"lookback", "1"}}, direct([](auto& c) { c.lookback = 1; }));
    expect_verbatim("mean_reversion", {{"symbols", "AAPL"}, {"lookback", "0"}}, direct([](auto& c) { c.lookback = 0; }));
    expect_verbatim("mean_reversion", {{"symbols", "AAPL"}, {"entry_threshold", "0.5"}},
                    direct([](auto& c) { c.entry_threshold = 0.5; }));   // not above rearm 0.5
    expect_verbatim("mean_reversion", {{"symbols", "AAPL"}, {"rearm_threshold", "-0.1"}},
                    direct([](auto& c) { c.rearm_threshold = -0.1; }));
    expect_verbatim("mean_reversion", {{"symbols", "AAPL"}, {"rearm_threshold", "2"}},
                    direct([](auto& c) { c.rearm_threshold = 2.0; }));
    for (const char* text : {"nan", "inf", "-inf"}) {
        expect_verbatim("mean_reversion", {{"symbols", "AAPL"}, {"entry_threshold", text}},
                        direct([&](auto& c) { c.entry_threshold = lab_test::parse_double(text); }));
    }
    EXPECT_EQ(param_error("mean_reversion", {{"symbols", "AAPL"}, {"lookback", "1"}}).what(),
              std::string{"configuration error: MeanReversionStrategy: lookback must be at least 2 (got 1)"});
}

TEST(LabCatalogConfig, AThresholdThatCanNeverFireIsAcceptedNotRejected) {
    // docs/strategies/mean_reversion.md section 1: accepted, and the lab warns instead (session tests).
    EXPECT_NO_THROW((void)lab::build_strategy(
        request("mean_reversion", {{"symbols", "AAPL"}, {"lookback", "3"}, {"entry_threshold", "9"}})));
}

// ---- the vocabulary a snapshot can use ------------------------------------------------------------------------------

TEST(LabCatalogReasons, EveryReasonTheStrategiesCanReportIsExplainedAndNothingElseIs) {
    std::set<std::string> explained;
    for (const lab::StrategyKind& k : lab::strategy_catalog()) {
        std::set<std::string> within;
        for (const lab::ReasonSpec& reason : k.reasons) {
            EXPECT_TRUE(within.insert(reason.code).second) << k.kind << " lists " << reason.code << " twice";
            EXPECT_FALSE(reason.text.empty()) << reason.code;
            explained.insert(reason.code);
        }
    }
    std::set<std::string> vocabulary;
    for (int value = 0; value <= static_cast<int>(strategy::BarReason::BetweenBands); ++value) {
        vocabulary.insert(std::string{strategy::to_string(static_cast<strategy::BarReason>(value))});
    }
    EXPECT_EQ(explained, vocabulary) << "the catalog's text and the strategies' codes must match exactly";

    // Each reason's verdict agrees with the strategy vocabulary.
    for (const lab::StrategyKind& k : lab::strategy_catalog()) {
        for (const lab::ReasonSpec& reason : k.reasons) {
            EXPECT_TRUE(reason.verdict == "ignored" || reason.verdict == "warming_up" || reason.verdict == "evaluated")
                << reason.code << " -> " << reason.verdict;
        }
    }
    EXPECT_FALSE(kind("sma_crossover").reasons.empty());
    // price_above_max_close exists only for the crossover.
    const auto has = [](const lab::StrategyKind& k, const std::string& code) {
        for (const lab::ReasonSpec& r : k.reasons) {
            if (r.code == code) return true;
        }
        return false;
    };
    EXPECT_TRUE(has(kind("sma_crossover"), "price_above_max_close"));
    EXPECT_FALSE(has(kind("mean_reversion"), "price_above_max_close"));
}

TEST(LabCatalogReasons, TheIndicatorAndStateNamesMatchWhatTheStrategiesReport) {
    EXPECT_EQ(kind("sma_crossover").indicators, (std::vector<std::string>{"short_sma", "long_sma", "short_minus_long"}));
    EXPECT_EQ(kind("sma_crossover").states, (std::vector<std::string>{"relation_before", "relation_now", "relation_after"}));
    EXPECT_EQ(kind("mean_reversion").indicators, (std::vector<std::string>{"mean", "standard_deviation", "z_score"}));
    EXPECT_EQ(kind("mean_reversion").states, (std::vector<std::string>{"latch_before", "latch_after"}));
}

TEST(LabCatalogDescribe, TheDescribeDocumentCarriesTheCatalogWithoutAWallClock) {
    const std::string text = lab::catalog_json(false);
    EXPECT_EQ(text.back(), '\n');
    EXPECT_EQ(text.find('\r'), std::string::npos);
    for (const char* needle : {"\"schema\":\"strategy_lab.catalog\"", "\"schema_version\":\"1.0\"",
                               "\"kind\":\"sma_crossover\"", "\"kind\":\"mean_reversion\"", "\"kind\":\"ml\"",
                               "\"available\":false", "\"name\":\"short_window\",\"type\":\"uint\",\"required\":false,\"default\":5",
                               "\"name\":\"entry_threshold\",\"type\":\"double\",\"required\":false,\"default\":2",
                               "\"name\":\"requested_quantity\",\"type\":\"uint\",\"required\":false,\"default\":1,",
                               "The price is zero or negative.", "INT64_MAX / long_window^2",
                               "\"name\":\"symbols\",\"type\":\"string_list\",\"required\":true",
                               "\"id\":\"strategy_lab_bars_csv/1\"", "\"name\":\"reject-signals\""}) {
        EXPECT_NE(text.find(needle), std::string::npos) << needle;
    }
    EXPECT_EQ(text.find("generated_at"), std::string::npos) << "the catalog is static: no wall clock";
    EXPECT_EQ(text, lab::catalog_json(false)) << "deterministic";
}

}  // namespace
