# Assert that a shared build exports the API of jitterentropy.h and nothing
# else. JENT_PRIVATE_STATIC on an internal declaration overrides
# -fvisibility=hidden, and the version script is not applied on macOS or
# Windows, so only the built library tells.
#
# nm prints one of three formats: BSD (the default of GNU, LLVM and Apple),
# POSIX (-P) and SVR4 (Solaris). Output none of them parses is a skip; a table
# that parses but holds no jent_ symbol fails.
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

# Ask for the POSIX format; an nm rejecting -P is asked again without it.
execute_process(COMMAND "${JENT_NM}" -P ${JENT_NM_ARGS} "${JENT_LIB}"
                OUTPUT_VARIABLE nm_out
                ERROR_VARIABLE nm_err
                RESULT_VARIABLE nm_res)
if(NOT nm_res EQUAL 0)
    execute_process(COMMAND "${JENT_NM}" ${JENT_NM_ARGS} "${JENT_LIB}"
                    OUTPUT_VARIABLE nm_out
                    ERROR_VARIABLE nm_err
                    RESULT_VARIABLE nm_res)
endif()
if(NOT nm_res EQUAL 0)
    message(FATAL_ERROR "${JENT_NM} failed on ${JENT_LIB}: ${nm_res}\n${nm_err}")
endif()

# A symbol name; the Mach-O leading underscore is matched and dropped.
set(sym "[A-Za-z_][A-Za-z0-9_.$@]*")

string(REPLACE "\n" ";" nm_lines "${nm_out}")
set(exported "")
set(parsed 0)
foreach(line IN LISTS nm_lines)
    set(name "")
    set(defined TRUE)

    if(line MATCHES "^_?(${sym})[ \t]+([A-Za-z])([ \t]|$)")
        # POSIX: "<name> <type> <value> <size>".
        set(name "${CMAKE_MATCH_1}")
        if(CMAKE_MATCH_2 STREQUAL "U")
            set(defined FALSE)
        endif()
    elseif(line MATCHES "^[0-9a-fA-F]*[ \t]+([A-Za-z])[ \t]+_?(${sym})")
        # BSD: "<value> <type> <name>", an undefined symbol carrying no value.
        set(name "${CMAKE_MATCH_2}")
        if(CMAKE_MATCH_1 STREQUAL "U")
            set(defined FALSE)
        endif()
    elseif(line MATCHES "^\\[[0-9]+\\][ \t]*\\|.*\\|[ \t]*_?(${sym})[ \t]*$")
        # SVR4: "[i] |value|size|type|bind|other|shndx|name", the section
        # index of an undefined symbol being UNDEF.
        set(name "${CMAKE_MATCH_1}")
        if(line MATCHES "\\|[ \t]*UNDEF")
            set(defined FALSE)
        endif()
    else()
        continue()
    endif()

    math(EXPR parsed "${parsed} + 1")
    if(defined AND name MATCHES "^jent_")
        list(APPEND exported "${name}")
    endif()
endforeach()

if(parsed EQUAL 0)
    # An unknown format rather than an empty export set: skip, never pass,
    # and quote the output so the format can be added.
    string(REPLACE ";" "\n" nm_head "${nm_lines}")
    string(LENGTH "${nm_head}" nm_len)
    if(nm_len GREATER 400)
        string(SUBSTRING "${nm_head}" 0 400 nm_head)
    endif()
    message("the export check does not know the output format of ${JENT_NM}")
    message("no symbol parsed from:\n${nm_head}")
    return()
endif()

if(NOT exported)
    message(FATAL_ERROR
        "${JENT_NM} parsed ${parsed} symbols in ${JENT_LIB} but not one "
        "jent_* among them - the shared library exports none of its API")
endif()

# The allowed set: the global block of the version script.
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
