# check_tsan_fences.cmake — test_tsan_fences: the engine issues its standalone fences through
# dag::threadFence(), and a translation unit that uses them compiles under -fsanitize=thread without
# GCC's -Wtsan.
#
# Run by ctest as
#
#   cmake -DCXX=<compiler> -DCXX_ID=<id> -DINCLUDE_DIR=<source>/include -DSPDLOG_SOURCE_DIR=<dir>
#         -DNLOHMANN_JSON_SOURCE_DIR=<dir> -DWORK_DIR=<dir> -P check_tsan_fences.cmake
#
# and registered at the top level for GCC and Clang. ThreadSanitizer does not model
# std::atomic_thread_fence, and GCC (11 and later) says so with -Wtsan at every fence it compiles under
# -fsanitize=thread, which -Werror makes an error. CycleSeqLock's four fences order atomics only, where the
# warning costs nothing, so they go through dag::threadFence<Order>() (dag_thread_fence.hpp), which turns
# the warning off for that one call on GCC. Nothing else would notice a bare
# std::atomic_thread_fence, or a pragma that does not work: the suite is not built under ThreadSanitizer,
# and the default build is not -Werror.
#
# Four things are held:
#
#   the scan     no header or .inl under include/flywheel/ but dag_thread_fence.hpp says
#                atomic_thread_fence. The compile cases only see what they instantiate; the scan sees every
#                file, on every compiler.
#   the helper   a unit that includes only dag_thread_fence.hpp and issues a fence of every order compiles.
#   the engine   a unit that steps an Engine (which opens a WriteScope) and reads through its CycleSeqLock
#                compiles.
#   the control  (GCC) a bare std::atomic_thread_fence does not compile, and says -Werror=tsan. Without it a
#                pass of the two cases above would say nothing about the flags. If GCC stops warning about
#                fences, the pragma is dead code, and this fails once to say so.
#
# The compile cases run at -O0 and -O2, because the sanitizer pass runs at another place in each, with -c and
# -fsanitize=thread: nothing is linked, so no ThreadSanitizer runtime is needed. They pass only the flags that
# bear on the property: -Werror=tsan and -Werror=pragmas on GCC (the pragma's option name is spelt right), and
# -Werror=unknown-warning-option on Clang (the guard that keeps the pragma from Clang is there). Not
# -Wall -Wextra -Wpedantic -Werror: a newer compiler's unrelated warning must not fail this test, because a
# contributor's build is warning-lenient by design.
#
# A compiler that refuses -fsanitize=thread on an empty unit skips the compile cases, and the status line says
# so; the scan still runs.

cmake_minimum_required(VERSION 4.0)

foreach(var IN ITEMS CXX CXX_ID INCLUDE_DIR SPDLOG_SOURCE_DIR NLOHMANN_JSON_SOURCE_DIR WORK_DIR)
    if(NOT ${var})
        message(FATAL_ERROR "usage: cmake -DCXX=<compiler> -DCXX_ID=<id> -DINCLUDE_DIR=<dir> "
                            "-DSPDLOG_SOURCE_DIR=<dir> -DNLOHMANN_JSON_SOURCE_DIR=<dir> -DWORK_DIR=<dir> "
                            "-P check_tsan_fences.cmake")
    endif()
endforeach()

set(problems "")

# ── the scan ─────────────────────────────────────────────────────────────────
# The glob is the list of files, so a file added later is covered, and finding none is an error, so a wrong
# path cannot pass by checking nothing.
file(GLOB_RECURSE sources RELATIVE "${INCLUDE_DIR}"
     "${INCLUDE_DIR}/flywheel/*.hpp" "${INCLUDE_DIR}/flywheel/*.inl")
list(SORT sources)
list(LENGTH sources source_count)
if(source_count EQUAL 0)
    message(FATAL_ERROR "found no header under ${INCLUDE_DIR}/flywheel")
endif()

set(helper flywheel/dag_thread_fence.hpp)
if(NOT helper IN_LIST sources)
    string(APPEND problems "  include/${helper} is missing\n")
endif()

foreach(source IN LISTS sources)
    if(source STREQUAL helper)
        continue()
    endif()
    file(READ "${INCLUDE_DIR}/${source}" text)
    # Each occurrence is reported with its line: cut the text after it, and count the lines cut.
    set(lines_before 0)
    while(TRUE)
        string(FIND "${text}" "atomic_thread_fence" at)
        if(at EQUAL -1)
            break()
        endif()
        string(SUBSTRING "${text}" 0 ${at} before)
        string(REGEX MATCHALL "\n" breaks "${before}")
        list(LENGTH breaks break_count)
        math(EXPR line "${lines_before} + ${break_count} + 1")
        string(APPEND problems "  include/${source}:${line} says atomic_thread_fence; issue the fence "
                               "through dag::threadFence<Order>() (dag_thread_fence.hpp)\n")
        math(EXPR lines_before "${lines_before} + ${break_count}")
        math(EXPR resume "${at} + 1")
        string(SUBSTRING "${text}" ${resume} -1 text)
    endwhile()
endforeach()

# ── the compile cases ────────────────────────────────────────────────────────
file(REMOVE_RECURSE "${WORK_DIR}")
file(MAKE_DIRECTORY "${WORK_DIR}")

set(unit_empty [[
int main() { return 0; }
]])

set(unit_fence [[
#include <flywheel/dag_thread_fence.hpp>

void issueEveryOrder() {
    dag::threadFence<std::memory_order_relaxed>();
    dag::threadFence<std::memory_order_acquire>();
    dag::threadFence<std::memory_order_release>();
    dag::threadFence<std::memory_order_acq_rel>();
    dag::threadFence<std::memory_order_seq_cst>();
}
]])

# step() runs a cycle, which opens a WriteScope; readConsistent() is the reader's two fences.
set(unit_engine [[
#include <flywheel/dag_engine.hpp>

int useTheEngine() {
    dag::async::Engine engine;
    auto x = engine.makeInput<int>("x", 0);
    x->set(1);
    engine.step();
    const auto lock = engine.cycleSeqLock();
    return lock->readConsistent([] { return 1; });
}
]])

set(unit_control [[
#include <atomic>

void bareFence() { std::atomic_thread_fence(std::memory_order_seq_cst); }
]])

set(common_flags -std=c++23 -DSPDLOG_COMPILED_LIB "-I${INCLUDE_DIR}"
    -isystem "${SPDLOG_SOURCE_DIR}/include" -isystem "${NLOHMANN_JSON_SOURCE_DIR}/include")
if(CXX_ID STREQUAL "GNU")
    set(diagnostic_flags -Werror=tsan -Werror=pragmas)
else()
    set(diagnostic_flags -Werror=unknown-warning-option)
endif()

# compile_unit(<unit> <extra flags...>): compiles the text of unit_<unit> to an object, and sets result and
# errors in the caller's scope.
function(compile_unit unit)
    file(WRITE "${WORK_DIR}/${unit}.cpp" "${unit_${unit}}")
    execute_process(
        COMMAND "${CXX}" ${common_flags} ${ARGN} -c "${WORK_DIR}/${unit}.cpp" -o "${WORK_DIR}/${unit}.o"
        RESULT_VARIABLE result
        ERROR_VARIABLE  errors
        OUTPUT_QUIET)
    set(result "${result}" PARENT_SCOPE)
    set(errors "${errors}" PARENT_SCOPE)
endfunction()

# first_error(<output> <out-var>): a compiler's output opens with the chain of includes, and the verdict is its
# first error line. Without one, the head of the output.
function(first_error output out)
    string(REGEX MATCH "[^\n]*: error: [^\n]*" line "${output}")
    if(line STREQUAL "")
        string(SUBSTRING "${output}" 0 300 line)
    endif()
    set(${out} "${line}" PARENT_SCOPE)
endfunction()

# A compiler that cannot compile an empty unit at all is broken, not unsupported.
compile_unit(empty)
if(NOT result EQUAL 0)
    first_error("${errors}" shown)
    message(FATAL_ERROR "${CXX} cannot compile an empty unit:\n${shown}")
endif()

compile_unit(empty -fsanitize=thread)
if(result EQUAL 0)
    foreach(opt IN ITEMS -O0 -O2)
        foreach(unit IN ITEMS fence engine)
            compile_unit(${unit} -fsanitize=thread ${diagnostic_flags} ${opt})
            if(NOT result EQUAL 0)
                first_error("${errors}" shown)
                string(APPEND problems
                    "  the ${unit} unit does not compile at ${opt} under -fsanitize=thread:\n    ${shown}\n")
            endif()
        endforeach()

        if(CXX_ID STREQUAL "GNU")
            compile_unit(control -fsanitize=thread ${diagnostic_flags} ${opt})
            if(result EQUAL 0)
                string(APPEND problems
                    "  the control compiled at ${opt}: GCC no longer warns about a bare std::atomic_thread_fence "
                    "under -fsanitize=thread, so the pragma in dag_thread_fence.hpp has nothing left to silence, "
                    "and the cases above prove nothing. Delete the pragma, and this control.\n")
            elseif(NOT errors MATCHES "Werror=tsan")
                first_error("${errors}" shown)
                string(APPEND problems
                    "  the control failed at ${opt}, but not with -Werror=tsan:\n    ${shown}\n")
            endif()
        endif()
    endforeach()
    set(compile_note "the helper and the engine compile under -fsanitize=thread at -O0 and -O2")
    if(CXX_ID STREQUAL "GNU")
        string(APPEND compile_note ", and a bare fence still draws -Wtsan")
    endif()
else()
    first_error("${errors}" shown)
    set(compile_note "the compile cases were skipped: ${CXX} does not compile an empty unit with -fsanitize=thread:\n  ${shown}")
endif()

if(NOT problems STREQUAL "")
    message(FATAL_ERROR "the engine's standalone fences do not go through dag::threadFence() cleanly:\n${problems}")
endif()
message(STATUS "all ${source_count} headers and .inl files keep their fences behind dag::threadFence(); ${compile_note}")
