#include "trading_engine/strategy/logistic_model.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace trading_engine::strategy::ml {

namespace {

// Same tolerance as MeanReversionStrategy: a spread under one part in 10^12 of the
// mean is no spread at all, so the z-score of a constant window is 0, not huge.
constexpr double kNegligibleVariation = 1e-12;

constexpr std::size_t kShortAverage = 5;
constexpr std::size_t kLongAverage  = 20;

double log_return(common::Price newer, common::Price older) {
    return std::log(static_cast<double>(newer.micros()) / static_cast<double>(older.micros()));
}

// Mean of `values` as an OFFSET from `origin` (in millionths), taken in exact int64
// arithmetic before it becomes a double, as MeanReversionStrategy does.
double mean_offset(std::span<const common::Price> values, std::int64_t origin) {
    double sum = 0.0;
    for (const common::Price value : values) {
        sum += static_cast<double>(value.micros() - origin);
    }
    return sum / static_cast<double>(values.size());
}

// Population standard deviation, two-pass.
template <class Values, class Get>
double population_stddev(const Values& values, Get get) {
    double sum = 0.0;
    for (const auto& value : values) {
        sum += get(value);
    }
    const double mean = sum / static_cast<double>(values.size());
    double squares    = 0.0;
    for (const auto& value : values) {
        const double deviation = get(value) - mean;
        squares += deviation * deviation;
    }
    return std::sqrt(squares / static_cast<double>(values.size()));
}

}  // namespace

const std::array<std::string_view, kFeatureCount>& feature_names() noexcept {
    static constexpr std::array<std::string_view, kFeatureCount> kNames{
        "return_1", "return_5", "return_20", "z_score_20", "sma_gap_5_20", "volatility_20"};
    return kNames;
}

std::optional<FeatureVector> compute_features(std::span<const common::Price> closes) {
    if (closes.size() < kFeatureWindow) {
        return std::nullopt;
    }
    const std::span<const common::Price> window = closes.last(kFeatureWindow);
    const common::Price current = window.back();
    const std::size_t   last    = kFeatureWindow - 1;

    FeatureVector features{};
    features[0] = log_return(current, window[last - 1]);
    features[1] = log_return(current, window[last - 5]);
    features[2] = log_return(current, window[last - 20]);

    const std::int64_t origin = current.micros();
    const std::span<const common::Price> long_window  = window.last(kLongAverage);
    const std::span<const common::Price> short_window = window.last(kShortAverage);
    const double long_offset  = mean_offset(long_window, origin);
    const double short_offset = mean_offset(short_window, origin);
    const double long_mean    = static_cast<double>(origin) + long_offset;

    const double spread = population_stddev(long_window, [origin](common::Price close) {
        return static_cast<double>(close.micros() - origin);
    });
    features[3] = spread <= kNegligibleVariation * long_mean ? 0.0 : (0.0 - long_offset) / spread;
    features[4] = (short_offset - long_offset) / long_mean;

    std::array<double, kLongAverage> returns{};
    for (std::size_t i = 0; i < kLongAverage; ++i) {
        returns[i] = log_return(window[i + 1], window[i]);
    }
    features[5] = population_stddev(returns, [](double r) { return r; });

    for (const double feature : features) {
        if (!std::isfinite(feature)) {
            return std::nullopt;
        }
    }
    return features;
}

// --- RunningStandardizer ------------------------------------------------------------

FeatureVector RunningStandardizer::scale(const FeatureVector& raw) const noexcept {
    FeatureVector scaled{};
    if (count_ < 2) {
        return scaled;
    }
    for (std::size_t i = 0; i < kFeatureCount; ++i) {
        const double stddev = std::sqrt(m2_[i] / static_cast<double>(count_));
        if (!(stddev > 0.0)) {
            continue;
        }
        scaled[i] = std::clamp((raw[i] - mean_[i]) / stddev, -kClip, kClip);
    }
    return scaled;
}

void RunningStandardizer::observe(const FeatureVector& raw) noexcept {
    ++count_;
    const double n = static_cast<double>(count_);
    for (std::size_t i = 0; i < kFeatureCount; ++i) {
        const double delta = raw[i] - mean_[i];
        mean_[i] += delta / n;
        m2_[i] += delta * (raw[i] - mean_[i]);
    }
}

void RunningStandardizer::reset() noexcept {
    count_ = 0;
    mean_  = {};
    m2_    = {};
}

// --- LogisticModel --------------------------------------------------------------------

double sigmoid(double z) noexcept {
    if (z >= 0.0) {
        return 1.0 / (1.0 + std::exp(-z));
    }
    const double e = std::exp(z);   // z < 0: e^z cannot overflow
    return e / (1.0 + e);
}

LogisticModel::LogisticModel(double learning_rate, double l2, const Parameters& initial) noexcept
    : learning_rate_{learning_rate}, l2_{l2}, initial_{initial}, parameters_{initial} {}

double LogisticModel::predict(const FeatureVector& x) const noexcept {
    double z = parameters_[kFeatureCount];   // the bias
    for (std::size_t i = 0; i < kFeatureCount; ++i) {
        z += parameters_[i] * x[i];
    }
    return sigmoid(z);
}

double LogisticModel::train(const FeatureVector& x, bool label) noexcept {
    const double p     = predict(x);
    const double error = p - (label ? 1.0 : 0.0);
    for (std::size_t i = 0; i < kFeatureCount; ++i) {
        parameters_[i] -= learning_rate_ * (error * x[i] + l2_ * parameters_[i]);
    }
    parameters_[kFeatureCount] -= learning_rate_ * error;
    return p;
}

}  // namespace trading_engine::strategy::ml
