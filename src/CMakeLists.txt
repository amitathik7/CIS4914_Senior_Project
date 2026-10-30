# =============================================================================
#  Library targets.
#
#  Source files are listed explicitly (NO recursive globbing) so that adding a
#  file is a deliberate, reviewable change and CMake always knows when to
#  re-configure.
# =============================================================================

# -----------------------------------------------------------------------------
#  trading_engine_core -- domain model, interfaces, and the service/orchestration
#  classes that depend ONLY on those interfaces. Zero third-party dependencies:
#  nothing here may include an Alpaca, PostgreSQL, or JSON-library header.
# -----------------------------------------------------------------------------
add_library(trading_engine_core STATIC
    common/clock.cpp
    common/timestamp_text.cpp
    domain/domain.cpp
    configuration/engine_config.cpp
    events/event_bus.cpp
    events/event_codec.cpp
    events/json.cpp
    events/message_transport.cpp
    events/topic_event_bus.cpp
    market_data/market_data_source.cpp
    market_data/market_data_service.cpp
    strategy/strategy.cpp
    strategy/strategy_engine.cpp
    strategy/moving_average_crossover_strategy.cpp
    strategy/mean_reversion_strategy.cpp
    strategy/logistic_model.cpp
    strategy/logistic_regression_strategy.cpp
    risk/risk_policy.cpp
    risk/risk_manager.cpp
    execution/fill_model.cpp
    execution/execution_simulator.cpp
    portfolio/portfolio_manager.cpp
    portfolio/portfolio_bus_bridge.cpp
    persistence/repositories.cpp
    analytics/performance_analyzer.cpp
    analytics/system_metrics.cpp
    backtesting/historical_replay.cpp
    app/pipeline.cpp
    app/trading_engine.cpp)
add_library(trading_engine::core ALIAS trading_engine_core)

target_include_directories(trading_engine_core PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${TRADING_ENGINE_GENERATED_INCLUDE_DIR}>)

target_compile_features(trading_engine_core PUBLIC cxx_std_20)
# The event buses run a dispatcher thread.
find_package(Threads REQUIRED)
target_link_libraries(trading_engine_core PUBLIC Threads::Threads)
trading_engine_set_warnings(trading_engine_core)
set_target_properties(trading_engine_core PROPERTIES
    FOLDER "trading_engine"
    POSITION_INDEPENDENT_CODE ON)

# -----------------------------------------------------------------------------
#  Adapter targets -- one per future third-party dependency.
#
#  The core library never links these. Only the executable and (later)
#  integration tests do. When a real dependency is introduced, its
#  find_package()/FetchContent and target_link_libraries calls go in THIS file,
#  scoped to the relevant adapter target and PRIVATE where possible, so the
#  dependency never becomes transitive to the core or to strategies.
# -----------------------------------------------------------------------------

# Alpaca market-data adapter (future: WebSocket + HTTP + TLS + JSON).
add_library(trading_engine_alpaca_adapter STATIC
    market_data/alpaca_live_source.cpp
    market_data/alpaca_historical_source.cpp)
add_library(trading_engine::alpaca_adapter ALIAS trading_engine_alpaca_adapter)
target_link_libraries(trading_engine_alpaca_adapter PUBLIC trading_engine_core)
target_compile_features(trading_engine_alpaca_adapter PUBLIC cxx_std_20)
trading_engine_set_warnings(trading_engine_alpaca_adapter)
set_target_properties(trading_engine_alpaca_adapter PROPERTIES
    FOLDER "trading_engine/adapters"
    POSITION_INDEPENDENT_CODE ON)
# TODO(alpaca): find_package(OpenSSL); FetchContent websocketpp / Boost.Beast;
#               a JSON library -- all PRIVATE to this target.

# PostgreSQL persistence adapter (future: libpq / libpqxx).
add_library(trading_engine_postgres_adapter STATIC
    persistence/postgres_repository.cpp)
add_library(trading_engine::postgres_adapter ALIAS trading_engine_postgres_adapter)
target_link_libraries(trading_engine_postgres_adapter PUBLIC trading_engine_core)
target_compile_features(trading_engine_postgres_adapter PUBLIC cxx_std_20)
trading_engine_set_warnings(trading_engine_postgres_adapter)
set_target_properties(trading_engine_postgres_adapter PROPERTIES
    FOLDER "trading_engine/adapters"
    POSITION_INDEPENDENT_CODE ON)
# TODO(postgres): find_package(PostgreSQL) or FetchContent libpqxx -- PRIVATE.

# Kafka event-bus adapter (librdkafka) -- OPTIONAL, -DTRADING_ENGINE_WITH_KAFKA=ON.
# Implements events::IMessageTransport for events::TopicEventBus. librdkafka is
# found as a CMake package (vcpkg: `vcpkg install librdkafka`, target
# RdKafka::rdkafka) or through pkg-config (apt: librdkafka-dev, brew: librdkafka)
# and linked PRIVATE, so it never becomes a dependency of the core.
if(TRADING_ENGINE_WITH_KAFKA)
    add_library(trading_engine_kafka_adapter STATIC
        kafka/rdkafka_transport.cpp)
    add_library(trading_engine::kafka_adapter ALIAS trading_engine_kafka_adapter)
    target_link_libraries(trading_engine_kafka_adapter PUBLIC trading_engine_core)
    target_compile_features(trading_engine_kafka_adapter PUBLIC cxx_std_20)
    trading_engine_set_warnings(trading_engine_kafka_adapter)
    set_target_properties(trading_engine_kafka_adapter PROPERTIES
        FOLDER "trading_engine/adapters"
        POSITION_INDEPENDENT_CODE ON)

    find_package(RdKafka CONFIG QUIET)
    if(TARGET RdKafka::rdkafka)
        message(STATUS "librdkafka: using CMake package (${RdKafka_DIR})")
        target_link_libraries(trading_engine_kafka_adapter PRIVATE RdKafka::rdkafka)
    else()
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(RDKAFKA IMPORTED_TARGET rdkafka)
        endif()
        if(NOT RDKAFKA_FOUND)
            message(FATAL_ERROR
                "TRADING_ENGINE_WITH_KAFKA=ON but librdkafka was not found. Install it "
                "(vcpkg install librdkafka / apt install librdkafka-dev / brew install "
                "librdkafka) or configure with -DTRADING_ENGINE_WITH_KAFKA=OFF.")
        endif()
        message(STATUS "librdkafka: using pkg-config (${RDKAFKA_VERSION})")
        target_link_libraries(trading_engine_kafka_adapter PRIVATE PkgConfig::RDKAFKA)
    endif()
endif()
