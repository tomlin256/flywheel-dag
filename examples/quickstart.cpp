// =============================================================================
// flywheel-dag — A header-only C++17 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

// quickstart.cpp — a small end-to-end graph, driven deterministically.
//
// A signal arrives on an AsyncInput: uniform noise in [-1, 1) with a five-cycle
// spike of +10 every 250 cycles. The graph scores each sample against the 50
// before it (a rolling z-score), flags |z| > 3 with hysteresis, ignores the flag
// until the window has filled, and latches it — so the output callback fires
// exactly once when a spike starts and once when it ends.
//
// Engine::step() runs one cycle synchronously, so a given cycle count always
// prints the same transitions. A live application calls engine.run() instead,
// and its feed threads post() as data arrives.
//
// Usage: quickstart [cycles]   (default 2000)

#include <flywheel/dag.hpp>
#include <flywheel/dag_async.hpp>
#include <flywheel/dag_engine.hpp>
#include <flywheel/dag_timeseries.hpp>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>

namespace {

// The noise comes from splitmix64, a tiny, fully specified generator, so every
// platform sees the same signal. The standard library's distributions are
// implementation-defined and would not.
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

} // namespace

int main(int argc, char** argv) {
    using namespace dag;
    const long cycles = argc > 1 ? std::strtol(argv[1], nullptr, 10) : 2000;

    async::Engine engine;

    // The source. A live application posts to it from a feed thread; the engine
    // drains it on the eval thread at the start of every cycle.
    auto signal = async::AsyncInput<double>::make("signal", 0.0);
    engine.addSource(signal);

    // The graph: rolling z-score → |z| → threshold with hysteresis.
    auto stats = ts::RollingStats::make("signal.stats", signal, 50);
    auto z     = ts::ZScoreNode::make("signal.z", signal, stats);
    auto absZ  = ComputeNode<double, double>::make(
        "signal.abs_z", {z}, [](const double& v) { return std::abs(v); },
        InvalidationMode::Lazy);   // a pure function of its input
    auto alert = ts::ThresholdNode<double>::make(
        "signal.alert", absZ, 3.0, ts::ThresholdNode<double>::Direction::Above, 0.5);

    // A z-score over a part-filled window means little, so the flag counts only
    // once the window is full. windowStatusNode() reports how full it is.
    auto armed = ComputeNode<bool, bool, ts::WindowStatus>::make(
        "signal.armed", {alert, stats->windowStatusNode()},
        [](const bool& flagged, const ts::WindowStatus& window) {
            return flagged && window.full();
        },
        InvalidationMode::Lazy);

    // One event when an episode starts (two flagged cycles in a row), one when
    // it ends.
    auto episode = ts::LatchedDebounceNode::make("signal.episode", armed, 2);

    int onsets = 0, resolutions = 0;
    engine.addOutput<std::optional<bool>>(episode, [&](const std::optional<bool>& t) {
        if (!t) return;   // no transition this cycle
        ++(*t ? onsets : resolutions);
        std::printf("cycle %4llu: %s\n",
                    static_cast<unsigned long long>(engine.cycleCount()),
                    *t ? "onset" : "resolved");
    });

    for (long i = 0; i < cycles; ++i) {
        const bool spike = i % 250 >= 125 && i % 250 < 130;
        signal->post(noise(i) + (spike ? 10.0 : 0.0));
        engine.step();
    }

    std::printf("onsets=%d resolved=%d\n", onsets, resolutions);
    return 0;
}
