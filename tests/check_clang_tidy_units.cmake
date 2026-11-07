# check_clang_tidy_units.cmake — the units that were registered are the units that the tree compiles.
#
# Run as
#
#   cmake -DSOURCE_DIR=<repo> -DBUILD_DIR=<tree> "-DREGISTERED=<unit>|<unit>|..." -P check_clang_tidy_units.cmake
#
# REGISTERED is the list of registered units, bar-separated, because a ; would split it into arguments of the command that runs
# this. It reads the tree's compile_commands.json, takes the units among the database's files by the filter that the registration
# uses (cmake/flywheel_dag_clang_tidy.cmake), and compares them with the registered list. The database is what clang-tidy itself
# reads, and a unit that is not in it would be analysed with flags that clang-tidy guesses from its neighbours' entries, not the
# flags a build uses. A difference in either direction fails, and names the units:
#
#   - in the database and not registered: a target made after the registration call in the root CMakeLists.txt, or a source that
#     the registration could not read (one inside a generator expression), so a unit that is built is not checked;
#   - registered and not in the database: a unit that the tree does not build.
#
# A gate that silently checks fewer files than the tree builds passes, which is why this is a test. CMake writes the "file" of each
# entry as an absolute path, and the script reads it so. It needs no clang-tidy and no build.

cmake_minimum_required(VERSION 4.0)

foreach(required IN ITEMS SOURCE_DIR BUILD_DIR REGISTERED)
    if(NOT DEFINED ${required})
        message(FATAL_ERROR "usage: cmake -DSOURCE_DIR=<repo> -DBUILD_DIR=<tree> \"-DREGISTERED=<unit>|<unit>|...\" "
                            "-P check_clang_tidy_units.cmake")
    endif()
endforeach()
set(module "${SOURCE_DIR}/cmake/flywheel_dag_clang_tidy.cmake")
if(NOT EXISTS "${module}")
    message(FATAL_ERROR "${module} does not exist")
endif()
include("${module}")

set(database "${BUILD_DIR}/compile_commands.json")
if(NOT EXISTS "${database}")
    message(FATAL_ERROR "${database} does not exist: the tree was configured without a compile database, and this reads one")
endif()
file(READ "${database}" json)
string(JSON type ERROR_VARIABLE problem TYPE "${json}")
if(problem)
    message(FATAL_ERROR "${database} is not a compile database: ${problem}")
endif()
if(NOT type STREQUAL "ARRAY")
    message(FATAL_ERROR "${database} is not a compile database: its top level is ${type}, and a database is an array of entries")
endif()
string(JSON count LENGTH "${json}")

set(candidates "")
if(count GREATER 0)
    math(EXPR last "${count} - 1")
    foreach(i RANGE ${last})
        string(JSON file ERROR_VARIABLE problem GET "${json}" ${i} file)
        if(problem)
            message(FATAL_ERROR "${database}: entry ${i} has no file (${problem})")
        endif()
        list(APPEND candidates "${file}")
    endforeach()
endif()
flywheel_dag_clang_tidy_select("${SOURCE_DIR}" "${BUILD_DIR}" "${candidates}" compiled problem)
if(problem)
    message(FATAL_ERROR "${database}: ${problem}")
endif()

string(REPLACE "|" ";" listed "${REGISTERED}")
set(registered "")
foreach(unit IN LISTS listed)
    cmake_path(NORMAL_PATH unit)
    list(APPEND registered "${unit}")
endforeach()
list(SORT registered)

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

difference("${compiled}" "${registered}" unchecked)
difference("${registered}" "${compiled}" unbuilt)
set(failures "")
if(NOT "${unchecked}" STREQUAL "")
    string(REPLACE ";" "\n    " shown "${unchecked}")
    string(APPEND failures
        "\n  the compile database lists units that were not registered, so they are built and not checked (a target made after "
        "the registration call in the root CMakeLists.txt, or a source inside a generator expression?):\n    ${shown}")
endif()
if(NOT "${unbuilt}" STREQUAL "")
    string(REPLACE ";" "\n    " shown "${unbuilt}")
    string(APPEND failures
        "\n  units were registered that the compile database does not list, so clang-tidy would guess their flags (a target "
        "that an option switches off, or a source that no target compiles?):\n    ${shown}")
endif()
if(NOT "${failures}" STREQUAL "")
    message(FATAL_ERROR "the clang-tidy units are not the tree's units:${failures}")
endif()
list(LENGTH compiled units)
message(STATUS "the ${units} units the gate checks are exactly the ${units} that the tree compiles")
