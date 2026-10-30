# The trading_engine CLI: runs the components that exist today wired through
# one event bus (apps/trading_engine_main.cpp, docs/EVENT_BUS.md). Links the
# core library plus every adapter so a build failure anywhere surfaces here.
add_executable(trading_engine
    trading_engine_main.cpp
    bar_csv.cpp)

target_link_libraries(trading_engine PRIVATE
    trading_engine::core
    trading_engine::alpaca_adapter
    trading_engine::postgres_adapter)

if(TARGET trading_engine_kafka_adapter)
    target_link_libraries(trading_engine PRIVATE trading_engine::kafka_adapter)
    target_compile_definitions(trading_engine PRIVATE TRADING_ENGINE_HAVE_KAFKA=1)
endif()

target_compile_features(trading_engine PRIVATE cxx_std_20)
trading_engine_set_warnings(trading_engine)
set_target_properties(trading_engine PROPERTIES FOLDER "trading_engine")

# Optional developer/demo tool (docs/STRATEGY_LAB.md). Off by default, so the default build
# gains no target and needs nothing new. Depends only on trading_engine::core: no Python, no
# UI library, no GoogleTest, and it builds with BUILD_TESTING=OFF.
option(TRADING_ENGINE_BUILD_STRATEGY_LAB
       "Build strategy_lab_replay, the optional strategy replay/diagnostics tool" OFF)
if(TRADING_ENGINE_BUILD_STRATEGY_LAB)
    add_subdirectory(strategy_lab)
endif()
