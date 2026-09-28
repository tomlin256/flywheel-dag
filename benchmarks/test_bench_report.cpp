// =============================================================================
// flywheel-dag — A header-only C++17 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

// test_bench_report.cpp — bench_hot_path's command line and output formats.
//
// CI diffs `bench_hot_path --invariants` against a committed file, so that
// output must round-trip every double and must never carry a timing. The
// timings themselves are not tested: a wall-clock threshold would be flaky.

#include "bench_report.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

namespace {

/// parseArgs over `args`, with the program name in front as main() sees it.
bench::Options parse(std::vector<const char*> args) {
    args.insert(args.begin(), "bench_hot_path");
    return bench::parseArgs(static_cast<int>(args.size()), args.data());
}

/// The text after `key` in `text`, up to the next space or newline.
std::string field(const std::string& text, const std::string& key) {
    const std::size_t start = text.find(key);
    if (start == std::string::npos) return {};
    const std::size_t from = start + key.size();
    return text.substr(from, text.find_first_of(" \n", from) - from);
}

std::uint64_t bitsOf(double v) {
    std::uint64_t b = 0;
    std::memcpy(&b, &v, sizeof b);
    return b;
}

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// The command line: bench_hot_path [cycles] [--invariants]
// ─────────────────────────────────────────────────────────────────────────────

TEST(BenchArgs, DefaultsTo200000Cycles) {
    const bench::Options options = parse({});
    EXPECT_TRUE(options.error.empty()) << options.error;
    EXPECT_EQ(options.cycles, 200000);
    EXPECT_FALSE(options.invariants);
}

TEST(BenchArgs, TakesACycleCount) {
    const bench::Options options = parse({"5000"});
    EXPECT_TRUE(options.error.empty()) << options.error;
    EXPECT_EQ(options.cycles, 5000);
    EXPECT_FALSE(options.invariants);
}

TEST(BenchArgs, TakesInvariantsBeforeOrAfterTheCount) {
    for (const auto& args : {std::vector<const char*>{"--invariants", "5000"},
                             std::vector<const char*>{"5000", "--invariants"}}) {
        const bench::Options options = parse(args);
        EXPECT_TRUE(options.error.empty()) << options.error;
        EXPECT_EQ(options.cycles, 5000);
        EXPECT_TRUE(options.invariants);
    }
}

TEST(BenchArgs, RejectsAnUnknownFlag) {
    EXPECT_FALSE(parse({"--json"}).error.empty());
}

TEST(BenchArgs, RejectsABadCycleCount) {
    for (const char* bad : {"0", "-5", "12x", "abc", "", "99999999999999999999999"}) {
        EXPECT_FALSE(parse({bad}).error.empty()) << "accepted '" << bad << "'";
    }
}

TEST(BenchArgs, RejectsASecondCycleCount) {
    EXPECT_FALSE(parse({"100", "200"}).error.empty());
}

// ─────────────────────────────────────────────────────────────────────────────
// The output formats
// ─────────────────────────────────────────────────────────────────────────────

TEST(BenchReport, InvariantsRoundTripEveryDouble) {
    for (const double v : {0.1 + 0.2, 1e-300, 1.0 / 3.0, 1.0 / 200000.0, 69780.955311892729}) {
        bench::Row row;
        row.name           = "row";
        row.allocsPerCycle = v;
        row.checksum       = v;
        const std::string text = bench::invariants(1000, {row});

        const double checksum = std::strtod(field(text, "checksum=").c_str(), nullptr);
        const double allocs   = std::strtod(field(text, "allocs/cycle=").c_str(), nullptr);
        EXPECT_EQ(bitsOf(checksum), bitsOf(v)) << text;
        EXPECT_EQ(bitsOf(allocs), bitsOf(v)) << text;
    }
}

TEST(BenchReport, InvariantsIgnoreTiming) {
    bench::Row fast;
    fast.name       = "row";
    fast.nsPerCycle = 100.0;
    fast.callbacks  = 7;
    fast.checksum   = 1.5;
    bench::Row slow = fast;
    slow.nsPerCycle = 250.5;

    EXPECT_EQ(bench::invariants(1000, {fast}), bench::invariants(1000, {slow}));
}

TEST(BenchReport, InvariantsNameTheCycleCount) {
    const std::string text = bench::invariants(5000, {});
    EXPECT_EQ(text.substr(0, text.find('\n')), "cycles=5000");
}

TEST(BenchReport, TableShowsAStrayAllocation) {
    bench::Row row;
    row.name           = "stray";
    row.allocsPerCycle = 1.0 / 200000.0;   // one allocation in the default run
    const std::string text = bench::table(200000, {row});

    // The row's line is: name, ns/cycle, allocs/cycle, callbacks, checksum.
    std::istringstream line(text.substr(text.find("\nstray ") + 1));
    std::string name, ns, allocs;
    line >> name >> ns >> allocs;
    EXPECT_GT(std::strtod(allocs.c_str(), nullptr), 0.0)
        << "one allocation in 200,000 cycles printed as '" << allocs << "':\n" << text;
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
