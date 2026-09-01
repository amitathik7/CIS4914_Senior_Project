# -----------------------------------------------------------------------------
#  Third-party dependencies for the scaffold.
#
#  The ONLY hard dependency today is GoogleTest, and only when BUILD_TESTING is
#  ON. PostgreSQL and Alpaca client libraries are deliberately NOT required to
#  configure or build the scaffold -- their future find_package()/FetchContent
#  calls must live in the corresponding adapter target's CMakeLists.txt
#  (src/ alpaca / postgres adapter sections), never here or in the core library.
# -----------------------------------------------------------------------------

include_guard(GLOBAL)

# --- GoogleTest ------------------------------------------------------------
# Strategy: prefer an installed package; otherwise fetch a pinned release.
set(TRADING_ENGINE_GTEST_TAG "v1.15.2"
    CACHE STRING "Pinned GoogleTest git tag used when no system package is found")
option(TRADING_ENGINE_FORCE_FETCH_GTEST
       "Ignore any installed GTest and always fetch the pinned tag" OFF)

set(_te_have_gtest FALSE)

if(NOT TRADING_ENGINE_FORCE_FETCH_GTEST)
    find_package(GTest CONFIG QUIET)
    if(GTest_FOUND)
        set(_te_have_gtest TRUE)
        message(STATUS "GoogleTest: using installed package (${GTest_DIR})")
    endif()
endif()

if(NOT _te_have_gtest)
    message(STATUS "GoogleTest: fetching pinned ${TRADING_ENGINE_GTEST_TAG}")
    include(FetchContent)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        ${TRADING_ENGINE_GTEST_TAG}
        GIT_SHALLOW    TRUE
        GIT_PROGRESS   TRUE)

    # On MSVC, match the dynamic CRT used by this project to avoid link errors.
    set(gtest_force_shared_crt ON  CACHE BOOL "" FORCE)
    set(INSTALL_GTEST          OFF CACHE BOOL "" FORCE)
    set(BUILD_GMOCK            ON  CACHE BOOL "" FORCE)

    FetchContent_MakeAvailable(googletest)
endif()

# Normalise target names so callers can always link GTest::gtest_main.
if(NOT TARGET GTest::gtest_main AND TARGET gtest_main)
    add_library(GTest::gtest_main ALIAS gtest_main)
endif()
if(NOT TARGET GTest::gtest AND TARGET gtest)
    add_library(GTest::gtest ALIAS gtest)
endif()
