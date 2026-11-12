# check_clang_tidy_wiring.cmake — test_clang_tidy_wiring: the clang-tidy gate is registered for the units that the targets compile, and
# for no other file.
#
# Run by ctest as
#
#   cmake -DSOURCE_DIR=<source> -DSCRATCH_DIR=<dir> -DGENERATOR=<generator> -DSPDLOG_SOURCE_DIR=<dir>
#         -DNLOHMANN_JSON_SOURCE_DIR=<dir> -DGOOGLETEST_SOURCE_DIR=<dir> -P check_clang_tidy_wiring.cmake
#
# SOURCE_DIR is the checkout whose root, module and script are under test: a hand-made change to a copy of any of them is passed here,
# and must fail the case that names it. SCRATCH_DIR holds the trees and is emptied first. Each case configures a tree and asks it for
# its tests with `ctest --show-only=json-v1`. The tool is sh, which a test lists and nothing runs, so no clang-tidy is needed; nothing
# is compiled; the dependencies come from sources already on disk, so no network is needed. There are two kinds of tree:
#
#   - A stub project laid out like a checkout (the module, the script and the canary fixtures, copied), with a target of every kind
#     the gate reads and a source of every kind it must not: a unit in a subdirectory two levels down, one compiled by two targets,
#     a .cc, a name with a dot in it; a generated source, one that only a custom target lists, one under a _deps and one outside the
#     root. Two variants of it break a rule that clang_tidy_units exists to notice: a target made after the registration call, and a
#     source inside a generator expression.
#   - The real root, in the four combinations of the examples and the benchmarks. The expected units come from the checkout and not
#     from the module: every .cpp directly in tests/, and in examples/ and benchmarks/ when their option is on. A .cpp there that no
#     target compiles is expected and not registered, so it fails here, naming the file.
#
# For each tree the checks are the names of the gate's tests (the units, and version, checks, units, the three canaries and
# headers_alone), the command of each unit's test, and the command and the properties of the others.

cmake_minimum_required(VERSION 4.0)

foreach(var IN ITEMS SOURCE_DIR SCRATCH_DIR GENERATOR SPDLOG_SOURCE_DIR NLOHMANN_JSON_SOURCE_DIR GOOGLETEST_SOURCE_DIR)
    if(NOT ${var})
        message(FATAL_ERROR "usage: cmake -DSOURCE_DIR=<dir> -DSCRATCH_DIR=<dir> -DGENERATOR=<name> -DSPDLOG_SOURCE_DIR=<dir> "
                            "-DNLOHMANN_JSON_SOURCE_DIR=<dir> -DGOOGLETEST_SOURCE_DIR=<dir> -P check_clang_tidy_wiring.cmake")
    endif()
endforeach()
set(module "${SOURCE_DIR}/cmake/flywheel_dag_clang_tidy.cmake")
set(script "${SOURCE_DIR}/tests/check_clang_tidy_units.cmake")
set(canaries "${SOURCE_DIR}/tests/clang_tidy_canary")
set(canary_files canary.cpp canary_engine.hpp canary_dep.hpp)
foreach(file IN ITEMS "${module}" "${script}")
    if(NOT EXISTS "${file}")
        message(FATAL_ERROR "${file} does not exist")
    endif()
endforeach()
foreach(name IN LISTS canary_files)
    if(NOT EXISTS "${canaries}/${name}")
        message(FATAL_ERROR "${canaries}/${name} does not exist")
    endif()
endforeach()

# The tool is a command that is never run.
find_program(tool NAMES sh NO_CACHE)
if(NOT tool)
    message(FATAL_ERROR "no sh on this host: the cases need a command that a test names and nothing runs")
endif()

# The SDK that the registration hands clang-tidy on macOS, from the same question to the same tool.
set(sysroot "")
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
    execute_process(COMMAND xcrun --show-sdk-path OUTPUT_VARIABLE sdk OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE status)
    if(NOT status EQUAL 0 OR "${sdk}" STREQUAL "")
        message(FATAL_ERROR "xcrun --show-sdk-path names no SDK on this host")
    endif()
    set(sysroot --extra-arg=-isysroot "--extra-arg=${sdk}")
endif()

# The tests that every tree has, whatever its units.
set(fixed_names clang_tidy_version clang_tidy_checks clang_tidy_units clang_tidy_canary_source clang_tidy_canary_engine_header
                clang_tidy_canary_dependency_header clang_tidy_headers_alone)

# The major that the registration tells the headers-alone script on an Apple host: the one the module holds the tool to.
set(library "")
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
    file(READ "${module}" module_text)
    if(NOT module_text MATCHES "set\\(_flywheel_dag_clang_tidy_major ([0-9]+)\\)")
        message(FATAL_ERROR "${module} does not set _flywheel_dag_clang_tidy_major to a number")
    endif()
    set(library "-DLIBCXX_MAJOR=${CMAKE_MATCH_1}")
endif()

# The cases are counted and the failures kept in global properties, so that the functions below record them without macros.
set_property(GLOBAL PROPERTY CASES 0)
set_property(GLOBAL PROPERTY FAILED 0)
set_property(GLOBAL PROPERTY FAILURES "")

function(count_case)
    get_property(n GLOBAL PROPERTY CASES)
    math(EXPR n "${n} + 1")
    set_property(GLOBAL PROPERTY CASES ${n})
endfunction()

function(record_failure label detail)
    get_property(n GLOBAL PROPERTY FAILED)
    math(EXPR n "${n} + 1")
    set_property(GLOBAL PROPERTY FAILED ${n})
    set_property(GLOBAL APPEND_STRING PROPERTY FAILURES "  ${label}: ${detail}\n")
endfunction()

# The items of <a> that are not in <b>.
function(difference a b out)
    set(result "")
    foreach(item IN LISTS a)
        if(NOT item IN_LIST b)
            list(APPEND result "${item}")
        endif()
    endforeach()
    set(${out} "${result}" PARENT_SCOPE)
endfunction()

# ── trees ────────────────────────────────────────────────────────────────────
# Configures <source> into <tree>. <text> gets the output and the error output together, runs of whitespace as one space, because
# CMake wraps an error at about 80 columns.
function(configure rc_var text_var source tree)
    execute_process(COMMAND ${CMAKE_COMMAND} -G "${GENERATOR}" -S "${source}" -B "${tree}" ${ARGN}
                    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    string(REGEX REPLACE "[ \t\r\n]+" " " flat "${out} ${err}")
    set(${rc_var} "${rc}" PARENT_SCOPE)
    set(${text_var} "${flat}" PARENT_SCOPE)
endfunction()

# The tests array of the JSON that ctest prints for the gate's tests of <tree>. <problem> says why <out> is empty.
function(gate_tests out problem_out tree)
    set(${out} "" PARENT_SCOPE)
    set(${problem_out} "" PARENT_SCOPE)
    execute_process(COMMAND ${CMAKE_CTEST_COMMAND} --show-only=json-v1 -R "^clang_tidy_"
                    WORKING_DIRECTORY "${tree}" RESULT_VARIABLE rc OUTPUT_VARIABLE json ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
        set(${problem_out} "ctest in ${tree} exited ${rc}: ${err}" PARENT_SCOPE)
        return()
    endif()
    string(JSON tests ERROR_VARIABLE problem GET "${json}" tests)
    if(problem)
        set(${problem_out} "ctest in ${tree} printed no list of tests: ${problem}" PARENT_SCOPE)
        return()
    endif()
    set(${out} "${tests}" PARENT_SCOPE)
endfunction()

# Runs the gate's clang_tidy_units test in <tree>. <rc> gets the exit status and <text> the output.
function(run_units_test rc_var text_var tree)
    execute_process(COMMAND ${CMAKE_CTEST_COMMAND} -R "^clang_tidy_units$" --no-tests=error --output-on-failure
                    WORKING_DIRECTORY "${tree}" RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    string(REGEX REPLACE "[ \t\r\n]+" " " flat "${out} ${err}")
    set(${rc_var} "${rc}" PARENT_SCOPE)
    set(${text_var} "${flat}" PARENT_SCOPE)
endfunction()

# ── what ctest says about a test ─────────────────────────────────────────────
# The names of the tests in the array, sorted.
function(listed_names out tests)
    string(JSON count LENGTH "${tests}")
    set(names "")
    if(count GREATER 0)
        math(EXPR last "${count} - 1")
        foreach(i RANGE ${last})
            string(JSON name GET "${tests}" ${i} name)
            list(APPEND names "${name}")
        endforeach()
    endif()
    list(SORT names)
    set(${out} "${names}" PARENT_SCOPE)
endfunction()

# The index of the test called <name> in the array, or -1.
function(test_index out tests name)
    set(${out} -1 PARENT_SCOPE)
    string(JSON count LENGTH "${tests}")
    if(count GREATER 0)
        math(EXPR last "${count} - 1")
        foreach(i RANGE ${last})
            string(JSON candidate GET "${tests}" ${i} name)
            if(candidate STREQUAL name)
                set(${out} ${i} PARENT_SCOPE)
                return()
            endif()
        endforeach()
    endif()
endfunction()

# The command of the test called <name> as a list, empty when there is no such test.
function(test_command out tests name)
    set(${out} "" PARENT_SCOPE)
    test_index(at "${tests}" "${name}")
    if(at EQUAL -1)
        return()
    endif()
    string(JSON count LENGTH "${tests}" ${at} command)
    math(EXPR last "${count} - 1")
    set(command "")
    foreach(j RANGE ${last})
        string(JSON item GET "${tests}" ${at} command ${j})
        list(APPEND command "${item}")
    endforeach()
    set(${out} "${command}" PARENT_SCOPE)
endfunction()

# The value of a property of the test called <name>: for a property that holds a list, its first item. Empty when the test has no
# such property.
function(test_property out tests name property)
    set(${out} "" PARENT_SCOPE)
    test_index(at "${tests}" "${name}")
    if(at EQUAL -1)
        return()
    endif()
    string(JSON count ERROR_VARIABLE problem LENGTH "${tests}" ${at} properties)
    if(problem OR count EQUAL 0)
        return()
    endif()
    math(EXPR last "${count} - 1")
    foreach(j RANGE ${last})
        string(JSON candidate GET "${tests}" ${at} properties ${j} name)
        if(candidate STREQUAL property)
            string(JSON type TYPE "${tests}" ${at} properties ${j} value)
            if(type STREQUAL "ARRAY")
                string(JSON value GET "${tests}" ${at} properties ${j} value 0)
            else()
                string(JSON value GET "${tests}" ${at} properties ${j} value)
            endif()
            set(${out} "${value}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
endfunction()

# ── checks on one tree ───────────────────────────────────────────────────────
# The name of the unit test of each path in ARGN.
function(unit_names out)
    set(names "")
    foreach(unit IN LISTS ARGN)
        cmake_path(GET unit STEM LAST_ONLY stem)
        list(APPEND names "clang_tidy_${stem}")
    endforeach()
    set(${out} "${names}" PARENT_SCOPE)
endfunction()

# The tree's gate has the tests of the units in ARGN and of the fixed names, and no other.
function(expect_listing label tests)
    count_case()
    unit_names(unit_tests ${ARGN})
    set(expected ${unit_tests} ${fixed_names})
    list(SORT expected)
    listed_names(actual "${tests}")
    difference("${expected}" "${actual}" lacking)
    difference("${actual}" "${expected}" surplus)
    if(NOT "${lacking}" STREQUAL "")
        # A unit's path says more than its test's name: a missing one is a source that no target compiles, or a target the walk missed.
        set(shown "")
        foreach(name IN LISTS lacking)
            list(FIND unit_tests "${name}" at)
            if(at EQUAL -1)
                list(APPEND shown "${name}")
            else()
                list(GET ARGN ${at} path)
                list(APPEND shown "${name} (${path})")
            endif()
        endforeach()
        string(REPLACE ";" ", " shown "${shown}")
        record_failure("${label}" "the gate lacks tests that it should have: ${shown}")
    endif()
    if(NOT "${surplus}" STREQUAL "")
        string(REPLACE ";" ", " shown "${surplus}")
        record_failure("${label}" "the gate has tests that it should not have: ${shown}")
    endif()
endfunction()

# What is wrong with the command of the test called <name>, when it is not the list <expected>. <out> is empty when the command is right,
# and when the tree lacks the test, which is for the listing to report.
function(command_problem out tests name expected)
    set(${out} "" PARENT_SCOPE)
    test_index(at "${tests}" "${name}")
    if(at EQUAL -1)
        return()
    endif()
    test_command(actual "${tests}" "${name}")
    if("${actual}" STREQUAL "${expected}")
        return()
    endif()
    list(LENGTH actual have)
    list(LENGTH expected want)
    set(detail "it has ${have} arguments and should have ${want}")
    set(i 0)
    while(i LESS have AND i LESS want)
        list(GET actual ${i} got)
        list(GET expected ${i} wanted)
        if(NOT got STREQUAL wanted)
            set(detail "argument ${i} is '${got}' and should be '${wanted}'")
            break()
        endif()
        math(EXPR i "${i} + 1")
    endwhile()
    set(${out} "the command of ${name} is wrong: ${detail}" PARENT_SCOPE)
endfunction()

function(expect_command label tests name expected)
    command_problem(problem "${tests}" "${name}" "${expected}")
    if(NOT "${problem}" STREQUAL "")
        record_failure("${label}" "${problem}")
    endif()
endfunction()

# The test called <name> has the property <property> with a value, and when <pattern> is given, one that matches it.
function(expect_property label tests name property)
    test_index(at "${tests}" "${name}")
    if(at EQUAL -1)
        return()
    endif()
    test_property(value "${tests}" "${name}" ${property})
    if("${value}" STREQUAL "")
        record_failure("${label}" "${name} has no ${property}")
    elseif(NOT "${ARGN}" STREQUAL "" AND NOT "${value}" MATCHES "${ARGN}")
        record_failure("${label}" "the ${property} of ${name} is '${value}', which does not match '${ARGN}'")
    endif()
endfunction()

# The command of each unit's test is the tool on the unit, with the arguments the registration builds. One failure for the tree, naming
# the first wrong command, because one slip in the registration gets every unit wrong.
function(expect_unit_commands label tests tree extra)    # ARGN: the units
    count_case()
    set(wrong 0)
    set(first "")
    foreach(unit IN LISTS ARGN)
        cmake_path(GET unit STEM LAST_ONLY stem)
        set(expected "${tool}" -p "${tree}" --quiet "--warnings-as-errors=*" "--extra-arg=-Wno-error=deprecated-declarations"
                     ${extra} ${sysroot} "${unit}")
        command_problem(problem "${tests}" "clang_tidy_${stem}" "${expected}")
        if(NOT "${problem}" STREQUAL "")
            math(EXPR wrong "${wrong} + 1")
            if("${first}" STREQUAL "")
                set(first "${problem}")
            endif()
        endif()
    endforeach()
    if(wrong GREATER 0)
        list(LENGTH ARGN total)
        record_failure("${label}" "${wrong} of ${total} unit commands are wrong; the first: ${first}")
    endif()
endfunction()

# The tests that every tree has: the version, the checks, the three canaries, the units test and the headers-alone test.
# <includes> is the list of include directories that the registration was given.
function(expect_fixed_tests label tests root tree extra includes)    # ARGN: the units
    count_case()
    set(config "--config-file=${root}/.clang-tidy")

    expect_command("${label}" "${tests}" clang_tidy_version "${tool};--version")
    expect_property("${label}" "${tests}" clang_tidy_version PASS_REGULAR_EXPRESSION "^LLVM version [0-9]+")

    expect_command("${label}" "${tests}" clang_tidy_checks "${tool};${config};--list-checks")
    expect_property("${label}" "${tests}" clang_tidy_checks PASS_REGULAR_EXPRESSION)
    expect_property("${label}" "${tests}" clang_tidy_checks FAIL_REGULAR_EXPRESSION)

    # The canaries run one command, over copies of the fixtures under the tree.
    set(canary "${tree}/clang_tidy_canary")
    set(expected "${tool}" "${config}" "--warnings-as-errors=*" "--extra-arg=-Wno-error=deprecated-declarations" ${extra} ${sysroot}
                 "${canary}/canary.cpp" -- -std=c++23 "-I${canary}/include" "-I${canary}/_deps/canary-src/include")
    foreach(name IN ITEMS clang_tidy_canary_source clang_tidy_canary_engine_header clang_tidy_canary_dependency_header)
        expect_command("${label}" "${tests}" ${name} "${expected}")
        expect_property("${label}" "${tests}" ${name} PASS_REGULAR_EXPRESSION)
    endforeach()
    expect_property("${label}" "${tests}" clang_tidy_canary_dependency_header FAIL_REGULAR_EXPRESSION)
    foreach(path IN ITEMS canary.cpp include/flywheel/canary_engine.hpp _deps/canary-src/include/canary_dep.hpp)
        if(NOT EXISTS "${canary}/${path}")
            record_failure("${label}" "the fixture ${canary}/${path} was not copied into the tree")
        endif()
    endforeach()

    # The units test is the script, on the units, bar-separated, in order.
    set(registered ${ARGN})
    list(SORT registered)
    string(REPLACE ";" "|" registered "${registered}")
    expect_command("${label}" "${tests}" clang_tidy_units
                   "${CMAKE_COMMAND};-DSOURCE_DIR=${root};-DBUILD_DIR=${tree};-DREGISTERED=${registered};-P;${root}/tests/check_clang_tidy_units.cmake")

    # The headers-alone test is its script, on the tool, with the arguments of a unit's run and the include directories, each
    # bar-separated, and on an Apple host the major of the libc++ that the units must meet.
    set(alone_args "--warnings-as-errors=*" "--extra-arg=-Wno-error=deprecated-declarations" ${extra} ${sysroot})
    string(REPLACE ";" "|" alone_args "${alone_args}")
    string(REPLACE ";" "|" alone_includes "${includes}")
    set(expected "${CMAKE_COMMAND}" "-DCLANG_TIDY=${tool}" "-DCONFIG_FILE=${root}/.clang-tidy" "-DCLANG_TIDY_ARGS=${alone_args}"
                 "-DINCLUDE_DIR=${root}/include" "-DDEPENDENCY_INCLUDE_DIRS=${alone_includes}"
                 "-DWORK_DIR=${tree}/clang_tidy_headers_alone" ${library} -P "${root}/tests/check_headers_alone.cmake")
    expect_command("${label}" "${tests}" clang_tidy_headers_alone "${expected}")
endfunction()

# The tree's clang_tidy_units test passes.
function(expect_units_pass label tree)
    count_case()
    run_units_test(rc text "${tree}")
    if(NOT rc EQUAL 0)
        record_failure("${label}" "clang_tidy_units failed (${rc}): ${text}")
    endif()
endfunction()

# The tree's clang_tidy_units test fails, and says <text>.
function(expect_units_fail label tree text)
    count_case()
    run_units_test(rc text_out "${tree}")
    if(rc EQUAL 0)
        record_failure("${label}" "clang_tidy_units passed, and should fail")
        return()
    endif()
    string(FIND "${text_out}" "${text}" at)
    if(at EQUAL -1)
        record_failure("${label}" "clang_tidy_units failed without saying '${text}': ${text_out}")
    endif()
endfunction()

# ── the stub project ─────────────────────────────────────────────────────────
# Writes a stub checkout into <root>: its tree is <root>/build, so that a generated source is under the root and under the tree.
# <late> makes a target after the registration call, and <genex> a target whose source is inside a generator expression.
set(outside "${SCRATCH_DIR}/outside")
function(write_stub root late genex)
    foreach(source IN ITEMS src/exe.cpp src/stat.cpp src/stat.hpp src/shared.cpp src/mod.cpp src/obj.cpp src/cc_unit.cc
                            src/dotted.name.cpp src/twice.cpp src/listed.cpp src/late.cpp src/genex.cpp sub/deep.cpp
                            sub/nested/inner.cpp _deps/dep-src/dep.cpp)
        file(WRITE "${root}/${source}" "int unit() { return 0; }\n")
    endforeach()
    file(WRITE "${outside}/other.cpp" "int outside() { return 0; }\n")
    file(COPY "${module}" DESTINATION "${root}/cmake")
    file(COPY "${script}" DESTINATION "${root}/tests")
    foreach(name IN LISTS canary_files)
        file(COPY "${canaries}/${name}" DESTINATION "${root}/tests/clang_tidy_canary")
    endforeach()
    file(WRITE "${root}/sub/CMakeLists.txt" "add_library(stub_deep STATIC deep.cpp)\nadd_subdirectory(nested)\n")
    file(WRITE "${root}/sub/nested/CMakeLists.txt" "add_library(stub_inner STATIC inner.cpp)\n")

    set(TOOL "${tool}")
    set(OUTSIDE "${outside}")
    set(GENEX "")
    set(LATE "")
    if(genex)
        set(GENEX [=[add_executable(stub_genex "src/$<1:genex>.cpp")]=])
    endif()
    if(late)
        set(LATE "add_executable(stub_late src/late.cpp)")
    endif()
    file(CONFIGURE OUTPUT "${root}/CMakeLists.txt" @ONLY CONTENT [=[
cmake_minimum_required(VERSION 4.0)
project(wiring_stub LANGUAGES CXX)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
enable_testing()
include(${CMAKE_CURRENT_LIST_DIR}/cmake/flywheel_dag_clang_tidy.cmake)

# Units, one kind of target each; a header listed with a source; one source in two targets; a directory two levels down.
add_executable(stub_exe src/exe.cpp)
add_library(stub_static STATIC src/stat.cpp src/stat.hpp)
add_library(stub_shared SHARED src/shared.cpp)
add_library(stub_module MODULE src/mod.cpp)
add_library(stub_object OBJECT src/obj.cpp src/cc_unit.cc src/dotted.name.cpp)
add_executable(stub_twice_a src/twice.cpp)
add_executable(stub_twice_b src/twice.cpp)
add_subdirectory(sub)

# Sources that are not units: one generated into the build tree, one that only a custom target lists, one under a _deps in the
# root, one outside the root.
add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/generated/gen.cpp
                   COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/generated
                   COMMAND ${CMAKE_COMMAND} -E touch ${CMAKE_BINARY_DIR}/generated/gen.cpp)
add_library(stub_generated OBJECT ${CMAKE_BINARY_DIR}/generated/gen.cpp)
add_custom_target(stub_listing SOURCES src/listed.cpp)
add_library(stub_dependency OBJECT _deps/dep-src/dep.cpp)
add_library(stub_outside OBJECT @OUTSIDE@/other.cpp)

@GENEX@
flywheel_dag_clang_tidy_add_tests("@TOOL@" "${CMAKE_CURRENT_LIST_DIR}" "--extra-arg=-DWIRING_A;--extra-arg=-DWIRING_B"
                                  "${CMAKE_CURRENT_LIST_DIR}/dep_a/include;${CMAKE_CURRENT_LIST_DIR}/dep_b/include")
@LATE@
]=])
endfunction()

file(REMOVE_RECURSE "${SCRATCH_DIR}")

# The units of the stub, relative to its root.
set(stub_units src/cc_unit.cc src/dotted.name.cpp src/exe.cpp src/mod.cpp src/obj.cpp src/shared.cpp src/stat.cpp src/twice.cpp
               sub/deep.cpp sub/nested/inner.cpp)
set(stub_extra "--extra-arg=-DWIRING_A;--extra-arg=-DWIRING_B")

# Configures a stub into <SCRATCH_DIR>/<name> and sets <root>, <tree> and <tests> (empty, with the failure recorded, when it cannot).
function(make_stub name late genex)
    set(root "${SCRATCH_DIR}/${name}")
    set(tree "${root}/build")
    write_stub("${root}" ${late} ${genex})
    count_case()
    configure(rc text "${root}" "${tree}")
    set(tests "")
    if(NOT rc EQUAL 0)
        record_failure("stub ${name}: configure" "the configure failed (${rc}): ${text}")
    else()
        gate_tests(tests problem "${tree}")
        if(NOT "${problem}" STREQUAL "")
            record_failure("stub ${name}: configure" "${problem}")
        endif()
    endif()
    set(root "${root}" PARENT_SCOPE)
    set(tree "${tree}" PARENT_SCOPE)
    set(tests "${tests}" PARENT_SCOPE)
endfunction()

set(absolute_units "")
make_stub(stub OFF OFF)
foreach(unit IN LISTS stub_units)
    list(APPEND absolute_units "${root}/${unit}")
endforeach()
list(SORT absolute_units)
if(NOT "${tests}" STREQUAL "")
    expect_listing("stub: the units are the sources of the targets, and no other file" "${tests}" ${absolute_units})
    expect_unit_commands("stub: the command of a unit" "${tests}" "${tree}" "${stub_extra}" ${absolute_units})
    expect_fixed_tests("stub: the fixed tests" "${tests}" "${root}" "${tree}" "${stub_extra}"
                       "${root}/dep_a/include;${root}/dep_b/include" ${absolute_units})
    expect_units_pass("stub: clang_tidy_units passes" "${tree}")
endif()

# A target made after the registration call is a unit in the compile database that nothing registered.
make_stub(stub_late ON OFF)
if(NOT "${tests}" STREQUAL "")
    set(late_units "")
    foreach(unit IN LISTS stub_units)
        list(APPEND late_units "${root}/${unit}")
    endforeach()
    list(SORT late_units)
    expect_listing("stub with a late target: the registration is of the targets made before it" "${tests}" ${late_units})
    expect_units_fail("stub with a late target: clang_tidy_units names its source" "${tree}" "${root}/src/late.cpp")
endif()

# A source inside a generator expression is one the walk cannot read.
make_stub(stub_genex OFF ON)
if(NOT "${tests}" STREQUAL "")
    set(genex_units "")
    foreach(unit IN LISTS stub_units)
        list(APPEND genex_units "${root}/${unit}")
    endforeach()
    list(SORT genex_units)
    expect_listing("stub with a generator-expression source: the walk skips it" "${tests}" ${genex_units})
    expect_units_fail("stub with a generator-expression source: clang_tidy_units names it" "${tree}" "${root}/src/genex.cpp")
endif()

# ── the real root ────────────────────────────────────────────────────────────
# The units of the checkout that the options leave on: every .cpp directly in the directories.
function(real_units out examples benchmarks)
    file(GLOB sources "${SOURCE_DIR}/tests/*.cpp")
    if(examples)
        file(GLOB more "${SOURCE_DIR}/examples/*.cpp")
        list(APPEND sources ${more})
    endif()
    if(benchmarks)
        file(GLOB more "${SOURCE_DIR}/benchmarks/*.cpp")
        list(APPEND sources ${more})
    endif()
    list(SORT sources)
    set(${out} "${sources}" PARENT_SCOPE)
endfunction()

set(real_extra "--extra-arg=-DWIRING_PROBE")
function(check_real_root name examples benchmarks)
    set(tree "${SCRATCH_DIR}/${name}/build")
    count_case()
    configure(rc text "${SOURCE_DIR}" "${tree}"
              -DFETCHCONTENT_FULLY_DISCONNECTED=ON
              -DFETCHCONTENT_SOURCE_DIR_SPDLOG=${SPDLOG_SOURCE_DIR}
              -DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON=${NLOHMANN_JSON_SOURCE_DIR}
              -DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=${GOOGLETEST_SOURCE_DIR}
              -DFLYWHEEL_DAG_BUILD_TESTS=ON
              -DFLYWHEEL_DAG_BUILD_EXAMPLES=${examples}
              -DFLYWHEEL_DAG_BUILD_BENCHMARKS=${benchmarks}
              -DFLYWHEEL_DAG_INSTALL=OFF
              -DFLYWHEEL_DAG_CLANG_TIDY=sh
              "-DFLYWHEEL_DAG_CLANG_TIDY_ARGS=${real_extra}")
    if(NOT rc EQUAL 0)
        record_failure("real root ${name}: configure" "the configure failed (${rc}): ${text}")
        return()
    endif()
    gate_tests(tests problem "${tree}")
    if(NOT "${problem}" STREQUAL "")
        record_failure("real root ${name}: configure" "${problem}")
        return()
    endif()
    real_units(units ${examples} ${benchmarks})
    expect_listing("real root ${name}: the gate is registered for the files of the options that are on" "${tests}" ${units})
    expect_unit_commands("real root ${name}: the command of a unit" "${tests}" "${tree}" "${real_extra}" ${units})
    expect_fixed_tests("real root ${name}: the fixed tests" "${tests}" "${SOURCE_DIR}" "${tree}" "${real_extra}"
                       "${SPDLOG_SOURCE_DIR}/include;${NLOHMANN_JSON_SOURCE_DIR}/include" ${units})
    expect_units_pass("real root ${name}: clang_tidy_units passes" "${tree}")
endfunction()

check_real_root(defaults ON ON)
check_real_root(examples_off OFF ON)
check_real_root(benchmarks_off ON OFF)
check_real_root(both_off OFF OFF)

get_property(cases GLOBAL PROPERTY CASES)
get_property(failed GLOBAL PROPERTY FAILED)
get_property(failures GLOBAL PROPERTY FAILURES)
if(failed GREATER 0)
    message(FATAL_ERROR "test_clang_tidy_wiring: ${failed} failure(s) over ${cases} cases:\n${failures}")
endif()
message(STATUS "test_clang_tidy_wiring: all ${cases} cases hold")
