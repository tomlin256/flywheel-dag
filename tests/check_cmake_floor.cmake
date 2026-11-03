# check_cmake_floor.cmake — test_cmake_floor: the project has one CMake floor, and
# everything that states one states the same.
#
# Run by ctest as
#
#   cmake -DSOURCE_DIR=<source> -P check_cmake_floor.cmake
#
# The floor is the root CMakeLists.txt's minimum, which must be a plain version:
# CMakeLists.txt says why it is never a <min>...<max> range. Every other minimum in the
# checkout, the consumer project and every script, one added later too, must be exactly
# that version, and README.md must say "CMake <floor> or later". Otherwise the next
# script copied from another project brings its CMake 3 back, and nothing notices.
#
# CMakePresets.json is not held to it. Its cmakeMinimumRequired is the CMake that can
# read the file, not what the project needs, and raising it turns CMake 3's clear refusal
# at the root into a confusing one (check_presets.cmake says so).
#
# A build tree holds other projects' files, so build*/ is skipped, and so is .git/.

cmake_minimum_required(VERSION 4.0)

if(NOT SOURCE_DIR)
    message(FATAL_ERROR "usage: cmake -DSOURCE_DIR=<dir> -P check_cmake_floor.cmake")
endif()

# The text between the parentheses of a call that sets a minimum version.
set(call_regex "cmake_minimum_required\\([ \t\r\n]*VERSION[ \t\r\n]+([^)]*)\\)")

# The version of each such call in a file, trimmed.
function(minimums_in file out)
    file(READ "${file}" text)
    string(REGEX MATCHALL "${call_regex}" calls "${text}")
    set(versions "")
    foreach(call IN LISTS calls)
        string(REGEX REPLACE "${call_regex}" "\\1" version "${call}")
        string(STRIP "${version}" version)
        list(APPEND versions "${version}")
    endforeach()
    set(${out} "${versions}" PARENT_SCOPE)
endfunction()

# ── the floor ────────────────────────────────────────────────────────────────
minimums_in("${SOURCE_DIR}/CMakeLists.txt" root_versions)
list(LENGTH root_versions root_count)
if(NOT root_count EQUAL 1)
    message(FATAL_ERROR "found ${root_count} calls that set a minimum version in "
                        "${SOURCE_DIR}/CMakeLists.txt; the floor is its one")
endif()
set(floor "${root_versions}")
if(NOT floor MATCHES "^[0-9]+(\\.[0-9]+)*$")
    message(FATAL_ERROR "the floor in ${SOURCE_DIR}/CMakeLists.txt is '${floor}', not a "
                        "plain version")
endif()

# ── every file that may state a minimum ──────────────────────────────────────
file(GLOB top RELATIVE "${SOURCE_DIR}" "${SOURCE_DIR}/*")
set(files "")
foreach(entry IN LISTS top)
    if(entry MATCHES "^(build.*|\\.git)$")
        continue()
    endif()
    if(IS_DIRECTORY "${SOURCE_DIR}/${entry}")
        file(GLOB_RECURSE found RELATIVE "${SOURCE_DIR}"
             "${SOURCE_DIR}/${entry}/CMakeLists.txt"
             "${SOURCE_DIR}/${entry}/*.cmake"
             "${SOURCE_DIR}/${entry}/*.cmake.in")
        list(APPEND files ${found})
    elseif(entry STREQUAL "CMakeLists.txt" OR entry MATCHES "\\.cmake(\\.in)?$")
        list(APPEND files "${entry}")
    endif()
endforeach()
list(SORT files)

set(problems "")
set(stating 0)
foreach(file IN LISTS files)
    minimums_in("${SOURCE_DIR}/${file}" versions)
    foreach(version IN LISTS versions)
        math(EXPR stating "${stating} + 1")
        if(version MATCHES "\\.\\.\\.")
            string(APPEND problems "  ${file} says VERSION ${version}, a range, not the plain ${floor}\n")
        elseif(NOT version STREQUAL floor)
            string(APPEND problems "  ${file} says VERSION ${version}, not ${floor}\n")
        endif()
    endforeach()
endforeach()

# ── the README ───────────────────────────────────────────────────────────────
file(READ "${SOURCE_DIR}/README.md" readme)
string(FIND "${readme}" "CMake ${floor} or later" at)
if(at EQUAL -1)
    string(APPEND problems "  README.md does not say \"CMake ${floor} or later\"\n")
endif()

if(NOT problems STREQUAL "")
    message(FATAL_ERROR "the CMake floor is ${floor} (CMakeLists.txt), but:\n${problems}")
endif()
message(STATUS "the CMake floor is ${floor}: ${stating} files state it, and README.md says so")
