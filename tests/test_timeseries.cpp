// =============================================================================
// flywheel-dag — A header-only C++17 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

#include <gtest/gtest.h>
#include <cmath>
#include <algorithm>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include "flywheel/dag.hpp"
#include "flywheel/dag_engine.hpp"
#include "flywheel/dag_state_store.hpp"
#include "flywheel/dag_timeseries.hpp"

using namespace dag;
using namespace dag::ts;

// ── Helper function ──────────────────────────────────────────────────────────
template<typename Node>
double get_value(const std::shared_ptr<Node>& n) {
    EvalContext ctx;
    return get_value<double>(n->eval(ctx));
}

// ─────────────────────────────────────────────────────────────────────────────
// TimeSeries Tests
// ─────────────────────────────────────────────────────────────────────────────

// RollingStats + ZScore — detect anomalies in ramp + spike
// ─────────────────────────────────────────────────────────────────────────────
TEST(TimeSeriesTests, RollingStatsAndZScore) {
    auto raw   = Input<double>::make("raw", 0.0);
    auto stats = RollingStats::make("stats", raw, 5);
    auto z     = ZScoreNode::make("z", raw, stats);

    std::vector<double> series = { 1, 2, 3, 4, 5, 6, 7, 100, 8, 9 };

    EvalContext ctx;
    int t = 0;
    for (double v : series) {
        raw->set(v);
        stats->eval(ctx);
        double zv = get_value<double>(z->eval(ctx));

        if (t == 7) {
            // At the spike (100) the previous window is 3..7: mean 5, stddev ≈ 1.58,
            // so z ≈ (100-5)/1.58 ≈ 60.
            EXPECT_GT(std::abs(zv), 50.0); // Spike is clearly anomalous
        } else {
            // Normal values should have z-score closer to 0
            EXPECT_LT(std::abs(zv), 5.0);
        }
        ++t;
    }
}

// OutlierGateNode — imputes spike with rolling mean
// ─────────────────────────────────────────────────────────────────────────────
TEST(TimeSeriesTests, OutlierGateNode) {
    auto raw  = Input<double>::make("raw", 0.0);
    auto gate = OutlierGateNode::make("gate", raw, 5, 2.5);

    std::vector<double> series = { 10, 11, 10.5, 9.8, 10.2, 9.9, 150.0, 10.1, 10.3 };

    EvalContext ctx;
    int t = 0;
    for (double v : series) {
        raw->set(v);
        double cleaned = get_value<double>(gate->eval(ctx));

        if (t == 6) {
            // Spike at 150 should be flagged as outlier
            EXPECT_TRUE(gate->lastWasOutlier());
            // And replaced with rolling mean (≈10)
            EXPECT_LT(cleaned, 100.0); // Much less than 150
        } else {
            // Normal values pass through roughly unchanged
            EXPECT_LT(std::abs(cleaned - v), 1.0);
        }
        ++t;
    }
}

// EWMANode — a set() to the value the Input holds leaves it clean
// ─────────────────────────────────────────────────────────────────────────────
TEST(TimeSeriesTests, EWMANodeAndLaziness) {
    auto raw  = Input<double>::make("raw", 0.0);
    auto ewma = EWMANode::make("ewma", raw, 0.3);

    std::vector<double> series = { 0, 10, 10, 10, 0, 0, 5, 5 };

    EvalContext ctx;
    for (double v : series) {
        raw->set(v);
        double e = get_value<double>(ewma->eval(ctx));
        (void)e; // suppress unused warning
    }

    // The series ended on 5, and an Input ignores a set() to the value it holds.
    raw->set(5.0);
    EXPECT_FALSE(ewma->dirty());
}

// ThresholdNode + DebounceCountNode — alarm after N consecutive ticks
// ─────────────────────────────────────────────────────────────────────────────
TEST(TimeSeriesTests, ThresholdAndDebounce) {
    auto raw   = Input<double>::make("raw", 0.0);
    auto above = ThresholdNode<double>::make("above50", raw, 50.0,
                     ThresholdNode<double>::Direction::Above, 2.0);
    auto alarm = DebounceCountNode::make("alarm", above, 3);

    // Series with: spike to 80 at t=2, then sustained rise at t=5-7
    std::vector<double> series = { 10, 20, 80, 20, 30, 55, 60, 65, 40, 55, 60, 65, 70 };

    EvalContext ctx;
    int t = 0;
    bool alarmFiredAt7 = false, alarmFiredAt11 = false;
    for (double v : series) {
        raw->set(v);
        get_value<bool>(above->eval(ctx));
        bool al = get_value<bool>(alarm->eval(ctx));

        if (t == 7 && al) alarmFiredAt7 = true;  // Alarm fires after 3 sustained ticks
        if (t == 11 && al) alarmFiredAt11 = true; // Fires again after the dip at t=8
        ++t;
    }

    EXPECT_TRUE(alarmFiredAt7);
    EXPECT_TRUE(alarmFiredAt11);
}

// RollingMinMaxNode + DeltaNode — range and rate of change
// ─────────────────────────────────────────────────────────────────────────────
TEST(TimeSeriesTests, MinMaxAndDelta) {
    auto raw    = Input<double>::make("raw", 0.0);
    auto minmax = RollingMinMaxNode::make("minmax", raw, 4);
    auto delta  = DeltaNode<double>::make("delta", raw);

    std::vector<double> series = { 5, 3, 8, 1, 7, 4, 9, 2 };

    EvalContext ctx;
    int t = 0;
    for (double v : series) {
        raw->set(v);
        auto [mn, mx] = get_value<std::pair<double,double>>(minmax->eval(ctx));
        double d = get_value<double>(delta->eval(ctx));

        // Min should always be <= max
        EXPECT_LE(mn, mx);

        // Delta should match difference from previous (or 0 on first tick)
        if (t > 0) {
            EXPECT_DOUBLE_EQ(d, v - series[t-1]);
        } else {
            EXPECT_DOUBLE_EQ(d, 0.0);
        }
        ++t;
    }
}

// RateLimiterNode — a Lazy consumer skips while the limiter holds
//
// The sink is declared Lazy, which is how a consumer asks for suppression: an
// EAGER sink recomputes on every upstream change, so it could not show it.
// ValueSlot.RateLimiterReleaseReachesAnEagerConsumerToo covers the Eager case.
// ─────────────────────────────────────────────────────────────────────────────
TEST(TimeSeriesTests, RateLimiterNode) {
    auto raw     = Input<double>::make("raw", 0.0);
    auto limited = RateLimiterNode<double>::make("limited", raw, 2.0);

    int recomputes = 0;
    auto sink = ComputeNode<double, double>::make(
        "sink",
        std::make_tuple(std::static_pointer_cast<INode>(limited)),
        [&](const double& v) { ++recomputes; return v; },
        InvalidationMode::Lazy
    );

    std::vector<double> vals = { 0, 0.5, 1.0, 1.5, 2.0, 2.3, 2.5, 4.0, 4.1 };

    EvalContext ctx;
    for (double v : vals) {
        raw->set(v);
        get_value<double>(limited->eval(ctx));
        get_value<double>(sink->eval(ctx));
    }

    // A value less than 2.0 from the last emitted one is held, so the sink
    // recomputes fewer times than the input is set.
    EXPECT_LT(recomputes, static_cast<int>(vals.size()));
}

// ─────────────────────────────────────────────────────────────────────────────
// EWMATickRateNode tests
// ─────────────────────────────────────────────────────────────────────────────

// The first eval applies one tick to the initial rate of 0.0: the result is alpha.
TEST(TimeSeriesTests, EWMATickRateFirstEvalReturnsAlpha) {
    auto trigger = Input<double>::make("t", 0.0);
    auto rate    = EWMATickRateNode::make("rate", trigger, 0.5);
    EvalContext ctx;
    double v = get_value<double>(rate->eval(ctx));
    EXPECT_DOUBLE_EQ(v, 0.5);
}

// Each trigger folds in one tick: rate = alpha + (1 − alpha)·rate.
TEST(TimeSeriesTests, EWMATickRateMatchesManualEWMA) {
    const double alpha = 0.5;
    auto trigger = Input<double>::make("t", 0.0);
    auto rate    = EWMATickRateNode::make("rate", trigger, alpha);

    EvalContext ctx;
    double expected = 0.0;
    for (int i = 0; i < 10; ++i) {
        trigger->set(static_cast<double>(i + 1));  // new value each tick
        expected = alpha + (1.0 - alpha) * expected;
        double v = get_value<double>(rate->eval(ctx));
        EXPECT_DOUBLE_EQ(v, expected);
    }
}

// Evaluating the node after the trigger fires leaves it clean.
TEST(TimeSeriesTests, EWMATickRateNotDirtyWhenTriggerClean) {
    auto trigger = Input<double>::make("t", 0.0);
    auto rate    = EWMATickRateNode::make("rate", trigger, 0.5);

    EvalContext ctx;
    trigger->set(1.0);
    rate->eval(ctx);
    EXPECT_FALSE(rate->dirty());
}

// Alpha controls convergence rate: higher alpha converges faster.
TEST(TimeSeriesTests, EWMATickRateAlphaControlsConvergence) {
    auto t1   = Input<double>::make("t1", 0.0);
    auto t2   = Input<double>::make("t2", 0.0);
    auto fast = EWMATickRateNode::make("fast", t1, 0.8);
    auto slow = EWMATickRateNode::make("slow", t2, 0.1);

    EvalContext ctx;
    for (int i = 0; i < 5; ++i) {
        t1->set(static_cast<double>(i + 1));
        t2->set(static_cast<double>(i + 1));
        fast->eval(ctx);
        slow->eval(ctx);
    }
    EXPECT_GT(get_value<double>(fast->eval(ctx)), get_value<double>(slow->eval(ctx)));
}

// After a restore, the next tick continues from the saved rate.
TEST(TimeSeriesTests, EWMATickRateStateRoundTrip) {
    const double alpha = 0.5;
    auto trigger = Input<double>::make("rate.trigger", 0.0);
    auto rate    = EWMATickRateNode::make("rate", trigger, alpha);

    EvalContext ctx;
    double savedRate = 0.0;
    for (int i = 0; i < 5; ++i) {
        trigger->set(static_cast<double>(i + 1));
        savedRate = alpha + (1.0 - alpha) * savedRate;
        rate->eval(ctx);
    }
    EXPECT_DOUBLE_EQ(get_value<double>(rate->eval(ctx)), savedRate);

    InMemoryStateStore store;
    store.save({rate});

    // A fresh node under the same name: state is keyed by node name.
    auto trigger2 = Input<double>::make("rate.trigger", 0.0);
    auto rate2    = EWMATickRateNode::make("rate", trigger2, alpha);
    store.restore({rate2});

    // The next tick on rate2 continues from the saved rate.
    trigger2->set(1.0);
    double v2       = get_value<double>(rate2->eval(ctx));
    double expected = alpha + (1.0 - alpha) * savedRate;
    EXPECT_DOUBLE_EQ(v2, expected);
}

// ─────────────────────────────────────────────────────────────────────────────
// LatchedDebounceNode tests
//
// Wired through ThresholdNode<double> so each tick sets a distinct raw value: an
// Input drops a repeated one, and the latch must see every tick while the
// boolean stays true, as it does when a real input changes every tick under a
// persisting condition.
// ─────────────────────────────────────────────────────────────────────────────

// raw → ThresholdNode (above `level`) → LatchedDebounceNode(required). A true
// tick sets a fresh value above the level, a false tick a fresh one below it.
struct LatchFixture {
    std::shared_ptr<Input<double>>       raw;
    std::shared_ptr<LatchedDebounceNode> node;
    int tick = 0;

    explicit LatchFixture(std::size_t required, double level = 50.0) {
        raw  = Input<double>::make("raw", 0.0);
        auto thresh = ThresholdNode<double>::make(
            "thresh", raw, level, ThresholdNode<double>::Direction::Above);
        node = LatchedDebounceNode::make("latch", thresh, required);
    }

    std::optional<bool> push_true() {
        raw->set(51.0 + static_cast<double>(++tick));  // distinct value each call
        EvalContext ctx;
        return get_value<std::optional<bool>>(node->eval(ctx));
    }

    std::optional<bool> push_false() {
        raw->set(0.0 - static_cast<double>(++tick));  // distinct value below threshold
        EvalContext ctx;
        return get_value<std::optional<bool>>(node->eval(ctx));
    }
};

// Onset fires exactly once after N consecutive true ticks.
TEST(LatchedDebounceNodeTests, OnsetAfterNTicks) {
    LatchFixture f(3);
    EXPECT_EQ(f.push_true(), std::nullopt);             // count=1
    EXPECT_EQ(f.push_true(), std::nullopt);             // count=2
    EXPECT_EQ(f.push_true(), std::make_optional(true)); // onset
}

// nullopt on every tick while latched — no repeated onset.
TEST(LatchedDebounceNodeTests, NulloptWhileLatched) {
    LatchFixture f(2);
    f.push_true();
    EXPECT_EQ(f.push_true(), std::make_optional(true)); // onset

    EXPECT_EQ(f.push_true(), std::nullopt);
    EXPECT_EQ(f.push_true(), std::nullopt);
}

// Resolved fires exactly once on first false after onset; nullopt on further false ticks.
TEST(LatchedDebounceNodeTests, ResolvedOnFirstFalseAfterOnset) {
    LatchFixture f(2);
    f.push_true();
    f.push_true();  // onset

    EXPECT_EQ(f.push_false(), std::make_optional(false)); // resolved
    EXPECT_EQ(f.push_false(), std::nullopt);               // already unlatched
}

// nullopt while unlatched and input stays false — no resolved without prior onset.
TEST(LatchedDebounceNodeTests, NulloptWhenUnlatchedAndFalse) {
    LatchFixture f(3);
    EXPECT_EQ(f.push_false(), std::nullopt);
    EXPECT_EQ(f.push_false(), std::nullopt);
    EXPECT_EQ(f.push_false(), std::nullopt);
}

// Count resets on premature false — onset requires N fresh consecutive trues.
TEST(LatchedDebounceNodeTests, CountResetsOnPrematureFalse) {
    LatchFixture f(3);
    f.push_true();                                       // count=1
    f.push_true();                                       // count=2
    EXPECT_EQ(f.push_false(), std::nullopt);             // reset — not latched

    EXPECT_EQ(f.push_true(), std::nullopt);              // count=1
    EXPECT_EQ(f.push_true(), std::nullopt);              // count=2
    EXPECT_EQ(f.push_true(), std::make_optional(true));  // onset
}

// State save/restore mid-active: no spurious onset on next true tick after restore.
TEST(LatchedDebounceNodeTests, StateSaveRestoreNoSpuriousOnset) {
    LatchFixture f(3);
    f.push_true();
    f.push_true();
    EXPECT_EQ(f.push_true(), std::make_optional(true));  // latched=true

    InMemoryStateStore store;
    store.save({f.node});

    // Fresh chain with same node name — restore latched=true into it.
    LatchFixture f2(3);
    store.restore({f2.node});

    // Next true tick on restored node: already latched, must not fire onset again.
    EXPECT_EQ(f2.push_true(), std::nullopt);

    // First false tick: resolved, because latched=true was restored.
    EXPECT_EQ(f2.push_false(), std::make_optional(false));
}

// What an engine output callback receives. A transition fires, and so does the
// return to nullopt on the next cycle, because that is a change of value too.
// Nothing fires between them. So a callback must still ignore nullopt, and it
// runs once per transition rather than once per cycle.
TEST(LatchedDebounceNodeTests, EngineCallbacksFireOnTransitionsAndOnTheReturnToNullopt) {
    auto in    = Input<bool>::make("in", false, std::make_shared<AlwaysChangedPolicy>());
    auto latch = LatchedDebounceNode::make("latch", in, 2);

    async::Engine engine;
    std::string seen;
    engine.addOutput<std::optional<bool>>(latch, [&](const std::optional<bool>& t) {
        seen += !t ? 'n' : (*t ? 'T' : 'F');
    });

    for (const bool x : {false, true, true, true, true, false, false, false}) {
        in->set(x);
        engine.step();
    }

    // input:     F  T  T  T  T  F  F  F
    // output:    n  n  T  n  n  F  n  n
    // delivered: n     T  n     F  n
    EXPECT_EQ(seen, "nTnFn");
}

// ─────────────────────────────────────────────────────────────────────────────
// makeTimeDelayNode<T> — the value as of now − horizonUs
// ─────────────────────────────────────────────────────────────────────────────

namespace {
// Drive both inputs and read the optional output for one cycle.
std::optional<double> evalDelay(
    const std::shared_ptr<Input<double>>& value,
    const std::shared_ptr<Input<std::int64_t>>& timeUs,
    const ComputeNodePtr<std::optional<double>, double, std::int64_t>& node,
    double v, std::int64_t t) {
    value->set(v);
    timeUs->set(t);
    EvalContext ctx;
    return get_value<std::optional<double>>(node->eval(ctx));
}
} // namespace

TEST(TimeDelayNode, WarmupYieldsNulloptUntilHorizonElapses) {
    auto value = Input<double>::make("mid", 0.0);
    auto timeUs = Input<std::int64_t>::make("t", 0);
    auto delay = makeTimeDelayNode<double>("mid.delay1s", value, timeUs, 1'000'000);

    // First sample at t=0: no history a full second ago → nullopt.
    EXPECT_FALSE(evalDelay(value, timeUs, delay, 100.0, 0).has_value());
    // 0.4s and 0.9s later: still inside the 1s horizon → nullopt.
    EXPECT_FALSE(evalDelay(value, timeUs, delay, 101.0, 400'000).has_value());
    EXPECT_FALSE(evalDelay(value, timeUs, delay, 102.0, 900'000).has_value());
    // 1.2s: the as-of instant (0.2s) now has the t=0 sample (100) at or before it.
    auto out = evalDelay(value, timeUs, delay, 103.0, 1'200'000);
    ASSERT_TRUE(out.has_value());
    EXPECT_DOUBLE_EQ(*out, 100.0);
}

TEST(TimeDelayNode, TracksValueAsOfHorizonAgo) {
    auto value = Input<double>::make("mid", 0.0);
    auto timeUs = Input<std::int64_t>::make("t", 0);
    auto delay = makeTimeDelayNode<double>("mid.delay5s", value, timeUs, 5'000'000);

    // (t seconds, mid): the mid in effect changes at 0s,2s,6s,8s,12s.
    EXPECT_FALSE(evalDelay(value, timeUs, delay, 100.0,  0).has_value());        // asOf −5s
    EXPECT_FALSE(evalDelay(value, timeUs, delay, 110.0,  2'000'000).has_value()); // asOf −3s
    // t=6s → asOf 1s → mid at 1s was still 100 (changed to 110 only at 2s).
    EXPECT_DOUBLE_EQ(*evalDelay(value, timeUs, delay, 120.0, 6'000'000), 100.0);
    // t=8s → asOf 3s → mid at 3s was 110 (changed at 2s).
    EXPECT_DOUBLE_EQ(*evalDelay(value, timeUs, delay, 130.0, 8'000'000), 110.0);
    // t=12s → asOf 7s → mid at 7s was 120 (changed at 6s).
    EXPECT_DOUBLE_EQ(*evalDelay(value, timeUs, delay, 140.0, 12'000'000), 120.0);
}

TEST(TimeDelayNode, RobustToLongUpdateGap) {
    // A quiet stream: one update, then a 40s gap. A tick-count DelayNode
    // could never resolve; the time delay resolves against the last known value.
    auto value = Input<double>::make("mid", 0.0);
    auto timeUs = Input<std::int64_t>::make("t", 0);
    auto delay = makeTimeDelayNode<double>("mid.delay1s", value, timeUs, 1'000'000);

    EXPECT_FALSE(evalDelay(value, timeUs, delay, 100.0, 0).has_value());
    // 40s later, mid moves to 105. As of 39s the mid was still 100 (last known).
    EXPECT_DOUBLE_EQ(*evalDelay(value, timeUs, delay, 105.0, 40'000'000), 100.0);
    // 41s: as of 40s the mid was 105 (the update landed at 40s). No dup sample was
    // stored for the unchanged 105, but the query still resolves correctly.
    EXPECT_DOUBLE_EQ(*evalDelay(value, timeUs, delay, 105.0, 41'000'000), 105.0);
}

// ─────────────────────────────────────────────────────────────────────────────
// RollingSumNode
//
// The thing worth testing hardest is that "incremental" never means "drifting":
// an exact re-sum every `window` pushes corrects the running total.
// ─────────────────────────────────────────────────────────────────────────────

namespace {

/// The naive definition this node has to match: sum of the last `window`
/// values, computed from scratch.
double naiveTrailingSum(const std::vector<double>& xs, std::size_t upTo,
                        std::size_t window) {
    const std::size_t first = (upTo + 1 > window) ? (upTo + 1 - window) : 0;
    double total = 0.0;
    for (std::size_t i = first; i <= upTo; ++i) total += xs[i];
    return total;
}

}  // namespace

TEST(RollingSumNodeTests, MatchesNaiveSumBeforeTheWindowFills) {
    auto in  = Input<double>::make("in", 0.0);
    auto sum = RollingSumNode::make("sum", in, 5);

    EvalContext ctx;
    const std::vector<double> xs{1.0, 2.0, 3.0};
    for (std::size_t i = 0; i < xs.size(); ++i) {
        in->set(xs[i]);
        const double got = get_value<double>(sum->eval(ctx));
        EXPECT_DOUBLE_EQ(got, naiveTrailingSum(xs, i, 5)) << "at tick " << i;
        EXPECT_EQ(sum->filled(), i + 1);
    }
    EXPECT_EQ(sum->capacity(), 5u);
}

TEST(RollingSumNodeTests, EvictsTheOldestOnceFull) {
    auto in  = Input<double>::make("in", 0.0);
    auto sum = RollingSumNode::make("sum", in, 3);

    EvalContext ctx;
    const std::vector<double> xs{1.0, 2.0, 3.0, 4.0, 5.0};
    for (std::size_t i = 0; i < xs.size(); ++i) {
        in->set(xs[i]);
        EXPECT_DOUBLE_EQ(get_value<double>(sum->eval(ctx)),
                         naiveTrailingSum(xs, i, 3)) << "at tick " << i;
    }
    // Last three are 3+4+5 = 12.
    EXPECT_DOUBLE_EQ(get_value<double>(sum->eval(ctx)), 12.0);
    EXPECT_EQ(sum->filled(), 3u);
}

// The headline check: 10k pseudo-random ticks, incremental vs naive.
TEST(RollingSumNodeTests, IncrementalMatchesNaiveOverALongRandomSequence) {
    constexpr std::size_t kWindow = 50;
    auto in  = Input<double>::make("in", 0.0);
    auto sum = RollingSumNode::make("sum", in, kWindow);

    // Deterministic pseudo-random values spanning several orders of magnitude,
    // which is where an incremental total drifts fastest.
    std::vector<double> xs;
    xs.reserve(10000);
    std::uint64_t state = 88172645463325252ull;
    for (int i = 0; i < 10000; ++i) {
        state ^= state << 13; state ^= state >> 7; state ^= state << 17;
        const double mantissa = static_cast<double>(state % 1000000) / 1000.0;
        const double scale    = (i % 100 == 0) ? 1e6 : 1.0;   // occasional big print
        xs.push_back(mantissa * scale);
    }

    EvalContext ctx;
    double worst = 0.0;
    for (std::size_t i = 0; i < xs.size(); ++i) {
        in->set(xs[i]);
        const double got      = get_value<double>(sum->eval(ctx));
        const double expected = naiveTrailingSum(xs, i, kWindow);
        const double scale    = std::max(1.0, std::abs(expected));
        worst = std::max(worst, std::abs(got - expected) / scale);
    }
    EXPECT_LT(worst, 1e-9) << "worst relative deviation from the naive sum was " << worst;
}

// The periodic re-sum is what makes "incremental" safe to run indefinitely. Its
// effect is invisible on ordinary input: a naive incremental total drifts by a
// few parts in 1e9 over 5M ticks. So this uses catastrophic cancellation to make
// it unmistakable: without the re-sum the total ends at 72, not 74.
//
// 1e16 + 1.0 == 1e16 in IEEE double (the gap at that magnitude is 2), so the
// small values are absorbed on the way in. When the huge value is later
// subtracted on eviction, the total is left short by what was absorbed and
// cannot recover on its own: the information was lost, not merely rounded. Only
// an exact re-sum from the buffer restores it.
TEST(RollingSumNodeTests, PeriodicRecomputeRecoversFromCatastrophicCancellation) {
    constexpr std::size_t kWindow = 4;
    auto in  = Input<double>::make("in", 0.0);
    auto sum = RollingSumNode::make("sum", in, kWindow);

    EvalContext ctx;
    in->set(1e16);
    sum->eval(ctx);

    // Flush the huge value out with ordinary ones. They must be DISTINCT:
    // Input<double>'s equality policy suppresses a repeated value, so setting
    // 1.0 twenty times would advance the window exactly once.
    for (int i = 1; i <= 20; ++i) {
        in->set(static_cast<double>(i));
        sum->eval(ctx);
    }

    // The window now holds the last four values, 17..20.
    EXPECT_DOUBLE_EQ(get_value<double>(sum->eval(ctx)), 17.0 + 18.0 + 19.0 + 20.0)
        << "the running total never recovered from absorbing 1e16 — the "
           "periodic exact re-sum is not firing";
}

TEST(RollingSumNodeTests, SaveRestoreRoundTrip) {
    auto in  = Input<double>::make("in", 0.0);
    auto sum = RollingSumNode::make("sum", in, 4);

    EvalContext ctx;
    const std::vector<double> xs{1.5, 2.5, 3.5, 4.5, 5.5};
    for (double x : xs) { in->set(x); sum->eval(ctx); }
    const double before = get_value<double>(sum->eval(ctx));

    InMemoryStateStore store;
    store.save({ sum });

    // A fresh node restored from the snapshot must continue the same series.
    auto in2  = Input<double>::make("in", 0.0);
    auto sum2 = RollingSumNode::make("sum", in2, 4);
    ASSERT_TRUE(store.restore({ sum2 }));

    EvalContext ctx2;
    EXPECT_DOUBLE_EQ(sum2->sum(), before);
    EXPECT_EQ(sum2->filled(), sum->filled());

    // And both advance identically from here.
    in->set(9.0);  const double next1 = get_value<double>(sum->eval(ctx));
    in2->set(9.0); const double next2 = get_value<double>(sum2->eval(ctx2));
    EXPECT_DOUBLE_EQ(next1, next2);
}

TEST(RollingSumNodeTests, WindowStatusReportsCapacityAndFilled) {
    auto in     = Input<double>::make("in", 0.0);
    auto sum    = RollingSumNode::make("sum", in, 3);
    auto status = sum->windowStatusNode();

    EvalContext ctx;
    in->set(1.0); sum->eval(ctx);
    auto st = get_value<WindowStatus>(status->eval(ctx));
    EXPECT_EQ(st.capacity, 3u);
    EXPECT_EQ(st.filled,   1u);
    EXPECT_FALSE(st.full());

    in->set(2.0); sum->eval(ctx);
    in->set(3.0); sum->eval(ctx);
    st = get_value<WindowStatus>(status->eval(ctx));
    EXPECT_EQ(st.filled, 3u);
    EXPECT_TRUE(st.full());
}

// ─────────────────────────────────────────────────────────────────────────────
// Alpha validation — EWMANode / EWMATickRateNode
//
// The checks throw rather than assert(), so they hold in the default build,
// which defines NDEBUG.
// ─────────────────────────────────────────────────────────────────────────────

TEST(EWMANodeAlpha, RejectsOutOfRangeAlpha) {
    auto in = Input<double>::make("in", 1.0);

    EXPECT_THROW(EWMANode::make("zero", in, 0.0),        std::invalid_argument);
    EXPECT_THROW(EWMANode::make("negative", in, -0.5),   std::invalid_argument);
    EXPECT_THROW(EWMANode::make("above_one", in, 1.5),   std::invalid_argument);
    EXPECT_THROW(EWMANode::make("nan", in,
                                std::numeric_limits<double>::quiet_NaN()),
                 std::invalid_argument);
}

TEST(EWMANodeAlpha, AcceptsBoundaryAlpha) {
    auto in = Input<double>::make("in", 1.0);

    // 1.0 is valid (pure passthrough); anything just above 0 is valid too.
    EXPECT_NO_THROW(EWMANode::make("one", in, 1.0));
    EXPECT_NO_THROW(EWMANode::make("tiny", in, 1e-9));
}

TEST(EWMANodeAlpha, MessageNamesTheOffendingNode) {
    auto in = Input<double>::make("in", 1.0);
    try {
        EWMANode::make("ew_fast", in, 2.0);
        FAIL() << "expected std::invalid_argument";
    } catch (const std::invalid_argument& e) {
        const std::string msg = e.what();
        EXPECT_NE(msg.find("ew_fast"), std::string::npos) << "actual: " << msg;
        EXPECT_NE(msg.find("alpha"),   std::string::npos) << "actual: " << msg;
    }
}

TEST(EWMATickRateNodeAlpha, RejectsOutOfRangeAlpha) {
    auto trigger = Input<double>::make("trigger", 1.0);

    EXPECT_THROW(EWMATickRateNode::make("zero", trigger, 0.0),      std::invalid_argument);
    EXPECT_THROW(EWMATickRateNode::make("negative", trigger, -1.0), std::invalid_argument);
    EXPECT_THROW(EWMATickRateNode::make("above_one", trigger, 1.1), std::invalid_argument);
    EXPECT_NO_THROW(EWMATickRateNode::make("valid", trigger, 1.0));
}

// ─────────────────────────────────────────────────────────────────────────────
// Every IWindowed::windowStatusNode() must be InvalidationMode::Eager: its
// functor reads filled() off the captured node, and filled() advances while that
// node's value, the declared input, sits still. The note above the
// implementations in dag_timeseries.inl has the reason in full.
//
// Only the mode assertion pins this, and only for RollingStats's companion: the
// sequence below passes for a Lazy companion too (flywheel-dag#33).
// ─────────────────────────────────────────────────────────────────────────────
TEST(TimeSeries, WindowStatusChangesWhileItsDeclaredInputDoesNot) {
    auto in     = Input<double>::make("in", 0.0);
    auto stats  = ts::RollingStats::make("stats", in, 4);
    auto status = stats->windowStatusNode();

    // invalidationMode() is on NodeBase, not INode: INode gets no virtual for a
    // read-only accessor.
    const auto asBase = std::dynamic_pointer_cast<NodeBase>(status);
    ASSERT_TRUE(asBase);
    EXPECT_EQ(asBase->invalidationMode(), InvalidationMode::Eager)
        << "windowStatusNode() must build its companion Eager";

    EvalContext ctx;
    in->set(7.0);
    status->eval(ctx);
    const auto first = get_value<ts::WindowStatus>(status->eval(ctx));

    // The nudge lets the source propagate. It also moves the mean by about 1e-12.
    for (int i = 0; i < 3; ++i) {
        in->set(7.0 + 1e-12 * (i + 1));
        status->eval(ctx);
    }
    const auto later = get_value<ts::WindowStatus>(status->eval(ctx));

    EXPECT_GT(later.filled, first.filled)
        << "the status must follow the window as it fills";
    EXPECT_EQ(later.capacity, first.capacity);
}

// ─────────────────────────────────────────────────────────────────────────────
// Every dag::ts node, registered as an engine output: an unchanged value fires
// no callback.
//
// The engine detects change by ValuePtr identity, so a node must keep its cached
// pointer when its equality policy says the value is unchanged. One that rebinds
// it fires on every dirty cycle.
//
// Each node sits on an AlwaysChangedPolicy input, so that every cycle dirties
// it, and is fed values that leave its output unchanged once warm. It must fire
// exactly once on the first cycle, which delivers the initial value, and not at
// all once its value has settled.
// ─────────────────────────────────────────────────────────────────────────────
namespace {

constexpr int kWarmCycles   = 10;   // longer than any node below takes to settle
constexpr int kSteadyCycles = 20;

struct OutputCallbacks {
    int first  = 0;   // on the first cycle
    int warm   = 0;   // over the rest of the warm-up
    int steady = 0;   // over kSteadyCycles once settled
};

template <typename T>
std::shared_ptr<Input<T>> alwaysChangedInput(T initial) {
    return Input<T>::make("in", std::move(initial), std::make_shared<AlwaysChangedPolicy>());
}

/// Registers `node` as an engine output, then runs a first cycle, the warm-up
/// and the steady phase, calling `feed` before every cycle to dirty the node.
template <typename Out>
OutputCallbacks countOutputCallbacks(const NodePtr& node, const std::function<void()>& feed) {
    async::Engine engine;
    OutputCallbacks n;
    int* phase = &n.first;
    engine.addOutput<Out>(node, [&phase](const Out&) { ++*phase; });

    feed();
    engine.step();
    phase = &n.warm;
    for (int i = 0; i < kWarmCycles; ++i) { feed(); engine.step(); }
    phase = &n.steady;
    for (int i = 0; i < kSteadyCycles; ++i) { feed(); engine.step(); }
    return n;
}

void expectFiresOnlyOnChange(const OutputCallbacks& n) {
    EXPECT_EQ(n.first, 1) << "the first cycle must deliver the initial value";
    EXPECT_EQ(n.steady, 0) << "fired " << n.steady << " times in " << kSteadyCycles
                           << " dirty cycles with an unchanged value";
}

}  // namespace

TEST(UnchangedStatefulOutput, RollingStats) {
    auto in = alwaysChangedInput(5.0);
    expectFiresOnlyOnChange(countOutputCallbacks<double>(
        RollingStats::make("stats", in, 4), [&] { in->set(5.0); }));
}

TEST(UnchangedStatefulOutput, RollingSumNode) {
    auto in = alwaysChangedInput(5.0);   // 5, 10, 15, then 20 for good
    expectFiresOnlyOnChange(countOutputCallbacks<double>(
        RollingSumNode::make("sum", in, 4), [&] { in->set(5.0); }));
}

TEST(UnchangedStatefulOutput, RollingMinMaxNode) {
    auto in = alwaysChangedInput(5.0);
    expectFiresOnlyOnChange(countOutputCallbacks<std::pair<double, double>>(
        RollingMinMaxNode::make("minmax", in, 4), [&] { in->set(5.0); }));
}

TEST(UnchangedStatefulOutput, EWMANode) {
    auto in = alwaysChangedInput(5.0);   // alpha 0.5: 0.5*5 + 0.5*5 is exactly 5
    expectFiresOnlyOnChange(countOutputCallbacks<double>(
        EWMANode::make("ewma", in, 0.5), [&] { in->set(5.0); }));
}

TEST(UnchangedStatefulOutput, EWMATickRateNode) {
    auto in = alwaysChangedInput(5.0);   // alpha 1: the rate is 1.0 from the start
    expectFiresOnlyOnChange(countOutputCallbacks<double>(
        EWMATickRateNode::make("rate", in, 1.0), [&] { in->set(5.0); }));
}

TEST(UnchangedStatefulOutput, DeltaNode) {
    auto in = alwaysChangedInput(5.0);
    expectFiresOnlyOnChange(countOutputCallbacks<double>(
        DeltaNode<double>::make("delta", in), [&] { in->set(5.0); }));
}

TEST(UnchangedStatefulOutput, DelayNode) {
    auto in = alwaysChangedInput(5.0);   // 0 while it fills, then 5 for good
    expectFiresOnlyOnChange(countOutputCallbacks<double>(
        DelayNode<double>::make("delay", in, 3), [&] { in->set(5.0); }));
}

TEST(UnchangedStatefulOutput, ThresholdNode) {
    auto in = alwaysChangedInput(0.0);   // distinct inputs, all below the level
    int i = 0;
    expectFiresOnlyOnChange(countOutputCallbacks<bool>(
        ThresholdNode<double>::make("thr", in, 100.0),
        [&] { in->set(static_cast<double>(++i)); }));
}

TEST(UnchangedStatefulOutput, ZScoreNode) {
    auto in = alwaysChangedInput(5.0);   // no spread, so z is 0
    expectFiresOnlyOnChange(countOutputCallbacks<double>(
        ZScoreNode::make("z", in, 8), [&] { in->set(5.0); }));
}

TEST(UnchangedStatefulOutput, OutlierGateNode) {
    auto in = alwaysChangedInput(5.0);   // never an outlier, so it passes 5 through
    expectFiresOnlyOnChange(countOutputCallbacks<double>(
        OutlierGateNode::make("gate", in, 8, 3.0), [&] { in->set(5.0); }));
}

TEST(UnchangedStatefulOutput, RateLimiterNode) {
    auto in = alwaysChangedInput(5.0);   // 5, 6, 7, 5, ...: every move is below minDelta
    int i = 0;
    expectFiresOnlyOnChange(countOutputCallbacks<double>(
        RateLimiterNode<double>::make("limiter", in, 10.0),
        [&] { in->set(5.0 + (i++ % 3)); }));
}

TEST(UnchangedStatefulOutput, DebounceCountNode) {
    auto in = alwaysChangedInput(false);
    expectFiresOnlyOnChange(countOutputCallbacks<bool>(
        DebounceCountNode::make("debounce", in, 3), [&] { in->set(false); }));
}

TEST(UnchangedStatefulOutput, LatchedDebounceNode) {
    auto in = alwaysChangedInput(false);   // nullopt throughout
    expectFiresOnlyOnChange(countOutputCallbacks<std::optional<bool>>(
        LatchedDebounceNode::make("latch", in, 3), [&] { in->set(false); }));
}

// The control. WindowNode's policy is AlwaysChangedPolicy, so an unchanged
// window still publishes on every evaluation, and its callback still fires.
TEST(UnchangedStatefulOutput, WindowNodeStillFiresUnderAlwaysChangedPolicy) {
    auto in = alwaysChangedInput(5.0);
    const auto n = countOutputCallbacks<std::deque<double>>(
        WindowNode<double>::make("window", in, 4), [&] { in->set(5.0); });
    EXPECT_EQ(n.first, 1);
    EXPECT_EQ(n.steady, kSteadyCycles)
        << "AlwaysChangedPolicy publishes on every evaluation";
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
