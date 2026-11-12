# check_warning_option.cmake — test_warning_option: -Werror is this project's default at the top level, one option relaxes
# it, and neither reaches a dependency, a project that pulls the engine in, or a workflow.
#
# Run by ctest as
#
#   cmake -DSOURCE_DIR=<source> -DSCRATCH_DIR=<dir> -DGENERATOR=<generator> -DCXX=<compiler> -DSPDLOG_SOURCE_DIR=<dir>
#         -DNLOHMANN_JSON_SOURCE_DIR=<dir> -DGOOGLETEST_SOURCE_DIR=<dir> -P check_warning_option.cmake
#
# test_warning_flags reads the running tree, and a tree keeps the value of an option that it was once given, so nothing
# there notices what the default is. This configures the real top-level project, as check_sanitize_option.cmake does, with
# the running tree's compiler and generator and the dependencies taken from sources already on disk, so it needs no network
# and compiles nothing, and it reads the compile_commands.json and the cache that each tree writes. Every tree is made in
# SCRATCH_DIR/build/<name>, and the names claim no platform, so the build-tree guard does not examine them.
#
# What it holds:
#
#   - no option: every unit of this project, at least one each under tests/, examples/ and benchmarks/, has -Wall -Wextra
#     -Wpedantic -Werror=deprecated-declarations and -Werror; no unit of a dependency has -Werror or -Wpedantic, at least
#     one such unit having been seen; the cache holds the option as ON, and a CMAKE_CXX_FLAGS with no warning flag in it;
#     and the configure does not print the line that a relaxed tree prints. DEFAULT below is the one line here that says
#     what the default is: change the root's default without it, or it without the root's, and this fails.
#   - the option off: no unit of this project has -Werror, the other four flags stay, the cache holds OFF, and the
#     configure prints the line, so that a relaxed tree says so each time it is configured. An option is cached, so a tree
#     that was made when the default was another keeps the old value, and the line is what shows it.
#   - a parent project that pulls the engine in (tests/consumer): with no option its cache has no entry of the option's
#     name, and with the option on its command line it is still the consumer's own build. Either way no unit has -Werror,
#     -Wpedantic or -Werror=deprecated-declarations, and the consumer's own unit, which must be among them, has none of the
#     five.
#   - the workflows: no line of code in a workflow names the option. The default is the one place that sets it, so CI
#     tests the default, and a leg that passed =OFF would be a leg that a warning cannot fail.
#
# A unit of this project is a source under tests/, examples/ or benchmarks/ of SOURCE_DIR; any other unit of a tree is a
# dependency's, wherever its sources are. The flags are matched as whole arguments, so that -Werror does not pass for
# -Werror=deprecated-declarations.
#
# SOURCE_DIR is a parameter so that a hand-made change can be made to a scratch copy of the repo and must fail here.

cmake_minimum_required(VERSION 4.0)

foreach(var IN ITEMS SOURCE_DIR SCRATCH_DIR GENERATOR CXX SPDLOG_SOURCE_DIR NLOHMANN_JSON_SOURCE_DIR GOOGLETEST_SOURCE_DIR)
    if(NOT ${var})
        message(FATAL_ERROR "usage: cmake -DSOURCE_DIR=<dir> -DSCRATCH_DIR=<dir> -DGENERATOR=<name> -DCXX=<compiler> "
                            "-DSPDLOG_SOURCE_DIR=<dir> -DNLOHMANN_JSON_SOURCE_DIR=<dir> -DGOOGLETEST_SOURCE_DIR=<dir> "
                            "-P check_warning_option.cmake")
    endif()
endforeach()

# The default of FLYWHEEL_DAG_WARNINGS_AS_ERRORS in the root CMakeLists.txt.
set(DEFAULT ON)

set(option FLYWHEEL_DAG_WARNINGS_AS_ERRORS)
set(warning_flags -Wall -Wextra -Wpedantic -Werror=deprecated-declarations)
set(leak_flags -Wpedantic -Werror -Werror=deprecated-declarations)
set(relaxed_line "flywheel-dag: warnings are not errors in this tree")
set(own_directories tests examples benchmarks)

# Fewer units than this and the scan has read nothing worth the name: the tests alone are some thirty.
set(min_units 20)

file(REMOVE_RECURSE "${SCRATCH_DIR}")
file(MAKE_DIRECTORY "${SCRATCH_DIR}/build")

set(failures "")

# Configure <source> into SCRATCH_DIR/build/<name>. Sets <prefix>_rc, <prefix>_text (the output and the error output, runs of
# whitespace as one space), <prefix>_db (the tree's compile_commands.json, or empty if it wrote none) and <prefix>_cache.
function(configure prefix source name)
    set(tree "${SCRATCH_DIR}/build/${name}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -G "${GENERATOR}" -S "${source}" -B "${tree}"
            "-DCMAKE_CXX_COMPILER=${CXX}"
            -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
            -DFETCHCONTENT_FULLY_DISCONNECTED=ON
            "-DFETCHCONTENT_SOURCE_DIR_SPDLOG=${SPDLOG_SOURCE_DIR}"
            "-DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON=${NLOHMANN_JSON_SOURCE_DIR}"
            "-DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=${GOOGLETEST_SOURCE_DIR}"
            ${ARGN}
        RESULT_VARIABLE rc
        OUTPUT_VARIABLE out
        ERROR_VARIABLE err)
    string(REGEX REPLACE "[ \t\r\n]+" " " text "${out} ${err}")
    set(db "")
    if(EXISTS "${tree}/compile_commands.json")
        file(READ "${tree}/compile_commands.json" db)
    endif()
    set(cache "")
    if(EXISTS "${tree}/CMakeCache.txt")
        file(READ "${tree}/CMakeCache.txt" cache)
    endif()
    set(${prefix}_rc "${rc}" PARENT_SCOPE)
    set(${prefix}_text "${text}" PARENT_SCOPE)
    set(${prefix}_db "${db}" PARENT_SCOPE)
    set(${prefix}_cache "${cache}" PARENT_SCOPE)
endfunction()

# The real root, with everything that has a unit switched on.
function(configure_root prefix name)
    configure(${prefix} "${SOURCE_DIR}" "${name}"
        -DFLYWHEEL_DAG_BUILD_TESTS=ON
        -DFLYWHEEL_DAG_BUILD_EXAMPLES=ON
        -DFLYWHEEL_DAG_BUILD_BENCHMARKS=ON
        -DFLYWHEEL_DAG_INSTALL=OFF
        ${ARGN})
    foreach(var IN ITEMS rc text db cache)
        set(${prefix}_${var} "${${prefix}_${var}}" PARENT_SCOPE)
    endforeach()
endfunction()

function(expect_configures label prefix)
    if(NOT ${prefix}_rc EQUAL 0)
        string(APPEND failures "\n  ${label}: the configure failed (${${prefix}_rc}): ${${prefix}_text}")
    endif()
    set(failures "${failures}" PARENT_SCOPE)
endfunction()

# The configure's output must (or must not) contain a text.
function(expect_output label prefix needle want_present)
    string(FIND "${${prefix}_text}" "${needle}" at)
    if(want_present AND at EQUAL -1)
        string(APPEND failures "\n  ${label}: the configure does not print '${needle}'")
    elseif(NOT want_present AND NOT at EQUAL -1)
        string(APPEND failures "\n  ${label}: the configure prints '${needle}', want it absent")
    endif()
    set(failures "${failures}" PARENT_SCOPE)
endfunction()

# The cache holds the option with a value, as a whole line.
function(expect_option label prefix value)
    string(FIND "\n${${prefix}_cache}" "\n${option}:BOOL=${value}\n" at)
    if(at EQUAL -1)
        string(APPEND failures "\n  ${label}: the cache does not hold ${option}:BOOL=${value}")
    endif()
    set(failures "${failures}" PARENT_SCOPE)
endfunction()

# The cache has no entry of the option's name, whatever its type.
function(expect_no_option label prefix)
    string(FIND "\n${${prefix}_cache}" "\n${option}:" at)
    if(NOT at EQUAL -1)
        string(APPEND failures "\n  ${label}: the cache holds ${option}, which is the top level's alone")
    endif()
    set(failures "${failures}" PARENT_SCOPE)
endfunction()

# The cache's CMAKE_CXX_FLAGS has no warning flag in it: the flags reach the units as compile options of this project's
# directories, never through the cache.
function(expect_flags_not_cached label prefix)
    if(NOT "\n${${prefix}_cache}" MATCHES "\nCMAKE_CXX_FLAGS:[A-Z]+=([^\n]*)\n")
        string(APPEND failures "\n  ${label}: the cache has no CMAKE_CXX_FLAGS")
    elseif(CMAKE_MATCH_1 MATCHES "-W(all|extra|pedantic|error)")
        string(APPEND failures "\n  ${label}: the cache's CMAKE_CXX_FLAGS holds a warning flag: ${CMAKE_MATCH_1}")
    endif()
    set(failures "${failures}" PARENT_SCOPE)
endfunction()

# Up to eight items of a list, one to a line, for a failure message.
function(first_lines list_var out)
    set(shown "${${list_var}}")
    list(LENGTH shown n)
    if(n GREATER 8)
        list(SUBLIST shown 0 8 shown)
    endif()
    list(JOIN shown "\n    " text)
    set(${out} "${text}" PARENT_SCOPE)
endfunction()

# The number of units in the tree's compile database, or empty, with the failure recorded, when it cannot be read.
function(unit_count out label prefix floor)
    set(${out} "" PARENT_SCOPE)
    if(NOT ${prefix}_rc EQUAL 0)
        return()    # the configure failed, and that is reported already
    endif()
    set(db "${${prefix}_db}")
    if("${db}" STREQUAL "")
        string(APPEND failures "\n  ${label}: the tree wrote no compile_commands.json (this test needs a Makefiles or Ninja generator)")
        set(failures "${failures}" PARENT_SCOPE)
        return()
    endif()
    string(JSON count ERROR_VARIABLE error LENGTH "${db}")
    if(error OR count LESS floor)
        string(APPEND failures "\n  ${label}: compile_commands.json has '${count}' entries (${error}), want at least ${floor}")
        set(failures "${failures}" PARENT_SCOPE)
        return()
    endif()
    set(${out} "${count}" PARENT_SCOPE)
endfunction()

# A top-level tree: every unit of this project has the four warning flags, and -Werror exactly when <want_werror>; no unit of
# a dependency has a flag of leak_flags.
function(expect_top_level_units label prefix want_werror)
    unit_count(count "${label}" ${prefix} ${min_units})
    if("${count}" STREQUAL "")
        set(failures "${failures}" PARENT_SCOPE)
        return()
    endif()
    math(EXPR last "${count} - 1")
    set(own 0)
    set(theirs 0)
    set(seen_directories "")
    set(lacking "")
    set(strict "")
    set(leaked "")
    foreach(i RANGE ${last})
        string(JSON file ERROR_VARIABLE file_error GET "${${prefix}_db}" ${i} file)
        string(JSON command ERROR_VARIABLE command_error GET "${${prefix}_db}" ${i} command)
        if(file_error OR command_error)
            list(APPEND lacking "entry ${i}: no \"file\" or no \"command\" (CMake's layout changed?)")
            continue()
        endif()
        separate_arguments(arguments UNIX_COMMAND "${command}")
        set(directory "")
        foreach(candidate IN LISTS own_directories)
            string(FIND "${file}" "${SOURCE_DIR}/${candidate}/" at)
            if(at EQUAL 0)
                set(directory "${candidate}")
            endif()
        endforeach()
        if(NOT directory STREQUAL "")
            math(EXPR own "${own} + 1")
            list(APPEND seen_directories "${directory}")
            file(RELATIVE_PATH relative "${SOURCE_DIR}" "${file}")
            set(missing "")
            foreach(flag IN LISTS warning_flags)
                if(NOT flag IN_LIST arguments)
                    list(APPEND missing "${flag}")
                endif()
            endforeach()
            if(want_werror AND NOT "-Werror" IN_LIST arguments)
                list(APPEND missing "-Werror")
            elseif(NOT want_werror AND "-Werror" IN_LIST arguments)
                list(APPEND strict "${relative}")
            endif()
            if(missing)
                list(JOIN missing ", " missing_text)
                list(APPEND lacking "${relative}: no ${missing_text}")
            endif()
        else()
            math(EXPR theirs "${theirs} + 1")
            set(has "")
            foreach(flag IN LISTS leak_flags)
                if(flag IN_LIST arguments)
                    list(APPEND has "${flag}")
                endif()
            endforeach()
            if(has)
                list(JOIN has ", " has_text)
                list(APPEND leaked "${file}: ${has_text}")
            endif()
        endif()
    endforeach()

    foreach(directory IN LISTS own_directories)
        if(NOT directory IN_LIST seen_directories)
            string(APPEND failures "\n  ${label}: no unit under ${directory}/ among the ${count}, so the scan did not meet one")
        endif()
    endforeach()
    if(theirs EQUAL 0)
        string(APPEND failures "\n  ${label}: no unit of a dependency among the ${count}, so a leak could not be seen")
    endif()
    if(lacking)
        list(LENGTH lacking n)
        first_lines(lacking report)
        string(APPEND failures "\n  ${label}: ${n} of ${own} units of this project lack a flag; the first:\n    ${report}")
    endif()
    if(strict)
        list(LENGTH strict n)
        first_lines(strict report)
        string(APPEND failures "\n  ${label}: ${n} of ${own} units of this project have -Werror, want none; the first:\n    ${report}")
    endif()
    if(leaked)
        list(LENGTH leaked n)
        first_lines(leaked report)
        string(APPEND failures "\n  ${label}: ${n} of ${theirs} units of the dependencies have this project's flags; the first:\n    ${report}")
    endif()
    set(failures "${failures}" PARENT_SCOPE)
endfunction()

# A parent project's tree: no unit has a flag of leak_flags, and the consumer's own unit, which must be among them, has
# none of the five.
function(expect_parent_units label prefix)
    unit_count(count "${label}" ${prefix} 2)
    if("${count}" STREQUAL "")
        set(failures "${failures}" PARENT_SCOPE)
        return()
    endif()
    math(EXPR last "${count} - 1")
    set(found_consumer FALSE)
    set(flagged "")
    foreach(i RANGE ${last})
        string(JSON file ERROR_VARIABLE file_error GET "${${prefix}_db}" ${i} file)
        string(JSON command ERROR_VARIABLE command_error GET "${${prefix}_db}" ${i} command)
        if(file_error OR command_error)
            list(APPEND flagged "entry ${i}: no \"file\" or no \"command\" (CMake's layout changed?)")
            continue()
        endif()
        separate_arguments(arguments UNIX_COMMAND "${command}")
        set(forbidden ${leak_flags})
        if(file STREQUAL "${SOURCE_DIR}/tests/consumer/main.cpp")
            set(found_consumer TRUE)
            set(forbidden ${warning_flags} -Werror)
        endif()
        set(has "")
        foreach(flag IN LISTS forbidden)
            if(flag IN_LIST arguments)
                list(APPEND has "${flag}")
            endif()
        endforeach()
        if(has)
            list(JOIN has ", " has_text)
            list(APPEND flagged "${file}: ${has_text}")
        endif()
    endforeach()
    if(NOT found_consumer)
        string(APPEND failures "\n  ${label}: the consumer's own unit is not among the ${count} units")
    endif()
    if(flagged)
        list(LENGTH flagged n)
        first_lines(flagged report)
        string(APPEND failures "\n  ${label}: ${n} of ${count} units have flags of this project; the first:\n    ${report}")
    endif()
    set(failures "${failures}" PARENT_SCOPE)
endfunction()

if(DEFAULT)
    set(other OFF)
else()
    set(other ON)
endif()

# ── no option: the default ───────────────────────────────────────────────────
configure_root(a opt-default)
expect_configures("no option" a)
expect_top_level_units("no option" a ${DEFAULT})
expect_option("no option" a ${DEFAULT})
expect_flags_not_cached("no option" a)
if(DEFAULT)
    expect_output("no option" a "${relaxed_line}" FALSE)
else()
    expect_output("no option" a "${relaxed_line}" TRUE)
endif()

# ── the other value ──────────────────────────────────────────────────────────
configure_root(b opt-other -D${option}=${other})
expect_configures("${option}=${other}" b)
expect_top_level_units("${option}=${other}" b ${other})
expect_option("${option}=${other}" b ${other})
expect_flags_not_cached("${option}=${other}" b)
if(other)
    expect_output("${option}=${other}" b "${relaxed_line}" FALSE)
else()
    expect_output("${option}=${other}" b "${relaxed_line}" TRUE)
endif()

# ── a parent project that pulls the engine in ────────────────────────────────
configure(c "${SOURCE_DIR}/tests/consumer" parent -DFLYWHEEL_DAG_SOURCE_DIR=${SOURCE_DIR})
expect_configures("a parent project" c)
expect_no_option("a parent project" c)
expect_parent_units("a parent project" c)
expect_output("a parent project" c "${relaxed_line}" FALSE)

configure(d "${SOURCE_DIR}/tests/consumer" parent-on -DFLYWHEEL_DAG_SOURCE_DIR=${SOURCE_DIR} -D${option}=ON)
expect_configures("a parent project with the option" d)
expect_parent_units("a parent project with the option" d)

# ── the workflows ────────────────────────────────────────────────────────────
file(GLOB workflows "${SOURCE_DIR}/.github/workflows/*.yml" "${SOURCE_DIR}/.github/workflows/*.yaml")
list(SORT workflows)
if("${workflows}" STREQUAL "")
    string(APPEND failures "\n  the workflows: found no workflow in .github/workflows of ${SOURCE_DIR}")
endif()
foreach(workflow IN LISTS workflows)
    file(READ "${workflow}" yaml)
    # Whole-line comments go, so that a comment that names the option is not a line that passes it.
    string(REGEX REPLACE "\n[ \t]*#[^\n]*" "\n" code "\n${yaml}")
    string(REGEX MATCHALL "[^\n]*${option}[^\n]*" naming "${code}")
    if(NOT "${naming}" STREQUAL "")
        get_filename_component(base "${workflow}" NAME)
        list(GET naming 0 first)
        string(STRIP "${first}" first)
        string(APPEND failures "\n  the workflows: ${base} names ${option}; the default is the one place that sets it:\n    ${first}")
    endif()
endforeach()

if(failures)
    message(FATAL_ERROR "test_warning_option failed:${failures}")
endif()
file(REMOVE_RECURSE "${SCRATCH_DIR}")
list(LENGTH workflows workflow_count)
message(STATUS "test_warning_option: ok (-Werror is the default and ${option}=${other} is the other way; the flags are on "
               "every unit of this project and on no dependency's; a parent project keeps its own flags; none of the "
               "${workflow_count} workflows names the option)")
