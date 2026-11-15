// =============================================================================
// flywheel-dag — A header-only C++23 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

// dag::Reduction through stateful dag::ts nodes.
//
// A stateful node's function is its last update, applied to another sample:
// what the node would hold now, had the sample it just took been x. The tests
// hold that against a twin, a second node of the same construction that is fed
// the same history and takes x as its last sample.
//
// Allocations are counted by allocation_counter.cpp, which this suite links.

#include <gtest/gtest.h>
#include "allocation_counter.hpp"
#include "reduce_test_nodes.hpp"
#include "flywheel/dag.hpp"
#include "flywheel/dag_aad.hpp"
#include "flywheel/dag_async.hpp"
#include "flywheel/dag_engine.hpp"
#include "flywheel/dag_ops.hpp"
#include "flywheel/dag_reduce.hpp"
#include "flywheel/dag_timeseries.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace dag;
using allocation_counter::allocations_during;
using namespace reduce_test;

namespace {

/// Builds the node under test over an upstream. A function pointer, so that a
/// table of makers copies nothing but pointers.
using Make  = NodePtr (*)(const NodePtr& upstream);
using Slope = std::function<double(double sample)>;

/// A node over an Input, fed a history. Every sample fires, an equal one too,
/// as a source that ticks does.
struct Fed {
    InputPtr<double> x = Input<double>::make("arg.x", 0.0, std::make_shared<AlwaysChangedPolicy>());
    NodePtr          node;
    EvalContext      ctx;

    explicit Fed(Make make) : node(make(x)) {}

    double feed(double v) {
        x->set(v);
        return value();
    }
    double value() {
        const ValuePtr held = node->eval(ctx);
        return get_value<double>(held);
    }
};

/// What a twin holds after `history` and then `last`.
double twinValue(Make make, const std::vector<double>& history, double last) {
    Fed twin(make);
    for (const double v : history) twin.feed(v);
    return twin.feed(last);
}

void expectWithin(double actual, double expected, double relative) {
    EXPECT_NEAR(actual, expected, relative * std::max(1.0, std::abs(expected)));
}

/// How a reduction through the node must compare with its twin away from the
/// sample the node took.
enum class Agrees : std::uint8_t { ToRounding, ToTheBit };

/// A reduction of the node over its sample, bound after `history` and `last`:
///   • at `last` it is the node's value, to the bit, and its gradient a tape's;
///   • at each trial sample it is what a twin holds that took the trial as its
///     last sample, and its gradient is slope(trial);
///   • a hundred calls later the node is as its control is, now and after one
///     more sample.
void expectItsLastUpdate(Make make, const std::vector<double>& history, double last,
                         std::span<const double> trials, const Slope& slope,
                         Agrees agrees = Agrees::ToRounding) {
    SCOPED_TRACE("after " + std::to_string(history.size()) + " samples, the last one "
                 + std::to_string(last));
    Fed subject(make), control(make);
    for (const double v : history) {
        subject.feed(v);
        control.feed(v);
    }
    const double own = subject.feed(last);
    control.feed(last);

    Reduction f = reduce(subject.node, {subject.x});
    ASSERT_EQ(f.size(), 1u);
    EXPECT_EQ(f({last}), own);
    double g = 0.0;
    EXPECT_EQ(f.gradient({last}, {&g, 1}), own);
    EXPECT_EQ(g, aad::adjoints(subject.node, {subject.x})[0]);
    expectWithin(g, slope(last), 1e-13);

    for (const double trial : trials) {
        SCOPED_TRACE("at " + std::to_string(trial));
        const double twin = twinValue(make, history, trial);
        if (agrees == Agrees::ToTheBit) {
            EXPECT_EQ(f({trial}), twin);
        } else {
            expectWithin(f({trial}), twin, 1e-12);
        }
        expectWithin(f.gradient({trial}, {&g, 1}), twin, 1e-12);
        expectWithin(g, slope(trial), 1e-13);
    }

    double sum = 0.0;
    for (int i = 0; i < 100; ++i) sum += f({last + 0.5 * i});
    EXPECT_TRUE(std::isfinite(sum));
    EXPECT_EQ(subject.value(), own);
    EXPECT_EQ(subject.value(), control.value());
    EXPECT_EQ(subject.feed(9.5), control.feed(9.5));
}

constexpr std::array<double, 4> kTrials{-2.0, 0.5, 7.25, 1.0e3};

NodePtr makeEwma(const NodePtr& up) { return ts::EWMANode::make("node.ewma", up, 0.25); }
NodePtr makeMean(const NodePtr& up) { return ts::RollingStats::make("node.mean", up, 4); }
NodePtr makeSum(const NodePtr& up) { return ts::RollingSumNode::make("node.sum", up, 3); }
NodePtr makeScore(const NodePtr& up) { return ts::ZScoreNode::make("node.z", up, 8); }
NodePtr makeGate(const NodePtr& up) { return ts::OutlierGateNode::make("node.gate", up, 8, 2.0); }
NodePtr makeTwinLevel(const NodePtr& up) { return ts::EWMANode::make("twin", up, 0.25); }

/// The mean of some samples, and their spread with n − 1 in the denominator, as
/// RollingStats takes them.
double meanOf(const std::vector<double>& v) {
    double sum = 0.0;
    for (const double x : v) sum += x;
    return sum / static_cast<double>(v.size());
}
double spreadOf(const std::vector<double>& v) {
    const double m = meanOf(v);
    double ss = 0.0;
    for (const double x : v) ss += (x - m) * (x - m);
    return std::sqrt(ss / static_cast<double>(v.size() - 1));
}

/// x → EWMANode(0.5) → RollingStats(4) = m, and the root k·m + m·m.
struct Chain {
    InputPtr<double> x = Input<double>::make("arg.x", 0.0, std::make_shared<AlwaysChangedPolicy>());
    NodePtr          level = ts::EWMANode::make("level", x, 0.5);
    NodePtr          mean  = ts::RollingStats::make("mean", level, 4);
    InputPtr<double> k     = Input<double>::make("k", 3.0);
    NodePtr          root  = ops::SumNode<>::make("root", {
        ops::ProductNode<>::make("k.m", {k, mean}), ops::ProductNode<>::make("m.m", {mean, mean})});
    EvalContext      ctx;

    double feed(double v) {
        x->set(v);
        const ValuePtr held = root->eval(ctx);
        return get_value<double>(held);
    }
};

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Each node's last update, at another sample
// ─────────────────────────────────────────────────────────────────────────────

// The first sample is taken whole, so the function is the identity there, to
// the bit. After it, the sample is blended in with weight α.
TEST(ReductionTimeseries, AnEwmaAppliesItsLastUpdateToAnotherSample) {
    const Slope whole   = [](double) { return 1.0; };
    const Slope blended = [](double) { return 0.25; };
    expectItsLastUpdate(makeEwma, {}, 3.0, kTrials, whole, Agrees::ToTheBit);
    expectItsLastUpdate(makeEwma, {3.0}, 5.0, kTrials, blended);
    expectItsLastUpdate(makeEwma, {3.0, 5.0, -1.0}, 2.0, kTrials, blended);
    expectItsLastUpdate(makeEwma, {3.0}, 3.0, kTrials, blended);   // a sample equal to the average
    expectItsLastUpdate(makeEwma, {0.1, 0.2, 0.7, 1.0 / 3.0}, 2.0 / 3.0, kTrials, blended);
}

// While the window fills, the mean is of the samples so far, and once it is
// full the update evicts the oldest before it takes the new one.
TEST(ReductionTimeseries, ARollingMeanAppliesItsLastUpdateToAnotherSample) {
    expectItsLastUpdate(makeMean, {}, 2.0, kTrials, [](double) { return 1.0; });
    expectItsLastUpdate(makeMean, {1.0}, 2.0, kTrials, [](double) { return 0.5; });
    expectItsLastUpdate(makeMean, {1.0, 2.0, 4.0}, 8.0, kTrials, [](double) { return 0.25; });
    expectItsLastUpdate(makeMean, {1.0, 2.0, 4.0, 8.0, 16.0, 32.0}, 64.0, kTrials,
                        [](double) { return 0.25; });
    expectItsLastUpdate(makeMean, {0.1, 0.2, 0.7, 1.0 / 3.0, 2.0 / 3.0}, 0.9, kTrials,
                        [](double) { return 0.25; });
}

// A window of 3 re-sums exactly on its third push and every third one after,
// and adds to its running total on the others. Histories of 0 to 7 samples
// take the last one on each kind of update, filling and full.
TEST(ReductionTimeseries, ARollingSumAppliesItsLastUpdateToAnotherSample) {
    const std::vector<double> samples = {0.1, 0.2, 0.7, 1.0 / 3.0, 2.0 / 3.0, 0.9, 1.1};
    for (std::size_t n = 0; n <= samples.size(); ++n) {
        const std::vector<double> history(samples.begin(),
                                          samples.begin() + static_cast<std::ptrdiff_t>(n));
        expectItsLastUpdate(makeSum, history, 1.7, kTrials, [](double) { return 1.0; });
    }
}

// The mean and the spread a z-score is taken against come from before its
// sample joined the window, so the step is the node's own expression on the
// node's own operands: its value to the bit at every sample.
TEST(ReductionTimeseries, AZScoreAppliesItsLastUpdateToAnotherSample) {
    const std::vector<double> history = {1.0, 2.0, 4.0};
    const double sd = spreadOf(history);
    expectItsLastUpdate(makeScore, history, 3.0, kTrials, [sd](double) { return 1.0 / sd; },
                        Agrees::ToTheBit);
    const std::vector<double> longer = {0.1, 0.2, 0.7, 1.0 / 3.0, 2.0 / 3.0};
    const double sdLonger = spreadOf(longer);
    expectItsLastUpdate(makeScore, longer, 0.9, kTrials, [sdLonger](double) { return 1.0 / sdLonger; },
                        Agrees::ToTheBit);
}

// One reduction, bound at a sample that passes, is asked on both sides of the
// threshold: a sample that passes is the value, and a gated one is replaced by
// the mean of the window that holds it. And one bound at a gated sample.
TEST(ReductionTimeseries, AnOutlierGateAppliesItsLastUpdateToAnotherSample) {
    const std::vector<double> calm = {1.0, 1.1, 0.9, 1.05, 0.95};
    const double mu = meanOf(calm), sd = spreadOf(calm);
    const Slope slope = [mu, sd](double x) {
        return std::abs((x - mu) / sd) >= 2.0 ? 1.0 / 6.0 : 1.0;
    };
    ASSERT_EQ(slope(1.02), 1.0);
    ASSERT_EQ(slope(50.0), 1.0 / 6.0);
    const std::vector<double> bothSides = {1.0, 1.02, 1.15, 1.2, 50.0, -40.0, 0.8, 0.5};
    expectItsLastUpdate(makeGate, calm, 1.02, bothSides, slope);   // bound where it passes
    expectItsLastUpdate(makeGate, calm, 50.0, bothSides, slope);   // bound where it is gated

    // Passing, the function is the identity, to the bit.
    Fed subject(makeGate);
    for (const double v : calm) subject.feed(v);
    subject.feed(50.0);
    Reduction f = reduce(subject.node, {subject.x});
    EXPECT_EQ(f({1.02}), 1.02);
    EXPECT_NE(f({50.0}), 50.0);
}

// A delay of no ticks is the sample itself.
TEST(ReductionTimeseries, ADelayOfNoTicksPassesItsSampleThrough) {
    const Make through = [](const NodePtr& up) -> NodePtr {
        return ts::DelayNode<double>::make("node.through", up, 0);
    };
    expectItsLastUpdate(through, {}, 2.0, kTrials, [](double) { return 1.0; }, Agrees::ToTheBit);
    expectItsLastUpdate(through, {1.0, 2.0}, 4.0, kTrials, [](double) { return 1.0; },
                        Agrees::ToTheBit);

    // Over another type it passes that type's value through, and has no
    // derivative: x, truncated to an int, delayed by no ticks, and back.
    auto x = Input<double>::make("arg.x", 2.75);
    auto whole = ComputeNode<int, double>::make("whole", {x},
        [](const double& v) { return static_cast<int>(v); }, InvalidationMode::Lazy);
    auto same = ts::DelayNode<int>::make("same", whole, 0);
    auto root = ComputeNode<double, int>::make("root", {same},
        [](const int& v) { return static_cast<double>(v); }, InvalidationMode::Lazy);
    EvalContext ctx;
    root->eval(ctx);
    Reduction f = reduce(root, {x});
    EXPECT_EQ(f.size(), 3u);
    EXPECT_EQ(f({2.75}), 2.0);
    EXPECT_EQ(f({7.9}), 7.0);
}

// ─────────────────────────────────────────────────────────────────────────────
// What "to rounding" leaves out
// ─────────────────────────────────────────────────────────────────────────────

// A step works from the value its node holds, which is the result of the update
// and not what the update started from. Two things follow, and neither is a
// property of an update applied to its own start.
TEST(ReductionTimeseries, AStepWorksFromTheValueItsNodeHolds) {
    // A negative zero comes back positive: −0 + α·(x − x) is −0 + 0.
    {
        Fed subject(makeEwma);
        subject.feed(-0.0);
        const double own = subject.feed(-0.0);
        ASSERT_TRUE(std::signbit(own));
        Reduction f = reduce(subject.node, {subject.x});
        EXPECT_EQ(f({-0.0}), own);
        EXPECT_FALSE(std::signbit(f({-0.0})));
    }
    // A sample that is not finite leaves the node nothing finite to work from:
    // the node holds +∞, and the step has no value at any sample, although the
    // update at a finite sample is finite.
    {
        const double inf = std::numeric_limits<double>::infinity();
        Fed subject(makeEwma);
        subject.feed(1.0);
        ASSERT_EQ(subject.feed(inf), inf);
        Reduction f = reduce(subject.node, {subject.x});
        EXPECT_TRUE(std::isnan(f({inf})));
        EXPECT_TRUE(std::isnan(f({2.0})));
        EXPECT_EQ(twinValue(makeEwma, {1.0}, 2.0), 1.25);
    }
    // The first update takes its sample whole, so it is exact there too.
    {
        const double inf = std::numeric_limits<double>::infinity();
        Fed subject(makeEwma);
        ASSERT_EQ(subject.feed(inf), inf);
        Reduction f = reduce(subject.node, {subject.x});
        EXPECT_EQ(f({inf}), inf);
        EXPECT_EQ(f({2.0}), 2.0);
    }
    // The rounding is that of the larger numbers on the way, not of the answer.
    // After a sample of 1e17 the average is 2.5e16, and has rounded the 0.75 the
    // earlier sample left away. Moving it back by a quarter of 1e17 gives 0,
    // where the update at 2 gives 1.25: an error of 1.25, which is less than
    // half a unit in the last place of 2.5e16.
    {
        Fed subject(makeEwma);
        subject.feed(1.0);
        ASSERT_EQ(subject.feed(1.0e17), 2.5e16);
        Reduction f = reduce(subject.node, {subject.x});
        EXPECT_EQ(f({1.0e17}), 2.5e16);
        EXPECT_EQ(f({2.0}), 0.0);
        EXPECT_EQ(twinValue(makeEwma, {1.0}, 2.0), 1.25);
    }
}

// A step applies its node's update and no equality policy, as every step does.
// Under a tolerance the node goes on publishing 1 while its average moves to
// 1.25, and the function is the average.
TEST(ReductionTimeseries, AStepAppliesNoEqualityPolicy) {
    auto x     = Input<double>::make("arg.x", 0.0, std::make_shared<AlwaysChangedPolicy>());
    auto level = ts::EWMANode::make("level", x, 0.25, std::make_shared<EpsilonPolicy<double>>(10.0));
    EvalContext ctx;
    x->set(1.0);
    level->eval(ctx);
    x->set(2.0);
    const ValuePtr published = level->eval(ctx);
    ASSERT_EQ(get_value<double>(published), 1.0);

    Reduction f = reduce(level, {x});
    EXPECT_EQ(f({2.0}), 1.25);
    EXPECT_EQ(f({6.0}), 2.25);
}

// ─────────────────────────────────────────────────────────────────────────────
// The point a step is bound to
// ─────────────────────────────────────────────────────────────────────────────

// bind() reads the node's state. Once the node takes another sample, a
// reduction that is not bound again still answers for the point it was bound
// at, as it does for any value it read.
TEST(ReductionTimeseries, AStepAnswersForThePointItWasBoundAt) {
    const std::vector<std::pair<std::string, Make>> makers = {
        {"ewma", makeEwma}, {"mean", makeMean}, {"sum", makeSum}, {"z", makeScore}, {"gate", makeGate}};
    for (const auto& [name, make] : makers) {
        SCOPED_TRACE(name);
        Fed subject(make);
        for (const double v : {1.0, 2.5, 4.0}) subject.feed(v);
        Reduction f = reduce(subject.node, {subject.x});
        const double before = f({30.0});
        expectWithin(before, twinValue(make, {1.0, 2.5}, 30.0), 1e-12);

        subject.feed(7.0);   // the node moves on, and the reduction is not bound again
        EXPECT_EQ(f({30.0}), before);

        f.bind();
        const double after = f({30.0});
        expectWithin(after, twinValue(make, {1.0, 2.5, 4.0}, 30.0), 1e-12);
        EXPECT_NE(after, before);
    }
}

// A step is bound only where the bound point runs it. The average on the branch
// the graph did not take is stale, and so is the node it averages: binding its
// step would pull that node, which evaluates it.
TEST(ReductionTimeseries, AStepTheBoundPointDoesNotRunIsNotBound) {
    auto x    = Input<double>::make("arg.x", 2.0);
    auto flag = Input<bool>::make("flag", true);
    int calls = 0;
    auto doubled = ComputeNode<double, double>::make("doubled", {x},
        [&calls](const double& v) { ++calls; return 2.0 * v; }, InvalidationMode::Lazy);
    auto other = ts::EWMANode::make("other", doubled, 0.5);
    auto taken = ops::NegateNode<>::make("taken", x);
    auto root  = ConditionNode::make("root", flag, taken, other);
    EvalContext ctx;
    root->eval(ctx);
    ASSERT_EQ(calls, 0);

    Reduction f = reduce(root, {x});
    EXPECT_EQ(f.size(), 2u);   // the branch taken and the selection
    EXPECT_EQ(f({5.0}), -5.0);
    EXPECT_EQ(calls, 0);
    EXPECT_TRUE(doubled->dirty());
    EXPECT_TRUE(other->dirty());

    // Once the graph takes the other branch, its steps run and are bound: the
    // average took its first sample whole, so the function is twice x.
    flag->set(false);
    root->eval(ctx);
    ASSERT_EQ(calls, 1);
    f.bind();
    EXPECT_EQ(f.size(), 3u);   // the doubling, the average and the selection
    EXPECT_EQ(f({5.0}), 10.0);
    EXPECT_EQ(calls, 2);       // the step called the functor, at 5
}

// ─────────────────────────────────────────────────────────────────────────────
// Constants and barriers
// ─────────────────────────────────────────────────────────────────────────────

// A delayed sample, a tick rate and a z-score with no spread to measure against
// do not depend on the sample the node last took. The node reads nothing, so it
// is a constant: the reduction has no step for it, ignores the argument through
// it, and never reaches the barrier above it.
TEST(ReductionTimeseries, ANodeThatDoesNotReadItsSampleIsAConstant) {
    const std::vector<std::pair<std::string, Make>> makers = {
        {"delayed", [](const NodePtr& up) -> NodePtr {
             return ts::DelayNode<double>::make("node", up, 1);
         }},
        {"rate", [](const NodePtr& up) -> NodePtr {
             return ts::EWMATickRateNode::make("node", up, 0.5);
         }},
        {"flat z", makeScore}};
    for (const auto& [name, make] : makers) {
        SCOPED_TRACE(name);
        auto x      = Input<double>::make("arg.x", 0.0, std::make_shared<AlwaysChangedPolicy>());
        auto opaque = ComputeNode<double, double>::make("node.opaque", {x},
            [](const double& v) { return 2.0 * v; });   // Eager: a barrier
        const NodePtr node = make(opaque);
        auto root = ops::SumNode<>::make("root.out", {node, x});
        EvalContext ctx;
        for (const double v : {1.5, 1.5}) {   // two equal samples: no spread yet
            x->set(v);
            root->eval(ctx);
        }
        const ValuePtr heldNode = node->eval(ctx);
        const double held = get_value<double>(heldNode);

        Reduction f = reduce(root, {x});
        EXPECT_EQ(f.size(), 1u);   // the sum alone
        EXPECT_EQ(f({1.5}), held + 1.5);
        EXPECT_EQ(f({4.0}), held + 4.0);
        double g = 0.0;
        f.gradient({4.0}, {&g, 1});
        EXPECT_EQ(g, 1.0);
        EXPECT_EQ(g, aad::adjoints(root, {x})[0]);

        Reduction alone = reduce(node, {x});
        EXPECT_EQ(alone.size(), 0u);
        EXPECT_EQ(alone({4.0}), held);
    }
}

// The same z-score, once there is a spread, reads its sample: the reduction
// then has a step for it, and the barrier above it is one. A delay of no ticks
// always reads its sample.
TEST(ReductionTimeseries, WhatANodeReadsIsAskedAtEachBind) {
    auto x      = Input<double>::make("arg.x", 0.0, std::make_shared<AlwaysChangedPolicy>());
    auto opaque = ComputeNode<double, double>::make("node.opaque", {x},
        [](const double& v) { return 2.0 * v; });
    auto zOfOpaque = ts::ZScoreNode::make("node.z", opaque, 8);
    auto zOfX      = ts::ZScoreNode::make("node.zx", x, 8);
    auto root      = ops::SumNode<>::make("root.out", {zOfOpaque, zOfX});
    EvalContext ctx;
    for (const double v : {1.5, 1.5}) {
        x->set(v);
        root->eval(ctx);
    }
    Reduction f = reduce(root, {x});
    EXPECT_EQ(f.size(), 0u);   // both scores are flat, so the root is a constant
    EXPECT_EQ(f({9.0}), 0.0);

    for (const double v : {4.0, 2.0}) {   // the second of these is scored against a spread
        x->set(v);
        root->eval(ctx);
    }
    const std::string why = messageOf<std::domain_error>([&] { f.bind(); });
    EXPECT_TRUE(mentions(why, "arg.x reaches root root.out through node.opaque")) << why;
    EXPECT_TRUE(mentions(why, "is Eager")) << why;

    Reduction ofX = reduce(zOfX, {x});
    EXPECT_EQ(ofX.size(), 1u);

    auto through = ts::DelayNode<double>::make("node.through", opaque, 0);
    through->eval(ctx);
    Reduction g(through, {x});
    EXPECT_TRUE(mentions(messageOf<std::domain_error>([&] { g.bind(); }),
                         "through node.opaque, which is Eager"));
}

// A node that cannot apply its last update to another sample gives no step, and
// is a barrier with the argument above it.
TEST(ReductionTimeseries, ANodeThatGivesNoStepIsABarrier) {
    auto x = Input<double>::make("arg.x", 2.0, std::make_shared<AlwaysChangedPolicy>());
    EvalContext ctx;
    const auto refusal = [&](const NodePtr& root) {
        x->set(2.0);
        root->eval(ctx);
        x->set(3.0);
        root->eval(ctx);
        Reduction f(root, {x});
        return messageOf<std::domain_error>([&] { f.bind(); });
    };

    // What the update started from is written over by the update.
    auto limiter = ts::RateLimiterNode<double>::make("node.limiter", x, 0.5);
    const std::string whyLimiter = refusal(ops::NegateNode<>::make("root.limited", limiter));
    EXPECT_TRUE(mentions(whyLimiter, "arg.x reaches root root.limited through node.limiter"))
        << whyLimiter;
    EXPECT_TRUE(mentions(whyLimiter, "is a time-series node that gives no step")) << whyLimiter;
    EXPECT_TRUE(mentions(whyLimiter, "cannot apply its last update to another sample"))
        << whyLimiter;
    EXPECT_FALSE(mentions(whyLimiter, "Lazy")) << whyLimiter;   // which a stateful node cannot be

    // A node whose value is not a double: the width of a window's range.
    auto range = ts::RollingMinMaxNode::make("node.range", x, 4);
    auto width = ComputeNode<double, std::pair<double, double>>::make("root.width", {range},
        [](const std::pair<double, double>& minMax) { return minMax.second - minMax.first; },
        InvalidationMode::Lazy);
    const std::string whyRange = refusal(width);
    EXPECT_TRUE(mentions(whyRange, "arg.x reaches root root.width through node.range")) << whyRange;
    EXPECT_TRUE(mentions(whyRange, "is a time-series node that gives no step")) << whyRange;

    auto delta = ts::DeltaNode<double>::make("node.delta", x);
    const std::string whyDelta = refusal(ops::NegateNode<>::make("root.differenced", delta));
    EXPECT_TRUE(mentions(whyDelta, "arg.x reaches root root.differenced through node.delta"))
        << whyDelta;
    EXPECT_TRUE(mentions(whyDelta, "is a time-series node that gives no step")) << whyDelta;
}

// ─────────────────────────────────────────────────────────────────────────────
// A chain, and a solver
// ─────────────────────────────────────────────────────────────────────────────

// Two stateful steps and three ops: the argument reaches the root through an
// average and the mean of the last four averages.
TEST(ReductionTimeseries, AChainOfStatefulNodesAndOps) {
    const std::vector<double> history = {1.0, 2.0, 0.5, 3.0, 1.5};
    Chain subject, control;
    for (const double v : history) {
        subject.feed(v);
        control.feed(v);
    }
    const double own = subject.feed(2.5);
    control.feed(2.5);
    const ValuePtr heldMean = subject.mean->eval(subject.ctx);
    const double m = get_value<double>(heldMean);

    Reduction f = reduce(subject.root, {subject.x});
    EXPECT_EQ(f.size(), 5u);   // the average, the mean, two products and the sum
    EXPECT_EQ(f({2.5}), own);
    double g = 0.0;
    EXPECT_EQ(f.gradient({2.5}, {&g, 1}), own);
    expectWithin(g, aad::adjoints(subject.root, {subject.x})[0], 1e-13);
    expectWithin(g, (3.0 + 2.0 * m) * 0.25 * 0.5, 1e-13);   // (k + 2m) · 1/count · α

    for (const double trial : kTrials) {
        SCOPED_TRACE("at " + std::to_string(trial));
        Chain twin;
        for (const double v : history) twin.feed(v);
        const double there = twin.feed(trial);
        expectWithin(f({trial}), there, 1e-12);
        const ValuePtr twinMean = twin.mean->eval(twin.ctx);
        f.gradient({trial}, {&g, 1});
        expectWithin(g, (3.0 + 2.0 * get_value<double>(twinMean)) * 0.25 * 0.5, 1e-12);
    }

    for (int i = 0; i < 100; ++i) f({0.1 * i});
    EXPECT_EQ(subject.feed(4.0), control.feed(4.0));
}

// The sample at which the square of a smoothed signal meets a target, by
// Newton's method on a reduction of the square over the signal: an Eager node
// in an engine, which binds in its functor. Fed to a twin as its last sample,
// the solution gives the target.
TEST(ReductionTimeseries, ASolverFindsTheSampleAtWhichASmoothedSignalMeetsATarget) {
    async::Engine engine;
    auto signal = async::AsyncInput<double>::make("signal", 0.0);
    auto target = async::AsyncInput<double>::make("target", 5.0);
    engine.addSource(signal);
    engine.addSource(target);
    auto level  = ts::EWMANode::make("level", signal, 0.25);
    auto energy = ops::ProductNode<>::make("energy", {level, level});

    auto f = std::make_shared<Reduction>(energy, std::vector<NodePtr>{signal});
    int solves = 0;
    auto wanted = ComputeNode<double, double, double>::make("sample for target", {target, energy},
        [f, &solves](const double& goal, const double&) {
            ++solves;
            f->bind();
            double x = 1.0, slope = 0.0;
            for (int i = 0; i < 50; ++i) {
                const double miss = f->gradient({x}, {&slope, 1}) - goal;
                if (std::abs(miss) < 1e-12) break;
                x -= miss / slope;
            }
            return x;
        });   // Eager, the default
    std::vector<double> delivered;
    engine.addOutput<double>(wanted, [&](const double& v) { delivered.push_back(v); });

    // The square of the level a twin holds after `history` and then `last`.
    const auto energyAt = [](const std::vector<double>& history, double last) {
        const double twin = twinValue(makeTwinLevel, history, last);
        return twin * twin;
    };

    signal->post(2.0);   // the first sample: the level is the sample
    engine.step();
    ASSERT_EQ(delivered.size(), 1u);
    EXPECT_EQ(solves, 1);
    EXPECT_NEAR(energyAt({}, delivered[0]), 5.0, 1e-9);
    EXPECT_NEAR(delivered[0], std::sqrt(5.0), 1e-9);

    signal->post(3.0);   // blended into the 2 the level held
    engine.step();
    ASSERT_EQ(delivered.size(), 2u);
    EXPECT_EQ(solves, 2);
    EXPECT_NEAR(energyAt({2.0}, delivered[1]), 5.0, 1e-9);

    target->post(7.0);   // the signal does not move, so the level does not tick
    engine.step();
    ASSERT_EQ(delivered.size(), 3u);
    EXPECT_EQ(solves, 3);
    EXPECT_NEAR(energyAt({2.0}, delivered[2]), 7.0, 1e-9);

    signal->post(-1.0);
    engine.step();
    ASSERT_EQ(delivered.size(), 4u);
    EXPECT_EQ(solves, 4);
    EXPECT_NEAR(energyAt({2.0, 3.0}, delivered[3]), 7.0, 1e-9);

    engine.step();   // nothing moved
    EXPECT_EQ(delivered.size(), 4u);
    EXPECT_EQ(solves, 4);

    // The level is where the samples it took put it: no solve moved it.
    EvalContext ctx;
    const ValuePtr heldLevel = level->eval(ctx);
    EXPECT_EQ(get_value<double>(heldLevel), twinValue(makeTwinLevel, {2.0, 3.0}, -1.0));
}

// ─────────────────────────────────────────────────────────────────────────────
// Allocation
// ─────────────────────────────────────────────────────────────────────────────

// Every stateful step at once: an average, a mean over it, a sum, a z-score, a
// gate and a delay of no ticks, under one n-ary sum.
TEST(ReductionTimeseries, AWarmCallGradientAndBindAllocateNothing) {
    auto x = Input<double>::make("arg.x", 0.0, std::make_shared<AlwaysChangedPolicy>());
    auto root = ops::SumNode<>::make("root", {
        ts::RollingStats::make("mean", ts::EWMANode::make("level", x, 0.5), 4),
        ts::RollingSumNode::make("sum", x, 3),
        ts::ZScoreNode::make("z", x, 8),
        ts::OutlierGateNode::make("gate", x, 8, 2.0),
        ts::DelayNode<double>::make("through", x, 0)});
    EvalContext ctx;
    for (const double v : {1.0, 1.1, 0.9, 1.05, 0.95, 1.02}) {
        x->set(v);
        root->eval(ctx);
    }
    Reduction f = reduce(root, {x});
    EXPECT_EQ(f.size(), 7u);
    double g = 0.0;
    for (int i = 0; i < 3; ++i) {   // two calls between binds: every step has both of its buffers
        f.bind();
        f({1.0 + i});
        f.gradient({1.0 + i}, {&g, 1});
    }

    double sum = 0.0;
    const long allocs = allocations_during([&] {
        for (int i = 0; i < 100; ++i) {
            f.bind();
            sum += f({1.0 + 0.01 * i});
            sum += f.gradient({40.0 - i}, {&g, 1}) + g;   // gated, most of them
        }
    });
    EXPECT_EQ(allocs, 0);
    EXPECT_TRUE(std::isfinite(sum));
    EXPECT_NE(sum, 0.0);
}

// A cycle that moves the signal, in which a node binds a reduction through the
// average and runs six Newton steps on it.
TEST(ReductionTimeseries, ASolvingCycleThroughAStatefulNodeAllocatesNothing) {
    async::Engine engine;
    auto signal = async::AsyncInput<double>::make("signal", 0.0);
    engine.addSource(signal);
    auto level  = ts::EWMANode::make("level", signal, 0.25);
    auto energy = ops::ProductNode<>::make("energy", {level, level});

    auto f = std::make_shared<Reduction>(energy, std::vector<NodePtr>{signal});
    auto wanted = ComputeNode<double, double>::make("sample for 9", {energy},
        [f](const double&) {
            f->bind();
            double x = 20.0, slope = 0.0;
            for (int i = 0; i < 6; ++i) {
                const double miss = f->gradient({x}, {&slope, 1}) - 9.0;
                x -= miss / slope;
            }
            return x;
        });   // Eager, the default
    double last = 0.0;
    long delivered = 0;
    engine.addOutput<double>(wanted, [&](const double& v) { last = v; ++delivered; });

    for (int i = 0; i < 10; ++i) {   // every buffer of every slot
        signal->post(2.0 + i);
        engine.step();
    }
    delivered = 0;
    const long allocs = allocations_during([&] {
        for (int i = 0; i < 200; ++i) {
            signal->post(3.0 + (i % 7));
            engine.step();
        }
    });
    EXPECT_EQ(allocs, 0);
    EXPECT_EQ(delivered, 200);
    EXPECT_TRUE(std::isfinite(last));
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
