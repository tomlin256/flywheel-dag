# check_strict_configure.cmake — test_strict_configure: the strict configure that CI runs configures the real project on CMake 4.5 or later,
# with the dependencies' own non-target directives as warnings, and with nothing demoted for this project's files or for a parent's.
#
# Run by ctest as
#
#   cmake -DSOURCE_DIR=<source> -DSCRATCH_DIR=<dir> -DGENERATOR=<generator> -DCXX=<compiler> -DSPDLOG_SOURCE_DIR=<dir>
#         -DNLOHMANN_JSON_SOURCE_DIR=<dir> -DGOOGLETEST_SOURCE_DIR=<dir> -P check_strict_configure.cmake
#
# CMake 4.5 reports a directory-wide option or include path as a non-target-directive, and -Werror=author makes that an error
# (test_cmake_diagnostics holds that, and check_non_target_directives.cmake holds that no file of this project makes one). googletest
# makes two, and is not ours to change, so the root demotes the category to a warning while it includes FetchContent, at the top level
# only (the comment above that include in CMakeLists.txt says why). This configures the real top-level project, as
# check_warning_option.cmake does, with the running tree's compiler and generator and the dependencies taken from sources already on
# disk, so it needs no network and compiles nothing. Every tree is made in SCRATCH_DIR/build/<name>, and the names claim no platform,
# so the build-tree guard does not examine them.
#
# What it holds:
#
#   - with CI's two flags, in the default, asan and tsan configurations, and the tests, the examples and the benchmarks on: the
#     configure exits 0 and prints no CMake Error; it prints at least one CMake Warning (non-target-directive) for a file of googletest,
#     and every report is for a file of a fetched dependency, none for a file of this project;
#   - with no flags the configure says nothing about the category: the demotion changes nothing where the category is off;
#   - a copy of the root with a directive of its own at the end: with the flags its configure fails, with one CMake Error
#     (non-target-directive) for that line and no other error, so the demotion stops at what the root fetches;
#   - a parent project that pulls the engine in as a subproject and then fetches a dependency of its own that makes a non-target
#     directive: with the flags its configure fails, with one CMake Error (non-target-directive) for that dependency's file and no other
#     error, and without them it configures. The engine demotes nothing for its parent.
#
# Before CMake 4.5 the category does not exist and the check is skipped, with a status line that says so. The directives that it
# writes into projects are text here, so check_non_target_directives.cmake does not read this file.
#
# SOURCE_DIR is a parameter so that a hand-made change can be made to a scratch copy of the repo and must fail here.

cmake_minimum_required(VERSION 4.0)

foreach(var IN ITEMS SOURCE_DIR SCRATCH_DIR GENERATOR CXX SPDLOG_SOURCE_DIR NLOHMANN_JSON_SOURCE_DIR GOOGLETEST_SOURCE_DIR)
    if(NOT ${var})
        message(FATAL_ERROR "usage: cmake -DSOURCE_DIR=<dir> -DSCRATCH_DIR=<dir> -DGENERATOR=<name> -DCXX=<compiler> "
                            "-DSPDLOG_SOURCE_DIR=<dir> -DNLOHMANN_JSON_SOURCE_DIR=<dir> -DGOOGLETEST_SOURCE_DIR=<dir> "
                            "-P check_strict_configure.cmake")
    endif()
endforeach()

if(CMAKE_VERSION VERSION_LESS 4.5)
    message(STATUS "test_strict_configure: skipped: CMake ${CMAKE_VERSION} has no non-target-directive category, which came with 4.5")
    return()
endif()

set(strict_flags -Werror=author -Werror=deprecated)
list(JOIN strict_flags " " flags_text)
set(dependency_dirs "${SPDLOG_SOURCE_DIR}" "${NLOHMANN_JSON_SOURCE_DIR}" "${GOOGLETEST_SOURCE_DIR}")

file(REMOVE_RECURSE "${SCRATCH_DIR}")
file(MAKE_DIRECTORY "${SCRATCH_DIR}/build")

set(failures "")
set(cases 0)

# configure(<prefix> <source> <name> [<argument>...]): configures <source> into SCRATCH_DIR/build/<name> with the running tree's
# compiler and generator, offline. Sets <prefix>_rc and <prefix>_text, the output and the error output with runs of whitespace as one
# space, because CMake wraps a message at about 80 columns.
function(configure prefix source name)
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -G "${GENERATOR}" -S "${source}" -B "${SCRATCH_DIR}/build/${name}"
            "-DCMAKE_CXX_COMPILER=${CXX}"
            -DFETCHCONTENT_FULLY_DISCONNECTED=ON
            ${ARGN}
        RESULT_VARIABLE rc
        OUTPUT_VARIABLE out
        ERROR_VARIABLE err)
    string(REGEX REPLACE "[ \t\r\n]+" " " text "${out} ${err}")
    set(${prefix}_rc "${rc}" PARENT_SCOPE)
    set(${prefix}_text "${text}" PARENT_SCOPE)
endfunction()

# configure_root(<prefix> <name> [<argument>...]): the real root, with everything that has a unit switched on.
function(configure_root prefix name)
    configure(${prefix} "${SOURCE_DIR}" "${name}"
        "-DFETCHCONTENT_SOURCE_DIR_SPDLOG=${SPDLOG_SOURCE_DIR}"
        "-DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON=${NLOHMANN_JSON_SOURCE_DIR}"
        "-DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=${GOOGLETEST_SOURCE_DIR}"
        -DFLYWHEEL_DAG_BUILD_TESTS=ON
        -DFLYWHEEL_DAG_BUILD_EXAMPLES=ON
        -DFLYWHEEL_DAG_BUILD_BENCHMARKS=ON
        -DFLYWHEEL_DAG_INSTALL=OFF
        ${ARGN})
    set(${prefix}_rc "${${prefix}_rc}" PARENT_SCOPE)
    set(${prefix}_text "${${prefix}_text}" PARENT_SCOPE)
endfunction()

# reports(<out> <text> <source>): the non-target-directive reports in <text>, each as <Error|Warning>|<absolute file>|<line> (<command>).
# CMake prints a file under the top source directory, <source>, relative to it.
function(reports out text source)
    set(found "")
    string(REGEX MATCHALL "CMake (Error|Warning) \\(non-target-directive\\) at [^:]+:[0-9]+ \\([a-z_]+\\)" matches "${text}")
    foreach(match IN LISTS matches)
        string(REGEX MATCH "CMake (Error|Warning) \\(non-target-directive\\) at ([^:]+):([0-9]+) \\(([a-z_]+)\\)" ignored "${match}")
        set(kind "${CMAKE_MATCH_1}")
        set(path "${CMAKE_MATCH_2}")
        set(place "${CMAKE_MATCH_3} (${CMAKE_MATCH_4})")
        file(REAL_PATH "${path}" file BASE_DIRECTORY "${source}")
        list(APPEND found "${kind}|${file}|${place}")
    endforeach()
    set(${out} "${found}" PARENT_SCOPE)
endfunction()

# is_under(<out> <file> <directory>): TRUE when <file> is inside <directory>.
function(is_under out file directory)
    file(REAL_PATH "${directory}" real)
    string(FIND "${file}/" "${real}/" at)
    if(at EQUAL 0)
        set(${out} TRUE PARENT_SCOPE)
    else()
        set(${out} FALSE PARENT_SCOPE)
    endif()
endfunction()

# describe(<out> <report>...): the reports, one to a line, for a message.
function(describe out)
    set(lines "")
    foreach(report IN LISTS ARGN)
        string(REPLACE "|" " " readable "${report}")
        string(APPEND lines "\n      ${readable}")
    endforeach()
    set(${out} "${lines}" PARENT_SCOPE)
endfunction()

# A failed configure exits with a positive number. Anything else, such as the text execute_process gives when it cannot start CMake,
# is not a failure that CMake reported.
function(failed rc out)
    if("${rc}" MATCHES "^[1-9][0-9]*$")
        set(${out} TRUE PARENT_SCOPE)
    else()
        set(${out} FALSE PARENT_SCOPE)
    endif()
endfunction()

# expect_one_error(<label> <rc> <text> <source> <file> <meaning>): the configure failed with exactly one CMake Error, which is the
# non-target-directive report for <file>. <meaning> says what it means when the configure did not fail.
function(expect_one_error label rc text source file meaning)
    reports(found "${text}" "${source}")
    describe(shown ${found})
    string(REGEX MATCHALL "CMake Error" errors "${text}")
    list(LENGTH errors error_count)
    failed("${rc}" did_fail)
    file(REAL_PATH "${file}" real_file)
    string(FIND "${found}" "Error|${real_file}|" first_report)
    if(NOT did_fail)
        string(APPEND failures "\n  ${label}: the configure with ${flags_text} exited ${rc}, want a failure: ${meaning}. "
                               "The reports:${shown}")
    elseif(NOT error_count EQUAL 1 OR NOT first_report EQUAL 0)
        string(APPEND failures "\n  ${label}: the configure failed, but not with the one CMake Error (non-target-directive) for "
                               "${real_file}: ${text}")
    endif()
    set(failures "${failures}" PARENT_SCOPE)
endfunction()

# ── the real project, strict, in each configuration ──────────────────────────
foreach(mode IN ITEMS OFF asan tsan)
    set(name "strict-${mode}")
    configure_root(run "${name}" -DFLYWHEEL_DAG_SANITIZE=${mode} ${strict_flags})
    math(EXPR cases "${cases} + 1")
    reports(found "${run_text}" "${SOURCE_DIR}")
    describe(shown ${found})
    if(NOT run_rc EQUAL 0)
        string(APPEND failures "\n  ${name}: ${flags_text} on the real project exited ${run_rc}, want 0. The reports:${shown}\n"
                               "    ${run_text}")
        continue()
    endif()
    string(FIND "${run_text}" "CMake Error" at)
    if(NOT at EQUAL -1)
        string(APPEND failures "\n  ${name}: the configure exited 0 but printed a CMake Error. The reports:${shown}")
    endif()
    set(googletest_reports 0)
    foreach(report IN LISTS found)
        string(REGEX MATCH "^([A-Za-z]+)\\|([^|]*)\\|(.*)$" ignored "${report}")
        set(file "${CMAKE_MATCH_2}")
        set(fetched FALSE)
        foreach(dependency IN LISTS dependency_dirs)
            is_under(inside "${file}" "${dependency}")
            if(inside)
                set(fetched TRUE)
            endif()
        endforeach()
        if(NOT fetched)
            string(APPEND failures "\n  ${name}: a report for ${file}, which is no fetched dependency's file:${shown}")
        endif()
        is_under(inside "${file}" "${GOOGLETEST_SOURCE_DIR}")
        if(inside)
            math(EXPR googletest_reports "${googletest_reports} + 1")
        endif()
    endforeach()
    if(googletest_reports EQUAL 0)
        string(APPEND failures "\n  ${name}: no CMake Warning (non-target-directive) for a file of googletest, so this proves nothing: "
                               "either CMake no longer reports the category (test_cmake_diagnostics says whether it does), or the "
                               "configure no longer reaches googletest")
    endif()
endforeach()

# ── no flags: the demotion changes nothing where the category is off ─────────
configure_root(run lax)
math(EXPR cases "${cases} + 1")
if(NOT run_rc EQUAL 0)
    string(APPEND failures "\n  lax: the configure with no flags exited ${run_rc}, want 0: ${run_text}")
elseif(run_text MATCHES "non-target-directive")
    string(APPEND failures "\n  lax: the configure with no flags says something about the category, which is off without "
                           "${flags_text}: ${run_text}")
endif()

# ── a file of this project: still an error ───────────────────────────────────
# The demotion is for what the root fetches. The root is copied with a directive of its own after the include, and with the tests, the
# examples and the benchmarks off, which leaves nothing to read but its cmake/ directory and the three directories that it names to
# flywheel_dag_add_target_options, which refuses a name that is no directory. Nothing adds them here, so they are empty.
set(own "${SCRATCH_DIR}/own")
file(MAKE_DIRECTORY "${own}/tests" "${own}/examples" "${own}/benchmarks")
file(COPY "${SOURCE_DIR}/CMakeLists.txt" "${SOURCE_DIR}/cmake" DESTINATION "${own}")
file(APPEND "${own}/CMakeLists.txt" "\nadd_compile_options(-Wall)\n")
configure(run "${own}" own-strict
    "-DFETCHCONTENT_SOURCE_DIR_SPDLOG=${SPDLOG_SOURCE_DIR}"
    "-DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON=${NLOHMANN_JSON_SOURCE_DIR}"
    -DFLYWHEEL_DAG_BUILD_TESTS=OFF
    -DFLYWHEEL_DAG_BUILD_EXAMPLES=OFF
    -DFLYWHEEL_DAG_BUILD_BENCHMARKS=OFF
    -DFLYWHEEL_DAG_INSTALL=OFF
    ${strict_flags})
math(EXPR cases "${cases} + 1")
expect_one_error(own-strict "${run_rc}" "${run_text}" "${own}" "${own}/CMakeLists.txt"
                 "the demotion reached a directive of the root's own")

# ── a parent project: nothing is demoted for it ──────────────────────────────
# The parent includes FetchContent, adds the real root as a subproject, and then fetches a dependency of its own that makes a
# non-target directive, as googletest does. The root's include of FetchContent runs in a subproject here, so it must not demote for
# what the parent fetches after it.
set(other "${SCRATCH_DIR}/other")
set(parent "${SCRATCH_DIR}/parent")
file(MAKE_DIRECTORY "${other}" "${parent}")
file(WRITE "${other}/CMakeLists.txt"
     "cmake_minimum_required(VERSION 4.0)\n"
     "project(other LANGUAGES NONE)\n"
     "include_directories(\${CMAKE_CURRENT_SOURCE_DIR})\n")
file(WRITE "${parent}/CMakeLists.txt"
     "cmake_minimum_required(VERSION 4.0)\n"
     "project(parent LANGUAGES NONE)\n"
     "include(FetchContent)\n"
     "add_subdirectory(\"${SOURCE_DIR}\" flywheel_dag)\n"
     "FetchContent_Declare(other SOURCE_DIR \"${other}\")\n"
     "FetchContent_MakeAvailable(other)\n")
set(parent_arguments
    "-DFETCHCONTENT_SOURCE_DIR_SPDLOG=${SPDLOG_SOURCE_DIR}"
    "-DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON=${NLOHMANN_JSON_SOURCE_DIR}")

configure(run "${parent}" parent-lax ${parent_arguments})
math(EXPR cases "${cases} + 1")
if(NOT run_rc EQUAL 0)
    string(APPEND failures "\n  parent-lax: the parent with no flags exited ${run_rc}, want 0: ${run_text}")
endif()

configure(run "${parent}" parent-strict ${parent_arguments} ${strict_flags})
math(EXPR cases "${cases} + 1")
expect_one_error(parent-strict "${run_rc}" "${run_text}" "${parent}" "${other}/CMakeLists.txt"
                 "the engine demoted the category for the dependency that the parent fetches after it")

if(NOT failures STREQUAL "")
    message(FATAL_ERROR "test_strict_configure: the strict configure, ${flags_text}, does not hold on CMake ${CMAKE_VERSION}:${failures}")
endif()
message(STATUS "test_strict_configure: ok (${cases} configures with ${flags_text} on CMake ${CMAKE_VERSION})")
