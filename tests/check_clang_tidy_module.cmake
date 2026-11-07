# check_clang_tidy_module.cmake — test_clang_tidy_module: the unit filter of cmake/flywheel_dag_clang_tidy.cmake over a table, and
# tests/check_clang_tidy_units.cmake over compile databases that are written here.
#
# Run by ctest as
#
#   cmake -DSOURCE_DIR=<source> -DSCRATCH_DIR=<dir> -P check_clang_tidy_module.cmake
#
# SOURCE_DIR is the checkout whose module and script are under test: a hand-made change to a copy of either is passed here, and must
# fail the row or the case that names it. SCRATCH_DIR holds the databases and is emptied first. The filter is a pure function of its
# arguments, so its table uses invented paths and reads no disk. The script runs in a process of its own, over a database that it
# reads from disk. Neither needs clang-tidy, a configure or a build.

cmake_minimum_required(VERSION 4.0)

foreach(var IN ITEMS SOURCE_DIR SCRATCH_DIR)
    if(NOT ${var})
        message(FATAL_ERROR "usage: cmake -DSOURCE_DIR=<dir> -DSCRATCH_DIR=<dir> -P check_clang_tidy_module.cmake")
    endif()
endforeach()
set(module "${SOURCE_DIR}/cmake/flywheel_dag_clang_tidy.cmake")
set(script "${SOURCE_DIR}/tests/check_clang_tidy_units.cmake")
foreach(file IN ITEMS "${module}" "${script}")
    if(NOT EXISTS "${file}")
        message(FATAL_ERROR "${file} does not exist")
    endif()
endforeach()
include("${module}")

# The cases are counted and the failures kept in global properties, so that the functions below record them without macros: a macro
# re-reads its arguments, and a regular expression or a path with a backslash in it would change.
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

# ── the filter ───────────────────────────────────────────────────────────────
# The units for a root, a build tree and a list of candidates are exactly the arguments after the candidates, and there is no
# problem.
function(expect_units label root build candidates)
    count_case()
    flywheel_dag_clang_tidy_select("${root}" "${build}" "${candidates}" actual problem)
    if(NOT "${problem}" STREQUAL "")
        record_failure("${label}" "the filter refused: ${problem}")
    elseif(NOT "${actual}" STREQUAL "${ARGN}")
        record_failure("${label}" "expected '${ARGN}', got '${actual}'")
    endif()
endfunction()

# The filter refuses, with no unit, and its problem contains <text>.
function(expect_problem label root build candidates text)
    count_case()
    flywheel_dag_clang_tidy_select("${root}" "${build}" "${candidates}" actual problem)
    if("${problem}" STREQUAL "")
        record_failure("${label}" "no problem, and the units were '${actual}'")
        return()
    endif()
    if(NOT "${actual}" STREQUAL "")
        record_failure("${label}" "units '${actual}' came with the problem")
    endif()
    string(FIND "${problem}" "${text}" at)
    if(at EQUAL -1)
        record_failure("${label}" "the problem '${problem}' lacks '${text}'")
    endif()
endfunction()

expect_units("extensions: .cpp, .cc and .cxx are units, headers and build files are not"
             /repo /repo/build/tree
             "/repo/tests/a.cpp;/repo/tests/b.cc;/repo/tests/c.cxx;/repo/tests/d.hpp;/repo/tests/e.inl;/repo/tests/f.c;/repo/tests/CMakeLists.txt"
             /repo/tests/a.cpp /repo/tests/b.cc /repo/tests/c.cxx)

expect_units("root: a path outside it, or beside it under a longer name, is not under it"
             /repo /repo/build/tree
             "/repo/tests/a.cpp;/other/x.cpp;/repo2/x.cpp;/repo.cpp"
             /repo/tests/a.cpp)
expect_units("root: a trailing separator on it changes nothing"
             /repo/ /repo/build/tree "/repo/tests/a.cpp" /repo/tests/a.cpp)

expect_units("build tree: what is generated under it is not a unit, what is under a longer-named tree is"
             /repo /repo/build/tree
             "/repo/build/tree/gen/g.cpp;/repo/build/tree2/h.cpp;/repo/tests/a.cpp"
             /repo/build/tree2/h.cpp /repo/tests/a.cpp)
expect_units("build tree: one outside the root drops nothing of the root's"
             /repo /elsewhere/tree "/repo/tests/a.cpp;/elsewhere/tree/g.cpp" /repo/tests/a.cpp)

expect_units("_deps: a dependency under any _deps below the root is not a unit, this tree's or another's"
             /repo /repo/build/tree
             "/repo/tests/a.cpp;/repo/build/other/_deps/spdlog-src/src/s.cpp;/repo/third/_deps/y.cpp;/repo/_deps/z.cpp"
             /repo/tests/a.cpp)
expect_units("_deps: a name that only contains _deps is a unit"
             /repo /repo/build/tree
             "/repo/tests/my_deps/a.cpp;/repo/tests/_deps_b.cpp;/repo/tests/deps/c.cpp"
             /repo/tests/_deps_b.cpp /repo/tests/deps/c.cpp /repo/tests/my_deps/a.cpp)
expect_units("_deps: a root that is itself under a _deps is not held against its own sources"
             /work/_deps/proj /work/_deps/proj/build "/work/_deps/proj/tests/a.cpp" /work/_deps/proj/tests/a.cpp)

expect_units("paths: dot segments and doubled separators are the file they name"
             /repo /repo/build/tree
             "/repo/tests/../tests/a.cpp;/repo/./tests/b.cpp;/repo//tests//c.cpp"
             /repo/tests/a.cpp /repo/tests/b.cpp /repo/tests/c.cpp)
expect_units("order: sorted, and each unit once"
             /repo /repo/build/tree
             "/repo/tests/b.cpp;/repo/tests/a.cpp;/repo/tests/b.cpp;/repo/examples/c.cpp"
             /repo/examples/c.cpp /repo/tests/a.cpp /repo/tests/b.cpp)
expect_units("names: a dotted file name keeps its dot, so a.b.cpp and a.cpp are two tests"
             /repo /repo/build/tree "/repo/tests/a.b.cpp;/repo/tests/a.cpp" /repo/tests/a.b.cpp /repo/tests/a.cpp)

expect_problem("none: no candidate at all" /repo /repo/build/tree "" "found no .cpp, .cc or .cxx source under /repo among 0")
expect_problem("none: no candidate is a unit" /repo /repo/build/tree "/repo/tests/a.hpp;/other/b.cpp;/repo/build/tree/c.cpp"
               "found no .cpp, .cc or .cxx source under /repo among 3")
expect_problem("collision: two directories, one file name" /repo /repo/build/tree "/repo/tests/a.cpp;/repo/examples/a.cpp"
               "/repo/examples/a.cpp and /repo/tests/a.cpp would both be the test clang_tidy_a")
expect_problem("collision: one name, two extensions" /repo /repo/build/tree "/repo/tests/a.cpp;/repo/tests/a.cc"
               "/repo/tests/a.cc and /repo/tests/a.cpp would both be the test clang_tidy_a")
expect_problem("arguments: no root" "" /repo/build/tree "/repo/tests/a.cpp" "needs a root and a build tree")
expect_problem("arguments: no build tree" /repo "" "/repo/tests/a.cpp" "needs a root and a build tree")

# ── the script ───────────────────────────────────────────────────────────────
# The script runs against a checkout of its own under SCRATCH_DIR that holds nothing but a copy of the module, with the build trees
# of the cases inside it, so that the cases do not depend on where the real checkout and its build tree are. The units are invented:
# the script reads the database and never the files.
file(REMOVE_RECURSE "${SCRATCH_DIR}")
set(root "${SCRATCH_DIR}/checkout")
file(COPY "${module}" DESTINATION "${root}/cmake")
set(unit_a "${root}/tests/unit_a.cpp")
set(unit_b "${root}/tests/unit_b.cpp")
set(unit_c "${root}/examples/unit_c.cpp")

# A compile database in <dir> that lists the files, one entry each, in the form CMake writes: the "file" paths are absolute.
function(write_database dir)
    file(MAKE_DIRECTORY "${dir}")
    set(entries "")
    foreach(file IN LISTS ARGN)
        list(APPEND entries "  {\"directory\": \"${dir}\", \"command\": \"c++ -c ${file}\", \"file\": \"${file}\"}")
    endforeach()
    string(REPLACE ";" ",\n" body "${entries}")
    file(WRITE "${dir}/compile_commands.json" "[\n${body}\n]\n")
endfunction()

# The script's arguments for a tree and a registered list, bar-separated as the gate gives it.
function(script_arguments out build registered)
    set(${out} "-DSOURCE_DIR=${root}" "-DBUILD_DIR=${build}" "-DREGISTERED=${registered}" PARENT_SCOPE)
endfunction()

# Runs the script. <rc> gets the exit status and <text> the output, runs of whitespace as one space, because CMake wraps an error
# at about 80 columns.
function(run_script rc_var text_var)
    execute_process(COMMAND ${CMAKE_COMMAND} ${ARGN} -P "${script}"
                    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    string(REGEX REPLACE "[ \t\r\n]+" " " flat "${out} ${err}")
    set(${rc_var} "${rc}" PARENT_SCOPE)
    set(${text_var} "${flat}" PARENT_SCOPE)
endfunction()

# The script accepts the arguments, and says that it holds <units> units.
function(expect_script_accepts label units)
    count_case()
    run_script(rc text ${ARGN})
    if(NOT rc EQUAL 0)
        record_failure("${label}" "refused, exit ${rc}: ${text}")
        return()
    endif()
    string(FIND "${text}" "the ${units} units the gate checks are exactly the ${units} that the tree compiles" at)
    if(at EQUAL -1)
        record_failure("${label}" "accepted without saying that it holds ${units} units: ${text}")
    endif()
endfunction()

# The script refuses the arguments with a non-zero exit, and its text has every string of <musts> (a list) and none of <must_nots>
# (a list).
function(expect_script_refuses label musts must_nots)
    count_case()
    run_script(rc text ${ARGN})
    if(rc EQUAL 0)
        record_failure("${label}" "accepted: ${text}")
        return()
    endif()
    foreach(must IN LISTS musts)
        string(FIND "${text}" "${must}" at)
        if(at EQUAL -1)
            record_failure("${label}" "the text lacks '${must}': ${text}")
        endif()
    endforeach()
    foreach(must_not IN LISTS must_nots)
        string(FIND "${text}" "${must_not}" at)
        if(NOT at EQUAL -1)
            record_failure("${label}" "the text has '${must_not}': ${text}")
        endif()
    endforeach()
endfunction()

set(dir "${root}/build/accepted")
write_database("${dir}" "${unit_a}" "${unit_b}" "${unit_b}" "${unit_b}"
               "${root}/build/other/_deps/dep-src/d.cpp" "${dir}/gen/g.cpp" /elsewhere/ext.cpp)
script_arguments(arguments "${dir}" "${unit_a}|${unit_b}")
expect_script_accepts("script: two units, a dependency, a generated source, a file outside the root, one file in three entries"
                      2 ${arguments})
script_arguments(arguments "${dir}" "${root}/tests/../tests/unit_a.cpp|${unit_b}")
expect_script_accepts("script: a registered path with a dot segment is the file it names" 2 ${arguments})

set(dir "${root}/build/unregistered")
write_database("${dir}" "${unit_a}" "${unit_b}" "${unit_c}")
script_arguments(arguments "${dir}" "${unit_a}|${unit_b}")
expect_script_refuses("script: a unit in the database and not registered is named"
                      "not registered;${unit_c}" "does not list" ${arguments})
script_arguments(arguments "${dir}" "")
expect_script_refuses("script: nothing registered names every unit"
                      "not registered;${unit_a};${unit_b};${unit_c}" "does not list" ${arguments})

set(dir "${root}/build/unbuilt")
write_database("${dir}" "${unit_a}")
script_arguments(arguments "${dir}" "${unit_a}|${unit_b}")
expect_script_refuses("script: a registered unit that the database does not list is named"
                      "does not list;${unit_b}" "not registered" ${arguments})

set(dir "${root}/build/both")
write_database("${dir}" "${unit_a}" "${unit_c}")
script_arguments(arguments "${dir}" "${unit_a}|${unit_b}")
expect_script_refuses("script: both directions at once"
                      "not registered;${unit_c};does not list;${unit_b}" "" ${arguments})

set(dir "${root}/build/empty")
write_database("${dir}")
script_arguments(arguments "${dir}" "${unit_a}")
expect_script_refuses("script: an empty database" "found no" "" ${arguments})
script_arguments(arguments "${dir}" "")
expect_script_refuses("script: an empty database and nothing registered" "found no" "" ${arguments})

set(dir "${root}/build/no_database")
file(MAKE_DIRECTORY "${dir}")
script_arguments(arguments "${dir}" "${unit_a}")
expect_script_refuses("script: no compile database" "compile_commands.json does not exist" "" ${arguments})

set(dir "${root}/build/malformed")
file(MAKE_DIRECTORY "${dir}")
script_arguments(arguments "${dir}" "${unit_a}")
file(WRITE "${dir}/compile_commands.json" "this is not json")
expect_script_refuses("script: a database that is not JSON, with the parser's reason and not a guess at its top level"
                      "is not a compile database" "its top level is" ${arguments})
file(WRITE "${dir}/compile_commands.json" "{\"file\": \"${unit_a}\"}")
expect_script_refuses("script: a database that is an object" "is not a compile database;its top level is OBJECT" "" ${arguments})
file(WRITE "${dir}/compile_commands.json" "[{\"directory\": \"${dir}\"}]")
expect_script_refuses("script: an entry with no file" "entry 0 has no file" "" ${arguments})

set(dir "${root}/build/accepted")
foreach(omitted IN ITEMS SOURCE_DIR BUILD_DIR REGISTERED)
    script_arguments(arguments "${dir}" "${unit_a}|${unit_b}")
    list(FILTER arguments EXCLUDE REGEX "^-D${omitted}=")
    expect_script_refuses("script: no ${omitted}" "usage: cmake" "" ${arguments})
endforeach()

get_property(cases GLOBAL PROPERTY CASES)
get_property(failed GLOBAL PROPERTY FAILED)
get_property(failures GLOBAL PROPERTY FAILURES)
if(failed GREATER 0)
    message(FATAL_ERROR "test_clang_tidy_module: ${failed} of ${cases} cases failed:\n${failures}")
endif()
message(STATUS "test_clang_tidy_module: all ${cases} cases hold")
