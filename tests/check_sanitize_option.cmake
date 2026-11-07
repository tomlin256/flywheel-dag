# check_sanitize_option.cmake — test_sanitize_option: the FLYWHEEL_DAG_SANITIZE option of the root CMakeLists.txt reaches every
# translation unit of the tree, the fetched dependencies' included, and reaches nothing of a project that pulls the engine in.
#
# Run by ctest as
#
#   cmake -DSOURCE_DIR=<source> -DSCRATCH_DIR=<dir> -DGENERATOR=<generator>
#         -DSPDLOG_SOURCE_DIR=<dir> -DNLOHMANN_JSON_SOURCE_DIR=<dir> -P check_sanitize_option.cmake
#
# It configures the real top-level project, as check_tree_name.cmake does, with the tests off, the examples and the benchmarks on
# (their sources and spdlog's are the units) and the two dependencies taken from sources already on disk, so it needs no network and
# compiles nothing. Every tree is made in SCRATCH_DIR/build/<name>, and the names claim no platform, so the build-tree guard does not
# examine them. The generator must write compile_commands.json, which Makefiles and Ninja do.
#
# What it holds, from the compile_commands.json each tree writes:
#
#   - with no option, the status line is absent, the cache says OFF and no unit has a -fsanitize option;
#   - with asan or tsan, the status line names the mode, the cache holds it, and every translation unit carries every compile option
#     of that mode, the fetched spdlog's units among them (the expected list is read from cmake/flywheel_dag_sanitize.cmake, so
#     test_sanitize_flags is the one place that pins the text of the options);
#   - a mistyped mode stops the configure, naming the value and listing the modes;
#   - a parent project that pulls the engine in, with the option on its command line, has no -fsanitize option on any unit: the option
#     is the top level's alone.
#
# The tests are off here, so the units are the examples', the benchmarks' and spdlog's: that is enough to prove that the root applies
# the options at all, and before it makes the dependencies. The units of the tests and the link line of every executable are held in
# a sanitizer tree by test_sanitizer_coverage.
#
# SOURCE_DIR is a parameter so that a hand-made change to a copy of the module or of the root is passed here, and must fail.

cmake_minimum_required(VERSION 4.0)

foreach(var IN ITEMS SOURCE_DIR SCRATCH_DIR GENERATOR SPDLOG_SOURCE_DIR NLOHMANN_JSON_SOURCE_DIR)
    if(NOT ${var})
        message(FATAL_ERROR "usage: cmake -DSOURCE_DIR=<dir> -DSCRATCH_DIR=<dir> -DGENERATOR=<name> "
                            "-DSPDLOG_SOURCE_DIR=<dir> -DNLOHMANN_JSON_SOURCE_DIR=<dir> -P check_sanitize_option.cmake")
    endif()
endforeach()
include("${SOURCE_DIR}/cmake/flywheel_dag_sanitize.cmake")

# Fewer units than this and the scan has read nothing worth the name: spdlog alone is seven.
set(min_units 5)

file(REMOVE_RECURSE "${SCRATCH_DIR}")
file(MAKE_DIRECTORY "${SCRATCH_DIR}/build")

set(cases 0)
set(failures "")

# Configure <source> into SCRATCH_DIR/build/<name>. Sets <prefix>_rc, <prefix>_text (the output and the error output, runs of
# whitespace as one space, because CMake wraps an error at about 80 columns), <prefix>_tree and <prefix>_db (the tree's
# compile_commands.json, or empty if it wrote none).
function(configure prefix source name)
    set(tree "${SCRATCH_DIR}/build/${name}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -G "${GENERATOR}" -S "${source}" -B "${tree}"
            -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
            -DFETCHCONTENT_FULLY_DISCONNECTED=ON
            "-DFETCHCONTENT_SOURCE_DIR_SPDLOG=${SPDLOG_SOURCE_DIR}"
            "-DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON=${NLOHMANN_JSON_SOURCE_DIR}"
            ${ARGN}
        RESULT_VARIABLE rc
        OUTPUT_VARIABLE out
        ERROR_VARIABLE err)
    string(REGEX REPLACE "[ \t\r\n]+" " " text "${out} ${err}")
    set(db "")
    if(EXISTS "${tree}/compile_commands.json")
        file(READ "${tree}/compile_commands.json" db)
    endif()
    set(${prefix}_rc "${rc}" PARENT_SCOPE)
    set(${prefix}_text "${text}" PARENT_SCOPE)
    set(${prefix}_tree "${tree}" PARENT_SCOPE)
    set(${prefix}_db "${db}" PARENT_SCOPE)
endfunction()

# The real root, the tests off and the examples and the benchmarks on.
function(configure_root prefix name)
    configure(${prefix} "${SOURCE_DIR}" "${name}"
        -DFLYWHEEL_DAG_BUILD_TESTS=OFF
        -DFLYWHEEL_DAG_BUILD_EXAMPLES=ON
        -DFLYWHEEL_DAG_BUILD_BENCHMARKS=ON
        -DFLYWHEEL_DAG_INSTALL=OFF
        ${ARGN})
    foreach(var IN ITEMS rc text tree db)
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
        string(APPEND failures "\n  ${label}: the output does not contain '${needle}': ${${prefix}_text}")
    elseif(NOT want_present AND NOT at EQUAL -1)
        string(APPEND failures "\n  ${label}: the output contains '${needle}', want it absent")
    endif()
    set(failures "${failures}" PARENT_SCOPE)
endfunction()

# The cache of the tree must hold a line.
function(expect_cache label prefix line)
    set(cache "")
    if(EXISTS "${${prefix}_tree}/CMakeCache.txt")
        file(READ "${${prefix}_tree}/CMakeCache.txt" cache)
    endif()
    string(FIND "\n${cache}" "\n${line}\n" at)
    if(at EQUAL -1)
        string(APPEND failures "\n  ${label}: the cache does not hold '${line}'")
    endif()
    set(failures "${failures}" PARENT_SCOPE)
endfunction()

# The units of the tree's compile_commands.json, as the number of entries, with a floor under it so that a tree that wrote
# nothing, or a layout this script cannot read, fails instead of passing on nothing. <out> is empty when the database is unusable,
# and the problem is a failure.
function(unit_count out label prefix)
    set(${out} "" PARENT_SCOPE)
    set(db "${${prefix}_db}")
    if("${db}" STREQUAL "")
        string(APPEND failures "\n  ${label}: the tree wrote no compile_commands.json (this test needs a Makefiles or Ninja generator)")
        set(failures "${failures}" PARENT_SCOPE)
        return()
    endif()
    string(JSON count ERROR_VARIABLE error LENGTH "${db}")
    if(error OR count LESS min_units)
        string(APPEND failures "\n  ${label}: compile_commands.json has '${count}' entries (${error}), want at least ${min_units}")
        set(failures "${failures}" PARENT_SCOPE)
        return()
    endif()
    set(${out} "${count}" PARENT_SCOPE)
endfunction()

# No translation unit has an option that starts with -fsanitize, and the database is one this script could read.
function(expect_no_sanitize_option label prefix)
    unit_count(count "${label}" ${prefix})
    if(NOT "${count}" STREQUAL "")
        string(FIND "${${prefix}_db}" "-fsanitize" at)
        if(NOT at EQUAL -1)
            string(APPEND failures "\n  ${label}: compile_commands.json has a -fsanitize option, want none")
        endif()
    endif()
    set(failures "${failures}" PARENT_SCOPE)
endfunction()

# Every translation unit carries every option in ARGN, as a whole argument of its command line, so that -fsanitize=address does
# not pass for -fsanitize=address,undefined. The fetched spdlog's units must be among the ones read: a dependency made before the
# options are set is the failure this exists for, and a scan that never met it would prove nothing.
function(expect_units_carry label prefix)
    unit_count(count "${label}" ${prefix})
    if("${count}" STREQUAL "")
        set(failures "${failures}" PARENT_SCOPE)
        return()
    endif()
    math(EXPR last "${count} - 1")
    set(lacking "")
    set(in_spdlog 0)
    foreach(i RANGE ${last})
        string(JSON file ERROR_VARIABLE file_error GET "${${prefix}_db}" ${i} file)
        string(JSON command ERROR_VARIABLE command_error GET "${${prefix}_db}" ${i} command)
        if(file_error OR command_error)
            list(APPEND lacking "entry ${i}: no \"file\" or no \"command\" (CMake's layout changed?)")
            continue()
        endif()
        string(FIND "${file}" "${SPDLOG_SOURCE_DIR}/" in_dep)
        if(in_dep EQUAL 0)
            math(EXPR in_spdlog "${in_spdlog} + 1")
        endif()
        separate_arguments(arguments UNIX_COMMAND "${command}")
        set(missing "")
        foreach(option IN LISTS ARGN)
            if(NOT option IN_LIST arguments)
                list(APPEND missing "${option}")
            endif()
        endforeach()
        if(missing)
            list(JOIN missing ", " missing_text)
            list(APPEND lacking "${file}: no ${missing_text}")
        endif()
    endforeach()
    if(in_spdlog EQUAL 0)
        string(APPEND failures "\n  ${label}: none of the ${count} units is spdlog's, so the scan has not met a dependency")
    endif()
    if(lacking)
        list(LENGTH lacking n_lacking)
        list(SUBLIST lacking 0 8 shown)
        list(JOIN shown "\n    " report)
        string(APPEND failures "\n  ${label}: ${n_lacking} of ${count} units miss an option; the first:\n    ${report}")
    endif()
    set(failures "${failures}" PARENT_SCOPE)
endfunction()

# ── no option: nothing changes ───────────────────────────────────────────────
configure_root(a opt-default)
expect_configures("no option" a)
expect_output("no option" a "flywheel-dag: sanitizer" FALSE)
expect_cache("no option" a "FLYWHEEL_DAG_SANITIZE:STRING=OFF")
expect_no_sanitize_option("no option" a)

# ── each mode: the status line, the cache, and every unit carries the mode's options ─────────────
foreach(mode IN ITEMS asan tsan)
    flywheel_dag_sanitizer_flags("${mode}" want_compile want_link)
    configure_root(m opt-${mode} -DFLYWHEEL_DAG_SANITIZE=${mode})
    expect_configures("${mode}" m)
    expect_output("${mode}" m "flywheel-dag: sanitizer '${mode}'" TRUE)
    expect_cache("${mode}" m "FLYWHEEL_DAG_SANITIZE:STRING=${mode}")
    expect_units_carry("${mode}" m ${want_compile})
endforeach()

# ── a mistyped mode stops the configure, naming the value and the modes ──────────────────────────
configure_root(b opt-bad -DFLYWHEEL_DAG_SANITIZE=adress)
if(b_rc EQUAL 0)
    string(APPEND failures "\n  adress: the configure succeeded, want it refused")
else()
    foreach(needle IN ITEMS "FLYWHEEL_DAG_SANITIZE='adress'" "OFF, asan, tsan")
        string(FIND "${b_text}" "${needle}" at)
        if(at EQUAL -1)
            string(APPEND failures "\n  adress: the output does not contain '${needle}': ${b_text}")
        endif()
    endforeach()
endif()

# ── a parent project that pulls the engine in: the option is the top level's alone ────────────────
# tests/consumer is the parent, with the option on its command line. Its units are spdlog's and its own, and none may be
# instrumented: the engine's options are not the consumer's, and instrumenting the engine's fetched spdlog alone would be
# partial instrumentation.
configure(c "${SOURCE_DIR}/tests/consumer" opt-consumer
    -DFLYWHEEL_DAG_SOURCE_DIR=${SOURCE_DIR}
    -DFLYWHEEL_DAG_SANITIZE=asan)
expect_configures("a parent with the option" c)
expect_output("a parent with the option" c "flywheel-dag: sanitizer" FALSE)
expect_no_sanitize_option("a parent with the option" c)

if(failures)
    message(FATAL_ERROR "test_sanitize_option failed:${failures}")
endif()
file(REMOVE_RECURSE "${SCRATCH_DIR}")
message(STATUS "test_sanitize_option: ok (no option changes nothing; asan and tsan reach every unit, spdlog's too; "
               "adress is refused; a parent project is left alone)")
