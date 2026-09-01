// Smoke test: proves the GoogleTest wiring compiles, links against the core
// library, and runs. Minimal, but every assertion checks a real value -- there
// are no always-true placeholders here.

#include <string_view>

#include <gtest/gtest.h>

#include "trading_engine/version.hpp"

TEST(Smoke, VersionHeaderIsPopulated) {
    EXPECT_FALSE(std::string_view{trading_engine::kProjectName}.empty());
    EXPECT_FALSE(std::string_view{trading_engine::kVersion}.empty());
    EXPECT_EQ(std::string_view{trading_engine::kProjectName}, "trading_engine");
}

TEST(Smoke, VersionComponentsMatchProjectVersion) {
    // Project version is 0.0.1 in the top-level CMakeLists.txt.
    EXPECT_EQ(trading_engine::kVersionMajor, 0u);
    EXPECT_EQ(trading_engine::kVersionMinor, 0u);
    EXPECT_EQ(trading_engine::kVersionPatch, 1u);
}

TEST(Smoke, BuildIsMarkedAsScaffold) {
    // Flip to false in cmake/version.hpp.in once an end-to-end path runs.
    EXPECT_TRUE(trading_engine::kIsScaffold);
}
