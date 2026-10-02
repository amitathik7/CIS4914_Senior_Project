# Thin executable. Links the core library plus both adapters so that a build
# failure anywhere surfaces here. Contains no logic of its own -- see
# apps/trading_engine_main.cpp.
add_executable(trading_engine trading_engine_main.cpp)

target_link_libraries(trading_engine PRIVATE
    trading_engine::core
    trading_engine::alpaca_adapter
    trading_engine::postgres_adapter)

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
