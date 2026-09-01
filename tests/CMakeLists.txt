# =============================================================================
#  Test tree.
#
#  GoogleTest is pulled in here (not at the top level) so a configure with
#  -DBUILD_TESTING=OFF needs no test dependency at all.
#
#  Layout:
#    unit/         -- fast, no external services; built by default
#    integration/  -- real PostgreSQL / Alpaca; opt-in via
#                     -DTRADING_ENGINE_BUILD_INTEGRATION_TESTS=ON
#    fixtures/     -- recorded payloads / CSV data for the above
# =============================================================================

include(Dependencies)   # cmake/Dependencies.cmake -> GoogleTest
include(GoogleTest)      # gtest_discover_tests()

# Register one test executable per component. Keeps link times low and lets
# `ctest -L <component>` filter. Call once per component as tests are added.
function(trading_engine_add_component_test component)
    set(target "te_test_${component}")
    add_executable(${target} ${ARGN})
    target_link_libraries(${target} PRIVATE
        trading_engine::core
        trading_engine::alpaca_adapter
        trading_engine::postgres_adapter
        GTest::gtest_main)
    target_compile_features(${target} PRIVATE cxx_std_20)
    trading_engine_set_warnings(${target})
    set_target_properties(${target} PROPERTIES FOLDER "tests")
    gtest_discover_tests(${target}
        PROPERTIES LABELS "${component}"
        DISCOVERY_MODE PRE_TEST)
endfunction()

add_subdirectory(unit)

if(TRADING_ENGINE_BUILD_INTEGRATION_TESTS)
    add_subdirectory(integration)
endif()
