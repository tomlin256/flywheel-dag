# check_stateful_node_list.cmake — test_stateful_node_list: the cast test of tests/test_engine_stateful_discovery.cpp
# builds one of every class that derives StatefulNodeBase.
#
# Run by ctest as
#
#   cmake -DSOURCE_DIR=<source> -P check_stateful_node_list.cmake
#
# StatefulDiscovery.CastWorksOnEveryStatefulNodeType constructs one of each concrete stateful node type and casts it the
# way discovery does. A class that it leaves out could lose the IStatefulNode mixin with the test still green, and
# nothing in C++ lists the classes that derive a base. So this reads them: every `public StatefulNodeBase<Name, ...>` in
# a header under include/flywheel/, comments dropped, is a class, and the body of the test must say Name::make( or
# Name<...>::make( for each. A class added later fails here until the test builds it.
#
# It fails too when it finds no class, or no such test, so that a moved file or a renamed test cannot pass by reading
# nothing.
#
# INCLUDE_DIR and TEST_FILE default to the checkout's. Each is a parameter so that a hand-made change can be made to a
# copy and must fail here.

cmake_minimum_required(VERSION 4.0)

if(NOT SOURCE_DIR)
    message(FATAL_ERROR "usage: cmake -DSOURCE_DIR=<dir> [-DINCLUDE_DIR=<dir>] [-DTEST_FILE=<file>] "
                        "-P check_stateful_node_list.cmake")
endif()
if(NOT INCLUDE_DIR)
    set(INCLUDE_DIR "${SOURCE_DIR}/include")
endif()
if(NOT TEST_FILE)
    set(TEST_FILE "${SOURCE_DIR}/tests/test_engine_stateful_discovery.cpp")
endif()
set(test "TEST(StatefulDiscovery, CastWorksOnEveryStatefulNodeType)")

# A file's text less its // comments, so that a class or a call that only a comment names is not one.
function(read_code file out)
    file(READ "${file}" text)
    string(REGEX REPLACE "//[^\n]*" "" text "${text}")
    set(${out} "${text}" PARENT_SCOPE)
endfunction()

# ── the classes that derive StatefulNodeBase ─────────────────────────────────
file(GLOB headers "${INCLUDE_DIR}/flywheel/*.hpp")
list(SORT headers)
set(classes "")
foreach(header IN LISTS headers)
    read_code("${header}" code)
    string(REGEX MATCHALL "public[ \t\r\n]+StatefulNodeBase[ \t\r\n]*<[ \t\r\n]*[A-Za-z_][A-Za-z0-9_]*" bases "${code}")
    foreach(base IN LISTS bases)
        string(REGEX REPLACE "^.*<[ \t\r\n]*" "" class "${base}")
        list(APPEND classes "${class}")
    endforeach()
endforeach()
list(REMOVE_DUPLICATES classes)
list(SORT classes)
list(LENGTH classes class_count)
if(class_count EQUAL 0)
    message(FATAL_ERROR "found no class that derives StatefulNodeBase in a header under ${INCLUDE_DIR}/flywheel")
endif()

# ── the body of the test ─────────────────────────────────────────────────────
if(NOT EXISTS "${TEST_FILE}")
    message(FATAL_ERROR "${TEST_FILE} does not exist")
endif()
read_code("${TEST_FILE}" test_code)
string(FIND "${test_code}" "${test}" at)
if(at EQUAL -1)
    message(FATAL_ERROR "found no ${test} in ${TEST_FILE}")
endif()
string(SUBSTRING "${test_code}" ${at} -1 body)
# The body ends at the first closing brace in column 0.
string(FIND "${body}" "\n}" end)
if(end EQUAL -1)
    message(FATAL_ERROR "${test} in ${TEST_FILE} has no closing brace in column 0")
endif()
string(SUBSTRING "${body}" 0 ${end} body)

set(missing "")
foreach(class IN LISTS classes)
    if(NOT body MATCHES "(^|[^A-Za-z0-9_])${class}[ \t\r\n]*(<[^>;]*>)?[ \t\r\n]*::[ \t\r\n]*make[ \t\r\n]*\\(")
        list(APPEND missing "${class}")
    endif()
endforeach()

if(missing)
    list(LENGTH missing missing_count)
    list(JOIN missing ", " missing_text)
    message(FATAL_ERROR "${test} does not build ${missing_count} of the ${class_count} classes that derive "
                        "StatefulNodeBase: ${missing_text}. Give each a <Class>::make(...) row in ${TEST_FILE}")
endif()
message(STATUS "${test} builds all ${class_count} classes that derive StatefulNodeBase")
