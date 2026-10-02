#pragma once

// Exact fixed-point number for money and prices, stored as whole millionths
// (matches NUMERIC(18,6) in the schema). No implicit conversions and no
// Decimal * Decimal, so unit mistakes fail to compile. Overflow is unchecked.

#include <cmath>
#include <compare>
#include <cstdint>
#include <iomanip>
#include <ostream>

#include "trading_engine/common/errors.hpp"

namespace trading_engine::common {

class Decimal {
public:
    static constexpr std::int64_t kScale = 1'000'000;

    constexpr Decimal() noexcept = default;

    [[nodiscard]] static constexpr Decimal from_micros(std::int64_t micros) noexcept {
        return Decimal{micros};
    }

    [[nodiscard]] static constexpr Decimal from_units(std::int64_t units) noexcept {
        return Decimal{units * kScale};
    }

    // For inputs that arrive as doubles. Rounds to the nearest millionth.
    [[nodiscard]] static Decimal from_double(double value) {
        const double scaled = value * static_cast<double>(kScale);
        constexpr double kLimit = 9.2e18;
        if (!std::isfinite(scaled) || scaled > kLimit || scaled < -kLimit) {
            throw ValidationError("Decimal::from_double: value out of range");
        }
        return Decimal{std::llround(scaled)};
    }

    [[nodiscard]] constexpr std::int64_t micros() const noexcept { return micros_; }

    // Lossy: for analytics and display, never for accounting.
    [[nodiscard]] double to_double() const noexcept {
        return static_cast<double>(micros_) / static_cast<double>(kScale);
    }

    // For rates such as basis-point fees.
    [[nodiscard]] Decimal scaled_by(double factor) const {
        return from_double(to_double() * factor);
    }

    friend constexpr bool operator==(Decimal, Decimal) noexcept = default;
    friend constexpr auto operator<=>(Decimal, Decimal) noexcept = default;

    constexpr Decimal operator-() const noexcept { return Decimal{-micros_}; }

    constexpr Decimal& operator+=(Decimal rhs) noexcept {
        micros_ += rhs.micros_;
        return *this;
    }
    constexpr Decimal& operator-=(Decimal rhs) noexcept {
        micros_ -= rhs.micros_;
        return *this;
    }

    friend constexpr Decimal operator+(Decimal a, Decimal b) noexcept { return a += b; }
    friend constexpr Decimal operator-(Decimal a, Decimal b) noexcept { return a -= b; }

    friend constexpr Decimal operator*(Decimal a, std::int64_t n) noexcept {
        return Decimal{a.micros_ * n};
    }
    friend constexpr Decimal operator*(std::int64_t n, Decimal a) noexcept {
        return a * n;
    }

    // Rounds half away from zero. n must not be 0.
    friend constexpr Decimal operator/(Decimal a, std::int64_t n) noexcept {
        const std::int64_t q = a.micros_ / n;
        const std::int64_t r = a.micros_ % n;
        const std::int64_t abs_r = r < 0 ? -r : r;
        const std::int64_t abs_n = n < 0 ? -n : n;
        if (abs_r >= abs_n - abs_r) {
            return Decimal{(a.micros_ < 0) != (n < 0) ? q - 1 : q + 1};
        }
        return Decimal{q};
    }

    friend std::ostream& operator<<(std::ostream& os, Decimal d) {
        const std::int64_t whole = d.micros_ / kScale;
        std::int64_t frac = d.micros_ % kScale;
        if (d.micros_ < 0) {
            os << '-';
            frac = -frac;
        }
        const std::int64_t abs_whole = whole < 0 ? -whole : whole;
        const char fill = os.fill('0');
        os << abs_whole << '.' << std::setw(6) << frac;
        os.fill(fill);
        return os;
    }

private:
    constexpr explicit Decimal(std::int64_t micros) noexcept : micros_{micros} {}

    std::int64_t micros_{0};
};

}  // namespace trading_engine::common
