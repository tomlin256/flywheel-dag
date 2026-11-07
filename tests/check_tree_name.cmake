# check_tree_name.cmake — test_tree_name: the real top-level CMakeLists.txt
# refuses a build tree named for another platform, or for a sanitizer it is not
# configured for, before it detects a compiler, and records the platform of the
# first configure.
#
# Run by ctest as
#
#   cmake -DSOURCE_DIR=<source> -DSCRATCH_DIR=<dir> -DGENERATOR=<generator>
#         -DSPDLOG_SOURCE_DIR=<dir> -DNLOHMANN_JSON_SOURCE_DIR=<dir>
#         -P check_tree_name.cmake
#
# Every case configures a tree under SCRATCH_DIR with the tests, examples,
# benchmarks and install rules off and the two dependencies taken from sources
# already on disk, so it needs no network and compiles nothing of the project's.
# The host's own platform is the oracle, worked out here and not by the module
# under test, and the other architecture is the lie. test_platform_name holds the
# module to a table; this holds CMakeLists.txt to its wiring: that the guard runs
# at the top level only and before project(), that it reads the host's
# architecture where CMake knows it, that it hands the guard the sanitizer mode
# the configure was given, and that each kind of problem stops a configure.

cmake_minimum_required(VERSION 4.0)

foreach(var IN ITEMS SOURCE_DIR SCRATCH_DIR GENERATOR SPDLOG_SOURCE_DIR NLOHMANN_JSON_SOURCE_DIR)
    if(NOT ${var})
        message(FATAL_ERROR "usage: cmake -DSOURCE_DIR=<dir> -DSCRATCH_DIR=<dir> "
                            "-DGENERATOR=<name> -DSPDLOG_SOURCE_DIR=<dir> "
                            "-DNLOHMANN_JSON_SOURCE_DIR=<dir> -P check_tree_name.cmake")
    endif()
endforeach()

# ── the host, as an oracle of its own ────────────────────────────────────────
cmake_host_system_information(RESULT machine QUERY OS_PLATFORM)
string(TOLOWER "${machine}" arch)
if(arch STREQUAL "aarch64")
    set(arch arm64)
elseif(arch STREQUAL "amd64")
    set(arch x86_64)
endif()
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
    set(os macos)
elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
    set(os linux)
else()
    message(FATAL_ERROR "test_tree_name: ${CMAKE_HOST_SYSTEM_NAME} is not a host that "
                        "platform trees are named for (macos or linux)")
endif()
if(arch STREQUAL "x86_64")
    set(other_arch arm64)
else()
    set(other_arch x86_64)
endif()
set(host  "${os}-${arch}")
set(other "${os}-${other_arch}")

file(REMOVE_RECURSE "${SCRATCH_DIR}")

set(cases 0)
set(failed 0)
set(failures "")

macro(check_failed label detail)
    math(EXPR failed "${failed} + 1")
    string(APPEND failures "  ${label}: ${detail}\n")
endmacro()

# configure(<rc> <output> <source> <tree> [<args>...]): configure <source> into
# <tree>. <output> holds the output and the error output together.
function(configure rc_var out_var source tree)
    execute_process(
        COMMAND ${CMAKE_COMMAND} -G "${GENERATOR}" -S "${source}" -B "${tree}"
                -DFLYWHEEL_DAG_BUILD_TESTS=OFF
                -DFLYWHEEL_DAG_BUILD_EXAMPLES=OFF
                -DFLYWHEEL_DAG_BUILD_BENCHMARKS=OFF
                -DFLYWHEEL_DAG_INSTALL=OFF
                -DFETCHCONTENT_FULLY_DISCONNECTED=ON
                -DFETCHCONTENT_SOURCE_DIR_SPDLOG=${SPDLOG_SOURCE_DIR}
                -DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON=${NLOHMANN_JSON_SOURCE_DIR}
                ${ARGN}
        RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    set(${rc_var} "${rc}" PARENT_SCOPE)
    set(${out_var} "${out}\n${err}" PARENT_SCOPE)
endfunction()

# The cache lines of a tree that match a regular expression.
function(cache_lines out tree regex)
    set(found "")
    if(EXISTS "${tree}/CMakeCache.txt")
        file(STRINGS "${tree}/CMakeCache.txt" found REGEX "${regex}")
    endif()
    set(${out} "${found}" PARENT_SCOPE)
endfunction()

# A tree that configures, and records the host's platform.
macro(expect_configures label tree)
    math(EXPR cases "${cases} + 1")
    configure(rc output "${SOURCE_DIR}" "${tree}" ${ARGN})
    if(NOT rc EQUAL 0)
        check_failed("${label}" "the configure failed (${rc}): ${output}")
    else()
        cache_lines(recorded "${tree}" "^FLYWHEEL_DAG_TREE_PLATFORM:")
        if(NOT "${recorded}" STREQUAL "FLYWHEEL_DAG_TREE_PLATFORM:INTERNAL=${host}")
            check_failed("${label}" "the cache records '${recorded}', not ${host}")
        endif()
    endif()
endmacro()

# A tree that is refused, with the text it is refused with, before project().
macro(expect_refused label tree text)
    math(EXPR cases "${cases} + 1")
    configure(rc output "${SOURCE_DIR}" "${tree}" ${ARGN})
    if(rc EQUAL 0)
        check_failed("${label}" "the configure succeeded")
    else()
        # CMake wraps the text of an error at about 80 columns.
        string(REGEX REPLACE "[ \t\r\n]+" " " flat "${output}")
        string(FIND "${flat}" "${text}" at)
        if(at EQUAL -1)
            check_failed("${label}" "the output lacks '${text}': ${flat}")
        endif()
        cache_lines(compiler "${tree}" "^CMAKE_CXX_COMPILER:")
        if(NOT "${compiler}" STREQUAL "")
            check_failed("${label}" "the guard ran after compiler detection: ${compiler}")
        endif()
    endif()
endmacro()

# ── trees that tell the truth ────────────────────────────────────────────────
set(tree "${SCRATCH_DIR}/build/relwithdebinfo-${host}")
expect_configures("configure: relwithdebinfo-${host} configures and records the platform" "${tree}")
cache_lines(compiler "${tree}" "^CMAKE_CXX_COMPILER:")
if("${compiler}" STREQUAL "")
    # The check below, that a refused tree has no compiler entry, has teeth only
    # if a tree that configured does.
    check_failed("configure: a configured tree has a compiler entry" "it has none")
endif()
expect_configures("configure: relwithdebinfo-${host} configures again" "${tree}")

expect_configures("configure: build configures and records the platform" "${SCRATCH_DIR}/build/build")
expect_configures("configure: ${host} (no kind) configures" "${SCRATCH_DIR}/build/${host}")

# A sanitizer tree configured for the mode in its name, a sanitizer in a tree that is not named for one, and a kind that only starts
# like a sanitizer's.
expect_configures("configure: asan-${host}-gcc13 with the asan mode configures"
                  "${SCRATCH_DIR}/build/asan-${host}-gcc13" -DFLYWHEEL_DAG_SANITIZE=asan)
expect_configures("configure: release-${host} with the asan mode configures"
                  "${SCRATCH_DIR}/build/release-${host}" -DFLYWHEEL_DAG_SANITIZE=asan)
expect_configures("configure: asanx-${host} with no mode configures" "${SCRATCH_DIR}/build/asanx-${host}")

# ── trees that do not ───────────────────────────────────────────────────────
expect_refused("configure: a free-form tree that remembers ${other} is refused"
               "${SCRATCH_DIR}/build/scratch"
               "first configured on ${other}, but this host is ${host}"
               -DFLYWHEEL_DAG_TREE_PLATFORM=${other})
expect_refused("configure: release-${other} is refused and leaves no compiler entry"
               "${SCRATCH_DIR}/build/release-${other}"
               "\"release-${other}\" is for ${other}, but this host is ${host}")
expect_refused("configure: ${other} (no kind) is refused"
               "${SCRATCH_DIR}/build/${other}"
               "\"${other}\" is for ${other}, but this host is ${host}")
expect_refused("configure: release-${os} is refused"
               "${SCRATCH_DIR}/build/release-${os}"
               "\"release-${os}\" is for ${os}, but this host is ${host}")
# The text names the tree, the sanitizer in its name and the mode it was given. The second tree has the same name in another parent
# directory, so that no case configures over what another left behind.
expect_refused("configure: asan-${host} with no mode is refused and leaves no compiler entry"
               "${SCRATCH_DIR}/build/asan-${host}"
               "\"asan-${host}\" is named for the asan sanitizer, but FLYWHEEL_DAG_SANITIZE is 'OFF'")
expect_refused("configure: asan-${host} with the tsan mode is refused"
               "${SCRATCH_DIR}/tsan-mode/asan-${host}"
               "\"asan-${host}\" is named for the asan sanitizer, but FLYWHEEL_DAG_SANITIZE is 'tsan'"
               -DFLYWHEEL_DAG_SANITIZE=tsan)

# ── a consumer is never examined ─────────────────────────────────────────────
# The engine, pulled in as a subproject, sees a tree named for the other
# architecture and must not mind: the tree is the consumer's.
math(EXPR cases "${cases} + 1")
configure(rc output "${SOURCE_DIR}/tests/consumer" "${SCRATCH_DIR}/consumer/build/release-${other}"
          -DFLYWHEEL_DAG_SOURCE_DIR=${SOURCE_DIR})
if(NOT rc EQUAL 0)
    check_failed("configure: the consumer in release-${other} configures"
                 "the configure failed (${rc}): ${output}")
endif()

if(failed GREATER 0)
    message(FATAL_ERROR "test_tree_name: ${failed} of ${cases} cases failed:\n${failures}")
endif()
message(STATUS "test_tree_name: all ${cases} cases hold (host ${host}, lie ${other})")
