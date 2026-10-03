# Fast unit tests -- no network, no database, no threads.

# Proves the GoogleTest target builds, links against the core library, and runs.
trading_engine_add_component_test(smoke
    smoke_test.cpp)

# Exercises the small pieces of real logic the scaffold does ship.
trading_engine_add_component_test(common
    common_identifiers_test.cpp
    common_clock_test.cpp
    common_decimal_test.cpp)

# Shape-only checks for the ADR 0002 (Proposed, not Accepted) TradeSignal
# additions and the new SignalCancelRequest type. Not validation -- neither
# enforces invariants yet; see docs/adr/0002-strategy-risk-signal-contract.md.
trading_engine_add_component_test(domain
    domain_trade_signal_test.cpp)

# Pins the scaffold contract: unimplemented stubs throw NotImplemented, they do
# not return fabricated data.
trading_engine_add_component_test(scaffold_contract
    scaffold_contract_test.cpp)

# The header-only doubles in tests/support. The StrategyEngine tests are only as
# trustworthy as the bus double they run against, so it is tested directly too.
trading_engine_add_component_test(support
    support_recording_event_bus_test.cpp)
target_link_libraries(te_test_support PRIVATE trading_engine::test_support)

# StrategyEngine behaviour (real engine, RecordingEventBus, FakeStrategy). The
# reference strategies have their own component, below.
trading_engine_add_component_test(strategy
    strategy_engine_registration_test.cpp
    strategy_engine_lifecycle_test.cpp
    strategy_engine_lifetime_test.cpp
    strategy_engine_signals_test.cpp
    strategy_engine_isolation_test.cpp)
target_link_libraries(te_test_strategy PRIVATE trading_engine::test_support)

# Exact decimal text <-> common::Decimal / Quantity (common/decimal_text.hpp), used by the
# reference strategies' signal metadata and by the Strategy Lab's CSV and JSON.
trading_engine_add_component_test(decimal_text
    common_decimal_text_test.cpp)

# Reference strategies, each driven directly and through the real StrategyEngine.
trading_engine_add_component_test(reference_strategies
    moving_average_crossover_strategy_test.cpp
    moving_average_crossover_robustness_test.cpp
    moving_average_crossover_engine_test.cpp
    mean_reversion_strategy_test.cpp
    mean_reversion_robustness_test.cpp
    mean_reversion_engine_test.cpp
    strategies_combined_engine_test.cpp
    strategy_diagnostics_test.cpp
    strategy_diagnostics_invariance_test.cpp)
target_link_libraries(te_test_reference_strategies PRIVATE trading_engine::test_support)

# Strategy Lab tests (docs/STRATEGY_LAB.md). Registered only when the OPTIONAL lab is built
# (-DTRADING_ENGINE_BUILD_STRATEGY_LAB=ON; apps/CMakeLists.txt adds it before this directory), so the
# default build is unchanged. The lab itself needs neither GoogleTest nor Python; only these tests do.
if(TARGET trading_engine_strategy_lab)
    trading_engine_add_component_test(strategy_lab
        strategy_lab_foundation_test.cpp
        strategy_lab_dataset_test.cpp
        strategy_lab_bus_test.cpp
        strategy_lab_catalog_test.cpp
        strategy_lab_replay_test.cpp
        strategy_lab_replay_events_test.cpp
        strategy_lab_cli_test.cpp)
    target_link_libraries(te_test_strategy_lab PRIVATE trading_engine::strategy_lab trading_engine::test_support)
    target_compile_definitions(te_test_strategy_lab PRIVATE
        STRATEGY_LAB_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures/strategy_lab")

    # Process-level tests: the real executable, and a real JSON parser (Python's, stdlib only). The probe
    # writes documents real runs cannot produce (NaN prices, hostile text); it needs no GoogleTest.
    add_executable(strategy_lab_json_probe "${PROJECT_SOURCE_DIR}/tests/strategy_lab/json_probe.cpp")
    target_link_libraries(strategy_lab_json_probe PRIVATE trading_engine::strategy_lab)
    target_compile_features(strategy_lab_json_probe PRIVATE cxx_std_20)
    trading_engine_set_warnings(strategy_lab_json_probe)
    set_target_properties(strategy_lab_json_probe PROPERTIES FOLDER "tests")

    find_package(Python3 QUIET COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME strategy_lab_python
            COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/strategy_lab/run_tests.py"
                    --exe $<TARGET_FILE:strategy_lab_replay>
                    --probe $<TARGET_FILE:strategy_lab_json_probe>
                    --fixtures "${PROJECT_SOURCE_DIR}/tests/fixtures/strategy_lab")
        set_tests_properties(strategy_lab_python PROPERTIES LABELS "strategy_lab" TIMEOUT 600)
    else()
        message(STATUS "Strategy Lab: no Python 3 interpreter found; the process-level JSON tests (strategy_lab_python) are not registered")
    endif()
endif()

# TODO(team): add one call per component as behaviour lands, e.g.
#   trading_engine_add_component_test(market_data market_data_service_test.cpp)
#   trading_engine_add_component_test(risk        risk_manager_test.cpp)
#   trading_engine_add_component_test(execution   execution_simulator_test.cpp)
#   trading_engine_add_component_test(portfolio   portfolio_manager_test.cpp)
#   trading_engine_add_component_test(analytics   performance_analyzer_test.cpp)
