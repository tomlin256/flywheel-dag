# check_presets.cmake — test_presets: CMakePresets.json names one build tree per
# kind and platform, and says which of them a host can use; README.md names only
# those trees.
#
# Run by ctest as
#
#   cmake -DSOURCE_DIR=<source> -DHOST_SYSTEM_NAME=<CMAKE_HOST_SYSTEM_NAME>
#         [-DPRESETS_DIR=<dir>] [-DREADME_DIR=<dir>] -P check_presets.cmake
#
# PRESETS_DIR is the directory whose CMakePresets.json is read (default
# SOURCE_DIR); a hand-made change to a copy of the file is passed here, and must
# fail the rule that names it (docs/plans/platform_build_dirs_plan.md). README_DIR
# is the same for R8: the directory whose README.md is read (default SOURCE_DIR)
# (docs/plans/readme_build_snippets_plan.md). SOURCE_DIR is the checkout whose
# cmake/flywheel_dag_platform.cmake spells the hosts.
#
# CMake rejects a field it does not know, so the file cannot carry its own
# rationale. It is here. A tree is build/<preset name>, so the name of a preset is
# the name of its tree, and the guard in CMakeLists.txt (flywheel_dag_tree_problem)
# reads <kind>-<os>-<arch>. The shape is a hidden `base` preset that sets the
# directory from the preset's own name, a hidden preset per kind that sets the
# build type the kind's name says, a hidden preset per host that says which OS it
# is for, and one visible preset for each kind on each host that inherits all
# three. A preset cannot see the CPU: ${hostSystemName} is the only host fact a
# condition can read, so the two Linux hosts are both listed on any Linux host and
# the guard is what makes the wrong one an error. Each visible configure preset
# has a build preset and a test preset of the same name. The test presets carry
# what CI passes today: --output-on-failure, --timeout 120, and an error for a
# test preset that finds no tests, which ctest otherwise reports as success. No
# preset sets a warning flag (FLYWHEEL_DAG_WARNINGS_AS_ERRORS is the caller's).
#
# The file's cmakeMinimumRequired is 3.21, the CMake that can read a version 3 file,
# and not the project's floor, which is CMake 4.0 (test_cmake_floor). Raised to the
# floor it would make CMake 3 stop on `cmake --preset` with "major version 4 must be
# less than 3" before CMakeLists.txt could say "CMake 4.0 or higher is required".
#
# A command in the README builds the tree it names. A tree at build/ itself is one
# no preset defines: the guard lets it through, because the name claims no
# platform, and it sits among the named trees. So R8 reads the README, as
# test_cmake_floor and test_cmake_diagnostics do, and holds every tree it names to
# the presets.
#
# A new kind is a deliberate edit to the table of kinds below as well as to the
# file. The rules:
#
#   R1  version is at least 3, the first to have `condition`
#   R2  `base` is hidden and its binaryDir is ${sourceDir}/build/${presetName}
#   R3  one hidden host preset per known host, named <os>-<arch> in the module's
#       spelling, whose condition is ${hostSystemName} equal to that OS's CMake name
#   R4  one hidden kind preset per kind, and it sets CMAKE_BUILD_TYPE
#   R5  the visible configure presets are exactly the pairs of a kind and a host,
#       each inheriting base, that kind and that host, with no binaryDir of its own
#   R6  each has a build preset and a test preset of the same name, which name it
#       as configurePreset and inherit their host's hidden preset; the test presets
#       carry the three settings above, and only `base` sets them
#   R7  cmake --list-presets=all lists, for the configure, build and test presets,
#       exactly the presets for HOST_SYSTEM_NAME's OS: CMake agrees with R3
#   R8  README.md names only trees a preset defines: each `--preset <name>` and
#       each build/<name> is a visible configure preset, and no command is -B,
#       --build, --install or --test-dir on `build` itself

cmake_minimum_required(VERSION 4.0)

foreach(var IN ITEMS SOURCE_DIR HOST_SYSTEM_NAME)
    if(NOT ${var})
        message(FATAL_ERROR "usage: cmake -DSOURCE_DIR=<dir> -DHOST_SYSTEM_NAME=<name> "
                            "[-DPRESETS_DIR=<dir>] [-DREADME_DIR=<dir>] -P check_presets.cmake")
    endif()
endforeach()
if(NOT PRESETS_DIR)
    set(PRESETS_DIR "${SOURCE_DIR}")
endif()
if(NOT README_DIR)
    set(README_DIR "${SOURCE_DIR}")
endif()

include("${SOURCE_DIR}/cmake/flywheel_dag_platform.cmake")

# ── the vocabulary: kinds, and hosts as CMake reports them ───────────────────
set(kinds relwithdebinfo release debug)
set(build_type_of_relwithdebinfo RelWithDebInfo)
set(build_type_of_release        Release)
set(build_type_of_debug          Debug)

set(known_systems  Darwin Linux   Linux)
set(known_machines arm64  aarch64 x86_64)

# ── bookkeeping ──────────────────────────────────────────────────────────────
function(rule_failed rule detail)
    string(REPLACE ";" "," detail "${detail}")
    set_property(GLOBAL APPEND PROPERTY FAILURES "  ${rule}: ${detail}")
endfunction()

function(finish)
    get_property(failures GLOBAL PROPERTY FAILURES)
    if(failures)
        list(LENGTH failures count)
        string(REPLACE ";" "\n" text "${failures}")
        message(FATAL_ERROR "test_presets: ${count} rule(s) failed:\n${text}")
    endif()
    message(STATUS "test_presets: R1 to R8 hold")
endfunction()

# json_member(<out> <json> <member>...): the member's value, or empty if it is
# not there. JSON true is ON.
function(json_member out json)
    string(JSON value ERROR_VARIABLE error GET "${json}" ${ARGN})
    if(error)
        set(${out} "" PARENT_SCOPE)
    else()
        set(${out} "${value}" PARENT_SCOPE)
    endif()
endfunction()

# inherits_list(<out> <preset>): what `inherits` names, a string or an array.
function(inherits_list out preset)
    set(result "")
    json_member(inherits "${preset}" inherits)
    if(NOT "${inherits}" STREQUAL "")
        string(JSON type TYPE "${preset}" inherits)
        if(type STREQUAL "ARRAY")
            string(JSON count LENGTH "${preset}" inherits)
            if(count GREATER 0)
                math(EXPR last "${count} - 1")
                foreach(i RANGE ${last})
                    string(JSON item GET "${preset}" inherits ${i})
                    list(APPEND result "${item}")
                endforeach()
            endif()
        else()
            list(APPEND result "${inherits}")
        endif()
    endif()
    set(${out} "${result}" PARENT_SCOPE)
endfunction()

# preset_json(<out> <section> <name>): the preset's JSON, or empty.
function(preset_json out section name)
    set(${out} "${${section}__${name}}" PARENT_SCOPE)
endfunction()

# condition_ok(<out> <preset> <system>): ON if the preset's own condition is
# ${hostSystemName} equal to <system>.
function(condition_ok out preset system)
    json_member(type "${preset}" condition type)
    json_member(lhs  "${preset}" condition lhs)
    json_member(rhs  "${preset}" condition rhs)
    if(type STREQUAL "equals" AND lhs STREQUAL "\${hostSystemName}" AND rhs STREQUAL "${system}")
        set(${out} ON PARENT_SCOPE)
    else()
        set(${out} OFF PARENT_SCOPE)
    endif()
endfunction()

# ── the file ─────────────────────────────────────────────────────────────────
set(file "${PRESETS_DIR}/CMakePresets.json")
if(NOT EXISTS "${file}")
    rule_failed("the file" "${file} is missing")
    finish()
endif()
file(READ "${file}" json)

# R1
json_member(version "${json}" version)
if("${version}" STREQUAL "" OR "${version}" LESS 3)
    rule_failed(R1 "version is '${version}', not 3 or more")
endif()

# Index every preset by section and name: ${configure__<name>} and so on.
foreach(section IN ITEMS configure build test)
    set(${section}_names "")
    json_member(presets "${json}" ${section}Presets)
    if(NOT "${presets}" STREQUAL "")
        string(JSON count LENGTH "${presets}")
        if(count GREATER 0)
            math(EXPR last "${count} - 1")
            foreach(i RANGE ${last})
                string(JSON preset GET "${presets}" ${i})
                json_member(name "${preset}" name)
                list(APPEND ${section}_names "${name}")
                set(${section}__${name} "${preset}")
            endforeach()
        endif()
    endif()
    set(duplicates "${${section}_names}")
    list(REMOVE_DUPLICATES duplicates)
    list(LENGTH ${section}_names n_names)
    list(LENGTH duplicates n_unique)
    if(NOT n_names EQUAL n_unique)
        rule_failed(R5 "a ${section} preset name appears twice")
    endif()
endforeach()

# R2
preset_json(base configure base)
if("${base}" STREQUAL "")
    rule_failed(R2 "there is no configure preset 'base'")
else()
    json_member(hidden "${base}" hidden)
    json_member(dir "${base}" binaryDir)
    if(NOT hidden)
        rule_failed(R2 "'base' is not hidden")
    endif()
    if(NOT "${dir}" STREQUAL "\${sourceDir}/build/\${presetName}")
        rule_failed(R2 "'base' has binaryDir '${dir}', not '\${sourceDir}/build/\${presetName}'")
    endif()
endif()

# R3: the hosts, named as the module spells them.
set(hosts "")
foreach(i RANGE 2)
    list(GET known_systems  ${i} system)
    list(GET known_machines ${i} machine)
    flywheel_dag_platform(host "${system}" "${machine}")
    list(APPEND hosts "${host}")
    set(system_of_${host} "${system}")
    preset_json(preset configure ${host})
    if("${preset}" STREQUAL "")
        rule_failed(R3 "there is no configure preset '${host}' (${system}, ${machine})")
        continue()
    endif()
    json_member(hidden "${preset}" hidden)
    condition_ok(ok "${preset}" "${system}")
    if(NOT hidden)
        rule_failed(R3 "'${host}' is not hidden")
    endif()
    if(NOT ok)
        rule_failed(R3 "the condition of '${host}' is not \${hostSystemName} equals ${system}")
    endif()
endforeach()

# R4: the kinds.
foreach(kind IN LISTS kinds)
    preset_json(preset configure ${kind})
    if("${preset}" STREQUAL "")
        rule_failed(R4 "there is no configure preset '${kind}'")
        continue()
    endif()
    json_member(hidden "${preset}" hidden)
    json_member(type "${preset}" cacheVariables CMAKE_BUILD_TYPE)
    if(NOT hidden)
        rule_failed(R4 "'${kind}' is not hidden")
    endif()
    if(NOT "${type}" STREQUAL "${build_type_of_${kind}}")
        rule_failed(R4 "'${kind}' sets CMAKE_BUILD_TYPE '${type}', not '${build_type_of_${kind}}'")
    endif()
endforeach()

# R5: the visible configure presets, exactly the pairs of a kind and a host.
set(expected "")
foreach(kind IN LISTS kinds)
    foreach(host IN LISTS hosts)
        list(APPEND expected "${kind}-${host}")
        preset_json(preset configure ${kind}-${host})
        if("${preset}" STREQUAL "")
            rule_failed(R5 "there is no configure preset '${kind}-${host}'")
            continue()
        endif()
        json_member(hidden "${preset}" hidden)
        json_member(dir "${preset}" binaryDir)
        inherits_list(parents "${preset}")
        set(wanted base ${kind} ${host})
        list(SORT parents)
        list(SORT wanted)
        if(hidden)
            rule_failed(R5 "'${kind}-${host}' is hidden")
        endif()
        if(NOT "${dir}" STREQUAL "")
            rule_failed(R5 "'${kind}-${host}' sets a binaryDir of its own: '${dir}'")
        endif()
        if(NOT "${parents}" STREQUAL "${wanted}")
            rule_failed(R5 "'${kind}-${host}' inherits '${parents}', not '${wanted}'")
        endif()
    endforeach()
endforeach()
foreach(name IN LISTS configure_names)
    preset_json(preset configure ${name})
    json_member(hidden "${preset}" hidden)
    if(NOT hidden AND NOT name IN_LIST expected)
        rule_failed(R5 "'${name}' is a visible configure preset that is no pair of a kind and a host")
    endif()
endforeach()

# R6: the build and test presets.
foreach(section IN ITEMS build test)
    foreach(host IN LISTS hosts)
        preset_json(preset ${section} ${host})
        if("${preset}" STREQUAL "")
            rule_failed(R6 "there is no hidden ${section} preset '${host}'")
            continue()
        endif()
        json_member(hidden "${preset}" hidden)
        condition_ok(ok "${preset}" "${system_of_${host}}")
        if(NOT hidden)
            rule_failed(R6 "the ${section} preset '${host}' is not hidden")
        endif()
        if(NOT ok)
            rule_failed(R6 "the condition of the ${section} preset '${host}' is not \${hostSystemName} equals ${system_of_${host}}")
        endif()
    endforeach()

    foreach(kind IN LISTS kinds)
        foreach(host IN LISTS hosts)
            set(name ${kind}-${host})
            preset_json(preset ${section} ${name})
            if("${preset}" STREQUAL "")
                rule_failed(R6 "there is no ${section} preset '${name}'")
                continue()
            endif()
            json_member(hidden "${preset}" hidden)
            json_member(configure_preset "${preset}" configurePreset)
            inherits_list(parents "${preset}")
            if(hidden)
                rule_failed(R6 "the ${section} preset '${name}' is hidden")
            endif()
            if(NOT "${configure_preset}" STREQUAL "${name}")
                rule_failed(R6 "the ${section} preset '${name}' names configurePreset '${configure_preset}'")
            endif()
            if(NOT "${parents}" STREQUAL "${host}")
                rule_failed(R6 "the ${section} preset '${name}' inherits '${parents}', not '${host}'")
            endif()
        endforeach()
    endforeach()

    # No visible preset beyond the pairs.
    foreach(name IN LISTS ${section}_names)
        preset_json(preset ${section} ${name})
        json_member(hidden "${preset}" hidden)
        if(NOT hidden AND NOT name IN_LIST expected)
            rule_failed(R6 "'${name}' is a visible ${section} preset that is no pair of a kind and a host")
        endif()
    endforeach()
endforeach()

# The test presets carry the settings, and only `base` sets them.
preset_json(test_base test base)
if("${test_base}" STREQUAL "")
    rule_failed(R6 "there is no test preset 'base'")
else()
    json_member(hidden  "${test_base}" hidden)
    json_member(output  "${test_base}" output outputOnFailure)
    json_member(timeout "${test_base}" execution timeout)
    json_member(action  "${test_base}" execution noTestsAction)
    if(NOT hidden)
        rule_failed(R6 "the test preset 'base' is not hidden")
    endif()
    if(NOT output)
        rule_failed(R6 "the test preset 'base' does not set output.outputOnFailure")
    endif()
    if(NOT "${timeout}" STREQUAL "120")
        rule_failed(R6 "the test preset 'base' sets execution.timeout '${timeout}', not 120")
    endif()
    if(NOT "${action}" STREQUAL "error")
        rule_failed(R6 "the test preset 'base' sets execution.noTestsAction '${action}', not 'error'")
    endif()
endif()
foreach(host IN LISTS hosts)
    preset_json(preset test ${host})
    inherits_list(parents "${preset}")
    if(NOT "${parents}" STREQUAL "base")
        rule_failed(R6 "the test preset '${host}' inherits '${parents}', not 'base'")
    endif()
endforeach()
foreach(name IN LISTS test_names)
    if(NOT name STREQUAL "base")
        preset_json(preset test ${name})
        json_member(output "${preset}" output)
        json_member(execution "${preset}" execution)
        if(NOT "${output}" STREQUAL "" OR NOT "${execution}" STREQUAL "")
            rule_failed(R6 "the test preset '${name}' sets output or execution; only 'base' does")
        endif()
    endif()
endforeach()

# R7: what CMake lists for this host.
execute_process(COMMAND ${CMAKE_COMMAND} -S "${PRESETS_DIR}" --list-presets=all
                RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0)
    rule_failed(R7 "cmake --list-presets=all failed (${rc}): ${err}")
else()
    foreach(section IN ITEMS configure build test)
        set(listed_${section} "")
    endforeach()
    string(REPLACE "\n" ";" lines "${out}")
    set(current "")
    foreach(line IN LISTS lines)
        if(line MATCHES "^Available (configure|build|test) presets:")
            set(current "${CMAKE_MATCH_1}")
        elseif(current AND line MATCHES "^  \"(.*)\"")
            list(APPEND listed_${current} "${CMAKE_MATCH_1}")
        endif()
    endforeach()

    set(for_host "")
    foreach(kind IN LISTS kinds)
        foreach(host IN LISTS hosts)
            if(system_of_${host} STREQUAL "${HOST_SYSTEM_NAME}")
                list(APPEND for_host "${kind}-${host}")
            endif()
        endforeach()
    endforeach()
    list(SORT for_host)
    foreach(section IN ITEMS configure build test)
        list(SORT listed_${section})
        if(NOT "${listed_${section}}" STREQUAL "${for_host}")
            rule_failed(R7 "cmake lists the ${section} presets '${listed_${section}}' for a ${HOST_SYSTEM_NAME} host, not '${for_host}'")
        endif()
    endforeach()
endif()

# R8: the README names only trees a preset defines. The text is matched whole, as
# check_cmake_floor.cmake does, because it holds `;` and `[`, which a list of its
# lines would split. CMake's regex has no lookahead and no \b, so a command on
# `build` itself is the flag, spaces, `build`, and then anything that does not
# continue a path: `--build build/relwithdebinfo-macos-arm64` is not one.
set(readme_file "${README_DIR}/README.md")
if(NOT EXISTS "${readme_file}")
    rule_failed(R8 "${readme_file} is missing")
else()
    file(READ "${readme_file}" readme_text)

    set(flat_flags "-B"         "--build" "--install" "--test-dir")
    set(flat_verbs "configure"  "build"   "install"   "test")
    foreach(flag verb IN ZIP_LISTS flat_flags flat_verbs)
        string(REGEX MATCHALL "${flag} +build([^A-Za-z0-9_/.-]|$)" hits "${readme_text}")
        list(LENGTH hits n)
        if(n GREATER 0)
            rule_failed(R8 "README.md has ${n} command(s) that ${verb} 'build' itself (${flag} build)")
        endif()
    endforeach()

    # The names: `--preset <name>`, and the component after build/.
    set(named "")
    string(REGEX MATCHALL "--preset +[A-Za-z0-9_-]+" uses "${readme_text}")
    foreach(use IN LISTS uses)
        string(REGEX REPLACE "^--preset +" "" name "${use}")
        list(APPEND named "${name}")
    endforeach()
    string(REGEX MATCHALL "build/[A-Za-z0-9_-]+" uses "${readme_text}")
    foreach(use IN LISTS uses)
        string(REGEX REPLACE "^build/" "" name "${use}")
        list(APPEND named "${name}")
    endforeach()
    list(REMOVE_DUPLICATES named)
    foreach(name IN LISTS named)
        preset_json(preset configure ${name})
        if("${preset}" STREQUAL "")
            rule_failed(R8 "README.md names '${name}', which is no configure preset")
        else()
            json_member(hidden "${preset}" hidden)
            if(hidden)
                rule_failed(R8 "README.md names '${name}', which is a hidden preset")
            endif()
        endif()
    endforeach()
endif()

finish()
