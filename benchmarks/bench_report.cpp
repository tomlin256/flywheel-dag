// =============================================================================
// flywheel-dag — A header-only C++17 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

#include "bench_report.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>

namespace bench {

namespace {

/// %.17g, the shortest precision that round-trips every double.
std::string exact(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.17g", v);
    return buf;
}

/// A positive cycle count, or false for anything else, overflow included.
bool parseCycles(const std::string& text, long& cycles) {
    if (text.empty()) return false;
    errno = 0;
    char* end = nullptr;
    const long value = std::strtol(text.c_str(), &end, 10);
    if (errno == ERANGE || *end != '\0' || value <= 0) return false;
    cycles = value;
    return true;
}

}  // namespace

Options parseArgs(int argc, const char* const argv[]) {
    Options options;
    bool haveCycles = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--invariants") {
            options.invariants = true;
        } else if (arg.rfind("--", 0) == 0) {
            options.error = "unknown flag " + arg;
            return options;
        } else if (haveCycles) {
            options.error = "more than one cycle count: " + arg;
            return options;
        } else if (!parseCycles(arg, options.cycles)) {
            options.error = "cycles must be a positive integer, not '" + arg + "'";
            return options;
        } else {
            haveCycles = true;
        }
    }
    return options;
}

std::string table(long cycles, const std::vector<Row>& rows) {
    char line[256];
    std::snprintf(line, sizeof line,
                  "flywheel-dag hot-path benchmark: %ld measured cycles per row\n"
                  "ns/cycle depends on the machine; allocs/cycle, callbacks and "
                  "checksum are exact\n\n",
                  cycles);
    std::string out = line;
    std::snprintf(line, sizeof line, "%-12s %10s %13s %12s  %s\n",
                  "row", "ns/cycle", "allocs/cycle", "callbacks", "checksum");
    out += line;
    for (const Row& r : rows) {
        // %g, not a fixed-point format: one allocation in 200,000 cycles must
        // read 5e-06, not 0.0.
        std::snprintf(line, sizeof line, "%-12s %10.1f %13g %12ld  %s\n",
                      r.name.c_str(), r.nsPerCycle, r.allocsPerCycle, r.callbacks,
                      exact(r.checksum).c_str());
        out += line;
    }
    return out;
}

std::string invariants(long cycles, const std::vector<Row>& rows) {
    std::string out = "cycles=" + std::to_string(cycles) + "\n";
    for (const Row& r : rows) {
        out += r.name + " allocs/cycle=" + exact(r.allocsPerCycle)
             + " callbacks=" + std::to_string(r.callbacks)
             + " checksum=" + exact(r.checksum) + "\n";
    }
    return out;
}

}  // namespace bench
