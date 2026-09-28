# check_warning_flags.cmake — test_warning_flags: every translation unit of
# this project compiles with the warning flags, and no dependency's does
# (flywheel-dag#4).
#
# Run by ctest as
#
#   cmake -DCOMPILE_COMMANDS=<build>/compile_commands.json -DSOURCE_DIR=<source>
#         -DBINARY_DIR=<build> [-DWARNINGS_AS_ERRORS=ON] -P check_warning_flags.cmake
#
# and registered only where the root CMakeLists.txt sets the flags (the top
# level, GCC or Clang) under a generator that writes compile_commands.json.
# A warning gate that silently stops applying still passes CI, which is why the
# gate itself is tested.
#
# A translation unit under SOURCE_DIR but outside BINARY_DIR is this project's.
# Anything else, such as a FetchContent dependency under <build>/_deps, is a
# dependency's. With WARNINGS_AS_ERRORS, every translation unit of this project
# must also have -Werror, and no dependency's may: there, a warning of the
# dependency's own would fail this project's build. No dependency sets
# -Wpedantic for itself, so finding it on one means the flags leaked.

cmake_minimum_required(VERSION 3.19)   # string(JSON)

foreach(var IN ITEMS COMPILE_COMMANDS SOURCE_DIR BINARY_DIR)
    if(NOT ${var})
        message(FATAL_ERROR "usage: cmake -DCOMPILE_COMMANDS=<file> -DSOURCE_DIR=<dir> "
                            "-DBINARY_DIR=<dir> [-DWARNINGS_AS_ERRORS=ON] "
                            "-P check_warning_flags.cmake")
    endif()
endforeach()

set(required -Wall -Wextra -Wpedantic)
if(WARNINGS_AS_ERRORS)
    list(APPEND required -Werror)
endif()
set(forbidden_in_dependencies -Wpedantic -Werror)

file(READ "${COMPILE_COMMANDS}" db)
string(JSON count LENGTH "${db}")
if(count EQUAL 0)
    message(FATAL_ERROR "${COMPILE_COMMANDS} lists no translation units")
endif()
math(EXPR last "${count} - 1")

set(ours 0)
set(theirs 0)
set(dirs "")
set(missing "")
set(leaked "")
foreach(i RANGE ${last})
    string(JSON file GET "${db}" ${i} file)
    string(JSON command GET "${db}" ${i} command)
    separate_arguments(args UNIX_COMMAND "${command}")

    string(FIND "${file}" "${SOURCE_DIR}/" in_source)
    string(FIND "${file}" "${BINARY_DIR}/" in_build)
    if(in_source EQUAL 0 AND NOT in_build EQUAL 0)
        math(EXPR ours "${ours} + 1")
        file(RELATIVE_PATH rel "${SOURCE_DIR}" "${file}")
        string(REGEX REPLACE "/.*" "" top "${rel}")
        list(APPEND dirs "${top}")
        foreach(flag IN LISTS required)
            if(NOT flag IN_LIST args)
                list(APPEND missing "${rel}: ${flag}")
            endif()
        endforeach()
    else()
        math(EXPR theirs "${theirs} + 1")
        foreach(flag IN LISTS forbidden_in_dependencies)
            if(flag IN_LIST args)
                list(APPEND leaked "${file}: ${flag}")
            endif()
        endforeach()
    endif()
endforeach()

# A wrong path or a new file layout must not pass by scanning nothing.
foreach(dir IN ITEMS tests examples benchmarks)
    if(NOT dir IN_LIST dirs)
        message(FATAL_ERROR "${COMPILE_COMMANDS} has no translation unit under "
                            "${SOURCE_DIR}/${dir}/")
    endif()
endforeach()

if(missing)
    list(LENGTH missing n)
    list(JOIN missing "\n  " report)
    message(FATAL_ERROR "${n} warning flags missing from this project's "
                        "translation units:\n  ${report}")
endif()
if(leaked)
    list(LENGTH leaked n)
    list(JOIN leaked "\n  " report)
    message(FATAL_ERROR "${n} of this project's warning flags found on dependency "
                        "translation units:\n  ${report}")
endif()

list(JOIN required " " flags)
list(JOIN forbidden_in_dependencies " " unwanted)
message(STATUS "${ours} translation units of this project compile with ${flags}; "
               "none of the ${theirs} of its dependencies' has ${unwanted}")
