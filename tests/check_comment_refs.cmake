# check_comment_refs.cmake — test_comment_refs: a comment states what is true of the code
# now, so nothing the engine ships cites an issue that is not open or points into the plans.
#
# Run by ctest as
#
#   cmake -DSOURCE_DIR=<source> -DOPEN_ISSUES=<n>[,<n>...] -P check_comment_refs.cmake
#
# History lives in the commit, the issue and the plan. A comment that cites a closed issue,
# or points at a plan, tells how the code got here, and the citation outlives the reason for
# it. An open issue is cited only where the comment states the limitation it tracks.
#
# Three things are held in every file under include/, tests/, benchmarks/, examples/,
# cmake/ and .github/, and in every file at the top that is not a document (*.md):
#
#   references  each reference to an issue of this repository, written with a hash and a
#               number or as an address under issues/ or pull/, names a number in
#               OPEN_ISSUES.
#   plans       nothing points into the directory of the plans.
#   the list    each number in OPEN_ISSUES is cited by some file, so the list shrinks with
#               the last comment that states the limitation.
#
# OPEN_ISSUES is empty or numbers separated by commas. The real list is written once, where
# tests/CMakeLists.txt registers the test; the fixtures in tests/comment_refs/ use their
# own, and the scan skips that directory because its files cite issues on purpose. A tree
# with no file to read is a problem, not a pass: a check that read nothing would hold
# nothing.
#
# The patterns are written so that this file does not match them, and it quotes no
# reference, so the check holds the script too.

cmake_minimum_required(VERSION 4.0)

set(usage "usage: cmake -DSOURCE_DIR=<dir> -DOPEN_ISSUES=<n>[,<n>...] -P check_comment_refs.cmake")
if(NOT SOURCE_DIR)
    message(FATAL_ERROR "SOURCE_DIR is not set\n${usage}")
endif()
if(NOT DEFINED OPEN_ISSUES)
    message(FATAL_ERROR "OPEN_ISSUES is not set: name the open issues the code may cite, "
                        "or pass -DOPEN_ISSUES= for none\n${usage}")
endif()
if(NOT "${OPEN_ISSUES}" MATCHES "^([1-9][0-9]*(,[1-9][0-9]*)*)?$")
    message(FATAL_ERROR "OPEN_ISSUES is '${OPEN_ISSUES}', not issue numbers separated by commas\n${usage}")
endif()
string(REPLACE "," ";" open "${OPEN_ISSUES}")
if(OPEN_ISSUES STREQUAL "")
    set(open_text "none")
else()
    string(REPLACE "," ", " open_text "${OPEN_ISSUES}")
endif()

# A reference to an issue, in either way of writing it, and a pointer into the plans.
set(ref_regex "flywheel-dag(#|/issues/|/pull/)[0-9]+")
set(plan_regex "docs/[p]lans")

# hits(<out> <text> <regex>): "<line>:<match>" for each match of the regex in the text, in
# order. The text is matched whole because it holds `;` and `[`, which a list of its lines
# would split.
function(hits out text regex)
    string(REGEX MATCHALL "${regex}" matches "${text}")
    set(rest "${text}")
    set(line 1)
    set(found "")
    foreach(match IN LISTS matches)
        string(FIND "${rest}" "${match}" at)
        string(SUBSTRING "${rest}" 0 ${at} before)
        string(REGEX MATCHALL "\n" breaks "${before}")
        list(LENGTH breaks newlines)
        math(EXPR line "${line} + ${newlines}")
        list(APPEND found "${line}:${match}")
        string(LENGTH "${match}" length)
        math(EXPR from "${at} + ${length}")
        string(SUBSTRING "${rest}" ${from} -1 rest)
    endforeach()
    set(${out} "${found}" PARENT_SCOPE)
endfunction()

# ── what is read ─────────────────────────────────────────────────────────────
set(roots include tests benchmarks examples cmake .github)
set(files "")
foreach(root IN LISTS roots)
    file(GLOB_RECURSE found RELATIVE "${SOURCE_DIR}" "${SOURCE_DIR}/${root}/*")
    list(APPEND files ${found})
endforeach()
file(GLOB top RELATIVE "${SOURCE_DIR}" "${SOURCE_DIR}/*")
foreach(entry IN LISTS top)
    if(NOT IS_DIRECTORY "${SOURCE_DIR}/${entry}" AND NOT entry MATCHES "\\.md$")
        list(APPEND files "${entry}")
    endif()
endforeach()
list(FILTER files EXCLUDE REGEX "^tests/comment_refs/")
list(SORT files)
list(LENGTH files file_count)

set(problems "")
if(file_count EQUAL 0)
    string(APPEND problems "  nothing to scan under ${SOURCE_DIR}\n")
endif()

# ── the references and the plans ─────────────────────────────────────────────
set(cited "")
foreach(file IN LISTS files)
    file(READ "${SOURCE_DIR}/${file}" text)

    hits(ref_hits "${text}" "${ref_regex}")
    foreach(hit IN LISTS ref_hits)
        string(REGEX MATCH "^([0-9]+):(.*)$" unused "${hit}")
        set(line "${CMAKE_MATCH_1}")
        set(ref "${CMAKE_MATCH_2}")
        string(REGEX MATCH "[0-9]+$" number "${ref}")
        list(FIND open "${number}" known)
        if(known EQUAL -1)
            string(APPEND problems
                   "  ${file}:${line} cites ${ref}, which is not in OPEN_ISSUES (${open_text})\n")
        else()
            list(APPEND cited "${number}")
        endif()
    endforeach()

    hits(plan_hits "${text}" "${plan_regex}")
    foreach(hit IN LISTS plan_hits)
        string(REGEX MATCH "^([0-9]+):" unused "${hit}")
        string(APPEND problems "  ${file}:${CMAKE_MATCH_1} points into the plans directory\n")
    endforeach()
endforeach()

# ── the list ─────────────────────────────────────────────────────────────────
list(REMOVE_DUPLICATES cited)
foreach(number IN LISTS open)
    list(FIND cited "${number}" known)
    if(known EQUAL -1)
        string(APPEND problems "  flywheel-dag#${number} is in OPEN_ISSUES and no file cites it\n")
    endif()
endforeach()
list(LENGTH cited cited_count)

if(NOT problems STREQUAL "")
    message(FATAL_ERROR "comments must state the present, but:\n${problems}"
            "A comment states what is true of the code now. History lives in the commit, the "
            "issue and the plan, so a comment cites no closed issue and points at no plan. An "
            "open issue is cited only where the comment states the limitation it tracks, and "
            "its number is then in OPEN_ISSUES, where tests/CMakeLists.txt registers "
            "test_comment_refs.")
endif()
message(STATUS "${file_count} files cite no closed issue and point into no plan; "
               "${cited_count} open issue(s) cited")
