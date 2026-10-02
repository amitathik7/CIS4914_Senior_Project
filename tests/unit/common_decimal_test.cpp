// Fixed-point Decimal used for Money and Price.

#include <cmath>
#include <limits>
#include <sstream>

#include <gtest/gtest.h>

#include "trading_engine/common/errors.hpp"
#include "trading_engine/common/types.hpp"

namespace te = trading_engine::common;

using te::Decimal;

TEST(Decimal, DefaultIsZero) {
    EXPECT_EQ(Decimal{}.micros(), 0);
}

TEST(Decimal, UnitsAndMicrosAgree) {
    EXPECT_EQ(Decimal::from_units(10), Decimal::from_micros(10'000'000));
}

TEST(Decimal, FromDoubleRoundsToNearestMillionth) {
    EXPECT_EQ(Decimal::from_double(10.25).micros(), 10'250'000);
    EXPECT_EQ(Decimal::from_double(0.0000004).micros(), 0);
    EXPECT_EQ(Decimal::from_double(0.0000006).micros(), 1);
    EXPECT_EQ(Decimal::from_double(-10.25).micros(), -10'250'000);
}

TEST(Decimal, FromDoubleRejectsNonFiniteAndOutOfRange) {
    EXPECT_THROW((void)Decimal::from_double(std::nan("")), te::ValidationError);
    EXPECT_THROW((void)Decimal::from_double(std::numeric_limits<double>::infinity()),
                 te::ValidationError);
    EXPECT_THROW((void)Decimal::from_double(1e14), te::ValidationError);
}

TEST(Decimal, AdditionIsExactWhereDoubleIsNot) {
    // 0.1 + 0.2 != 0.3 in double.
    EXPECT_EQ(Decimal::from_double(0.1) + Decimal::from_double(0.2),
              Decimal::from_double(0.3));
}

TEST(Decimal, PriceTimesWholeSharesIsExact) {
    const te::Price price = Decimal::from_double(150.02);
    const te::Quantity qty = 100;
    EXPECT_EQ(price * qty, Decimal::from_units(15'002));
    EXPECT_EQ(qty * price, price * qty);
}

TEST(Decimal, DivisionRoundsHalfAwayFromZero) {
    // 10 / 3 = 3.3333333... -> 3.333333
    EXPECT_EQ((Decimal::from_units(10) / 3).micros(), 3'333'333);
    // 20 / 3 = 6.6666666... -> 6.666667
    EXPECT_EQ((Decimal::from_units(20) / 3).micros(), 6'666'667);
    // Exact half: 5 micros / 2 = 2.5 -> 3, and -2.5 -> -3.
    EXPECT_EQ((Decimal::from_micros(5) / 2).micros(), 3);
    EXPECT_EQ((Decimal::from_micros(-5) / 2).micros(), -3);
    EXPECT_EQ((Decimal::from_micros(5) / -2).micros(), -3);
    EXPECT_EQ((Decimal::from_micros(6) / 2).micros(), 3);
}

TEST(Decimal, ScaledByBasisPoints) {
    // 1 bp of 15,002.00 is 1.5002.
    const Decimal notional = Decimal::from_units(15'002);
    EXPECT_EQ(notional.scaled_by(te::from_basis_points(1.0)),
              Decimal::from_micros(1'500'200));
}

TEST(Decimal, SignAndOrdering) {
    const Decimal a = Decimal::from_units(5);
    EXPECT_EQ(-a, Decimal::from_units(-5));
    EXPECT_LT(-a, Decimal{});
    EXPECT_GT(a, Decimal{});
    EXPECT_EQ(a - a, Decimal{});
}

TEST(Decimal, PrintsSixPlaces) {
    std::ostringstream os;
    os << Decimal::from_double(10.25) << ' ' << Decimal::from_double(-0.5) << ' '
       << Decimal{};
    EXPECT_EQ(os.str(), "10.250000 -0.500000 0.000000");
}

TEST(Decimal, RoundTripPnlReconcilesToTheCent) {
    // M5: buy 100 @ 10, sell 100 @ 12, fee f per fill -> realised 200 - 2f.
    const te::Quantity qty = 100;
    const te::Money fee = Decimal::from_double(1.37);
    const te::Money cash_out = Decimal::from_units(10) * qty + fee;
    const te::Money cash_in = Decimal::from_units(12) * qty - fee;
    EXPECT_EQ(cash_in - cash_out, Decimal::from_units(200) - fee * 2);
}
