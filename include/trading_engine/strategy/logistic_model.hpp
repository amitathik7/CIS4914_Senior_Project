#pragma once

// -----------------------------------------------------------------------------
//  The building blocks of LogisticRegressionStrategy: its feature vector, a
//  running standardizer and an online logistic-regression model. Kept apart from
//  the strategy so each can be checked against hand-computed numbers on its own.
//
//  Everything here is DERIVED real numbers, never money: prices enter as exact
//  common::Price values and nothing computed is ever turned back into a Price.
//  All of it is deterministic: no random initialization, no shuffling, no threads,
//  and a fixed order of operations. Do not build with -ffast-math or /fp:fast.
//
//  Design notes and the reasoning behind the feature choice are in
//  docs/strategies/logistic_regression.md.
// -----------------------------------------------------------------------------

#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string_view>

#include "trading_engine/common/types.hpp"

namespace trading_engine::strategy::ml {

// The six features, in this order. Each is computed from closes only (open, high,
// low and volume are optional on MarketEvent and are not used).
//   0 return_1          ln(c_t / c_t-1)
//   1 return_5          ln(c_t / c_t-5)
//   2 return_20         ln(c_t / c_t-20)
//   3 z_score_20        (c_t - mean of the last 20 closes) / their population stddev, 0 if constant
//   4 sma_gap_5_20      mean of the last 5 closes / mean of the last 20 - 1
//   5 volatility_20     population stddev of the last 20 one-bar log returns
inline constexpr std::size_t kFeatureCount = 6;

// Closes needed for one feature vector: 20 one-bar returns need 21 closes.
inline constexpr std::size_t kFeatureWindow = 21;

// Weights plus the bias, which comes last.
inline constexpr std::size_t kParameterCount = kFeatureCount + 1;

using FeatureVector = std::array<double, kFeatureCount>;
using Parameters    = std::array<double, kParameterCount>;

[[nodiscard]] const std::array<std::string_view, kFeatureCount>& feature_names() noexcept;

// The feature vector of the NEWEST close in `closes` (oldest first). Uses only the
// last kFeatureWindow closes; every close must be positive. Empty when there are
// fewer than kFeatureWindow closes, or when a value would be non-finite (a guard
// no positive int64 close can reach).
[[nodiscard]] std::optional<FeatureVector> compute_features(std::span<const common::Price> closes);

// Per-feature running mean and population variance (Welford), used to put the
// features on one scale before the model sees them. scale() uses only what was
// observed BEFORE the call, so standardizing a bar never peeks at that bar.
class RunningStandardizer {
public:
    // Scaled values are clipped to [-kClip, kClip] so one extreme bar cannot
    // dominate an SGD step.
    static constexpr double kClip = 5.0;

    // (x - mean) / stddev per feature, clipped. A feature with fewer than two
    // observations, or a zero spread, scales to 0.
    [[nodiscard]] FeatureVector scale(const FeatureVector& raw) const noexcept;

    void observe(const FeatureVector& raw) noexcept;
    void reset() noexcept;

    [[nodiscard]] std::size_t count() const noexcept { return count_; }

private:
    std::size_t   count_{0};
    FeatureVector mean_{};
    FeatureVector m2_{};   // sum of squared deviations from the running mean
};

// sigma(z) = 1 / (1 + e^-z), computed without overflow for any finite z.
[[nodiscard]] double sigmoid(double z) noexcept;

// p = sigma(w . x + b), trained one sample at a time by stochastic gradient
// descent on the log loss with L2 regularization on the weights (not the bias):
//     w <- w - learning_rate * ((p - y) * x + l2 * w)
//     b <- b - learning_rate * (p - y)
class LogisticModel {
public:
    // `initial` holds the starting weights then the bias; reset() returns to it.
    // The caller validates the arguments (finite, learning_rate > 0, l2 >= 0).
    LogisticModel(double learning_rate, double l2, const Parameters& initial) noexcept;

    [[nodiscard]] double predict(const FeatureVector& x) const noexcept;

    // One SGD step on (x, label). Returns the probability predicted for x BEFORE
    // the step.
    double train(const FeatureVector& x, bool label) noexcept;

    void reset() noexcept { parameters_ = initial_; }

    // Weights then bias.
    [[nodiscard]] const Parameters& parameters() const noexcept { return parameters_; }

private:
    double     learning_rate_;
    double     l2_;
    Parameters initial_;
    Parameters parameters_;
};

}  // namespace trading_engine::strategy::ml
