# check_claude_md_budget.cmake — test_claude_md_budget: the files Claude Code loads stay
# small.
#
# Run by ctest as
#
#   cmake -DSOURCE_DIR=<source> -DCLAUDE_MD_BYTES=<n> -DRULES_BYTES=<n> -DNESTED_CHARS=<n>
#         -P check_claude_md_budget.cmake
#
# CLAUDE.md is in the context of every session in this repo, and so is docs/engine_rules.md,
# which it imports, and so are they in any project that imports the rules. Every byte is
# paid on every session, and a file that nothing holds to a size grows by a paragraph per
# plan. A CLAUDE.md in a subdirectory loads on demand, and Claude Code keeps only the start
# of one past 40,000 characters, leaving the rest to a Read call that nobody thinks of.
#
# Three things are held:
#
#   CLAUDE_MD_BYTES   CLAUDE.md at the top is at most this many bytes.
#   RULES_BYTES       docs/engine_rules.md is at most this many bytes.
#   NESTED_CHARS      no CLAUDE.md anywhere in the checkout is over this many characters.
#
# The two budgets are bytes, which is what the context pays and what `wc -c` prints. The cap
# is characters, which is what Claude Code counts and `wc -m` prints in a UTF-8 locale: a
# dash is one character and three bytes. CMake's string(LENGTH) counts bytes, so the
# characters are the bytes less the continuation bytes (0x80 to 0xBF) of UTF-8. The numbers
# are arguments so that the fixtures in tests/claude_md_budget/ can use small ones; the real
# ones are written once, where tests/CMakeLists.txt registers test_claude_md_budget.
#
# A file that is missing is a problem, not a pass: a check that found nothing to measure
# would hold nothing. build*/, .git/, .claude/ and any node_modules/ hold other projects'
# files and are skipped, as check_cmake_floor.cmake skips the first two.

cmake_minimum_required(VERSION 4.0)

set(usage "usage: cmake -DSOURCE_DIR=<dir> -DCLAUDE_MD_BYTES=<n> -DRULES_BYTES=<n> "
          "-DNESTED_CHARS=<n> -P check_claude_md_budget.cmake")
if(NOT SOURCE_DIR)
    message(FATAL_ERROR ${usage})
endif()
foreach(limit IN ITEMS CLAUDE_MD_BYTES RULES_BYTES NESTED_CHARS)
    if(NOT "${${limit}}" MATCHES "^[1-9][0-9]*$")
        message(FATAL_ERROR "${limit} is '${${limit}}', not a positive number\n${usage}")
    endif()
endforeach()

# The characters in a UTF-8 file: its bytes less its continuation bytes.
function(characters_in file out)
    file(READ "${file}" hex HEX)
    string(LENGTH "${hex}" hex_length)
    math(EXPR bytes "${hex_length} / 2")
    string(REGEX MATCHALL "[0-9a-f][0-9a-f]" each_byte "${hex}")
    list(FILTER each_byte INCLUDE REGEX "^[89ab]")
    list(LENGTH each_byte continuation)
    math(EXPR characters "${bytes} - ${continuation}")
    set(${out} "${characters}" PARENT_SCOPE)
endfunction()

set(problems "")

# ── the two files every session loads ────────────────────────────────────────
set(claude_md_bytes "no file")
if(EXISTS "${SOURCE_DIR}/CLAUDE.md")
    file(SIZE "${SOURCE_DIR}/CLAUDE.md" claude_md_bytes)
    if(claude_md_bytes GREATER CLAUDE_MD_BYTES)
        string(APPEND problems
               "  CLAUDE.md is ${claude_md_bytes} bytes, over its budget of ${CLAUDE_MD_BYTES}\n")
    endif()
else()
    string(APPEND problems "  CLAUDE.md is missing\n")
endif()

set(rules_bytes "no file")
if(EXISTS "${SOURCE_DIR}/docs/engine_rules.md")
    file(SIZE "${SOURCE_DIR}/docs/engine_rules.md" rules_bytes)
    if(rules_bytes GREATER RULES_BYTES)
        string(APPEND problems
               "  docs/engine_rules.md is ${rules_bytes} bytes, over its budget of ${RULES_BYTES}\n")
    endif()
else()
    string(APPEND problems "  docs/engine_rules.md is missing\n")
endif()

# ── every CLAUDE.md in the checkout ──────────────────────────────────────────
set(claude_files "")
if(EXISTS "${SOURCE_DIR}/CLAUDE.md")
    list(APPEND claude_files "CLAUDE.md")
endif()
file(GLOB top RELATIVE "${SOURCE_DIR}" "${SOURCE_DIR}/*")
foreach(entry IN LISTS top)
    if(NOT IS_DIRECTORY "${SOURCE_DIR}/${entry}"
       OR entry MATCHES "^(build.*|\\.git|\\.claude|node_modules)$")
        continue()
    endif()
    file(GLOB_RECURSE found RELATIVE "${SOURCE_DIR}" "${SOURCE_DIR}/${entry}/CLAUDE.md")
    foreach(path IN LISTS found)
        if(NOT path MATCHES "(^|/)node_modules/")
            list(APPEND claude_files "${path}")
        endif()
    endforeach()
endforeach()
list(SORT claude_files)

set(largest 0)
foreach(path IN LISTS claude_files)
    characters_in("${SOURCE_DIR}/${path}" characters)
    if(characters GREATER largest)
        set(largest ${characters})
    endif()
    if(characters GREATER NESTED_CHARS)
        string(APPEND problems "  ${path} is ${characters} characters, over the cap of ${NESTED_CHARS}\n")
    endif()
endforeach()
list(LENGTH claude_files claude_count)

if(NOT problems STREQUAL "")
    message(FATAL_ERROR "the files Claude Code loads must stay within budget, but:\n${problems}")
endif()
message(STATUS "CLAUDE.md is ${claude_md_bytes} of ${CLAUDE_MD_BYTES} bytes, docs/engine_rules.md "
               "${rules_bytes} of ${RULES_BYTES}; ${claude_count} CLAUDE.md file(s) found, the "
               "largest ${largest} of ${NESTED_CHARS} characters")
