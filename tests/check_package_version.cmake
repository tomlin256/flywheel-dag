# check_package_version.cmake — test_package_version: which find_package version
# requests the installed package accepts.
#
# Run by ctest as
#
#   cmake -DVERSION_FILE=<build>/flywheel_dagConfigVersion.cmake
#         -DPROJECT_VERSION=<version> -P check_package_version.cmake
#
# The version file is the one CMakeLists.txt writes and installs, and a consumer's
# find_package asks it, once per request, whether the request is compatible. The policy
# is SameMajorVersion: a request for the installed major version, at or below the
# installed version, is accepted, and nothing else. A consumer that asks for 0.3 must be
# refused by a package that needs C++23 and CMake 4.0 for the engine's own build, not
# handed one silently, and a request for a newer version than is installed must be
# refused as well.
#
# The table is derived from PROJECT_VERSION (major M, minor N, patch P):
#
#   accepted   M   M.0   M.N   M.N.P   M.N.0   and, when N > 0, M.(N-1)
#   refused    M.(N+1)   M.N.(P+1)   M+1   M+1.0   and every request for 0.3, 0.3.2 and
#              0.9, the series before the major release (while M is at least 1)
#
# CMake's own version file reads PACKAGE_FIND_VERSION and PACKAGE_FIND_VERSION_MAJOR,
# which find_package sets from the request; this sets them the same way.

cmake_minimum_required(VERSION 4.0)

foreach(var IN ITEMS VERSION_FILE PROJECT_VERSION)
    if(NOT ${var})
        message(FATAL_ERROR "usage: cmake -DVERSION_FILE=<file> -DPROJECT_VERSION=<version> "
                            "-P check_package_version.cmake")
    endif()
endforeach()
if(NOT EXISTS "${VERSION_FILE}")
    message(FATAL_ERROR "${VERSION_FILE} does not exist")
endif()
if(NOT PROJECT_VERSION MATCHES "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$")
    message(FATAL_ERROR "PROJECT_VERSION '${PROJECT_VERSION}' is not major.minor.patch")
endif()
set(M ${CMAKE_MATCH_1})
set(N ${CMAKE_MATCH_2})
set(P ${CMAKE_MATCH_3})
math(EXPR M_next "${M} + 1")
math(EXPR N_next "${N} + 1")
math(EXPR P_next "${P} + 1")

set(accepted "${M}" "${M}.0" "${M}.${N}" "${M}.${N}.${P}" "${M}.${N}.0")
if(N GREATER 0)
    math(EXPR N_prev "${N} - 1")
    list(APPEND accepted "${M}.${N_prev}")
endif()
set(refused "${M}.${N_next}" "${M}.${N}.${P_next}" "${M_next}" "${M_next}.0")
if(M GREATER 0)
    list(APPEND refused "0.3" "0.3.2" "0.9")
endif()
list(REMOVE_DUPLICATES accepted)
list(REMOVE_DUPLICATES refused)

# Ask the version file about one request: TRUE if it says compatible.
function(compatible request out)
    # Not string(REGEX REPLACE "^...") to peel the request apart: CMake restarts the match
    # at the end of each replacement, so a "^" strips every leading number, not the first.
    if(NOT request MATCHES "^([0-9]+)(\\.([0-9]+))?(\\.([0-9]+))?$")
        message(FATAL_ERROR "cannot read the request '${request}'")
    endif()
    set(PACKAGE_FIND_VERSION "${request}")
    set(PACKAGE_FIND_VERSION_MAJOR "${CMAKE_MATCH_1}")
    set(PACKAGE_FIND_VERSION_MINOR "${CMAKE_MATCH_3}")
    set(PACKAGE_FIND_VERSION_PATCH "${CMAKE_MATCH_5}")
    unset(PACKAGE_FIND_VERSION_RANGE)
    unset(PACKAGE_VERSION_COMPATIBLE)
    include("${VERSION_FILE}")
    if(PACKAGE_VERSION_COMPATIBLE)
        set(${out} TRUE PARENT_SCOPE)
    else()
        set(${out} FALSE PARENT_SCOPE)
    endif()
endfunction()

set(problems "")
foreach(request IN LISTS accepted)
    compatible("${request}" ok)
    if(NOT ok)
        string(APPEND problems "  a request for ${request} is refused, and should be accepted\n")
    endif()
endforeach()
foreach(request IN LISTS refused)
    compatible("${request}" ok)
    if(ok)
        string(APPEND problems "  a request for ${request} is accepted, and should be refused\n")
    endif()
endforeach()

if(NOT problems STREQUAL "")
    message(FATAL_ERROR "${VERSION_FILE} (version ${PROJECT_VERSION}) answers wrongly:\n${problems}")
endif()
list(LENGTH accepted n_accepted)
list(LENGTH refused n_refused)
message(STATUS "version ${PROJECT_VERSION} accepts ${n_accepted} requests and refuses ${n_refused}, as SameMajorVersion should")
