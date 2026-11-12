# check_fetch_declarations.cmake — test_fetch_declarations: every dependency that the checkout declares is fetched small
# and pinned.
#
# Run by ctest as `cmake -DSOURCE_DIR=<repo> -P check_fetch_declarations.cmake`. It reads the code, comments dropped, of
# every CMakeLists.txt and *.cmake in the checkout outside a build tree, itself excepted, and each call that declares a
# dependency to FetchContent must be one of three things:
#
#   - a SOURCE_DIR, with no GIT_REPOSITORY and no URL: it fetches nothing;
#   - a GIT_REPOSITORY with GIT_SHALLOW TRUE and a GIT_TAG that is not a commit. A clone at full depth brings a
#     repository's whole history to every cold configure, and a shallow clone can name a tag or a branch but not a
#     40-digit commit;
#   - a URL with a URL_HASH of SHA256= and 64 hex digits. An archive is pinned by its bytes, and one with no hash is
#     whatever its address serves that day.
#
# A shallow clone is not always the small fetch: CMake's fetches the tip of every branch and tag, so a repository whose
# tips are large is fetched as its release archive. The root CMakeLists.txt says which dependency is which.
#
# It fails too when the root CMakeLists.txt declares fewer than three dependencies that it fetches, so that a moved or
# rewritten block cannot pass by reading nothing.
#
# A build tree holds other projects' files, so build*/ and cmake-build-*/ (both ignored by git) are skipped, and so is
# .git/.
#
# SOURCE_DIR is a parameter so that a hand-made change can be made to a scratch copy of the repo and must fail here.

cmake_minimum_required(VERSION 4.0)

if(NOT DEFINED SOURCE_DIR)
    message(FATAL_ERROR "usage: cmake -DSOURCE_DIR=<repo> -P check_fetch_declarations.cmake")
endif()

set(self "tests/check_fetch_declarations.cmake")
set(command_name "FetchContent_Declare")
set(root_minimum 3)

# ── the CMake files of the checkout ──────────────────────────────────────────
file(GLOB top RELATIVE "${SOURCE_DIR}" "${SOURCE_DIR}/*")
set(files "")
foreach(entry IN LISTS top)
    if(entry MATCHES "^(build.*|cmake-build-.*|\\.git)$")
        continue()
    endif()
    if(IS_DIRECTORY "${SOURCE_DIR}/${entry}")
        file(GLOB_RECURSE found RELATIVE "${SOURCE_DIR}"
             "${SOURCE_DIR}/${entry}/CMakeLists.txt" "${SOURCE_DIR}/${entry}/*.cmake")
        list(APPEND files ${found})
    elseif(entry STREQUAL "CMakeLists.txt" OR entry MATCHES "\\.cmake$")
        list(APPEND files "${entry}")
    endif()
endforeach()
list(REMOVE_ITEM files "${self}")
list(SORT files)
if(NOT "CMakeLists.txt" IN_LIST files)
    message(FATAL_ERROR "${SOURCE_DIR} has no CMakeLists.txt")
endif()

# The value after <keyword> in a declaration's words, or empty when the keyword is absent or last.
function(value_after out words keyword)
    set(${out} "" PARENT_SCOPE)
    list(FIND words "${keyword}" at)
    if(at EQUAL -1)
        return()
    endif()
    math(EXPR next "${at} + 1")
    list(LENGTH words length)
    if(next LESS length)
        list(GET words ${next} value)
        set(${out} "${value}" PARENT_SCOPE)
    endif()
endfunction()

# ── each declaration ─────────────────────────────────────────────────────────
set(problems "")
set(declarations 0)
set(shallow 0)
set(archives 0)
set(local 0)
set(root_fetched 0)
foreach(file IN LISTS files)
    file(READ "${SOURCE_DIR}/${file}" text)
    # Comments go: a whole line, and the rest of a line after a space and a #. Then the characters that a CMake list gives
    # meaning to, so that a declaration can be an item of one.
    string(REGEX REPLACE "(^|\n)[ \t]*#[^\n]*" "\\1" code "${text}")
    string(REGEX REPLACE "[ \t]#[^\n]*" "" code "${code}")
    string(REPLACE ";" "<semicolon>" code "${code}")
    string(REPLACE "[" "<lbracket>" code "${code}")
    string(REPLACE "]" "<rbracket>" code "${code}")

    string(REGEX MATCHALL "${command_name}[ \t\r\n]*\\([^)]*\\)" calls "${code}")
    foreach(call IN LISTS calls)
        math(EXPR declarations "${declarations} + 1")
        string(REGEX REPLACE "^${command_name}[ \t\r\n]*\\(" "" body "${call}")
        string(REGEX REPLACE "\\)$" "" body "${body}")
        string(STRIP "${body}" body)
        string(REGEX REPLACE "[ \t\r\n]+" ";" words "${body}")
        list(GET words 0 name)
        set(where "${file}: ${name}")

        if("GIT_REPOSITORY" IN_LIST words AND "URL" IN_LIST words)
            string(APPEND problems "  ${where} has a GIT_REPOSITORY and a URL, and one declaration fetches from one place\n")
        elseif("GIT_REPOSITORY" IN_LIST words)
            if(file STREQUAL "CMakeLists.txt")
                math(EXPR root_fetched "${root_fetched} + 1")
            endif()
            value_after(is_shallow "${words}" GIT_SHALLOW)
            if(NOT is_shallow STREQUAL "TRUE")
                string(APPEND problems "  ${where} is cloned at full depth: it has a GIT_REPOSITORY and no GIT_SHALLOW TRUE\n")
            endif()
            value_after(tag "${words}" GIT_TAG)
            string(LENGTH "${tag}" tag_length)
            if(tag STREQUAL "")
                string(APPEND problems "  ${where} has a GIT_REPOSITORY and no GIT_TAG, so it is not pinned\n")
            elseif(tag_length EQUAL 40 AND tag MATCHES "^[0-9a-fA-F]+$")
                string(APPEND problems "  ${where} has a commit for its GIT_TAG, which a shallow clone cannot name: pin a tag\n")
            endif()
            math(EXPR shallow "${shallow} + 1")
        elseif("URL" IN_LIST words)
            if(file STREQUAL "CMakeLists.txt")
                math(EXPR root_fetched "${root_fetched} + 1")
            endif()
            value_after(hash "${words}" URL_HASH)
            string(LENGTH "${hash}" hash_length)
            if(hash STREQUAL "")
                string(APPEND problems "  ${where} is an archive with no hash: it has a URL and no URL_HASH\n")
            elseif(NOT hash MATCHES "^SHA256=[0-9a-fA-F]+$" OR NOT hash_length EQUAL 71)
                string(APPEND problems "  ${where} has a URL_HASH that is not SHA256= and 64 hex digits: ${hash}\n")
            endif()
            math(EXPR archives "${archives} + 1")
        elseif("SOURCE_DIR" IN_LIST words)
            math(EXPR local "${local} + 1")
        else()
            string(APPEND problems "  ${where} names no source that this check knows: a GIT_REPOSITORY, a URL or a SOURCE_DIR\n")
        endif()
    endforeach()
endforeach()

if(root_fetched LESS root_minimum)
    string(APPEND problems "  the root CMakeLists.txt declares ${root_fetched} dependencies that it fetches, and this check "
                           "expects at least ${root_minimum}: it may have read nothing\n")
endif()

if(NOT problems STREQUAL "")
    message(FATAL_ERROR "the dependency declarations of ${SOURCE_DIR}:\n${problems}")
endif()
list(LENGTH files file_count)
message(STATUS "${declarations} dependency declarations in ${file_count} CMake files hold: shallow clones of a tag, ${shallow}; "
               "archives with a SHA-256, ${archives}; declarations that fetch nothing, ${local}")
