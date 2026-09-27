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
#include <limits>
#include <stdexcept>
#include <string>
#include "flywheel/dag.hpp"
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

// Test: RollingStats + ZScore — detect anomalies in ramp + spike
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
            // At spike (100): previous mean ≈ 4.5, stddev ≈ 1.58, so z ≈ (100-4.5)/1.58 ≈ 60
            EXPECT_GT(std::abs(zv), 50.0); // Spike is clearly anomalous
        } else {
            // Normal values should have z-score closer to 0
            EXPECT_LT(std::abs(zv), 5.0);
        }
        ++t;
    }
}

// Test: OutlierGateNode — imputes spike with rolling mean
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

// Test: EWMANode — exponential weighted moving average + laziness
// ─────────────────────────────────────────────────────────────────────────────
TEST(TimeSeriesTests, EWMANodeAndLaziness) {
    auto raw  = Input<double>::make("raw", 0.0);
    auto ewma = EWMANode::make("ewma", raw, 0.3);

    std::vector<double> series = { 0, 10, 10, 10, 0, 0, 5, 5 };

    EvalContext ctx;
    int t = 0;
    for (double v : series) {
        raw->set(v);
        double e = get_value<double>(ewma->eval(ctx));
        (void)e; // suppress unused warning
        ++t;
    }

    // Laziness: setting same value should not mark dirty
    raw->set(5.0);
    EXPECT_FALSE(ewma->dirty()); // Lazy — no invalidation for same value
}

// Test: ThresholdNode + DebounceCountNode — alarm after N consecutive ticks
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
        bool ab = get_value<bool>(above->eval(ctx));
        bool al = get_value<bool>(alarm->eval(ctx));

        if (t == 7 && al) alarmFiredAt7 = true;  // Alarm fires after 3 sustained ticks
        if (t == 11 && al) alarmFiredAt11 = true; // Fires again after spike clears and resumes
        ++t;
    }

    EXPECT_TRUE(alarmFiredAt7);
    EXPECT_TRUE(alarmFiredAt11);
}

// Test: RollingMinMaxNode + DeltaNode — range and rate of change
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

// Test: RateLimiterNode — suppress downstream when change < threshold
//
// CONTRACT CHANGE. The sink is declared Lazy, and that is now what asking for
// suppression means.
//
// The limiter used to override invalidate() to absorb upstream dirt without
// forwarding, which suppressed downstream work for EVERY consumer — and was a
// bug, because a consumer that is never dirtied never pulls the limiter, so
// the limiter never reaches the eval() that would release, and a limiter wired
// mid-graph never propagated its release at all. The override is gone; the
// tri-state's Maybe is the suppression flag it was hand-rolling, and it does the
// job without swallowing the release.
//
// What changed for an EAGER consumer, stated rather than hidden: it now
// recomputes on every upstream change where it used to recompute on none, and it
// gains a release it never used to get. Eager means "recompute whenever anything
// upstream fired", so that is the mode behaving correctly, and this test would
// be asserting the opposite of the mode's meaning if it left the sink Eager.
// ValueSlot.RateLimiterReleaseReachesAnEagerConsumerToo covers that case.
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
    int t = 0;
    for (double v : vals) {
        raw->set(v);
        double em = get_value<double>(limited->eval(ctx));
        get_value<double>(sink->eval(ctx));
        ++t;
    }

    // Sink should recompute fewer times than input updates due to rate limiting
    // With minDelta=2.0, only changes >= 2.0 cause downstream updates
    EXPECT_LT(recomputes, static_cast<int>(vals.size()));
}

// ─────────────────────────────────────────────────────────────────────────────
// EWMATickRateNode tests
// ─────────────────────────────────────────────────────────────────────────────

// First eval applies one tick from the initial rate_ = 0.0: result = alpha.
TEST(TimeSeriesTests, EWMATickRateFirstEvalReturnsAlpha) {
    auto trigger = Input<double>::make("t", 0.0);
    auto rate    = EWMATickRateNode::make("rate", trigger, 0.5);
    EvalContext ctx;
    double v = get_value<double>(rate->eval(ctx));
    EXPECT_DOUBLE_EQ(v, 0.5);
}

// Each successive trigger increments the EWMA by exactly alpha.
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

// Node is not dirty when trigger has not fired.
TEST(TimeSeriesTests, EWMATickRateNotDirtyWhenTriggerClean) {
    auto trigger = Input<double>::make("t", 0.0);
    auto rate    = EWMATickRateNode::make("rate", trigger, 0.5);

    EvalContext ctx;
    trigger->set(1.0);
    rate->eval(ctx);   // clears dirty
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

// saveState / restoreState: next tick after restore continues from saved rate_.
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

    // Fresh node, restore state — same name required.
    auto trigger2 = Input<double>::make("rate.trigger", 0.0);
    auto rate2    = EWMATickRateNode::make("rate", trigger2, alpha);
    store.restore({rate2});

    // The next tick on rate2 must continue from the saved accumulator.
    trigger2->set(1.0);
    double v2       = get_value<double>(rate2->eval(ctx));
    double expected = alpha + (1.0 - alpha) * savedRate;
    EXPECT_DOUBLE_EQ(v2, expected);
}

// ─────────────────────────────────────────────────────────────────────────────
// LatchedDebounceNode tests
//
// Wire through ThresholdNode<double> so each tick has a distinct input value.
// This ensures the DAG dirty-propagation chain fires on every tick even when
// the boolean output stays true — matching real usage where an input changes
// every tick while a threshold condition persists.
// ─────────────────────────────────────────────────────────────────────────────

// Build a raw→ThresholdNode(>50)→LatchedDebounceNode chain with required=N.
// Each "above" tick sets a fresh value > 50; each "below" tick sets 0.0.
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

// ─────────────────────────────────────────────────────────────────────────────
// makeTimeDelayNode<T> — TIME-based delay (value as of now − horizonUs).
// Two pure inputs: the value series and a monotonic microsecond clock. Warm-up
// yields nullopt; robust to irregular cadence (unlike the N-tick DelayNode).
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
// Replaces the WindowNode + fold idiom with an O(1) incremental total. The
// thing worth testing hardest is that "incremental" never means "drifting":
// the running total is corrected by an exact re-sum every `window` pushes.
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

// The headline check: 10k randomised ticks, incremental vs naive.
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

// The periodic re-sum is what makes "incremental" safe to run for days. Its
// effect is invisible on ordinary input — a naive incremental total drifts only
// ~2e-9 relative over 5M ticks — so this uses catastrophic cancellation to make
// it unmistakable, and is verified red-then-green against a probe that disables
// the re-sum.
//
// 1e16 + 1.0 == 1e16 in IEEE double (the gap at that magnitude is 2), so the
// small values are absorbed on the way in. When the huge value is later
// subtracted on eviction, the total collapses to roughly zero and can never
// recover on its own — the information was lost, not merely rounded. Only an
// exact re-sum from the buffer restores it.
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
// These were assert()s until the default build type became RelWithDebInfo,
// which defines NDEBUG and would have compiled them out — turning an invalid
// alpha from a loud abort into a silently divergent EWMA. They throw now, so
// the validation survives an optimised build.
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
// Why every IWindowed::windowStatusNode() is built InvalidationMode::Eager, and
// must stay that way.
//
// The functor ignores its declared input and returns capacity()/filled() read
// off the captured upstream node: declared input is that node's VALUE, output is
// a function of its internal STATE. filled() advances while the value sits
// still, so feeding a CONSTANT is enough to separate them — the mean never
// moves, and the window fills underneath it.
//
// This family was caught by replaying a recorded session and flagging any node
// whose value moved in a cycle where none of its declared inputs did. It
// accounted for most of the witnesses, and every one of its skips would have
// been unsafe.
// ─────────────────────────────────────────────────────────────────────────────
TEST(TimeSeries, WindowStatusChangesWhileItsDeclaredInputDoesNot) {
    auto in     = Input<double>::make("in", 0.0);
    auto stats  = ts::RollingStats::make("stats", in, 4);
    auto status = stats->windowStatusNode();

    // invalidationMode() lives on NodeBase, not INode — deliberately: INode
    // gains no virtual for a read-only accessor.
    const auto asBase = std::dynamic_pointer_cast<NodeBase>(status);
    ASSERT_TRUE(asBase);
    EXPECT_EQ(asBase->invalidationMode(), InvalidationMode::Eager)
        << "windowStatusNode() must build its companion Eager";

    EvalContext ctx;
    in->set(7.0);
    status->eval(ctx);
    const auto first = get_value<ts::WindowStatus>(status->eval(ctx));

    // A CONSTANT input: the stats node's value (the mean) stays at 7.0 forever,
    // so a Lazy companion would resolve to "nothing moved" and never update.
    for (int i = 0; i < 3; ++i) {
        in->set(7.0 + 1e-12 * (i + 1));   // nudge so the source propagates
        status->eval(ctx);
    }
    const auto later = get_value<ts::WindowStatus>(status->eval(ctx));

    EXPECT_GT(later.filled, first.filled)
        << "the window filled while the mean stood still — which is precisely "
           "the evaluation a Lazy companion would have skipped";
    EXPECT_EQ(later.capacity, first.capacity);
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
