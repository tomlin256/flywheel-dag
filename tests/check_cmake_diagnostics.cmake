# check_cmake_diagnostics.cmake — test_cmake_diagnostics: the strict configure flags are
# spelled the way CMake 4.4 spells them wherever they are written, and they make a CMake
# author warning and a deprecation fail a configure.
#
# Run by ctest as
#
#   cmake -DSOURCE_DIR=<source> -DSCRATCH_DIR=<dir> -DGENERATOR=<generator>
#         -P check_cmake_diagnostics.cmake
#
# CI configures with -Werror=author -Werror=deprecated, so any CMake author or
# deprecation warning, ours or a dependency's, fails the run. CMake 4.4 calls the
# "dev" category "author": -Werror=dev still works there and prints "The error=dev
# option is deprecated". A CMake before 4.4 has no categories and ignores
# -Werror=author, or any other -W it does not know, without a word. So a gate spelled
# wrong, or run by an older CMake, still passes CI. That is why the gate itself is
# tested, as test_warning_flags tests the compiler's.
#
# Two things are held. The spelling: CI's workflow, README.md and CLAUDE.md each say
# -Werror=author -Werror=deprecated and none says -Werror=dev. The behaviour: a probe
# project that says message(AUTHOR_WARNING) or message(DEPRECATION), configured with
# the flags in this file, fails to configure, and without them it warns. The probe
# needs no network and compiles nothing. This file's flags are the oracle, and the
# spelling check holds the three files to them.
#
# The behaviour needs CMake 4.4. A CMake before it is one the README still allows and
# no CI job runs, so the behaviour is skipped, and the status line says so; the spelling
# is still checked.

cmake_minimum_required(VERSION 4.0)

foreach(var IN ITEMS SOURCE_DIR SCRATCH_DIR GENERATOR)
    if(NOT ${var})
        message(FATAL_ERROR "usage: cmake -DSOURCE_DIR=<dir> -DSCRATCH_DIR=<dir> "
                            "-DGENERATOR=<name> -P check_cmake_diagnostics.cmake")
    endif()
endforeach()

set(strict_flags -Werror=author -Werror=deprecated)
list(JOIN strict_flags " " strict_text)
set(stale_flag -Werror=dev)

set(problems "")

# ── the spelling ─────────────────────────────────────────────────────────────
set(spelled
    .github/workflows/ci.yml
    README.md
    CLAUDE.md)
foreach(file IN LISTS spelled)
    if(NOT EXISTS "${SOURCE_DIR}/${file}")
        string(APPEND problems "  ${file} cannot be read\n")
        continue()
    endif()
    file(READ "${SOURCE_DIR}/${file}" text)
    string(FIND "${text}" "${strict_text}" at)
    if(at EQUAL -1)
        string(APPEND problems "  ${file} does not say ${strict_text}\n")
    endif()
    string(FIND "${text}" "${stale_flag}" at)
    if(NOT at EQUAL -1)
        string(APPEND problems "  ${file} still says ${stale_flag}, which CMake 4.4 deprecates\n")
    endif()
endforeach()
list(LENGTH spelled spelled_count)

# ── the behaviour ────────────────────────────────────────────────────────────
file(REMOVE_RECURSE "${SCRATCH_DIR}")

# probe(<rc> <output> <name> <kind> [<flags>...]): configure a project that says
# message(<kind> "probe") with <flags>. <output> holds the output and the error output
# together.
function(probe rc_var out_var name kind)
    set(dir "${SCRATCH_DIR}/${name}")
    file(MAKE_DIRECTORY "${dir}")
    file(WRITE "${dir}/CMakeLists.txt"
         "cmake_minimum_required(VERSION 4.0)\n"
         "project(probe NONE)\n"
         "message(${kind} \"probe\")\n")
    execute_process(
        COMMAND ${CMAKE_COMMAND} -G "${GENERATOR}" -S "${dir}" -B "${dir}/build" ${ARGN}
        RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    set(${rc_var} "${rc}" PARENT_SCOPE)
    set(${out_var} "${out}${err}" PARENT_SCOPE)
endfunction()

# A failed configure exits with a positive number. Anything else, such as the text
# execute_process gives when it cannot start CMake, is not a failure CMake reported.
function(failed var out)
    if(NOT "${${var}}" MATCHES "^[1-9][0-9]*$")
        set(${out} FALSE PARENT_SCOPE)
    else()
        set(${out} TRUE PARENT_SCOPE)
    endif()
endfunction()

if(CMAKE_VERSION VERSION_LESS 4.4)
    string(CONCAT behaviour "skipped: CMake ${CMAKE_VERSION} ignores -Werror=author, which "
                            "came with CMake 4.4's diagnostic categories")
else()
    set(cases 0)

    # A clean project, configured with the strict flags, exits 0 and prints no notice.
    probe(rc out clean STATUS ${strict_flags})
    if(NOT rc STREQUAL "0")
        string(APPEND problems "  ${strict_text} on a clean project exited ${rc}:\n${out}\n")
    endif()
    if(out MATCHES "option is deprecated")
        string(APPEND problems "  ${strict_text} on a clean project printed a deprecation "
                               "notice:\n${out}\n")
    endif()
    math(EXPR cases "${cases} + 1")

    probe(rc out author AUTHOR_WARNING ${strict_flags})
    failed(rc did_fail)
    if(NOT did_fail OR NOT out MATCHES "CMake Error \\(author\\)")
        string(APPEND problems "  ${strict_text} did not make message(AUTHOR_WARNING) a "
                               "CMake Error (author); exit ${rc}:\n${out}\n")
    endif()
    math(EXPR cases "${cases} + 1")

    probe(rc out deprecation DEPRECATION ${strict_flags})
    failed(rc did_fail)
    if(NOT did_fail OR NOT out MATCHES "CMake Error \\(deprecated\\)")
        string(APPEND problems "  ${strict_text} did not make message(DEPRECATION) a "
                               "CMake Error (deprecated); exit ${rc}:\n${out}\n")
    endif()
    math(EXPR cases "${cases} + 1")

    # Without the flags it is a warning: the flags are what makes it fatal, not the probe.
    probe(rc out author_unflagged AUTHOR_WARNING)
    if(NOT rc STREQUAL "0" OR NOT out MATCHES "CMake Warning \\(author\\)")
        string(APPEND problems "  message(AUTHOR_WARNING) without the flags was not a "
                               "CMake Warning (author) that configured; exit ${rc}:\n${out}\n")
    endif()
    math(EXPR cases "${cases} + 1")

    # -Werror=dev draws a notice or fails on this CMake. Were it clean, the clean run
    # of the strict flags in the first case would prove nothing.
    probe(rc out stale STATUS ${stale_flag})
    failed(rc did_fail)
    if(NOT did_fail AND NOT out MATCHES "option is deprecated")
        string(APPEND problems "  ${stale_flag} on a clean project is clean on CMake "
                               "${CMAKE_VERSION}, so a clean ${strict_text} proves nothing:\n${out}\n")
    endif()
    math(EXPR cases "${cases} + 1")

    set(behaviour "${cases} cases ran on CMake ${CMAKE_VERSION}")
endif()

if(NOT problems STREQUAL "")
    message(FATAL_ERROR "the strict configure flags are ${strict_text}, but:\n${problems}")
endif()
message(STATUS "${spelled_count} files spell the strict flags ${strict_text} and none says "
               "${stale_flag}; behaviour ${behaviour}")
