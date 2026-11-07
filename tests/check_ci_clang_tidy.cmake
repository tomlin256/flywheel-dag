# check_ci_clang_tidy.cmake — test_ci_clang_tidy: the clang-tidy workflow runs the whole gate, at the LLVM major that the project
# holds it to.
#
# Run by ctest as `cmake -DSOURCE_DIR=<repo> -P check_ci_clang_tidy.cmake`. It reads .github/workflows/clang-tidy.yml,
# cmake/flywheel_dag_clang_tidy.cmake and CMakePresets.json. It needs no clang-tidy and no build, and runs in every tree.
#
# A workflow cannot be run here, so this holds what a run would find out late, or not at all. It holds:
#
#   - LLVM_MAJOR is set once, in the workflow's env, and is the major that cmake/flywheel_dag_clang_tidy.cmake registers the gate
#     with (_flywheel_dag_clang_tidy_major). The Ubuntu leg's apt source, package and tool are named by $LLVM_MAJOR, and no line has a
#     literal major in their place or in a step's name. A major moved in one place only is otherwise a red clang_tidy_version
#     after the push.
#   - The configure turns the gate on (-DFLYWHEEL_DAG_CLANG_TIDY=). Without it a leg configures, selects no clang_tidy_ test, and
#     only the test preset's noTestsAction is left to notice. The same line carries the strict CMake flags, -Werror=author
#     -Werror=deprecated: test_cmake_diagnostics holds their spelling and effect, but reads the whole file, so a comment that
#     says them meets it.
#   - Every ctest command selects the gate's tests, -R '^clang_tidy_'. Without it the job would run the whole suite, which nothing
#     has built. One of them runs the tests (the others may only list them, -N), and it is not piped: a pipe's status is its
#     last command's.
#   - Nothing lets the job pass by failing: no continue-on-error, no `|| true`. A lint step that cannot fail gates nothing.
#   - Every preset of the matrix names a configure preset and a test preset of CMakePresets.json, every --preset argument is the
#     matrix's, so that the tree that is configured is the tree that is tested, and the matrix has a Linux leg and a macOS leg
#     (the macOS leg is the only gate on what the macOS SDK adds).
#
# It reads the workflow's code and not its comments (whole-line ones are dropped first), so a rule is not met by a comment, and a
# comment that mentions a rule does not break it. What it does not hold: the triggers and their paths-ignore lists
# (test_ci_paths).
#
# SOURCE_DIR is a parameter so that a hand-made change can be made to a scratch copy of the repo and must fail here.

cmake_minimum_required(VERSION 4.0)

if(NOT DEFINED SOURCE_DIR)
    message(FATAL_ERROR "usage: cmake -DSOURCE_DIR=<repo> -P check_ci_clang_tidy.cmake")
endif()

set(workflow     "${SOURCE_DIR}/.github/workflows/clang-tidy.yml")
set(registration "${SOURCE_DIR}/cmake/flywheel_dag_clang_tidy.cmake")
set(presets      "${SOURCE_DIR}/CMakePresets.json")

set(failures "")
foreach(file IN ITEMS "${workflow}" "${registration}" "${presets}")
    if(NOT EXISTS "${file}")
        string(APPEND failures "\n  ${file} does not exist")
    endif()
endforeach()
if(NOT "${failures}" STREQUAL "")
    message(FATAL_ERROR "the clang-tidy workflow is wrong:${failures}")
endif()

# ── the major that the project holds the gate to ─────────────────────────────
file(READ "${registration}" registration_text)
string(REGEX REPLACE "\n[ \t]*#[^\n]*" "\n" registration_code "\n${registration_text}")
string(REGEX MATCHALL "set\\(_flywheel_dag_clang_tidy_major[^)]*\\)" major_sets "${registration_code}")
list(LENGTH major_sets major_set_count)
if(NOT major_set_count EQUAL 1)
    message(FATAL_ERROR "${registration} sets _flywheel_dag_clang_tidy_major ${major_set_count} times, and must set it once")
endif()
if(NOT "${major_sets}" MATCHES "^set\\(_flywheel_dag_clang_tidy_major[ \t]+([0-9]+)\\)$")
    message(FATAL_ERROR "${registration} does not set _flywheel_dag_clang_tidy_major to a number: ${major_sets}")
endif()
set(gate_major "${CMAKE_MATCH_1}")

# ── the workflow's code ──────────────────────────────────────────────────────
file(READ "${workflow}" yaml)
# Whole-line comments go. Then the characters that a CMake list gives meaning to, so that a line can be an item of one.
string(REGEX REPLACE "\n[ \t]*#[^\n]*" "\n" code "\n${yaml}")
string(REPLACE ";" "<semicolon>" code "${code}")
string(REPLACE "[" "<lbracket>" code "${code}")
string(REPLACE "]" "<rbracket>" code "${code}")

# A reference to a variable of the workflow's env, in the three forms a workflow can write one: $NAME, ${NAME} and
# ${{ env.NAME }}.
function(env_reference name out)
    set(${out} "(\\$${name}|\\$\\{${name}\\}|\\$\\{\\{ *env\\.${name} *\\}\\})" PARENT_SCOPE)
endfunction()
env_reference(LLVM_MAJOR major_ref)

# One character of the release's codename as the source line writes it: the letters of 'resolute', or $VERSION_CODENAME, or
# ${VERSION_CODENAME}. The class ends in $ because CMake reads a $ followed by a name and { as a reference of a kind it lacks.
set(codename "[A-Za-z_{}$]")

# The value of a variable that the env sets once: the one line, less its key, a trailing comment and quotes. <out> is empty and
# <out>_problem says why when the workflow sets the variable on no line or on several.
function(env_value name out)
    set(${out} "" PARENT_SCOPE)
    string(REGEX MATCHALL "\n[ \t]*${name}:[^\n]*" lines "${code}")
    list(LENGTH lines count)
    if(NOT count EQUAL 1)
        set(${out}_problem "${name} is set on ${count} lines of the workflow, and must be set once, in its env" PARENT_SCOPE)
        return()
    endif()
    string(REGEX REPLACE "^[\n \t]*${name}:[ \t]*" "" value "${lines}")
    string(REGEX REPLACE "[ \t]+#.*$" "" value "${value}")
    string(STRIP "${value}" value)
    string(REGEX REPLACE "^\"(.*)\"$" "\\1" value "${value}")
    string(REGEX REPLACE "^'(.*)'$" "\\1" value "${value}")
    set(${out} "${value}" PARENT_SCOPE)
    set(${out}_problem "" PARENT_SCOPE)
endfunction()

# The values of a key that each leg of the matrix gives, in order: `- os: ubuntu-latest` and `preset: relwithdebinfo-linux-x86_64`.
function(matrix_values key out)
    string(REGEX MATCHALL "\n[ \t]*(-[ \t]+)?${key}:[^\n]*" lines "${code}")
    set(values "")
    foreach(line IN LISTS lines)
        string(REGEX REPLACE "^\n[ \t]*(-[ \t]+)?${key}:[ \t]*" "" value "${line}")
        string(REGEX REPLACE "[ \t]+#.*$" "" value "${value}")
        string(STRIP "${value}" value)
        string(REGEX REPLACE "^\"(.*)\"$" "\\1" value "${value}")
        string(REGEX REPLACE "^'(.*)'$" "\\1" value "${value}")
        list(APPEND values "${value}")
    endforeach()
    set(${out} "${values}" PARENT_SCOPE)
endfunction()

# ── the major ────────────────────────────────────────────────────────────────
env_value(LLVM_MAJOR llvm_major)
if(NOT "${llvm_major_problem}" STREQUAL "")
    string(APPEND failures "\n  ${llvm_major_problem}")
elseif(NOT "${llvm_major}" MATCHES "^[0-9]+$")
    string(APPEND failures "\n  LLVM_MAJOR is '${llvm_major}', not a number")
elseif(NOT "${llvm_major}" STREQUAL "${gate_major}")
    string(APPEND failures
        "\n  LLVM_MAJOR is ${llvm_major} and _flywheel_dag_clang_tidy_major (cmake/flywheel_dag_clang_tidy.cmake) is ${gate_major}: "
        "clang_tidy_version would fail the run, so move both in one commit")
endif()

# No line names the tool or the repository with a literal major: they are $LLVM_MAJOR, so that there is one place to move.
string(REGEX MATCH "[^\n]*clang-tidy[ -][0-9][^\n]*" literal "${code}")
if(NOT "${literal}" STREQUAL "")
    string(STRIP "${literal}" literal)
    string(APPEND failures
        "\n  a line names the tool with a literal major, where it must say clang-tidy-$LLVM_MAJOR or leave the major out:"
        "\n      ${literal}")
endif()
string(REGEX MATCH "[^\n]*llvm-toolchain-${codename}*-[0-9][^\n]*" literal "${code}")
if(NOT "${literal}" STREQUAL "")
    string(STRIP "${literal}" literal)
    string(APPEND failures
        "\n  a line names apt.llvm.org's repository with a literal major, where it must say "
        "llvm-toolchain-<codename>-$LLVM_MAJOR:\n      ${literal}")
endif()

# And the places that must name it do.
if(NOT code MATCHES "apt-get install[^\n]*clang-tidy-${major_ref}")
    string(APPEND failures "\n  no 'apt-get install' line installs clang-tidy-$LLVM_MAJOR")
endif()
if(NOT code MATCHES "llvm-toolchain-${codename}*-${major_ref}")
    string(APPEND failures "\n  the apt source is not llvm-toolchain-<codename>-$LLVM_MAJOR")
endif()
if(NOT code MATCHES "CLANG_TIDY=clang-tidy-${major_ref}")
    string(APPEND failures "\n  the Ubuntu leg does not set CLANG_TIDY=clang-tidy-$LLVM_MAJOR")
endif()

# ── what the job holds ───────────────────────────────────────────────────────
string(REGEX MATCHALL "[^\n]*-DFLYWHEEL_DAG_CLANG_TIDY=[^\n]*" gate_lines "${code}")
if("${gate_lines}" STREQUAL "")
    string(APPEND failures
        "\n  the configure does not turn the gate on with -DFLYWHEEL_DAG_CLANG_TIDY=: a leg would select no clang_tidy_ test")
endif()
foreach(line IN LISTS gate_lines)
    if(NOT line MATCHES "-Werror=author -Werror=deprecated")
        string(STRIP "${line}" shown)
        string(APPEND failures
            "\n  the configure does not carry -Werror=author -Werror=deprecated, so a CMake author warning or deprecation would "
            "not fail the leg (test_cmake_diagnostics holds their spelling in the file, and a comment meets that):"
            "\n      ${shown}")
    endif()
endforeach()

string(REGEX MATCHALL "[^\n]*ctest[^\n]*" ctest_lines "${code}")
list(LENGTH ctest_lines ctest_count)
if(ctest_count EQUAL 0)
    string(APPEND failures "\n  the workflow never runs ctest")
endif()
set(runs_tests FALSE)
foreach(line IN LISTS ctest_lines)
    if(NOT line MATCHES "-R '\\^clang_tidy_'")
        string(STRIP "${line}" shown)
        string(APPEND failures "\n  a ctest command does not select the gate's tests with -R '^clang_tidy_':\n      ${shown}")
    endif()
    if(NOT line MATCHES "[ \t]-N([ \t]|$)")    # a command that runs the tests, and not one that only lists them
        set(runs_tests TRUE)
        if(line MATCHES "\\|")
            string(STRIP "${line}" shown)
            string(APPEND failures
                "\n  the ctest command that runs the tests is piped, and a pipe's status is its last command's, so a failing run "
                "would pass:\n      ${shown}")
        endif()
    endif()
endforeach()
if(ctest_count GREATER 0 AND NOT runs_tests)
    string(APPEND failures "\n  every ctest command only lists tests (-N): none runs them")
endif()

# ── nothing lets the job pass by failing ─────────────────────────────────────
if(code MATCHES "continue-on-error")
    string(APPEND failures "\n  the workflow says continue-on-error: a step that cannot fail gates nothing")
endif()
if(code MATCHES "\\|\\|[ \t]*(true|:)")
    string(APPEND failures "\n  a line ends in '|| true' (or '|| :'): a command that cannot fail gates nothing")
endif()

# ── the matrix ───────────────────────────────────────────────────────────────
matrix_values(os oses)
matrix_values(preset matrix_presets)
list(LENGTH oses os_count)
list(LENGTH matrix_presets preset_count)
if(os_count EQUAL 0 OR preset_count EQUAL 0)
    string(APPEND failures "\n  the matrix has no leg: found ${os_count} 'os:' and ${preset_count} 'preset:' lines")
elseif(NOT os_count EQUAL preset_count)
    string(APPEND failures "\n  the matrix has ${os_count} 'os:' lines and ${preset_count} 'preset:' lines, one of each per leg")
endif()
set(has_linux FALSE)
set(has_macos FALSE)
foreach(os IN LISTS oses)
    if(os MATCHES "^ubuntu")
        set(has_linux TRUE)
    elseif(os MATCHES "^macos")
        set(has_macos TRUE)
    endif()
endforeach()
if(NOT has_linux)
    string(APPEND failures "\n  the matrix has no Ubuntu leg (an 'os:' that starts ubuntu)")
endif()
if(NOT has_macos)
    string(APPEND failures
        "\n  the matrix has no macOS leg (an 'os:' that starts macos), the only gate on what the macOS SDK adds")
endif()

file(READ "${presets}" presets_json)
foreach(preset IN LISTS matrix_presets)
    foreach(section IN ITEMS configurePresets testPresets)
        string(JSON entries ERROR_VARIABLE problem LENGTH "${presets_json}" ${section})
        if(problem)
            string(APPEND failures "\n  CMakePresets.json has no ${section} (${problem})")
            continue()
        endif()
        set(found FALSE)
        if(entries GREATER 0)
            math(EXPR last "${entries} - 1")
            foreach(i RANGE ${last})
                string(JSON entry_name GET "${presets_json}" ${section} ${i} name)
                if("${entry_name}" STREQUAL "${preset}")
                    set(found TRUE)
                endif()
            endforeach()
        endif()
        if(NOT found)
            string(APPEND failures
                "\n  the matrix preset '${preset}' is not the name of any of CMakePresets.json's ${section}")
        endif()
    endforeach()
endforeach()

# Every --preset is the matrix's, so that what is configured is what is tested.
string(REGEX MATCHALL "[^\n]*--preset[^\n]*" preset_uses "${code}")
list(LENGTH preset_uses preset_use_count)
if(preset_use_count EQUAL 0)
    string(APPEND failures "\n  the workflow never says --preset")
endif()
foreach(line IN LISTS preset_uses)
    if(NOT line MATCHES "--preset[ \t]+\\$\\{\\{ *matrix\\.preset *\\}\\}")
        string(STRIP "${line}" shown)
        string(APPEND failures "\n  a --preset argument is not the matrix's preset:\n      ${shown}")
    endif()
endforeach()

if(NOT "${failures}" STREQUAL "")
    message(FATAL_ERROR "the clang-tidy workflow (${workflow}) is wrong:${failures}")
endif()
string(REPLACE ";" ", " presets_text "${matrix_presets}")
message(STATUS "the clang-tidy workflow runs LLVM ${gate_major} (the project's) on ${preset_count} legs (${presets_text}), "
               "with the gate on; its ${ctest_count} ctest command(s) select only clang_tidy_ tests, and nothing lets it "
               "pass by failing")
