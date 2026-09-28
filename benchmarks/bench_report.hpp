// =============================================================================
// flywheel-dag — A header-only C++17 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

#pragma once

// bench_report.hpp — bench_hot_path's command line and its two output formats.
//
// Kept apart from the measurement so both are unit-tested without running a
// benchmark (test_bench_report.cpp). CI diffs the --invariants output against
// a committed file, so that format must be exact and must never carry a timing.

#include <string>
#include <vector>

namespace bench {

/// Measured cycles per row when the command line gives no count.
constexpr long kDefaultCycles = 200000;

/// One row's results. nsPerCycle is a measurement; the other three are exact.
struct Row {
    std::string name;
    double      nsPerCycle     = 0.0;
    double      allocsPerCycle = 0.0;
    long        callbacks      = 0;
    double      checksum       = 0.0;
};

/// What the command line asked for. A non-empty `error` means it cannot run.
struct Options {
    long        cycles     = kDefaultCycles;
    bool        invariants = false;
    std::string error;
};

/// Parses `bench_hot_path [cycles] [--invariants]`, the flag in either position.
Options parseArgs(int argc, const char* const argv[]);

/// The table for people: a heading, then one line per row.
std::string table(long cycles, const std::vector<Row>& rows);

/// The exact columns only: `cycles=N`, then one line per row. Every double is
/// printed at %.17g, which round-trips, and no timing appears, so the text is
/// the same on every run of a build.
std::string invariants(long cycles, const std::vector<Row>& rows);

}  // namespace bench
