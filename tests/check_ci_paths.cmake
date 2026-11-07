# check_ci_paths.cmake — test_ci_paths: the paths that a workflow ignores are documentation that its run does not read.
#
# Run by ctest as `cmake -DSOURCE_DIR=<repo> -P check_ci_paths.cmake`. A workflow does not run for a push or a pull request
# whose changed paths all match its paths-ignore list. That is right for a file that the run does not read, and silently wrong
# for any other: an ignored file that a build or a check reads can break in a change that no run looked at. So this holds the
# lists to the checkout. It reads every .github/workflows/*.yml (and *.yaml) that has a push or a pull_request trigger, and
# fails unless:
#   - a workflow that has a paths-ignore list has one under each of its push and pull_request triggers, and the two are equal;
#   - each entry is a file name or <dir>/** and nothing else: the check does not guess what another pattern matches;
#   - each entry matches at least one file in the checkout (a misspelt entry ignores nothing);
#   - no entry covers a file that the build or the lint reads: CMakeLists.txt, CMakePresets.json, *.cmake, *.cmake.in, *.cpp,
#     *.hpp, *.inl, .clang-tidy, a file under tests/clang_tidy_canary/, or a file under .github/ (a workflow that ignored its
#     own file would not run for a change to it);
#   - in a workflow that runs more than the lint (a ctest command that does not select '^clang_tidy_', or no ctest command at
#     all), no entry covers a file that a check reads, found as ${SOURCE_DIR}/<name> in tests/**/*.cmake (CLAUDE.md,
#     docs/engine_rules.md and README.md today); a workflow whose every ctest command selects '^clang_tidy_' is the lint and
#     runs no check, so it may ignore them;
#   - some workflow has a list, so that a moved key cannot pass by reading nothing.
# A workflow may have no list: ci.yml has none, since the suite it runs reads CLAUDE.md, docs/engine_rules.md and README.md and a
# change to any of them must start a run. A workflow with neither trigger starts for nothing that a push or a pull request
# changes, so it has nothing to ignore: it is skipped, and the status line says which. The check also fails if it finds no
# workflow, none with either trigger, or no file that a check reads, for the same reason.
#
# It reads each workflow line by line and expects its layout: the triggers under a bare `on:`, two-space indentation, and a
# block list under the paths-ignore of each trigger. A workflow in another layout (`on: [push, pull_request]`, four-space
# indentation, a quoted "on" key) makes it fail and say so, and is never skipped: a workflow it cannot read is not one that
# needs no list.
#
# A build tree holds other projects' files, so build*/ and cmake-build-*/ (both ignored by git) are skipped, and so is .git/.
#
# SOURCE_DIR is a parameter so that a hand-made change can be made to a scratch copy of the repo and must fail here.

cmake_minimum_required(VERSION 4.0)

if(NOT DEFINED SOURCE_DIR)
    message(FATAL_ERROR "usage: cmake -DSOURCE_DIR=<repo> -P check_ci_paths.cmake")
endif()

# Does a rule ("dir|<dir>" or "file|<name>") cover a path (relative to the checkout)?
function(rule_covers rule path out)
    string(FIND "${rule}" "|" bar)
    string(SUBSTRING "${rule}" 0 ${bar} kind)
    math(EXPR start "${bar} + 1")
    string(SUBSTRING "${rule}" ${start} -1 base)
    set(${out} FALSE PARENT_SCOPE)
    if(kind STREQUAL "dir")
        string(LENGTH "${base}/" length)
        string(SUBSTRING "${path}" 0 ${length} head)
        if(head STREQUAL "${base}/")
            set(${out} TRUE PARENT_SCOPE)
        endif()
    elseif(path STREQUAL base)
        set(${out} TRUE PARENT_SCOPE)
    endif()
endfunction()

# How a rule reads in a message: docs/** or CLAUDE.md.
function(rule_text rule out)
    string(FIND "${rule}" "|" bar)
    string(SUBSTRING "${rule}" 0 ${bar} kind)
    math(EXPR start "${bar} + 1")
    string(SUBSTRING "${rule}" ${start} -1 base)
    if(kind STREQUAL "dir")
        set(${out} "${base}/**" PARENT_SCOPE)
    else()
        set(${out} "${base}" PARENT_SCOPE)
    endif()
endfunction()

# Does the build or the lint read this path (relative to the checkout)?
function(is_run_input path out)
    if(path MATCHES "(^|/)CMakeLists\\.txt$" OR path MATCHES "(^|/)CMakePresets\\.json$"
       OR path MATCHES "\\.(cmake|cpp|hpp|inl)$" OR path MATCHES "\\.cmake\\.in$"
       OR path STREQUAL ".clang-tidy" OR path MATCHES "^tests/clang_tidy_canary/" OR path MATCHES "^\\.github/")
        set(${out} TRUE PARENT_SCOPE)
    else()
        set(${out} FALSE PARENT_SCOPE)
    endif()
endfunction()

# The file names of some paths, as one text for a message: "ci.yml, clang-tidy.yml", or "none".
function(base_names paths out)
    set(names "")
    foreach(path IN LISTS paths)
        get_filename_component(base "${path}" NAME)
        list(APPEND names "${base}")
    endforeach()
    string(REPLACE ";" ", " text "${names}")
    if("${text}" STREQUAL "")
        set(text "none")
    endif()
    set(${out} "${text}" PARENT_SCOPE)
endfunction()

# ── the lists in the workflows ───────────────────────────────────────────────
file(GLOB workflow_files "${SOURCE_DIR}/.github/workflows/*.yml" "${SOURCE_DIR}/.github/workflows/*.yaml")
list(SORT workflow_files)
if("${workflow_files}" STREQUAL "")
    message(FATAL_ERROR "found no workflow (*.yml or *.yaml) in .github/workflows of ${SOURCE_DIR}")
endif()

set(problems "")
set(filtered "")                            # the workflows with a push or a pull_request trigger, relative to the checkout
set(listed "")                              # those of them that have a list
set(unlisted "")                            # and those that have none
set(skipped "")                             # the workflows with neither trigger
set(n 0)                                    # the index of a listed workflow: its name, its entries and its kind are in
                                            # listed_name_<n>, listed_entries_<n> and listed_suite_<n>
foreach(workflow IN LISTS workflow_files)
    file(RELATIVE_PATH name "${SOURCE_DIR}" "${workflow}")
    file(READ "${workflow}" yaml)
    # The lines go into a CMake list, so keep the characters that a list gives meaning to out of them.
    string(REPLACE "\r" "" yaml "${yaml}")
    string(REPLACE ";" "<semicolon>" yaml "${yaml}")
    string(REPLACE "[" "<lbracket>" yaml "${yaml}")
    string(REPLACE "]" "<rbracket>" yaml "${yaml}")
    string(REPLACE "\n" ";" lines "${yaml}")

    set(on_layout "")                       # "block": `on:` alone on its line; "inline": `on:` with its value after it
    set(in_on FALSE)
    set(trigger "")
    set(triggers "")
    set(collecting FALSE)
    set(ignore_push "")
    set(ignore_pull_request "")
    set(ctest_lines 0)
    set(ctest_more 0)                       # the ctest commands that select more than the lint's tests
    foreach(line IN LISTS lines)
        if(line MATCHES "^[ \t]*(#.*)?$")
            continue()                      # blank, or a comment
        endif()
        if(line MATCHES "ctest")
            math(EXPR ctest_lines "${ctest_lines} + 1")
            if(NOT line MATCHES "-R '\\^clang_tidy_'")
                math(EXPR ctest_more "${ctest_more} + 1")
            endif()
        endif()
        if(NOT line MATCHES "^[ \t]")       # a top-level key ends the one before it
            set(in_on FALSE)
            set(trigger "")
            set(collecting FALSE)
            if(line MATCHES "^on:[ \t]*(#.*)?$")
                set(in_on TRUE)
                set(on_layout "block")
            elseif(line MATCHES "^on:")
                set(on_layout "inline")
            endif()
            continue()
        endif()
        if(NOT in_on)
            continue()
        endif()
        if(line MATCHES "^  ([A-Za-z_]+):")  # a trigger: push, pull_request, ...
            set(trigger "${CMAKE_MATCH_1}")
            list(APPEND triggers "${trigger}")
            set(collecting FALSE)
            continue()
        endif()
        if(line MATCHES "^    paths-ignore:[ \t]*$")
            set(collecting TRUE)
            continue()
        endif()
        if(collecting AND line MATCHES "^      -[ \t]+(.*)$")
            set(entry "${CMAKE_MATCH_1}")
            string(REGEX REPLACE "[ \t]+#.*$" "" entry "${entry}")      # a comment after the entry
            string(STRIP "${entry}" entry)
            string(REGEX REPLACE "^'(.*)'$" "\\1" entry "${entry}")
            string(REGEX REPLACE "^\"(.*)\"$" "\\1" entry "${entry}")
            if(trigger STREQUAL "push")
                list(APPEND ignore_push "${entry}")
            elseif(trigger STREQUAL "pull_request")
                list(APPEND ignore_pull_request "${entry}")
            endif()
            continue()
        endif()
        set(collecting FALSE)               # any other line in a trigger ends its list
    endforeach()

    # A workflow that cannot be read in this layout is reported, and not skipped.
    if("${on_layout}" STREQUAL "")
        string(APPEND problems "  ${name} has no top-level 'on:' key, which is the layout this check reads\n")
        continue()
    elseif("${on_layout}" STREQUAL "inline")
        string(APPEND problems "  ${name} has its triggers on the line of 'on:'; this check needs the block layout, with 'on:' "
                               "alone and each trigger (push:, pull_request:) on a line of its own under it\n")
        continue()
    elseif("${triggers}" STREQUAL "")
        string(APPEND problems "  ${name} has no trigger under 'on:' that this check can read (two-space indentation, "
                               "'  push:')\n")
        continue()
    endif()

    if(NOT "push" IN_LIST triggers AND NOT "pull_request" IN_LIST triggers)
        list(APPEND skipped "${name}")
        continue()
    endif()
    list(APPEND filtered "${name}")

    if("${ignore_push}" STREQUAL "" AND "${ignore_pull_request}" STREQUAL "")
        list(APPEND unlisted "${name}")
        continue()
    endif()

    # It has a list: under each trigger it has, and the same.
    foreach(kind IN ITEMS push pull_request)
        if("${kind}" IN_LIST triggers AND "${ignore_${kind}}" STREQUAL "")
            string(APPEND problems "  found no paths-ignore list under the ${kind} trigger of ${name}, which has one under "
                                   "the other\n")
        endif()
    endforeach()
    set(sorted_push ${ignore_push})
    set(sorted_pull_request ${ignore_pull_request})
    list(SORT sorted_push)
    list(SORT sorted_pull_request)
    if(NOT "${sorted_push}" STREQUAL "" AND NOT "${sorted_pull_request}" STREQUAL ""
       AND NOT "${sorted_push}" STREQUAL "${sorted_pull_request}")
        string(REPLACE ";" ", " push_text "${sorted_push}")
        string(REPLACE ";" ", " pull_request_text "${sorted_pull_request}")
        string(APPEND problems "  the lists of ${name} differ, and the two must be the same:\n"
                               "      push: ${push_text}\n      pull_request: ${pull_request_text}\n")
    endif()

    # A workflow whose every ctest command selects the lint's tests is the lint; any other runs checks.
    if(ctest_lines EQUAL 0 OR ctest_more GREATER 0)
        set(suite TRUE)
    else()
        set(suite FALSE)
    endif()
    set(both ${ignore_push} ${ignore_pull_request})
    list(REMOVE_DUPLICATES both)
    list(SORT both)
    list(APPEND listed "${name}")
    set(listed_name_${n} "${name}")
    set(listed_entries_${n} ${both})
    set(listed_suite_${n} ${suite})
    math(EXPR n "${n} + 1")
endforeach()

if("${filtered}" STREQUAL "")
    string(APPEND problems "  found no workflow with a push or a pull_request trigger, so there is no list to hold\n")
elseif("${listed}" STREQUAL "")
    string(APPEND problems "  found no workflow with a paths-ignore list, so there is no list to hold\n")
endif()

# ── every file in the checkout ───────────────────────────────────────────────
file(GLOB top RELATIVE "${SOURCE_DIR}" "${SOURCE_DIR}/*")
set(all_files "")
foreach(top_entry IN LISTS top)
    if(top_entry MATCHES "^(build.*|cmake-build-.*|\\.git)$")
        continue()
    endif()
    if(IS_DIRECTORY "${SOURCE_DIR}/${top_entry}")
        file(GLOB_RECURSE found RELATIVE "${SOURCE_DIR}" "${SOURCE_DIR}/${top_entry}/*")
        list(APPEND all_files ${found})
    else()
        list(APPEND all_files "${top_entry}")
    endif()
endforeach()
list(SORT all_files)

# ── the files that the checks read, by name ──────────────────────────────────
file(GLOB_RECURSE check_scripts RELATIVE "${SOURCE_DIR}" "${SOURCE_DIR}/tests/*.cmake")
set(reads "")                               # "<name>|<script that reads it>"
foreach(script IN LISTS check_scripts)
    file(READ "${SOURCE_DIR}/${script}" text)
    string(REGEX MATCHALL "\\$\\{SOURCE_DIR\\}/[A-Za-z0-9_./-]+" references "${text}")
    foreach(reference IN LISTS references)
        string(REGEX REPLACE "^\\$\\{SOURCE_DIR\\}/" "" name "${reference}")
        if(NOT name MATCHES "/$")           # a directory is read by the build-input rule
            list(APPEND reads "${name}|${script}")
        endif()
    endforeach()
endforeach()
list(REMOVE_DUPLICATES reads)
list(LENGTH reads reads_count)
if(reads_count EQUAL 0)
    message(FATAL_ERROR "found no file in tests/ of ${SOURCE_DIR} that a check script reads as \${SOURCE_DIR}/<name>")
endif()

# ── each list against the checkout ───────────────────────────────────────────
set(summaries "")
if(n GREATER 0)
    math(EXPR last "${n} - 1")
    foreach(i RANGE ${last})
        set(name "${listed_name_${i}}")
        set(suite "${listed_suite_${i}}")

        # Each entry is a file name or <dir>/**: a rule "dir|<dir>" or "file|<name>".
        set(rules "")
        foreach(entry IN LISTS listed_entries_${i})
            if(entry MATCHES "^([A-Za-z0-9_./-]+)/\\*\\*$")
                list(APPEND rules "dir|${CMAKE_MATCH_1}")
            elseif(entry MATCHES "^[A-Za-z0-9_./-]+$")
                list(APPEND rules "file|${entry}")
            else()
                string(APPEND problems "  '${entry}' in ${name} is neither a file name nor <dir>/**, and this check does not "
                                       "guess what another pattern matches\n")
            endif()
        endforeach()

        set(workflow_covered "")
        foreach(rule IN LISTS rules)
            rule_text("${rule}" text)

            set(matched 0)
            set(input_count 0)
            set(input_example "")
            foreach(path IN LISTS all_files)
                rule_covers("${rule}" "${path}" covers)
                if(NOT covers)
                    continue()
                endif()
                math(EXPR matched "${matched} + 1")
                list(APPEND workflow_covered "${path}")
                is_run_input("${path}" is_input)
                if(is_input)
                    math(EXPR input_count "${input_count} + 1")
                    if(input_example STREQUAL "")
                        set(input_example "${path}")
                    endif()
                endif()
            endforeach()
            if(matched EQUAL 0)
                string(APPEND problems "  ${text} in ${name} matches no file in the checkout\n")
            endif()
            if(input_count GREATER 0)
                string(APPEND problems "  ${text} in ${name} covers ${input_count} files that the build or the lint reads, such "
                                       "as ${input_example}\n")
            endif()

            if(suite)
                foreach(read IN LISTS reads)
                    string(FIND "${read}" "|" bar)
                    string(SUBSTRING "${read}" 0 ${bar} read_name)
                    math(EXPR start "${bar} + 1")
                    string(SUBSTRING "${read}" ${start} -1 script)
                    rule_covers("${rule}" "${read_name}" covers)
                    if(covers)
                        string(APPEND problems "  ${text} in ${name} covers ${read_name}, which ${script} reads, and ${name} "
                                               "runs more than the clang_tidy_ tests\n")
                    endif()
                endforeach()
            endif()
        endforeach()

        list(REMOVE_DUPLICATES workflow_covered)
        list(LENGTH workflow_covered workflow_covered_count)
        set(entries_text "")
        foreach(rule IN LISTS rules)
            rule_text("${rule}" text)
            list(APPEND entries_text "${text}")
        endforeach()
        string(REPLACE ";" ", " entries_text "${entries_text}")
        get_filename_component(base "${name}" NAME)
        list(APPEND summaries "${base} ignores ${entries_text} (${workflow_covered_count} files)")
    endforeach()
endif()

base_names("${filtered}" filtered_text)
if(NOT problems STREQUAL "")
    message(FATAL_ERROR "the paths-ignore lists of the workflows that a push or a pull request starts (${filtered_text}), but:\n"
                        "${problems}")
endif()

base_names("${unlisted}" unlisted_text)
base_names("${skipped}" skipped_text)
string(REPLACE ";" "; " summaries_text "${summaries}")
message(STATUS "paths-ignore under push and pull_request: ${summaries_text}, none of which the build or the lint reads; "
               "no list: ${unlisted_text}; skipped, with neither trigger: ${skipped_text}; ${reads_count} files are read "
               "by the checks")
