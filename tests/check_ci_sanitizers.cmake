# check_ci_sanitizers.cmake — test_ci_sanitizers: the sanitizer workflow runs the whole suite of a sanitizer tree, with the options
# that make a run mean something.
#
# Run by ctest as `cmake -DSOURCE_DIR=<repo> -P check_ci_sanitizers.cmake`. It reads .github/workflows/sanitizers.yml,
# CMakePresets.json and cmake/flywheel_dag_sanitize.cmake. It needs no sanitizer and no build, and runs in every tree.
#
# A workflow cannot be run here, so this holds what a run would find out late, or not at all: a green run that sanitised nothing, or
# that skipped what it was there to run. It holds:
#
#   - Every mode of the matrix is a sanitizer that cmake/flywheel_dag_sanitize.cmake accepts and that switches something on (OFF is
#     accepted by the module, and a run of it is a plain build), and <mode>-linux-x86_64 is a configure, a build and a test preset
#     of CMakePresets.json. The job's PRESET is that name, and every --preset argument is PRESET, so that the tree that is
#     configured is the tree that is built and tested. The job runs on Ubuntu: LeakSanitizer exists only on Linux.
#   - The configure line carries the strict CMake flags, -Werror=author -Werror=deprecated (test_cmake_diagnostics holds their
#     spelling and effect, but reads the whole file, so a comment that says them meets it) and names no FLYWHEEL_DAG_SANITIZE,
#     CMAKE_CXX_FLAGS or CMAKE_BUILD_TYPE: the preset is the one place that sets the mode and the build type, and a second one would
#     double an option or quietly override it.
#   - Every ctest command runs the whole suite: it selects and excludes nothing (-R, -E, -L, -LE, -I, --rerun-failed, and their long
#     forms), and it is not piped, since a pipe's status is its last command's. At least one ctest command runs tests, as opposed to
#     listing them (-N). The canaries and test_sanitizer_coverage are part of the suite, and a run that leaves them out is a
#     sanitizer run that cannot say it sanitised anything.
#   - Nothing lets the job pass by failing: no continue-on-error, no `|| true`, no cache of any kind (a dependency tree restored from
#     the other mode is silent partial instrumentation), and no suppressions= in a sanitizer's options (a suppression lands only with
#     a comment that names its cause, and this check is edited with it).
#   - ASAN_OPTIONS has detect_leaks=1, and TSAN_OPTIONS has halt_on_error=1 (ThreadSanitizer then stops at its first report, so the
#     finding is the end of the log and not buried under what follows).
#   - The job has a timeout-minutes, and fail-fast is off, so that one sanitizer red does not hide the other.
#   - The workflow runs on a schedule, so that a finding does not wait for someone to start a run, and on demand, so that a finding can
#     be reproduced on the runner.
#   - The step that reads and lowers vm.mmap_rnd_bits comes before the configure, and only the tsan job runs it. Every command on the
#     sysctl goes through sudo: the runner's user cannot read /proc/sys/vm/mmap_rnd_bits.
#
# It reads the workflow's code and not its comments (whole-line ones are dropped first), so a rule is not met by a comment, and a
# comment that mentions a rule does not break it.
#
# SOURCE_DIR is a parameter so that a hand-made change can be made to a scratch copy of the repo and must fail here.

cmake_minimum_required(VERSION 4.0)

if(NOT DEFINED SOURCE_DIR)
    message(FATAL_ERROR "usage: cmake -DSOURCE_DIR=<repo> -P check_ci_sanitizers.cmake")
endif()

set(workflow "${SOURCE_DIR}/.github/workflows/sanitizers.yml")
set(presets  "${SOURCE_DIR}/CMakePresets.json")
set(module   "${SOURCE_DIR}/cmake/flywheel_dag_sanitize.cmake")

set(failures "")
foreach(file IN ITEMS "${workflow}" "${presets}" "${module}")
    if(NOT EXISTS "${file}")
        string(APPEND failures "\n  ${file} does not exist")
    endif()
endforeach()
if(NOT "${failures}" STREQUAL "")
    message(FATAL_ERROR "the sanitizer workflow is wrong:${failures}")
endif()
include("${module}")

# ── the workflow's code ──────────────────────────────────────────────────────
file(READ "${workflow}" yaml)
# Whole-line comments go. Then the characters that a CMake list gives meaning to, so that a line can be an item of one.
string(REGEX REPLACE "\n[ \t]*#[^\n]*" "\n" code "\n${yaml}")
string(REPLACE ";" "<semicolon>" code "${code}")
string(REPLACE "[" "<lbracket>" code "${code}")
string(REPLACE "]" "<rbracket>" code "${code}")

# ── the matrix ───────────────────────────────────────────────────────────────
# The modes are the JSON arrays inside the kind: expression: ["asan","tsan"].
string(REGEX MATCHALL "\n[ \t]*kind:[^\n]*" kind_lines "${code}")
list(LENGTH kind_lines kind_count)
set(modes "")
if(NOT kind_count EQUAL 1)
    string(APPEND failures "\n  the matrix has ${kind_count} 'kind:' lines, and must have one")
else()
    string(REGEX MATCHALL "<lbracket>[^<]*<rbracket>" arrays "${kind_lines}")
    foreach(array IN LISTS arrays)
        string(REGEX MATCHALL "[A-Za-z_]+" words "${array}")
        foreach(word IN LISTS words)
            if(NOT word STREQUAL "lbracket" AND NOT word STREQUAL "rbracket")
                list(APPEND modes "${word}")
            endif()
        endforeach()
    endforeach()
    list(REMOVE_DUPLICATES modes)
endif()
if("${modes}" STREQUAL "")
    string(APPEND failures "\n  the matrix names no sanitizer: no [\"asan\"]-style array in its kind: line")
endif()

file(READ "${presets}" presets_json)
function(preset_exists section name out)
    set(${out} FALSE PARENT_SCOPE)
    string(JSON entries ERROR_VARIABLE problem LENGTH "${presets_json}" ${section})
    if(problem OR entries EQUAL 0)
        return()
    endif()
    math(EXPR last "${entries} - 1")
    foreach(i RANGE ${last})
        string(JSON entry_name GET "${presets_json}" ${section} ${i} name)
        if("${entry_name}" STREQUAL "${name}")
            set(${out} TRUE PARENT_SCOPE)
        endif()
    endforeach()
endfunction()

foreach(mode IN LISTS modes)
    # The module stops, naming the value, if it is not a mode.
    flywheel_dag_sanitizer_flags("${mode}" mode_compile mode_link)
    if("${mode_compile}" STREQUAL "" OR "${mode_link}" STREQUAL "")
        string(APPEND failures "\n  the matrix runs '${mode}', which switches no sanitizer on")
    endif()
    foreach(section IN ITEMS configurePresets buildPresets testPresets)
        preset_exists(${section} "${mode}-linux-x86_64" found)
        if(NOT found)
            string(APPEND failures "\n  ${mode}-linux-x86_64 is not the name of any of CMakePresets.json's ${section}")
        endif()
    endforeach()
endforeach()

# The job's PRESET is <kind>-linux-x86_64, and every --preset is PRESET.
string(REGEX MATCHALL "\n[ \t]*PRESET:[^\n]*" preset_lines "${code}")
list(LENGTH preset_lines preset_count)
if(NOT preset_count EQUAL 1)
    string(APPEND failures "\n  PRESET is set on ${preset_count} lines, and must be set once, in the job's env")
elseif(NOT preset_lines MATCHES "^\n[ \t]*PRESET:[ \t]*\\$\\{\\{ *matrix\\.kind *\\}\\}-linux-x86_64[ \t]*$")
    string(STRIP "${preset_lines}" shown)
    string(APPEND failures "\n  PRESET is not \${{ matrix.kind }}-linux-x86_64, the preset of the leg's kind on the runner:\n      ${shown}")
endif()
string(REGEX MATCHALL "[^\n]*--preset[^\n]*" preset_uses "${code}")
list(LENGTH preset_uses preset_use_count)
if(preset_use_count EQUAL 0)
    string(APPEND failures "\n  the workflow never says --preset")
endif()
foreach(line IN LISTS preset_uses)
    if(NOT line MATCHES "--preset[ \t]+\\$\\{\\{ *env\\.PRESET *\\}\\}")
        string(STRIP "${line}" shown)
        string(APPEND failures "\n  a --preset argument is not PRESET:\n      ${shown}")
    endif()
endforeach()

string(REGEX MATCHALL "\n[ \t]*runs-on:[^\n]*" runs_on "${code}")
foreach(line IN LISTS runs_on)
    if(NOT line MATCHES "runs-on:[ \t]*ubuntu")
        string(STRIP "${line}" shown)
        string(APPEND failures
            "\n  a job does not run on Ubuntu, the only OS with LeakSanitizer, and the presets are <kind>-linux-x86_64:\n      ${shown}")
    endif()
endforeach()
if("${runs_on}" STREQUAL "")
    string(APPEND failures "\n  no job says runs-on")
endif()

# ── the configure ────────────────────────────────────────────────────────────
string(REGEX MATCHALL "[^\n]*cmake --preset[^\n]*" configure_lines "${code}")
if("${configure_lines}" STREQUAL "")
    string(APPEND failures "\n  no line configures with cmake --preset")
endif()
foreach(line IN LISTS configure_lines)
    string(STRIP "${line}" shown)
    if(NOT line MATCHES "-Werror=author -Werror=deprecated")
        string(APPEND failures
            "\n  the configure does not carry -Werror=author -Werror=deprecated, so a CMake author warning or deprecation would "
            "not fail the run:\n      ${shown}")
    endif()
    if(NOT line MATCHES "-DFLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON")
        string(APPEND failures
            "\n  the configure does not carry -DFLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON, so a compiler warning would not fail the "
            "run:\n      ${shown}")
    endif()
    foreach(variable IN ITEMS FLYWHEEL_DAG_SANITIZE CMAKE_CXX_FLAGS CMAKE_BUILD_TYPE)
        if(line MATCHES "${variable}")
            string(APPEND failures
                "\n  the configure names ${variable}, which the preset sets: a second place would double or override it:"
                "\n      ${shown}")
        endif()
    endforeach()
endforeach()

# ── the ctest commands ───────────────────────────────────────────────────────
string(REGEX MATCHALL "[^\n]*ctest[^\n]*" ctest_lines "${code}")
list(LENGTH ctest_lines ctest_count)
if(ctest_count EQUAL 0)
    string(APPEND failures "\n  the workflow never runs ctest")
endif()
set(runs_tests FALSE)
foreach(line IN LISTS ctest_lines)
    string(STRIP "${line}" shown)
    if(line MATCHES "[ \t](-R|-E|-L|-LE|-I|-FA|--tests-regex|--exclude-regex|--label-regex|--label-exclude|--tests-information|--rerun-failed|--exclude-from-file)([ \t=]|$)")
        string(APPEND failures
            "\n  a ctest command selects or excludes tests, and the sanitizer run is the whole suite, canaries and "
            "test_sanitizer_coverage included:\n      ${shown}")
    endif()
    if(NOT line MATCHES "[ \t]-N([ \t]|$)")    # a command that runs the tests, and not one that only lists them
        set(runs_tests TRUE)
        if(line MATCHES "(^|[^|])\\|([^|]|$)")    # a pipe; the || of the rule below is not one
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
if(code MATCHES "actions/cache|cache:|restore-keys|ccache|sccache")
    string(APPEND failures
        "\n  the workflow caches something: a dependency or build tree restored from another mode is silent partial instrumentation")
endif()
if(code MATCHES "suppressions=")
    string(APPEND failures
        "\n  a sanitizer option says suppressions=: a suppression lands only with the issue that names its cause, and this "
        "check is edited with it")
endif()

# ── the runtime options ──────────────────────────────────────────────────────
if(NOT code MATCHES "\n[ \t]*ASAN_OPTIONS:[^\n]*(:|[ \t])detect_leaks=1([: \t\n]|$)")
    string(APPEND failures
        "\n  ASAN_OPTIONS does not set detect_leaks=1: leaks are what Linux can find and macOS cannot, which is why this runs on Linux")
endif()
if(NOT code MATCHES "\n[ \t]*TSAN_OPTIONS:[^\n]*(:|[ \t])halt_on_error=1([: \t\n]|$)")
    string(APPEND failures "\n  TSAN_OPTIONS does not set halt_on_error=1: a race would be reported and the run might go on")
endif()

# ── the bounds ───────────────────────────────────────────────────────────────
if(NOT code MATCHES "\n[ \t]*timeout-minutes:[ \t]*[0-9]+")
    string(APPEND failures "\n  the job has no timeout-minutes: a hung test runs until GitHub's six-hour cap")
endif()
if(NOT code MATCHES "\n[ \t]*fail-fast:[ \t]*false")
    string(APPEND failures "\n  the matrix does not say fail-fast: false: one sanitizer red would cancel the other")
endif()

# ── the mmap randomisation step ──────────────────────────────────────────────
string(FIND "${code}" "vm.mmap_rnd_bits" rnd_at)
string(FIND "${code}" "cmake --preset" configure_at)
if(rnd_at EQUAL -1)
    string(APPEND failures "\n  no step reads vm.mmap_rnd_bits: ThreadSanitizer dies at start-up on a kernel with too much randomisation")
elseif(NOT configure_at EQUAL -1 AND rnd_at GREATER configure_at)
    string(APPEND failures "\n  the vm.mmap_rnd_bits step comes after the configure: it must come before anything is built")
endif()
if(NOT rnd_at EQUAL -1)
    # The step: from the line that names it back to the "- name:" before it. It must say that it is for tsan.
    string(SUBSTRING "${code}" 0 ${rnd_at} before)
    string(REGEX MATCHALL "- name:[^\n]*" names "${before}")
    list(POP_BACK names step_name)
    string(FIND "${before}" "${step_name}" step_at)
    string(SUBSTRING "${before}" ${step_at} -1 step_head)
    if(NOT step_head MATCHES "if:[ \t]*matrix\\.kind == 'tsan'")
        string(APPEND failures "\n  the vm.mmap_rnd_bits step does not say 'if: matrix.kind == 'tsan'': only the tsan job needs it")
    endif()
endif()
# The runner's user cannot read /proc/sys/vm/mmap_rnd_bits (ubuntu-latest answers "Permission denied"), and a write needs root, so a
# command on the sysctl goes through sudo. An echo of the value is not a command on it, unless it substitutes one.
string(REGEX MATCHALL "[^\n]*mmap_rnd_bits[^\n]*" rnd_lines "${code}")
foreach(line IN LISTS rnd_lines)
    if(NOT line MATCHES "sudo" AND (NOT line MATCHES "^[ \t]*echo[ \t]" OR line MATCHES "[$][(]|`"))
        string(STRIP "${line}" shown)
        string(APPEND failures
            "\n  a command on vm.mmap_rnd_bits does not go through sudo, and the runner's user cannot read it:\n      ${shown}")
    endif()
endforeach()

# ── the triggers ─────────────────────────────────────────────────────────────
if(NOT code MATCHES "\n[ \t]*workflow_dispatch:")
    string(APPEND failures "\n  the workflow has no workflow_dispatch trigger: a finding could not be reproduced on the runner")
endif()
# A cron has five fields; CMake's regex has no counted repetition, so each is written out. The comment lines that were dropped leave
# empty lines between the key and its item.
set(cron_field "[0-9*/,-]+")
if(NOT code MATCHES "\n[ \t]*schedule:[ \t]*(\n[ \t]*)+-[ \t]*cron:[ \t]*[\"']${cron_field} ${cron_field} ${cron_field} ${cron_field} ${cron_field}[\"']")
    string(APPEND failures
        "\n  the workflow has no schedule with a cron of five fields: a finding would wait for someone to start a run")
endif()

if(NOT "${failures}" STREQUAL "")
    message(FATAL_ERROR "the sanitizer workflow (${workflow}) is wrong:${failures}")
endif()
string(REPLACE ";" ", " modes_text "${modes}")
message(STATUS "the sanitizer workflow runs ${modes_text} on Ubuntu, with the whole suite of each tree (${ctest_count} ctest "
               "command(s), none selecting or excluding a test), and nothing lets it pass by failing")
