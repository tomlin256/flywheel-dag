// =============================================================================
// flywheel-dag — A header-only C++23 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

// bench_hot_path.cpp — what one engine cycle costs, on four generic workloads.
//
// A measurement tool, not a test: it is EXCLUDE_FROM_ALL and not registered
// with ctest, because a wall-clock threshold would make ctest flaky. Run it by
// hand before and after a change, on the same machine:
//
//     cmake --build --preset relwithdebinfo-macos-arm64 --target bench_hot_path
//     B=build/relwithdebinfo-macos-arm64/bin/bench_hot_path
//     $B                # 200000 measured cycles per row
//     $B --invariants   # the exact columns only
//
// or the tree for your host (relwithdebinfo-linux-arm64, relwithdebinfo-linux-x86_64).
//
// ns/cycle depends on the machine and its load. The other three columns are
// exact, so a change that alters them is a change in what the engine did:
//
//   allocs/cycle  calls per measured cycle to the global allocation functions,
//                 all twenty, which tests/allocation_counter.hpp replaces; 0 in
//                 every row, since the engine's steady state recycles its value
//                 buffers.
//   callbacks     output callbacks fired over the measured cycles.
//   checksum      the sum of every value those callbacks received.
//
// The checksum is the same on every platform, not just on every run. The rows
// use only +, -, *, / and sqrt, which IEEE 754 rounds exactly, and CMake builds
// this file with -ffp-contract=off, so no compiler fuses a multiply-add that
// another would not.
//
// Usage: bench_hot_path [cycles] [--invariants]

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "allocation_counter.hpp"
#include "bench_report.hpp"

#include <flywheel/dag.hpp>
#include <flywheel/dag_async.hpp>
#include <flywheel/dag_engine.hpp>
#include <flywheel/dag_ops.hpp>
#include <flywheel/dag_reduce.hpp>
#include <flywheel/dag_timeseries.hpp>

using namespace dag;

namespace {

/// Cycles each row runs before measuring. The first four cycles allocate every
/// value buffer; the rest fill every window, so the measured cycles are all
/// steady state.
constexpr long kWarmupCycles = 1000;

// The noise comes from splitmix64, as in examples/quickstart.cpp: a fully
// specified generator, so every platform sees the same signal.
std::uint64_t splitmix64(std::uint64_t x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

/// Uniform in [-1, 1): the top 53 bits of the hash, scaled.
double noise(long i) {
    return static_cast<double>(splitmix64(static_cast<std::uint64_t>(i)) >> 11) * 0x1.0p-52 - 1.0;
}

/// Runs the warm-up, then `cycles` more with the allocation counter on.
/// `drive(i)` runs cycle i; the measured cycles continue the warm-up's
/// sequence rather than restarting it. `sink` and `fired` are what the row's
/// output callbacks write to, and only the measured cycles count.
template <typename Drive>
bench::Row measure(const char* name, long cycles, double& sink, long& fired, Drive&& drive) {
    for (long i = 0; i < kWarmupCycles; ++i) drive(i);

    sink  = 0.0;
    fired = 0;
    allocation_counter::start();
    const auto t0 = std::chrono::steady_clock::now();
    for (long i = kWarmupCycles; i < kWarmupCycles + cycles; ++i) drive(i);
    const auto t1 = std::chrono::steady_clock::now();
    const long allocs = allocation_counter::stop();

    const double n  = static_cast<double>(cycles);
    const double ns = static_cast<double>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    return { name, ns / n, static_cast<double>(allocs) / n, fired, sink };
}

// ─────────────────────────────────────────────────────────────────────────────
// chain — compute and time-series nodes over one AsyncInput.
//
//   signal ─┬─ EWMANode ── DeltaNode                                    → out
//           ├─ RollingStats(64) ── ZScoreNode ── |z| ── ThresholdNode   → out
//           ├─ RollingMinMaxNode(64) ── max − min                       → out
//           └─ DelayNode(16) ── signal − delayed ── RollingSumNode(64)  → out
//
// The signal is the quickstart's: noise in [-1, 1) with a five-cycle spike of
// +10 every 250 cycles, so the threshold fires. The width holds still between
// window extremes, so its equality policy suppresses callbacks as well.
// ─────────────────────────────────────────────────────────────────────────────
bench::Row chain(long cycles) {
    async::Engine engine;
    auto signal = async::AsyncInput<double>::make("signal", 0.0);
    engine.addSource(signal);

    auto level = ts::EWMANode::make("signal.level", signal, 0.05);
    auto slope = ts::DeltaNode<double>::make("signal.slope", level);

    auto stats = ts::RollingStats::make("signal.stats", signal, 64);
    auto z     = ts::ZScoreNode::make("signal.z", signal, stats);
    auto absZ  = ComputeNode<double, double>::make(
        "signal.abs_z", {z}, [](const double& v) { return std::abs(v); },
        InvalidationMode::Lazy);   // a pure function of its input
    auto alert = ts::ThresholdNode<double>::make(
        "signal.alert", absZ, 3.0, ts::ThresholdNode<double>::Direction::Above, 0.5);

    auto range = ts::RollingMinMaxNode::make("signal.range", signal, 64);
    auto width = ComputeNode<double, std::pair<double, double>>::make(
        "signal.width", {range},
        [](const std::pair<double, double>& minMax) { return minMax.second - minMax.first; },
        InvalidationMode::Lazy);

    auto delayed = ts::DelayNode<double>::make("signal.delayed", signal, 16);
    auto change  = ops::DiffNode<double>::make("signal.change", signal, delayed);
    auto drift   = ts::RollingSumNode::make("signal.drift", change, 64);

    double sink  = 0.0;
    long   fired = 0;
    engine.addOutput<double>(slope, [&](const double& v) { sink += v; ++fired; });
    engine.addOutput<bool>(alert,   [&](const bool& on)  { sink += on ? 1.0 : 0.0; ++fired; });
    engine.addOutput<double>(width, [&](const double& v) { sink += v; ++fired; });
    engine.addOutput<double>(drift, [&](const double& v) { sink += v; ++fired; });

    return measure("chain", cycles, sink, fired, [&](long i) {
        const bool spike = i % 250 >= 125 && i % 250 < 130;
        signal->post(noise(i) + (spike ? 10.0 : 0.0));
        engine.step();
    });
}

// ─────────────────────────────────────────────────────────────────────────────
// idle-queues — 32 AsyncQueues, stepped with nothing posted.
//
// The engine flushes every source on every cycle, so a quiet queue still costs
// a flush and a dirty check each cycle. The callbacks and checksum columns are
// 0 by construction: nothing is posted, so a callback here would be an idle
// queue delivering something.
// ─────────────────────────────────────────────────────────────────────────────
bench::Row idleQueues(long cycles) {
    constexpr int kQueues = 32;

    async::Engine engine;
    double sink  = 0.0;
    long   fired = 0;
    for (int q = 0; q < kQueues; ++q) {
        auto queue = async::AsyncQueue<double>::make("queue." + std::to_string(q));
        engine.addSource(queue);
        engine.addOutput<std::vector<double>>(queue, [&](const std::vector<double>& batch) {
            for (const double v : batch) sink += v;
            ++fired;
        });
    }

    return measure("idle-queues", cycles, sink, fired, [&](long) { engine.step(); });
}

// ─────────────────────────────────────────────────────────────────────────────
// ingest — a heap-owning value, one post() and one flush() per cycle.
//
// The producer keeps its Frame, rewrites one sample in place and posts it as
// an lvalue, as a source that merges each update into the value it holds
// would. post() copy-assigns into storage the input retains and flush() emits
// into a recycled buffer, so once warm the path allocates nothing, although
// both of Frame's members own heap.
//
// The input takes AlwaysChangedPolicy: every post differs from the last, so a
// comparison could only add cost. The sample written is the cycle index, so
// the total grows on every cycle and fires every callback. Its values are
// integers, so at the default cycle count the checksum is exact integer
// arithmetic.
// ─────────────────────────────────────────────────────────────────────────────
struct Frame {
    std::string         source;   // longer than any small-string buffer, so it owns heap
    std::vector<double> samples;

    // Never called here, but AsyncInput::make instantiates the default
    // equality policy, which needs it.
    bool operator==(const Frame& o) const { return source == o.source && samples == o.samples; }
};

bench::Row ingest(long cycles) {
    constexpr std::size_t kSamples = 64;

    async::Engine engine;
    auto frames = async::AsyncInput<Frame>::make(
        "frames", Frame{}, std::make_shared<AlwaysChangedPolicy>());
    engine.addSource(frames);

    auto total = ComputeNode<double, Frame>::make(
        "frames.total", {frames},
        [](const Frame& f) {
            double sum = 0.0;
            for (const double s : f.samples) sum += s;
            return sum;
        },
        InvalidationMode::Lazy);

    double sink  = 0.0;
    long   fired = 0;
    engine.addOutput<double>(total, [&](const double& v) { sink += v; ++fired; });

    Frame frame;   // the producer's own copy: rewritten in place, posted as an lvalue
    frame.source = "frames.source-name-longer-than-any-small-string-buffer";
    frame.samples.assign(kSamples, 0.0);

    return measure("ingest", cycles, sink, fired, [&](long i) {
        frame.samples[static_cast<std::size_t>(i) % kSamples] = static_cast<double>(i);
        frames->post(frame);
        engine.step();
    });
}

// ─────────────────────────────────────────────────────────────────────────────
// solve — a node that solves on a reduction, once per cycle.
//
//   a ──┐
//   x ──┴─ x·x − a ── six Newton steps from 1                           → out
//
// The solver is an Eager compute node over the residual. Each cycle moves `a`,
// so the node runs: it binds a reduction of the residual over x to the point
// the graph holds, then takes six Newton steps on it, each one a call with its
// gradient. x itself never moves: the reduction evaluates the residual at each
// guess on buffers of its own.
//
// Its allocs/cycle is 0, which holds the reduction and the source it reads: the
// reduction keeps a's value from one bind to the next, and that pins one of the
// source's two buffers. `a` runs through 2 … 98 and differs on every cycle, so
// every callback fires. Six steps do not converge for the larger `a`, and need
// not: each step is one +, −, × and ÷, so the checksum is the same everywhere.
// ─────────────────────────────────────────────────────────────────────────────
bench::Row solve(long cycles) {
    async::Engine engine;
    auto a = async::AsyncInput<double>::make("a", 1.0);
    engine.addSource(a);

    auto x        = Input<double>::make("x", 1.0);
    auto residual = ops::DiffNode<double>::make(
        "x.x-a", ops::ProductNode<double>::make("x.x", {x, x}), a);

    auto f = std::make_shared<Reduction>(residual, std::vector<NodePtr>{x});
    auto root = ComputeNode<double, double>::make(
        "sqrt(a)", {residual},
        [f](const double&) {
            f->bind();   // the residual has just been pulled, so it is clean
            double guess = 1.0;
            double slope = 0.0;
            for (int i = 0; i < 6; ++i) {
                const double miss = f->gradient({guess}, {&slope, 1});
                guess -= miss / slope;
            }
            return guess;
        });   // Eager, the default: the point can move while the residual stands still

    double sink  = 0.0;
    long   fired = 0;
    engine.addOutput<double>(root, [&](const double& v) { sink += v; ++fired; });

    return measure("solve", cycles, sink, fired, [&](long i) {
        a->post(2.0 + static_cast<double>(i % 97));
        engine.step();
    });
}

}  // namespace

int main(int argc, char** argv) try {
    const bench::Options options = bench::parseArgs(argc, argv);
    if (!options.error.empty()) {
        std::fprintf(stderr, "%s: %s\nusage: %s [cycles] [--invariants]\n",
                     argv[0], options.error.c_str(), argv[0]);
        return 2;
    }

    // Every row runs before anything prints, so a failure never leaves a
    // partial result that reads as a complete one.
    const std::vector<bench::Row> rows = {
        chain(options.cycles),
        idleQueues(options.cycles),
        ingest(options.cycles),
        solve(options.cycles),
    };

    const std::string out = options.invariants ? bench::invariants(options.cycles, rows)
                                               : bench::table(options.cycles, rows);
    std::fputs(out.c_str(), stdout);
    return 0;
} catch (const std::exception& e) {
    std::fprintf(stderr, "%s: %s\n", argv[0], e.what());
    return 1;
}
