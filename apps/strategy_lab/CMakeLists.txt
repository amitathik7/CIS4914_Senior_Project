# =============================================================================
#  Strategy Lab: an OPTIONAL developer/demo tool (docs/STRATEGY_LAB.md).
#
#  Built only with -DTRADING_ENGINE_BUILD_STRATEGY_LAB=ON (see apps/CMakeLists.txt).
#  Depends on trading_engine::core and the C++ standard library ONLY: no GoogleTest, no
#  tests/support, no Python, no JSON library, no UI dependency. It builds with
#  BUILD_TESTING=OFF. Its tests live in tests/ and are registered only when testing is on.
#
#  Files are listed explicitly (no globbing), like the rest of the project.
# =============================================================================

add_library(trading_engine_strategy_lab STATIC
    lab/catalog.cpp
    lab/cli.cpp
    lab/dataset.cpp
    lab/demo_bus.cpp
    lab/io.cpp
    lab/json_writer.cpp
    lab/result_json.cpp
    lab/runner.cpp
    lab/session.cpp
    lab/sha256.cpp
    lab/timestamp.cpp)
add_library(trading_engine::strategy_lab ALIAS trading_engine_strategy_lab)

# PUBLIC so a test target can include "lab/...".
target_include_directories(trading_engine_strategy_lab PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(trading_engine_strategy_lab PUBLIC trading_engine::core)
target_compile_features(trading_engine_strategy_lab PUBLIC cxx_std_20)
trading_engine_set_warnings(trading_engine_strategy_lab)
set_target_properties(trading_engine_strategy_lab PROPERTIES
    FOLDER "trading_engine/strategy_lab"
    POSITION_INDEPENDENT_CODE ON)

add_executable(strategy_lab_replay main.cpp)
target_link_libraries(strategy_lab_replay PRIVATE trading_engine::strategy_lab)
target_compile_features(strategy_lab_replay PRIVATE cxx_std_20)
trading_engine_set_warnings(strategy_lab_replay)
set_target_properties(strategy_lab_replay PROPERTIES FOLDER "trading_engine/strategy_lab")
