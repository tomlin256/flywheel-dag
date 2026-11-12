# check_headers_alone.cmake — test_headers_alone and clang_tidy_headers_alone: every public header compiles as the only
# include of a unit.
#
# Run by ctest in one of two ways, with a compiler or with clang-tidy:
#
#   cmake -DCXX=<compiler> -DINCLUDE_DIR=<source>/include "-DDEPENDENCY_INCLUDE_DIRS=<dir>|<dir>" -DWORK_DIR=<dir>
#         -P check_headers_alone.cmake
#   cmake -DCLANG_TIDY=<tool> -DCONFIG_FILE=<source>/.clang-tidy ["-DCLANG_TIDY_ARGS=<arg>|<arg>"] [-DLIBCXX_MAJOR=<major>]
#         -DINCLUDE_DIR=<source>/include "-DDEPENDENCY_INCLUDE_DIRS=<dir>|<dir>" -DWORK_DIR=<dir>
#         -P check_headers_alone.cmake
#
# The suite includes the engine's headers in its own order, so nothing else would notice a header that needs another to
# come first, or one that uses a name of the standard library whose header it does not include. Each header gets a unit
# of its own that includes nothing else, compiled at C++23, and every one must compile. The glob is the list of headers,
# so a header added later is covered, and finding none is an error, so a wrong path cannot pass by checking nothing.
# DEPENDENCY_INCLUDE_DIRS holds the include directories of the libraries the headers include, bar-separated.
#
# What a compile can show depends on the standard library it meets. A library whose headers include one another hides a
# missing #include, and one that keeps them apart shows it. So there are two callers:
#
#   test_headers_alone        the tree's compiler, -fsyntax-only, in every tree. It sees a header that leans on another
#                             engine header, and whatever the tree's library does not hide.
#   clang_tidy_headers_alone  clang-tidy, with .clang-tidy and the flags after "--". A unit run that way is compiled
#                             against the standard library that the tool's own LLVM carries, where it carries one, and
#                             not against the library of the compiler that a compile database names. Homebrew's LLVM
#                             carries its libc++, which keeps its headers apart. With LIBCXX_MAJOR, which the gate
#                             passes on an Apple host, a control unit must find that _LIBCPP_VERSION is of that major:
#                             without it the test could meet the SDK's library and pass for nothing.
#
# Both define _LIBCPP_REMOVE_TRANSITIVE_INCLUDES, which libc++ reads and libstdc++ ignores, and both run a control that
# must not compile, a unit that names std::string and includes nothing: a caller that did not read the exit status would
# pass every header.
#
# INCLUDE_DIR is a parameter so that a hand-made change can be made to a copy of the headers and must fail here.

cmake_minimum_required(VERSION 4.0)

set(usage "usage: cmake (-DCXX=<compiler> | -DCLANG_TIDY=<tool> -DCONFIG_FILE=<file> [-DCLANG_TIDY_ARGS=<arg>|<arg>] "
          "[-DLIBCXX_MAJOR=<major>]) -DINCLUDE_DIR=<dir> -DDEPENDENCY_INCLUDE_DIRS=<dir>|<dir> -DWORK_DIR=<dir> "
          "-P check_headers_alone.cmake")
foreach(var IN ITEMS INCLUDE_DIR DEPENDENCY_INCLUDE_DIRS WORK_DIR)
    if(NOT ${var})
        message(FATAL_ERROR ${usage})
    endif()
endforeach()
if((CXX AND CLANG_TIDY) OR (NOT CXX AND NOT CLANG_TIDY) OR (CLANG_TIDY AND NOT CONFIG_FILE))
    message(FATAL_ERROR ${usage})
endif()

file(GLOB headers RELATIVE "${INCLUDE_DIR}" "${INCLUDE_DIR}/flywheel/*.hpp")
list(SORT headers)
list(LENGTH headers count)
if(count EQUAL 0)
    message(FATAL_ERROR "found no header under ${INCLUDE_DIR}/flywheel")
endif()

# SPDLOG_COMPILED_LIB is what a target that links spdlog::spdlog defines.
set(flags -std=c++23 -D_LIBCPP_REMOVE_TRANSITIVE_INCLUDES -DSPDLOG_COMPILED_LIB "-I${INCLUDE_DIR}")
string(REPLACE "|" ";" dependency_dirs "${DEPENDENCY_INCLUDE_DIRS}")
foreach(dir IN LISTS dependency_dirs)
    list(APPEND flags "-I${dir}")
endforeach()

if(CXX)
    set(with "${CXX}")
    set(before "${CXX}" -fsyntax-only ${flags})
    set(after "")
else()
    set(with "${CLANG_TIDY}")
    string(REPLACE "|" ";" tool_args "${CLANG_TIDY_ARGS}")
    set(before "${CLANG_TIDY}" "--config-file=${CONFIG_FILE}" --quiet ${tool_args})
    set(after -- ${flags})
endif()

# compile(<result> <output> <unit>): the exit status of the caller's command on <unit>, and what it printed.
function(compile result_var output_var unit)
    execute_process(COMMAND ${before} "${unit}" ${after}
                    RESULT_VARIABLE result OUTPUT_VARIABLE out ERROR_VARIABLE err)
    set(${result_var} "${result}" PARENT_SCOPE)
    set(${output_var} "${out}${err}" PARENT_SCOPE)
endfunction()

file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

# ── the controls ─────────────────────────────────────────────────────────────
file(WRITE "${WORK_DIR}/control_must_not_compile.cpp" "std::string control;\n")
compile(result output "${WORK_DIR}/control_must_not_compile.cpp")
if(NOT result MATCHES "^[0-9]+$")
    message(FATAL_ERROR "${with} cannot be run: ${result}")
endif()
if(result EQUAL 0)
    message(FATAL_ERROR "the control compiled: a unit that names std::string and includes nothing passed ${with}, so a "
                        "header that lacks an #include would pass too")
endif()

set(library "")
if(CLANG_TIDY AND LIBCXX_MAJOR)
    file(WRITE "${WORK_DIR}/control_library.cpp"
         "#include <version>\n"
         "#if !defined(_LIBCPP_VERSION) || _LIBCPP_VERSION / 10000 != ${LIBCXX_MAJOR}\n"
         "#error \"this is not the libc++ of LLVM ${LIBCXX_MAJOR}\"\n"
         "#endif\n")
    compile(result output "${WORK_DIR}/control_library.cpp")
    if(NOT result EQUAL 0)
        string(SUBSTRING "${output}" 0 600 shown)
        message(FATAL_ERROR "the units were not compiled against the tool's libc++, LLVM ${LIBCXX_MAJOR}'s: a unit given "
                            "to ${with} with its flags after \"--\" met another standard library, which may hide a "
                            "missing #include:\n${shown}")
    endif()
    set(library ", against the libc++ of LLVM ${LIBCXX_MAJOR}")
endif()

# ── each header ──────────────────────────────────────────────────────────────
set(failed 0)
set(problems "")
foreach(header IN LISTS headers)
    get_filename_component(stem "${header}" NAME_WE)
    set(unit "${WORK_DIR}/${stem}.cpp")
    file(WRITE "${unit}" "#include <${header}>\n")
    compile(result output "${unit}")
    if(NOT result EQUAL 0)
        math(EXPR failed "${failed} + 1")
        string(SUBSTRING "${output}" 0 600 shown)
        string(APPEND problems "  ${header} does not compile alone:\n${shown}\n")
    endif()
endforeach()

if(failed GREATER 0)
    message(FATAL_ERROR "${failed} of ${count} headers do not compile as the only include of a unit, with "
                        "${with}${library}:\n${problems}")
endif()
message(STATUS "all ${count} headers compile as the only include of a unit, with ${with}${library}")
