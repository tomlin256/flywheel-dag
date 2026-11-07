# check_sanitize_flags.cmake — test_sanitize_flags: the FLYWHEEL_DAG_SANITIZE mode mapping in
# cmake/flywheel_dag_sanitize.cmake.
#
# Run by ctest as
#
#   cmake -DSOURCE_DIR=<source> -P check_sanitize_flags.cmake
#
# It needs no configure and no build tree, so it is registered in every tree and finishes in milliseconds.
#
# A mode that is accepted is checked in-process: the exact compile and link options it gives, compared as strings so that their
# order counts as well as their presence. A mode that is refused ends in a FATAL_ERROR, and that ends a `cmake -P` run, so each one
# is tried in a child run of this same script (-DCHILD_MODE=<value>) whose exit status and message are then checked. The message
# is part of the contract: it names the value and lists the modes, because a mistyped value is a configure error and whoever reads
# it needs to see what to type instead.
#
# SOURCE_DIR is a parameter so that a hand-made change to a copy of the module is passed here, and must fail.

cmake_minimum_required(VERSION 4.0)

if(NOT DEFINED SOURCE_DIR)
    message(FATAL_ERROR "usage: cmake -DSOURCE_DIR=<dir> -P check_sanitize_flags.cmake")
endif()
include("${SOURCE_DIR}/cmake/flywheel_dag_sanitize.cmake")

# Child run: map one mode and let any FATAL_ERROR end the process.
if(DEFINED CHILD_MODE)
    flywheel_dag_sanitizer_flags("${CHILD_MODE}" ignored_compile ignored_link)
    return()
endif()

set(SELF "${CMAKE_CURRENT_LIST_FILE}")
set(failures "")

# The options of an accepted mode.
function(expect_flags mode want_compile want_link)
    flywheel_dag_sanitizer_flags("${mode}" got_compile got_link)
    if(NOT "${got_compile}" STREQUAL "${want_compile}")
        string(APPEND failures "\n  '${mode}': compile options '${got_compile}', want '${want_compile}'")
    endif()
    if(NOT "${got_link}" STREQUAL "${want_link}")
        string(APPEND failures "\n  '${mode}': link options '${got_link}', want '${want_link}'")
    endif()
    set(failures "${failures}" PARENT_SCOPE)
endfunction()

# A mode that must be refused: non-zero exit, the value named, the modes listed.
function(expect_rejected mode)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DSOURCE_DIR=${SOURCE_DIR}" "-DCHILD_MODE=${mode}" -P "${SELF}"
        RESULT_VARIABLE status
        OUTPUT_QUIET
        ERROR_VARIABLE message)
    if("${status}" STREQUAL "0")
        string(APPEND failures "\n  '${mode}' was accepted, want a configure error")
    else()
        # CMake word-wraps a FATAL_ERROR at about 78 columns, which can split the list of modes across two lines. The words are
        # the contract, not where the lines break.
        string(REGEX REPLACE "[ \t\r\n]+" " " words "${message}")
        string(FIND "${words}" "FLYWHEEL_DAG_SANITIZE='${mode}'" names_value)
        string(FIND "${words}" "OFF, asan, tsan" lists_modes)
        if(names_value EQUAL -1)
            string(APPEND failures "\n  '${mode}': the error does not name the value:\n${message}")
        endif()
        if(lists_modes EQUAL -1)
            string(APPEND failures "\n  '${mode}': the error does not list the modes:\n${message}")
        endif()
    endif()
    set(failures "${failures}" PARENT_SCOPE)
endfunction()

expect_flags("OFF" "" "")
expect_flags("" "" "")
expect_flags("asan"
    "-fsanitize=address,undefined;-fno-omit-frame-pointer;-fno-sanitize-recover=undefined"
    "-fsanitize=address,undefined")
expect_flags("tsan"
    "-fsanitize=thread;-fno-omit-frame-pointer"
    "-fsanitize=thread")

# Near misses and plausible wrong guesses. None may quietly mean "no sanitizer", or mean a sanitizer that was not named: the match
# is exact and case-sensitive.
foreach(bad IN ITEMS
        address ASAN Asan thread TSAN msan adress ON off asan,tsan -fsanitize=address ubsan lsan)
    expect_rejected("${bad}")
endforeach()

if(failures)
    message(FATAL_ERROR "test_sanitize_flags failed:${failures}")
endif()
message(STATUS "test_sanitize_flags: ok (OFF, asan and tsan map to their options; every other value is refused)")
