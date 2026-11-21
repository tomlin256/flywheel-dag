# check_non_target_directives.cmake — test_non_target_directives: no CMake file of the checkout calls a non-target directive.
#
# Run by ctest as
#
#   cmake -DSOURCE_DIR=<source> -DMIN_FILES=<n> -P check_non_target_directives.cmake
#
# CMake 4.5 reports seven commands as non-target directives (the category non-target-directive of cmake-diagnostics(7), in the
# strict group of the author diagnostics): add_compile_definitions, add_compile_options, add_definitions, add_link_options,
# include_directories, link_directories and link_libraries. Each affects every target of a directory when a target command can
# name the target, and the strict configure that CI runs (-Werror=author) makes each report an error. A CMake before 4.5 reports
# nothing, so a configure on such a host cannot show one, and this reads the code instead: a call of any of the seven, in any
# CMake file, is refused, and the refusal names file:line and the command. The option, the definition, the directory or the library
# goes to a target (target_compile_options and the rest), or, for a whole directory tree, to flywheel_dag_add_target_options
# (cmake/flywheel_dag_target_options.cmake).
#
# It reads the code, comments dropped, of every CMakeLists.txt, *.cmake and *.cmake.in in the checkout, except a build tree
# (build*/ and cmake-build-*/, which hold the dependencies' files, and are ignored by git) and .git/. A call is a command name,
# in any case, followed by an open parenthesis or by the end of its line, and not part of a longer name: target_link_libraries
# is not link_libraries. A name inside a string, followed by a parenthesis, is a hit too: reword the string. Two files are not
# read, because each holds the seven names as text that it writes into a project to see this script refuse it, and a script that
# runs under cmake -P cannot call them: this one, and tests/check_non_target_directives_fixtures.cmake
# (test_non_target_directives_fixtures, which holds this script's own behaviour).
#
# It fails too when it reads fewer than MIN_FILES files, so that a moved or renamed layout cannot pass by reading nothing: the
# real run asks for three, and the checkout has dozens.
#
# SOURCE_DIR is a parameter so that a hand-made change can be made to a scratch copy of the repo and must fail here.

cmake_minimum_required(VERSION 4.0)

set(usage "usage: cmake -DSOURCE_DIR=<dir> -DMIN_FILES=<n> -P check_non_target_directives.cmake")
if(NOT SOURCE_DIR)
    message(FATAL_ERROR "SOURCE_DIR is not set\n${usage}")
endif()
if(NOT "${MIN_FILES}" MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR "MIN_FILES is '${MIN_FILES}', not a count of at least one\n${usage}")
endif()

set(directives
    add_compile_definitions
    add_compile_options
    add_definitions
    add_link_options
    include_directories
    link_directories
    link_libraries)
set(not_read tests/check_non_target_directives.cmake tests/check_non_target_directives_fixtures.cmake)

# A command name that is not the tail of a longer one, then an open parenthesis, or nothing more on its line.
string(JOIN "|" alternatives ${directives})
set(call_regex "(^|[^a-z0-9_])(${alternatives})[ \t]*(\\(|$)")

# ── the CMake files of the checkout ──────────────────────────────────────────
file(GLOB top RELATIVE "${SOURCE_DIR}" "${SOURCE_DIR}/*")
set(files "")
foreach(entry IN LISTS top)
    if(entry MATCHES "^(build.*|cmake-build-.*|\\.git)$")
        continue()
    endif()
    if(IS_DIRECTORY "${SOURCE_DIR}/${entry}")
        file(GLOB_RECURSE found RELATIVE "${SOURCE_DIR}"
             "${SOURCE_DIR}/${entry}/CMakeLists.txt" "${SOURCE_DIR}/${entry}/*.cmake" "${SOURCE_DIR}/${entry}/*.cmake.in")
        list(APPEND files ${found})
    elseif(entry STREQUAL "CMakeLists.txt" OR entry MATCHES "\\.cmake(\\.in)?$")
        list(APPEND files "${entry}")
    endif()
endforeach()
list(REMOVE_ITEM files ${not_read})
list(SORT files)
list(LENGTH files file_count)

# ── each call ────────────────────────────────────────────────────────────────
set(problems "")
foreach(file IN LISTS files)
    file(READ "${SOURCE_DIR}/${file}" text)
    # Comments go: a whole line, and the rest of a line after a space and a #. The line breaks stay, so a line keeps its number.
    string(REGEX REPLACE "(^|\n)[ \t]*#[^\n]*" "\\1" code "${text}")
    string(REGEX REPLACE "[ \t]#[^\n]*" "" code "${code}")
    # CMake reads a command in any case. The characters that a CMake list gives meaning to go, so that a line can be an item of
    # one, and each line starts with a bar, so that no line is an empty item.
    string(TOLOWER "${code}" code)
    string(REPLACE ";" "<semicolon>" code "${code}")
    string(REPLACE "[" "<lbracket>" code "${code}")
    string(REPLACE "]" "<rbracket>" code "${code}")
    string(REPLACE "\n" ";|" lines "|${code}")
    set(number 0)
    foreach(line IN LISTS lines)
        math(EXPR number "${number} + 1")
        if(line MATCHES "${call_regex}")
            string(APPEND problems "  ${file}:${number}: ${CMAKE_MATCH_2}\n")
        endif()
    endforeach()
endforeach()

if(file_count LESS MIN_FILES)
    string(APPEND problems "  read ${file_count} CMake files, and this check expects at least ${MIN_FILES}: "
                           "it may have read nothing\n")
endif()

if(NOT problems STREQUAL "")
    message(FATAL_ERROR "non-target directives in ${SOURCE_DIR}, which CMake 4.5 reports and a strict configure refuses:\n"
                        "${problems}"
                        "Give the option, definition, directory or library to a target: target_compile_options, "
                        "target_compile_definitions, target_link_options, target_include_directories, target_link_directories or "
                        "target_link_libraries, or flywheel_dag_add_target_options for every target of a directory tree.")
endif()
message(STATUS "non-target directives: none in ${file_count} CMake files")
