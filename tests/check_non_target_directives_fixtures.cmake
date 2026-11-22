# check_non_target_directives_fixtures.cmake — test_non_target_directives_fixtures: check_non_target_directives.cmake finds every
# call of a non-target directive, and nothing else, and names where.
#
# Run by ctest as
#
#   cmake -DSOURCE_DIR=<source> -DSCRATCH_DIR=<dir> -P check_non_target_directives_fixtures.cmake
#
# It writes small trees under SCRATCH_DIR and runs SOURCE_DIR/tests/check_non_target_directives.cmake over each, which is the script
# that test_non_target_directives runs over the checkout. The trees hold the seven commands as text, so this file is one of the four that
# that script does not read (it names them): a script run by cmake -P cannot call them, and this is the one that holds the script's
# own behaviour.
#
# What it holds:
#
#   - accepts: a tree whose comments, strings and calls of the target commands name or resemble the seven, and that has a call of one
#     inside a build tree and inside .git, passes and says how many files it read;
#   - rejects: a tree with every one of the seven, in the spellings a call has, is refused, and the report is exactly the list of
#     file:line: command that the tree holds, in order, each command named in lower case, with no line for a comment, a string or a
#     call of a target command;
#   - the floor: a tree with no CMake file, and a tree with fewer than MIN_FILES, are refused, saying how many files were read;
#   - the usage: a missing or a bad MIN_FILES is refused, naming it.
#
# SOURCE_DIR is a parameter so that a hand-made change can be made to a scratch copy of the script and must fail here.

cmake_minimum_required(VERSION 4.0)

foreach(var IN ITEMS SOURCE_DIR SCRATCH_DIR)
    if(NOT ${var})
        message(FATAL_ERROR "usage: cmake -DSOURCE_DIR=<dir> -DSCRATCH_DIR=<dir> -P check_non_target_directives_fixtures.cmake")
    endif()
endforeach()
set(script "${SOURCE_DIR}/tests/check_non_target_directives.cmake")
if(NOT EXISTS "${script}")
    message(FATAL_ERROR "test_non_target_directives_fixtures: ${script} does not exist")
endif()

file(REMOVE_RECURSE "${SCRATCH_DIR}")
set(failures "")
set(cases 0)

# put(<tree> <file> <text>): the file SCRATCH_DIR/<tree>/<file>, holding the text. The text is one argument, so a semicolon or a
# bracket in it stays where it is.
function(put tree file text)
    get_filename_component(directory "${SCRATCH_DIR}/${tree}/${file}" DIRECTORY)
    file(MAKE_DIRECTORY "${directory}")
    file(WRITE "${SCRATCH_DIR}/${tree}/${file}" "${text}")
endfunction()

# run_script(<prefix> <tree> [<arguments>...]): runs the script over SCRATCH_DIR/<tree>, with MIN_FILES=3 unless an argument gives
# another. Sets <prefix>_rc and <prefix>_text, the output and the error output with the whitespace as it is.
function(run_script prefix tree)
    set(arguments ${ARGN})
    if(NOT arguments MATCHES "MIN_FILES")
        list(APPEND arguments -DMIN_FILES=3)
    endif()
    execute_process(
        COMMAND "${CMAKE_COMMAND}" "-DSOURCE_DIR=${SCRATCH_DIR}/${tree}" ${arguments} -P "${script}"
        RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    set(${prefix}_rc "${rc}" PARENT_SCOPE)
    set(${prefix}_text "${out}\n${err}" PARENT_SCOPE)
endfunction()

# The report lines of a refusal: "  <file>:<line>: <command>", in order, as one list.
function(report_lines out text)
    string(REPLACE "\n" ";" lines "${text}")
    set(found "")
    foreach(line IN LISTS lines)
        if(line MATCHES "^ +([^ :]+:[0-9]+: [a-z_]+)$")
            list(APPEND found "${CMAKE_MATCH_1}")
        endif()
    endforeach()
    set(${out} "${found}" PARENT_SCOPE)
endfunction()

function(note_failure text)
    math(EXPR n "${cases} + 1")
    set(cases ${n} PARENT_SCOPE)
    string(APPEND failures "\n  ${text}")
    set(failures "${failures}" PARENT_SCOPE)
endfunction()

function(count_case)
    math(EXPR n "${cases} + 1")
    set(cases ${n} PARENT_SCOPE)
endfunction()

# ── accepts ──────────────────────────────────────────────────────────────────
put(accepts CMakeLists.txt [=[
# The seven names are in this comment and none is called: add_compile_definitions(X) add_compile_options(-Wall) add_definitions(-DX)
# add_link_options(-s) include_directories(include) link_directories(lib) link_libraries(m)
add_executable(app main.cpp)
target_compile_definitions(app PRIVATE X=1)
target_compile_options(app PRIVATE -Wall)
target_include_directories(app PRIVATE include)
target_link_directories(app PRIVATE lib)
target_link_libraries(app PRIVATE m)
target_link_options(app PRIVATE -s)
message(STATUS "include_directories is a word in this string, and link_libraries is another")
target_link_libraries(app PRIVATE n) # link_libraries(x) is a trailing comment
set(files a.cpp;b.cpp [list] add_definitions)
]=])
put(accepts cmake/module.cmake [=[
function(give_options target)
    target_compile_options(${target} PRIVATE ${ARGN})
endfunction()
]=])
put(accepts tests/CMakeLists.txt [=[
add_test(NAME app COMMAND app)
]=])
# A build tree and .git hold other projects' files: a dependency's include_directories() is not this project's.
put(accepts build/relwithdebinfo-macos-arm64/_deps/dep-src/CMakeLists.txt "include_directories(x)\n")
put(accepts cmake-build-debug/CMakeLists.txt "link_libraries(m)\n")
put(accepts .git/hooks/fixture.cmake "add_definitions(-DX)\n")
run_script(accepts accepts)
count_case()
if(NOT accepts_rc EQUAL 0)
    note_failure("accepts: the script refused a tree with no call (${accepts_rc}): ${accepts_text}")
elseif(NOT accepts_text MATCHES "none in 3 CMake files")
    note_failure("accepts: the script does not say how many files it read ('none in 3 CMake files'): ${accepts_text}")
endif()

# ── rejects ──────────────────────────────────────────────────────────────────
# Line numbers are those of the text below: the first line of each is the comment.
put(rejects CMakeLists.txt [=[
# One call of each of the seven commands, then a call of a target command, which is not one.
add_compile_definitions(X=1)
add_compile_options(-Wall)
add_definitions(-DX)
add_link_options(-s)
include_directories(include)
link_directories(lib)
link_libraries(m)
target_link_libraries(app PRIVATE m)
]=])
put(rejects cmake/config.cmake.in [=[
link_directories(@PREFIX@/lib)
]=])
put(rejects cmake/module.cmake [=[
function(setup)
    set(pair "a;b;c")
    set(open "[")
    ADD_COMPILE_OPTIONS(-Wall)
    Include_Directories (include)
    if(TRUE) add_definitions(-DX) endif()
    link_libraries
        (m)
    target_include_directories(app PRIVATE include)
endfunction()
]=])
put(rejects tests/CMakeLists.txt [=[
add_test(NAME app COMMAND app)
]=])
run_script(rejects rejects)
count_case()
set(want
    "CMakeLists.txt:2: add_compile_definitions"
    "CMakeLists.txt:3: add_compile_options"
    "CMakeLists.txt:4: add_definitions"
    "CMakeLists.txt:5: add_link_options"
    "CMakeLists.txt:6: include_directories"
    "CMakeLists.txt:7: link_directories"
    "CMakeLists.txt:8: link_libraries"
    "cmake/config.cmake.in:1: link_directories"
    "cmake/module.cmake:4: add_compile_options"
    "cmake/module.cmake:5: include_directories"
    "cmake/module.cmake:6: add_definitions"
    "cmake/module.cmake:7: link_libraries")
report_lines(got "${rejects_text}")
if(rejects_rc EQUAL 0)
    note_failure("rejects: the script passed a tree that calls all seven commands")
elseif(NOT "${got}" STREQUAL "${want}")
    string(REPLACE ";" "\n      " want_text "${want}")
    string(REPLACE ";" "\n      " got_text "${got}")
    note_failure("rejects: the report is not the calls the tree holds.\n    want:\n      ${want_text}\n    got:\n      ${got_text}")
endif()
if(NOT rejects_rc EQUAL 0 AND NOT rejects_text MATCHES "target_compile_options")
    note_failure("rejects: the refusal does not say what to use instead (target_compile_options): ${rejects_text}")
endif()

# ── the floor ────────────────────────────────────────────────────────────────
put(nothing_to_scan README.md "no CMake file here, and nothing to read\n")
run_script(nothing nothing_to_scan)
count_case()
if(nothing_rc EQUAL 0)
    note_failure("nothing_to_scan: the script passed a tree with no CMake file")
elseif(NOT nothing_text MATCHES "read 0 CMake files")
    note_failure("nothing_to_scan: the refusal does not say 'read 0 CMake files': ${nothing_text}")
endif()

run_script(short accepts -DMIN_FILES=4)
count_case()
if(short_rc EQUAL 0)
    note_failure("floor: the script passed three files with MIN_FILES=4")
elseif(NOT short_text MATCHES "read 3 CMake files, and this check expects at least 4")
    note_failure("floor: the refusal does not say 'read 3 CMake files, and this check expects at least 4': ${short_text}")
endif()

# ── the usage ────────────────────────────────────────────────────────────────
foreach(bad IN ITEMS "-DMIN_FILES=0" "-DMIN_FILES=three" "-DMIN_FILES=")
    run_script(usage accepts ${bad})
    count_case()
    if(usage_rc EQUAL 0)
        note_failure("usage: the script passed ${bad}")
    elseif(NOT usage_text MATCHES "MIN_FILES")
        note_failure("usage: the refusal of ${bad} does not name MIN_FILES: ${usage_text}")
    endif()
endforeach()
execute_process(COMMAND "${CMAKE_COMMAND}" -DMIN_FILES=3 -P "${script}"
                RESULT_VARIABLE unset_rc OUTPUT_VARIABLE unset_out ERROR_VARIABLE unset_err)
count_case()
if(unset_rc EQUAL 0)
    note_failure("usage: the script passed with no SOURCE_DIR")
elseif(NOT "${unset_out}${unset_err}" MATCHES "SOURCE_DIR")
    note_failure("usage: the refusal of no SOURCE_DIR does not name it: ${unset_out}${unset_err}")
endif()

if(failures)
    message(FATAL_ERROR "test_non_target_directives_fixtures failed:${failures}")
endif()
file(REMOVE_RECURSE "${SCRATCH_DIR}")
message(STATUS "test_non_target_directives_fixtures: ok (${cases} cases)")
