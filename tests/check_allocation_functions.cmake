# check_allocation_functions.cmake — test_allocation_functions: one file of the checkout defines the global
# allocation functions, and it defines all twenty.
#
# Run by ctest as
#
#   cmake -DSOURCE_DIR=<source> -P check_allocation_functions.cmake
#
# tests/allocation_counter.cpp replaces the twenty global allocation functions the standard makes replaceable
# ([new.delete]), and every program that counts allocations links it: the suites that count, and the benchmark. A
# program that defines some of its own instead gets the rest from the standard library or, in a sanitized build,
# from the sanitizer, which does not forward them to the program's. The count then misses the forms the program left
# out, and AddressSanitizer aborts when the library takes memory from one family and returns it to the other. Nothing
# says so in an ordinary build, and a program that does not link gtest may never say so at all.
#
# Two things are held, in the code, comments dropped, of every .hpp, .inl and .cpp under include/, tests/,
# benchmarks/ and examples/:
#
#   the counter   tests/allocation_counter.cpp defines twenty: eight operator new and twelve operator delete. A
#                 count of anything else means the file lost a form, or that the pattern below no longer reads a
#                 definition, and then the next rule would hold nothing.
#   the rest      no other file declares or defines one. A match is `void* operator new` or `void operator
#                 delete`, array forms included, however it is spaced. A call names no return type
#                 (`::operator new(8)`), so a call is not a match.
#
# COUNTER and ROOTS default to the checkout's. Each is a parameter so that a hand-made change can be made to a copy
# and must fail here.

cmake_minimum_required(VERSION 4.0)

if(NOT SOURCE_DIR)
    message(FATAL_ERROR "usage: cmake -DSOURCE_DIR=<dir> [-DCOUNTER=<file under the dir>] [-DROOTS=<dir>[,<dir>...]] "
                        "-P check_allocation_functions.cmake")
endif()
if(NOT COUNTER)
    set(COUNTER "tests/allocation_counter.cpp")
endif()
if(NOT ROOTS)
    set(ROOTS "include,tests,benchmarks,examples")
endif()
string(REPLACE "," ";" roots "${ROOTS}")

set(new_regex    "(^|[^A-Za-z0-9_])void[ \t\r\n]*[*][ \t\r\n]*operator[ \t\r\n]+new[ \t\r\n]*(\\[[ \t]*\\])?[ \t\r\n]*\\(")
set(delete_regex "(^|[^A-Za-z0-9_])void[ \t\r\n]+operator[ \t\r\n]+delete[ \t\r\n]*(\\[[ \t]*\\])?[ \t\r\n]*\\(")

# A file's text less its // comments, so that a function which only a comment names is not one.
function(read_code file out)
    file(READ "${file}" text)
    string(REGEX REPLACE "//[^\n]*" "" text "${text}")
    set(${out} "${text}" PARENT_SCOPE)
endfunction()

# count(<out> <text> <regex>): how many times the regex matches. The text is matched whole, because it holds `;`
# and `[`, which a list of its lines would split.
function(count out text regex)
    set(n 0)
    set(rest "${text}")
    while(TRUE)
        string(REGEX MATCH "${regex}" match "${rest}")
        if(match STREQUAL "")
            break()
        endif()
        math(EXPR n "${n} + 1")
        string(FIND "${rest}" "${match}" at)
        string(LENGTH "${match}" length)
        math(EXPR from "${at} + ${length}")
        string(SUBSTRING "${rest}" ${from} -1 rest)
    endwhile()
    set(${out} ${n} PARENT_SCOPE)
endfunction()

set(files "")
foreach(root IN LISTS roots)
    file(GLOB_RECURSE found RELATIVE "${SOURCE_DIR}"
         "${SOURCE_DIR}/${root}/*.hpp" "${SOURCE_DIR}/${root}/*.inl" "${SOURCE_DIR}/${root}/*.cpp")
    list(APPEND files ${found})
endforeach()
list(SORT files)
list(LENGTH files file_count)
if(file_count EQUAL 0)
    message(FATAL_ERROR "found no source under ${SOURCE_DIR} in ${ROOTS}")
endif()

set(problems "")
if(NOT COUNTER IN_LIST files)
    string(APPEND problems "  ${COUNTER} is missing\n")
endif()

foreach(file IN LISTS files)
    read_code("${SOURCE_DIR}/${file}" code)
    count(news "${code}" "${new_regex}")
    count(deletes "${code}" "${delete_regex}")
    if(file STREQUAL COUNTER)
        if(NOT news EQUAL 8 OR NOT deletes EQUAL 12)
            string(APPEND problems "  ${file} defines ${news} operator new and ${deletes} operator delete, not the 8 "
                                   "and 12 that [new.delete] makes replaceable\n")
        endif()
    elseif(news GREATER 0 OR deletes GREATER 0)
        string(APPEND problems "  ${file} declares or defines ${news} operator new and ${deletes} operator delete "
                               "of its own\n")
    endif()
endforeach()

if(NOT problems STREQUAL "")
    message(FATAL_ERROR "the global allocation functions are not ${COUNTER}'s alone:\n${problems}"
            "A program that counts allocations links ${COUNTER}, which replaces all twenty, and calls "
            "allocation_counter::start() and stop(). One that replaces some of its own misses the rest, and aborts "
            "under AddressSanitizer when the standard library pairs them across forms.")
endif()
math(EXPR other_count "${file_count} - 1")
message(STATUS "${COUNTER} defines the twenty allocation functions, and none of the other ${other_count} sources "
               "declares one")
