// Real behaviour the scaffold ships: strongly-typed ids and the sequential id
// generator.

#include <cstdint>
#include <set>

#include <gtest/gtest.h>

#include "trading_engine/common/identifiers.hpp"

namespace te = trading_engine::common;

TEST(StrongId, DefaultConstructedIsZeroAndInvalid) {
    const te::OrderId id{};
    EXPECT_EQ(id.value, 0u);
    EXPECT_FALSE(id.valid());
}

TEST(StrongId, ComparisonAndValidity) {
    EXPECT_TRUE(te::OrderId{1}.valid());
    EXPECT_EQ(te::OrderId{7}, te::OrderId{7});
    EXPECT_NE(te::OrderId{7}, te::OrderId{8});
    EXPECT_LT(te::OrderId{7}, te::OrderId{8});
}

TEST(SequentialIdGenerator, StartsAtOneSoZeroAlwaysMeansUnset) {
    te::SequentialIdGenerator<te::OrderId> gen;
    EXPECT_EQ(gen.issued(), 0u);
    EXPECT_EQ(gen.next().value, 1u);
    EXPECT_EQ(gen.next().value, 2u);
    EXPECT_EQ(gen.issued(), 2u);
}

TEST(SequentialIdGenerator, ProducesUniqueMonotonicIds) {
    te::SequentialIdGenerator<te::FillId> gen;
    std::set<std::uint64_t> seen;
    std::uint64_t previous = 0;

    for (int i = 0; i < 1000; ++i) {
        const te::FillId id = gen.next();
        EXPECT_GT(id.value, previous) << "ids must strictly increase";
        EXPECT_TRUE(seen.insert(id.value).second) << "duplicate id " << id.value;
        previous = id.value;
    }
    EXPECT_EQ(gen.issued(), 1000u);
    EXPECT_EQ(previous, 1000u);
}
