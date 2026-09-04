# Assert that a shared build exports the API of jitterentropy.h and nothing
# else.
#
# The library is built with -fvisibility=hidden and, where the linker takes
# one, a version script - but neither is what decides the export set on its
# own. Marking an internal declaration with JENT_PRIVATE_STATIC overrides the
# visibility, and the version script is applied for neither Windows nor macOS,
# so an internal function so marked was exported there and nowhere else. That
# is what this catches, on the build rather than in the source: the five
# jent_gcd_* functions were exported from the macOS shared library for exactly
# that reason.
#
# Run in script mode with JENT_LIB, JENT_VERSION_SCRIPT, JENT_NM and JENT_NM_ARGS.

foreach(var JENT_LIB JENT_VERSION_SCRIPT JENT_NM)
    if(NOT DEFINED ${var})
        message(FATAL_ERROR "${var} is not set")
    endif()
endforeach()

if(NOT EXISTS "${JENT_LIB}")
    message(FATAL_ERROR "no library at ${JENT_LIB}")
endif()

# The export set of the built library.
execute_process(COMMAND "${JENT_NM}" ${JENT_NM_ARGS} "${JENT_LIB}"
                OUTPUT_VARIABLE nm_out
                ERROR_VARIABLE nm_err
                RESULT_VARIABLE nm_res)
if(NOT nm_res EQUAL 0)
    message(FATAL_ERROR "${JENT_NM} failed on ${JENT_LIB}: ${nm_res}\n${nm_err}")
endif()

string(REPLACE "\n" ";" nm_lines "${nm_out}")
set(exported "")
foreach(line IN LISTS nm_lines)
    # "<address> <type> <name>". An undefined symbol carries no address and
    # type U; a defined one is anything else, which is what is exported.
    if(line MATCHES "^[0-9a-fA-F]+[ \t]+([A-Za-z])[ \t]+_?(jent_[A-Za-z0-9_]+)")
        if(NOT CMAKE_MATCH_1 STREQUAL "U")
            list(APPEND exported "${CMAKE_MATCH_2}")
        endif()
    endif()
endforeach()

if(NOT exported)
    # A stripped table or an nm that read the wrong one. Reporting a pass here
    # would make the check silently vacuous, which is worse than not having it.
    message(FATAL_ERROR
        "${JENT_NM} listed no jent_* symbols in ${JENT_LIB} - the check "
        "cannot tell an empty export set from an unreadable one")
endif()

# What it is allowed to be: the global block of the version script, which is
# checked against jitterentropy.h at configure time.
file(STRINGS "${JENT_VERSION_SCRIPT}" script_lines)
set(allowed "")
set(in_global FALSE)
foreach(line IN LISTS script_lines)
    if(line MATCHES "global:")
        set(in_global TRUE)
    elseif(line MATCHES "local:")
        set(in_global FALSE)
    elseif(in_global AND line MATCHES "(jent_[A-Za-z0-9_]+)")
        list(APPEND allowed "${CMAKE_MATCH_1}")
    endif()
endforeach()

list(REMOVE_DUPLICATES exported)
list(REMOVE_DUPLICATES allowed)
list(SORT exported)
list(SORT allowed)

if(exported STREQUAL allowed)
    list(LENGTH exported n)
    message(STATUS "the shared library exports the ${n} functions of the API")
    return()
endif()

set(extra ${exported})
set(missing ${allowed})
if(allowed)
    list(REMOVE_ITEM extra ${allowed})
endif()
if(exported)
    list(REMOVE_ITEM missing ${exported})
endif()

message(FATAL_ERROR
    "the shared library does not export the API of version.lds. "
    "Exported but internal: ${extra}. Declared but not exported: ${missing}. "
    "An internal function appearing here is usually JENT_PRIVATE_STATIC on "
    "its declaration, which is the marker of the API and overrides "
    "-fvisibility=hidden.")
