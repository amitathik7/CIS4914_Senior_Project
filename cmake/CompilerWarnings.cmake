# -----------------------------------------------------------------------------
#  trading_engine_set_warnings(<target>)
#
#  Applies a sensible, portable warning set to a target as PRIVATE options so
#  the flags do not leak to consumers. Honours the global cache option
#  TRADING_ENGINE_WARNINGS_AS_ERRORS.
#
#  TODO(team): once the codebase has real logic, consider promoting warnings to
#  errors in CI, and adding clang-tidy / cppcheck / include-what-you-use passes.
# -----------------------------------------------------------------------------

function(trading_engine_set_warnings target)
    set(_msvc_warnings
        /W4
        /permissive-        # stricter standard conformance
        /w14242 /w14254 /w14263 /w14265 /w14287 /w14296 /w14311
        /w14545 /w14546 /w14547 /w14549 /w14555
        /w14619 /w14640 /w14826 /w14905 /w14906 /w14928
        /wd4251)           # 'type' needs dll-interface -- noise for static libs

    set(_gnu_clang_warnings
        -Wall
        -Wextra
        -Wpedantic
        -Wshadow
        -Wnon-virtual-dtor
        -Wcast-align
        -Wunused
        -Woverloaded-virtual
        -Wconversion
        -Wsign-conversion
        -Wnull-dereference
        -Wdouble-promotion
        -Wformat=2
        -Wimplicit-fallthrough)

    if(MSVC)
        set(_warnings ${_msvc_warnings})
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
        set(_warnings ${_gnu_clang_warnings})
    else()
        set(_warnings "")
        message(WARNING
            "trading_engine_set_warnings: unrecognised compiler "
            "'${CMAKE_CXX_COMPILER_ID}' -- no warning flags applied.")
    endif()

    if(TRADING_ENGINE_WARNINGS_AS_ERRORS)
        if(MSVC)
            list(APPEND _warnings /WX)
        else()
            list(APPEND _warnings -Werror)
        endif()
    endif()

    target_compile_options(${target} PRIVATE ${_warnings})
endfunction()
