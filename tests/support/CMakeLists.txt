# Header-only test doubles shared by the test binaries (docs/IMPLEMENTATION_PLAN.md,
# M1 work package 8). Link a test target against it to include them as
#   #include "support/<header>.hpp"
#
# Test-only: nothing under src/ or include/ may depend on this target.
add_library(trading_engine_test_support INTERFACE)
add_library(trading_engine::test_support ALIAS trading_engine_test_support)

# The include root is tests/, so the headers are spelled "support/...".
target_include_directories(trading_engine_test_support INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(trading_engine_test_support INTERFACE trading_engine::core)
target_compile_features(trading_engine_test_support INTERFACE cxx_std_20)
