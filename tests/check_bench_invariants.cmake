# check_bench_invariants.cmake — test_bench_invariants: the committed file of the benchmark's exact columns says
# what its rows promise by construction.
#
# Run by ctest as
#
#   cmake -DSOURCE_DIR=<source> -P check_bench_invariants.cmake
#
# CI diffs `bench_hot_path --invariants` against benchmarks/expected_invariants.txt, so that file is where a change
# in what the rows did is read. The diff holds the benchmark to the file, and nothing holds the file: one that is
# regenerated to make the diff pass takes a regression in with it. Four things are true of the rows however the
# engine computes them, and the file is held to each:
#
#   the cycle count   the first line is cycles=200000, the count of a run with no argument, which is the run CI
#                     makes.
#   allocations       every row reads allocs/cycle=0. The engine's steady state allocates nothing, so any other
#                     figure is a regression in the engine, and the fix is there.
#   the quiet rows    idle-queues and ops-held read callbacks=0 and checksum=0. Nothing is posted to the first, and
#                     nothing below a holder moves in the second, so a callback in either is a delivery of nothing.
#   the op pair       ops-compute and ops-moving read the same callbacks and the same checksum. They fold the same
#                     eight inputs, one with a ComputeNode and one with a SumNode.
#
# A row that a rule names and the file lacks is a problem, and so is a line that is not a row, so a renamed row or
# a changed format cannot pass by matching nothing.
#
# INVARIANTS defaults to the checkout's file. It is a parameter so that a hand-made change can be made to a copy and
# must fail here.

cmake_minimum_required(VERSION 4.0)

if(NOT SOURCE_DIR)
    message(FATAL_ERROR "usage: cmake -DSOURCE_DIR=<dir> [-DINVARIANTS=<file>] -P check_bench_invariants.cmake")
endif()
if(NOT INVARIANTS)
    set(INVARIANTS "${SOURCE_DIR}/benchmarks/expected_invariants.txt")
endif()
if(NOT EXISTS "${INVARIANTS}")
    message(FATAL_ERROR "${INVARIANTS} does not exist")
endif()

set(default_cycles 200000)
set(quiet_rows idle-queues ops-held)
set(pair ops-compute ops-moving)

file(STRINGS "${INVARIANTS}" lines)
list(LENGTH lines line_count)
set(problems "")

if(line_count EQUAL 0)
    string(APPEND problems "  the file is empty\n")
else()
    list(GET lines 0 first)
    if(NOT first STREQUAL "cycles=${default_cycles}")
        string(APPEND problems "  the first line is '${first}', not cycles=${default_cycles}\n")
    endif()
    list(REMOVE_AT lines 0)
endif()

# Each row line gives the row's name three variables: <name>_allocs, <name>_callbacks and <name>_checksum.
set(rows "")
foreach(line IN LISTS lines)
    if(NOT line MATCHES "^([a-z][a-z0-9-]*) allocs/cycle=([^ ]+) callbacks=([0-9]+) checksum=([^ ]+)$")
        string(APPEND problems "  '${line}' is not a row\n")
        continue()
    endif()
    set(row "${CMAKE_MATCH_1}")
    if(row IN_LIST rows)
        string(APPEND problems "  ${row} has two lines\n")
        continue()
    endif()
    list(APPEND rows "${row}")
    set(${row}_allocs    "${CMAKE_MATCH_2}")
    set(${row}_callbacks "${CMAKE_MATCH_3}")
    set(${row}_checksum  "${CMAKE_MATCH_4}")

    if(NOT ${row}_allocs STREQUAL "0")
        string(APPEND problems "  ${row} reads allocs/cycle=${${row}_allocs}: the engine's steady state allocates "
                               "nothing, so this is a regression in the engine, to fix there\n")
    endif()
endforeach()
list(LENGTH rows row_count)
if(row_count EQUAL 0)
    string(APPEND problems "  the file has no row\n")
endif()

foreach(row IN LISTS quiet_rows)
    if(NOT row IN_LIST rows)
        string(APPEND problems "  ${row} is missing\n")
    elseif(NOT ${row}_callbacks STREQUAL "0" OR NOT ${row}_checksum STREQUAL "0")
        string(APPEND problems "  ${row} reads callbacks=${${row}_callbacks} checksum=${${row}_checksum}: nothing "
                               "moves in that row, so it delivers nothing\n")
    endif()
endforeach()

list(GET pair 0 fold_compute)
list(GET pair 1 fold_op)
if(NOT fold_compute IN_LIST rows OR NOT fold_op IN_LIST rows)
    string(APPEND problems "  ${fold_compute} or ${fold_op} is missing\n")
elseif(NOT ${fold_compute}_callbacks STREQUAL ${fold_op}_callbacks
       OR NOT ${fold_compute}_checksum STREQUAL ${fold_op}_checksum)
    string(APPEND problems "  ${fold_compute} reads callbacks=${${fold_compute}_callbacks} "
                           "checksum=${${fold_compute}_checksum} and ${fold_op} reads "
                           "callbacks=${${fold_op}_callbacks} checksum=${${fold_op}_checksum}: the two fold the same "
                           "inputs, so they deliver the same\n")
endif()

if(NOT problems STREQUAL "")
    message(FATAL_ERROR "${INVARIANTS} does not say what the rows promise:\n${problems}"
            "Regenerate the file only when a change in what a row computes is the point of the commit. A figure "
            "above breaks something that is true however the rows are computed, so fix what changed it.")
endif()
message(STATUS "${row_count} rows: none allocates, the quiet rows deliver nothing, and the two folds agree")
