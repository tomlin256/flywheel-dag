# check_sanitizer_scripts.cmake — test_sanitizer_scripts: the two scripts that a sanitizer tree's gate is made of, each held to the
# outcomes it must tell apart.
#
# Run by ctest as
#
#   cmake -DSOURCE_DIR=<source> -DSCRATCH_DIR=<dir> -P check_sanitizer_scripts.cmake
#
# in every tree: it needs no sanitizer, no compiler and no network, so a regression in either script shows in an ordinary build and
# not only in a sanitizer tree. The scripts run as children, as ctest runs them.
#
#   - tests/run_sanitizer_canary.cmake gets stand-ins for a canary, sh scripts that print and exit. One stops with the sanitizer's
#     report, which is accepted. The others are the ways it must refuse: the report and exit 0, no report and exit 0, and a stop with
#     no report or with another sanitizer's.
#   - tests/check_sanitizer_coverage.cmake gets fixture build trees written here: a compile_commands.json and the link.txt files of a
#     Makefiles tree, made from the options of the mode. A tree that carries everything is accepted, and each defect is refused by
#     name: a unit without an option, a unit with only part of one, a unit or an executable with -fno-sanitize=, an executable without
#     a link option, a layout it cannot read, a tree below a floor, a wrong mode, and a mode that has no options.
#
# The two scripts are what a sanitizer tree's canaries and coverage test run, and they are tested nowhere else: a canary that is
# red tells the runner nothing about the cases it does not meet. SOURCE_DIR is a parameter so that a hand-made change to a copy of
# either script is passed here, and must fail.

cmake_minimum_required(VERSION 4.0)

foreach(var IN ITEMS SOURCE_DIR SCRATCH_DIR)
    if(NOT ${var})
        message(FATAL_ERROR "usage: cmake -DSOURCE_DIR=<dir> -DSCRATCH_DIR=<dir> -P check_sanitizer_scripts.cmake")
    endif()
endforeach()
include("${SOURCE_DIR}/cmake/flywheel_dag_sanitize.cmake")
set(runner   "${SOURCE_DIR}/tests/run_sanitizer_canary.cmake")
set(coverage "${SOURCE_DIR}/tests/check_sanitizer_coverage.cmake")
foreach(script IN ITEMS "${runner}" "${coverage}")
    if(NOT EXISTS "${script}")
        message(FATAL_ERROR "${script} does not exist")
    endif()
endforeach()
if(NOT EXISTS /bin/sh)
    message(FATAL_ERROR "no /bin/sh on this host: the stand-ins for a canary are sh scripts")
endif()

file(REMOVE_RECURSE "${SCRATCH_DIR}")
file(MAKE_DIRECTORY "${SCRATCH_DIR}")

set(cases 0)
set(failures "")

# Runs <command> and sets <rc_var> to its status and <text_var> to its output and error output, runs of whitespace as one space,
# because CMake wraps an error at about 80 columns.
function(run_child rc_var text_var)
    execute_process(COMMAND ${ARGN} RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    string(REGEX REPLACE "[ \t\r\n]+" " " flat "${out} ${err}")
    set(${rc_var} "${rc}" PARENT_SCOPE)
    set(${text_var} "${flat}" PARENT_SCOPE)
endfunction()

# The verdict on a run: <want> is accepted (status 0) or refused (anything else), and the text must hold <needle> either way.
function(expect label want rc text needle)
    math(EXPR count "${cases} + 1")
    set(cases ${count} PARENT_SCOPE)
    if("${want}" STREQUAL "accepted" AND NOT "${rc}" STREQUAL "0")
        string(APPEND failures "\n  ${label}: refused (${rc}), want accepted: ${text}")
    elseif("${want}" STREQUAL "refused" AND "${rc}" STREQUAL "0")
        string(APPEND failures "\n  ${label}: accepted, want refused: ${text}")
    else()
        string(FIND "${text}" "${needle}" at)
        if(at EQUAL -1)
            string(APPEND failures "\n  ${label}: the output lacks '${needle}': ${text}")
        endif()
    endif()
    set(failures "${failures}" PARENT_SCOPE)
endfunction()

# ── the canary runner ────────────────────────────────────────────────────────
set(report "==4242==ERROR: AddressSanitizer: heap-use-after-free on address 0x602000000010")
set(other_report "WARNING: ThreadSanitizer: data race (pid=4242)")

# A canary that prints <output> on stderr and exits <exit_code>, run by the runner against the expectation of AddressSanitizer.
function(expect_runner label output exit_code want needle)
    set(stand_in "${SCRATCH_DIR}/canary_${cases}.sh")
    file(WRITE "${stand_in}" "#!/bin/sh\nprintf '%s\\n' '${output}' >&2\nexit ${exit_code}\n")
    file(CHMOD "${stand_in}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
    run_child(rc text "${CMAKE_COMMAND}" "-DCANARY=${stand_in}" "-DEXPECT=AddressSanitizer: heap-use-after-free"
              -P "${runner}")
    expect("runner: ${label}" "${want}" "${rc}" "${text}" "${needle}")
    set(cases ${cases} PARENT_SCOPE)
    set(failures "${failures}" PARENT_SCOPE)
endfunction()

expect_runner("a stop with the report" "${report}" 1 accepted "stopped (1) with a report matching")
expect_runner("the report and exit 0" "${report}" 0 refused "printed the sanitizer's report and exited 0")
expect_runner("no report and exit 0" "" 0 refused "exited 0 with no report")
expect_runner("no report and exit 0 names the runtime option that switches a sanitizer off" "" 0 refused
              "ASAN_OPTIONS=detect_leaks=0")
expect_runner("a stop with no report" "Segmentation fault" 3 refused "died of something other than the sanitizer")
expect_runner("a stop with another sanitizer's report" "${other_report}" 1 refused
              "died of something other than the sanitizer")

run_child(rc text "${CMAKE_COMMAND}" -P "${runner}")
expect("runner: no arguments" refused "${rc}" "${text}" "usage: cmake -DCANARY=<exe> -DEXPECT=<regex>")

# ── the coverage check ───────────────────────────────────────────────────────
flywheel_dag_sanitizer_flags(asan mode_compile mode_link)

# Writes a build tree in the layout of the Makefiles generator under <dir>: a compile_commands.json of <units> entries and a link.txt
# for each of <executables> executables, plus a static library and a custom target, which the script must leave alone. Unit 3 and
# executable 2 are the ones to tamper with: UNIT_DROPS and LINK_DROPS take an option off, UNIT_ADDS and LINK_ADDS put one on.
# NO_COMMAND gives unit 4 an "arguments" array instead of a "command" string.
function(make_tree dir units executables)
    cmake_parse_arguments(PARSE_ARGV 3 tweak "NO_COMMAND" "UNIT_DROPS;UNIT_ADDS;LINK_DROPS;LINK_ADDS" "")
    file(REMOVE_RECURSE "${dir}")
    file(MAKE_DIRECTORY "${dir}/CMakeFiles")

    set(entries "")
    foreach(i RANGE 1 ${units})
        set(options ${mode_compile})
        if(i EQUAL 3)
            if(tweak_UNIT_DROPS)
                list(REMOVE_ITEM options "${tweak_UNIT_DROPS}")
            endif()
            if(tweak_UNIT_ADDS)
                list(APPEND options "${tweak_UNIT_ADDS}")
            endif()
        endif()
        list(JOIN options " " option_text)
        if(i EQUAL 4 AND tweak_NO_COMMAND)
            list(APPEND entries "{\"directory\":\"${dir}\",\"arguments\":[\"/usr/bin/c++\",\"-c\",\"/src/u${i}.cpp\"],\"file\":\"/src/u${i}.cpp\"}")
        else()
            list(APPEND entries "{\"directory\":\"${dir}\",\"command\":\"/usr/bin/c++ ${option_text} -c /src/u${i}.cpp\",\"file\":\"/src/u${i}.cpp\"}")
        endif()
    endforeach()
    list(JOIN entries ",\n" body)
    file(WRITE "${dir}/compile_commands.json" "[\n${body}\n]\n")

    set(dirs "")
    foreach(j RANGE 1 ${executables})
        set(options ${mode_link})
        if(j EQUAL 2)
            if(tweak_LINK_DROPS)
                list(REMOVE_ITEM options "${tweak_LINK_DROPS}")
            endif()
            if(tweak_LINK_ADDS)
                list(APPEND options "${tweak_LINK_ADDS}")
            endif()
        endif()
        list(JOIN options " " option_text)
        file(MAKE_DIRECTORY "${dir}/CMakeFiles/exe${j}.dir")
        file(WRITE "${dir}/CMakeFiles/exe${j}.dir/link.txt" "/usr/bin/c++ -O2 ${option_text} -o ../bin/exe${j} exe${j}.o\n")
        list(APPEND dirs "${dir}/CMakeFiles/exe${j}.dir")
    endforeach()
    file(MAKE_DIRECTORY "${dir}/CMakeFiles/library.dir")
    file(WRITE "${dir}/CMakeFiles/library.dir/link.txt" "/usr/bin/ar qc liblibrary.a library.o\n")
    list(APPEND dirs "${dir}/CMakeFiles/library.dir" "${dir}/CMakeFiles/custom.dir")
    list(JOIN dirs "\n" dirs_text)
    file(WRITE "${dir}/CMakeFiles/TargetDirectories.txt" "${dirs_text}\n")
endfunction()

# The coverage check over <dir> for <mode>. The floors are 40 units and 25 executables, so a tree of 45 and 30 is above both.
function(expect_coverage label want needle dir mode)
    run_child(rc text "${CMAKE_COMMAND}" "-DSOURCE_DIR=${SOURCE_DIR}" "-DBUILD_DIR=${dir}" "-DMODE=${mode}" -P "${coverage}")
    expect("coverage: ${label}" "${want}" "${rc}" "${text}" "${needle}")
    set(cases ${cases} PARENT_SCOPE)
    set(failures "${failures}" PARENT_SCOPE)
endfunction()

set(tree "${SCRATCH_DIR}/tree")

make_tree("${tree}" 45 30)
expect_coverage("a tree that carries every option" accepted
                "45 compile units and 30 executable link lines, all carry the 'asan' mode's options" "${tree}" asan)
expect_coverage("the same tree for another mode" refused "compile: /src/u1.cpp: no -fsanitize=thread" "${tree}" tsan)
expect_coverage("a mode with no options" refused "mode 'OFF' has no options" "${tree}" OFF)

make_tree("${tree}" 45 30 UNIT_DROPS -fno-omit-frame-pointer)
expect_coverage("a unit without one option" refused "compile: /src/u3.cpp: no -fno-omit-frame-pointer" "${tree}" asan)

make_tree("${tree}" 45 30 UNIT_DROPS -fsanitize=address,undefined UNIT_ADDS -fsanitize=address)
expect_coverage("a unit with part of an option" refused "compile: /src/u3.cpp: no -fsanitize=address,undefined" "${tree}" asan)

make_tree("${tree}" 45 30 UNIT_ADDS -fno-sanitize=all)
expect_coverage("a unit with -fno-sanitize=" refused "compile: /src/u3.cpp: has -fno-sanitize=all" "${tree}" asan)

make_tree("${tree}" 45 30 LINK_DROPS -fsanitize=address,undefined)
expect_coverage("an executable without a link option" refused "link: exe2: no -fsanitize=address,undefined" "${tree}" asan)

make_tree("${tree}" 45 30 LINK_ADDS -fno-sanitize=all)
expect_coverage("an executable with -fno-sanitize=" refused "link: exe2: has -fno-sanitize=all" "${tree}" asan)

make_tree("${tree}" 45 30 NO_COMMAND)
expect_coverage("an entry with no command" refused "compile: entry 3: no \"file\" or no \"command\" (CMake's layout changed?)"
                "${tree}" asan)

make_tree("${tree}" 39 30)
expect_coverage("one unit under the floor" refused "scanned only 39 compile units" "${tree}" asan)

make_tree("${tree}" 45 24)
expect_coverage("one executable under the floor" refused "scanned only 24 executable link lines" "${tree}" asan)

make_tree("${tree}" 45 30)
file(REMOVE "${tree}/compile_commands.json")
expect_coverage("no compile database" refused "compile_commands.json does not exist" "${tree}" asan)

make_tree("${tree}" 45 30)
file(REMOVE "${tree}/CMakeFiles/TargetDirectories.txt")
expect_coverage("no list of targets" refused "TargetDirectories.txt does not exist" "${tree}" asan)

if(failures)
    message(FATAL_ERROR "test_sanitizer_scripts: some of ${cases} cases failed:${failures}")
endif()
file(REMOVE_RECURSE "${SCRATCH_DIR}")
message(STATUS "test_sanitizer_scripts: all ${cases} cases hold")
