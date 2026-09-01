# =============================================================================
#  Event-Driven Simulated Trading Engine  --  root build script
#
#  Scaffold only. This configures a reusable core library and a thin executable.
#  No trading algorithms, networking, PostgreSQL, threading, or execution logic
#  is implemented yet -- see docs/IMPLEMENTATION_PLAN.md.
#
#  Provisional decision: C++20 (see docs/adr/0001-provisional-architecture.md).
# =============================================================================
cmake_minimum_required(VERSION 3.21)

project(trading_engine
    VERSION      0.0.1
    DESCRIPTION  "Event-driven simulated trading engine (senior project scaffold)"
    HOMEPAGE_URL "https://example.invalid/senior-project"
    LANGUAGES    CXX)

# --- Language standard --------------------------------------------------------
# Provisional: C++20. Change here (and in the ADR) if the team picks otherwise.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --- Top-level-only conveniences -------------------------------------------
if(PROJECT_IS_TOP_LEVEL)
    set_property(GLOBAL PROPERTY USE_FOLDERS ON)

    # Single-config generators: default to Debug for the scaffold.
    get_property(_te_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
    if(NOT _te_multi_config AND NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Build type" FORCE)
        set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
            Debug Release RelWithDebInfo MinSizeRel)
    endif()

    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/lib")
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/lib")

    # Export compile_commands.json for editors / clang-tooling.
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif()

# --- Options ---------------------------------------------------------------
option(TRADING_ENGINE_BUILD_APPS          "Build the trading_engine executable"        ON)
option(TRADING_ENGINE_WARNINGS_AS_ERRORS  "Treat compiler warnings as errors"          OFF)
option(TRADING_ENGINE_BUILD_INTEGRATION_TESTS
       "Build integration tests (require external services -- not ready yet)"           OFF)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")
include(CompilerWarnings)

# --- Generated version header --------------------------------------------
set(TRADING_ENGINE_GENERATED_INCLUDE_DIR "${PROJECT_BINARY_DIR}/generated/include")
configure_file(
    "${PROJECT_SOURCE_DIR}/cmake/version.hpp.in"
    "${TRADING_ENGINE_GENERATED_INCLUDE_DIR}/trading_engine/version.hpp"
    @ONLY)

# --- Testing -------------------------------------------------------------
# include(CTest) provides the standard BUILD_TESTING option (default ON) and
# calls enable_testing().
include(CTest)

# --- Targets -----------------------------------------------------------------
add_subdirectory(src)

if(TRADING_ENGINE_BUILD_APPS)
    add_subdirectory(apps)
endif()

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()

# --- Configuration summary ----------------------------------------------
if(PROJECT_IS_TOP_LEVEL)
    message(STATUS "")
    message(STATUS "trading_engine ${PROJECT_VERSION} -- scaffold configuration")
    message(STATUS "  C++ standard .............. ${CMAKE_CXX_STANDARD}")
    message(STATUS "  Build apps ............... ${TRADING_ENGINE_BUILD_APPS}")
    message(STATUS "  Build tests (BUILD_TESTING) ${BUILD_TESTING}")
    message(STATUS "  Integration tests ....... ${TRADING_ENGINE_BUILD_INTEGRATION_TESTS}")
    message(STATUS "  Warnings as errors ...... ${TRADING_ENGINE_WARNINGS_AS_ERRORS}")
    message(STATUS "")
endif()
