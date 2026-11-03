# check_standard_guard.cmake — test_standard_guard: every public header refuses to
# compile below C++23, and says why.
#
# Run by ctest as
#
#   cmake -DCXX=<compiler> -DINCLUDE_DIR=<source>/include -DWORK_DIR=<dir>
#         -P check_standard_guard.cmake
#
# and registered at the top level for GCC and Clang, whose -std= and -fsyntax-only
# it passes. The whole suite builds as C++23, so nothing else would notice a guard
# that stopped working; this holds the guards themselves to account.
#
# Each header gets a translation unit of its own that includes nothing else, and is
# compiled at C++17 and at C++20. Both must fail, and the failure must be the
# guard's: a header that fails for another reason, such as a dependency it cannot
# find, has not shown that it refuses. The glob is the list of headers, so a header
# added later is covered, and finding none is an error, so a wrong path cannot pass
# by checking nothing.

cmake_minimum_required(VERSION 4.0)

foreach(var IN ITEMS CXX INCLUDE_DIR WORK_DIR)
    if(NOT ${var})
        message(FATAL_ERROR "usage: cmake -DCXX=<compiler> -DINCLUDE_DIR=<dir> "
                            "-DWORK_DIR=<dir> -P check_standard_guard.cmake")
    endif()
endforeach()

file(GLOB headers RELATIVE "${INCLUDE_DIR}" "${INCLUDE_DIR}/flywheel/*.hpp")
list(SORT headers)
list(LENGTH headers count)
if(count EQUAL 0)
    message(FATAL_ERROR "found no header under ${INCLUDE_DIR}/flywheel")
endif()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

set(problems "")
foreach(header IN LISTS headers)
    get_filename_component(stem "${header}" NAME_WE)
    set(unit "${WORK_DIR}/${stem}.cpp")
    file(WRITE "${unit}" "#include <${header}>\n")
    foreach(std IN ITEMS c++17 c++20)
        execute_process(
            COMMAND "${CXX}" -std=${std} -fsyntax-only "-I${INCLUDE_DIR}" "${unit}"
            RESULT_VARIABLE result
            ERROR_VARIABLE  errors
            OUTPUT_QUIET)
        if(result EQUAL 0)
            string(APPEND problems "  ${header} compiled at -std=${std}\n")
        elseif(NOT errors MATCHES "requires C\\+\\+23")
            string(SUBSTRING "${errors}" 0 300 shown)
            string(APPEND problems
                "  ${header} failed at -std=${std}, but not with the guard's message:\n${shown}\n")
        endif()
    endforeach()
endforeach()

if(NOT problems STREQUAL "")
    message(FATAL_ERROR "a header does not refuse to compile below C++23:\n${problems}")
endif()
message(STATUS "all ${count} headers refuse -std=c++17 and -std=c++20")
