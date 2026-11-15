// =============================================================================
// flywheel-dag — A header-only C++23 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

// A tape through the dag::ts nodes: each node's partial is that of its last
// update with respect to the sample the update took, with the state the update
// started from held fixed (dag_timeseries.hpp).
//
// A partial is checked two ways: against its closed form, through a tape, and
// against a central difference between two twin nodes that take the same
// history and differ in their last sample alone.

#include <gtest/gtest.h>
#include "flywheel/dag.hpp"
#include "flywheel/dag_aad.hpp"
#include "flywheel/dag_async.hpp"
#include "flywheel/dag_engine.hpp"
#include "flywheel/dag_ops.hpp"
#include "flywheel/dag_timeseries.hpp"
#include <algorithm>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace dag;

namespace {

/// Builds the node under test over its upstream.
using Make = NodePtr (*)(const NodePtr& upstream);

/// A node over an Input, fed a history. Every sample fires, an equal one too,
/// as a source that ticks does.
struct Fed {
    InputPtr<double> x = Input<double>::make("x", 0.0, std::make_shared<AlwaysChangedPolicy>());
    NodePtr          node;
    EvalContext      ctx;

    explicit Fed(Make make) : node(make(x)) {}

    double feed(double v) {
        x->set(v);
        const ValuePtr held = node->eval(ctx);
        return get_value<double>(held);
    }
};

/// The node's partial with respect to its latest sample, from a tape, after
/// `history` and then `last`.
double tapePartial(Make make, const std::vector<double>& history, double last) {
    Fed f(make);
    for (const double v : history) f.feed(v);
    f.feed(last);
    return aad::adjoints(f.node, {f.x})[0];
}

/// The same, as a central difference between two twins that differ in the last
/// sample alone.
double twinDifference(Make make, const std::vector<double>& history, double last, double h) {
    Fed up(make);
    Fed dn(make);
    for (const double v : history) {
        up.feed(v);
        dn.feed(v);
    }
    const double fUp = up.feed(last + h);
    const double fDn = dn.feed(last - h);
    return (fUp - fDn) / ((last + h) - (last - h));
}

void expectPartial(Make make, const std::vector<double>& history, double last, double expected,
                   double h = 1e-3) {
    SCOPED_TRACE("after " + std::to_string(history.size()) + " samples, last "
                 + std::to_string(last));
    const double scale = std::max(1.0, std::abs(expected));
    EXPECT_NEAR(tapePartial(make, history, last), expected, 1e-13 * scale);
    EXPECT_NEAR(twinDifference(make, history, last, h), expected, 1e-9 * scale);
}

/// What the node's partials() writes after `history`: true, and no entry, for
/// a node that names no input.
aad::Partials partialsAfter(Make make, const std::vector<double>& history) {
    Fed f(make);
    for (const double v : history) f.feed(v);
    aad::Partials out;
    EXPECT_TRUE(std::dynamic_pointer_cast<NodeBase>(f.node)->partials(f.ctx, out));
    return out;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Each node's partial
// ─────────────────────────────────────────────────────────────────────────────

TEST(AadTimeseries, EwmaIsAlphaOnceItHoldsAValue) {
    const Make ewma = [](const NodePtr& up) -> NodePtr { return ts::EWMANode::make("ewma", up, 0.25); };
    expectPartial(ewma, {}, 3.0, 1.0);             // the first sample is taken whole
    expectPartial(ewma, {3.0}, 5.0, 0.25);
    expectPartial(ewma, {3.0, 5.0, -1.0}, 2.0, 0.25);
    expectPartial(ewma, {3.0}, 3.0, 0.25);         // a sample equal to the average held
}

TEST(AadTimeseries, RollingMeanIsOneOverItsCount) {
    const Make mean = [](const NodePtr& up) -> NodePtr { return ts::RollingStats::make("mean", up, 4); };
    expectPartial(mean, {}, 2.0, 1.0);
    expectPartial(mean, {1.0}, 2.0, 0.5);
    expectPartial(mean, {1.0, 2.0, 4.0}, 8.0, 0.25);
    expectPartial(mean, {1.0, 2.0, 4.0, 8.0, 16.0, 32.0}, 64.0, 0.25);   // full, and evicting
}

TEST(AadTimeseries, RollingSumIsOne) {
    const Make sum = [](const NodePtr& up) -> NodePtr { return ts::RollingSumNode::make("sum", up, 3); };
    expectPartial(sum, {}, 2.0, 1.0);
    expectPartial(sum, {1.0, 2.0}, 4.0, 1.0);                 // the third push re-sums the window
    expectPartial(sum, {1.0, 2.0, 4.0, 8.0}, 16.0, 1.0);      // full, and evicting
}

TEST(AadTimeseries, DeltaIsOneOnceItHasAPreviousSample) {
    const Make delta = [](const NodePtr& up) -> NodePtr { return ts::DeltaNode<double>::make("delta", up); };
    expectPartial(delta, {}, 2.0, 0.0);
    EXPECT_TRUE(partialsAfter(delta, {2.0}).entries().empty());   // the first update names no input
    expectPartial(delta, {2.0}, 5.0, 1.0);
    expectPartial(delta, {2.0, 5.0}, 5.0, 1.0);
}

TEST(AadTimeseries, ARateLimiterPassesOneOrHolds) {
    const Make limiter = [](const NodePtr& up) -> NodePtr {
        return ts::RateLimiterNode<double>::make("limiter", up, 1.0);
    };
    expectPartial(limiter, {}, 2.0, 1.0);          // the first sample always passes
    expectPartial(limiter, {2.0}, 2.5, 0.0);       // held back: within the band
    EXPECT_TRUE(partialsAfter(limiter, {2.0, 2.5}).entries().empty());
    expectPartial(limiter, {2.0}, 4.0, 1.0);       // passed
}

TEST(AadTimeseries, ADelayOfATickOrMoreDoesNotDependOnTheLatestSample) {
    const Make delayed = [](const NodePtr& up) -> NodePtr {
        return ts::DelayNode<double>::make("delayed", up, 2);
    };
    expectPartial(delayed, {}, 2.0, 0.0);
    expectPartial(delayed, {1.0, 2.0, 3.0}, 4.0, 0.0);
    EXPECT_TRUE(partialsAfter(delayed, {1.0, 2.0, 3.0}).entries().empty());

    const Make through = [](const NodePtr& up) -> NodePtr {
        return ts::DelayNode<double>::make("through", up, 0);
    };
    expectPartial(through, {}, 2.0, 1.0);
    expectPartial(through, {1.0, 2.0}, 4.0, 1.0);
}

TEST(AadTimeseries, ATickRateDoesNotDependOnItsSample) {
    const Make rate = [](const NodePtr& up) -> NodePtr {
        return ts::EWMATickRateNode::make("rate", up, 0.5);
    };
    expectPartial(rate, {}, 2.0, 0.0);
    expectPartial(rate, {1.0, 2.0}, 4.0, 0.0);
    EXPECT_TRUE(partialsAfter(rate, {1.0, 2.0}).entries().empty());
}

TEST(AadTimeseries, ZScoreIsOneOverTheSpreadBeforeTheSample) {
    const Make z = [](const NodePtr& up) -> NodePtr { return ts::ZScoreNode::make("z", up, 8); };
    expectPartial(z, {}, 2.0, 0.0);            // no spread yet: z is 0 whatever the sample
    expectPartial(z, {1.0}, 2.0, 0.0);
    EXPECT_TRUE(partialsAfter(z, {1.0, 2.0}).entries().empty());
    // After 1, 2 and 4 the spread before the next sample is the standard
    // deviation of those three.
    const double mean = 7.0 / 3.0;
    const double sd   = std::sqrt(((1.0 - mean) * (1.0 - mean) + (2.0 - mean) * (2.0 - mean)
                                   + (4.0 - mean) * (4.0 - mean)) / 2.0);
    expectPartial(z, {1.0, 2.0, 4.0}, 3.0, 1.0 / sd);
}

TEST(AadTimeseries, AnOutlierGatePassesOneOrTheMeansShare) {
    const Make gate = [](const NodePtr& up) -> NodePtr {
        return ts::OutlierGateNode::make("gate", up, 8, 2.0);
    };
    const std::vector<double> calm = {1.0, 1.1, 0.9, 1.05, 0.95};
    expectPartial(gate, calm, 1.02, 1.0);              // passes: the sample is the value
    expectPartial(gate, calm, 50.0, 1.0 / 6.0);        // gated: the mean of six holds it
}

// A node whose value is not a double has no derivative, and is a barrier.
TEST(AadTimeseries, ANodeThatHoldsNoDoubleGivesNone) {
    auto i = Input<int>::make("i", 1);
    EvalContext ctx;
    aad::Partials out;
    EXPECT_FALSE(ts::DeltaNode<int>::make("delta", i)->partials(ctx, out));
    EXPECT_FALSE(ts::DelayNode<int>::make("through", i, 0)->partials(ctx, out));
    EXPECT_TRUE(ts::DelayNode<int>::make("delayed", i, 3)->partials(ctx, out));
    EXPECT_FALSE(ts::RateLimiterNode<int>::make("limiter", i, 1)->partials(ctx, out));
    EXPECT_TRUE(out.entries().empty());

    auto x = Input<double>::make("x", 1.0);
    auto b = Input<bool>::make("b", true);
    EXPECT_FALSE(ts::RollingMinMaxNode::make("range", x, 4)->partials(ctx, out));
    EXPECT_FALSE(ts::WindowNode<double>::make("window", x, 4)->partials(ctx, out));
    EXPECT_FALSE(ts::ThresholdNode<double>::make("threshold", x, 1.0)->partials(ctx, out));
    EXPECT_FALSE(ts::DebounceCountNode::make("debounce", b, 2)->partials(ctx, out));
    EXPECT_FALSE(ts::LatchedDebounceNode::make("latched", b, 2)->partials(ctx, out));
    EXPECT_TRUE(out.entries().empty());
}

// ─────────────────────────────────────────────────────────────────────────────
// On a tape
// ─────────────────────────────────────────────────────────────────────────────

// The chain rule through two stateful nodes and an op, in both directions.
TEST(AadTimeseries, ATapeSweepsThroughStatefulNodes) {
    auto x     = Input<double>::make("x", 0.0);
    auto level = ts::EWMANode::make("level", x, 0.5);
    auto mean  = ts::RollingStats::make("mean", level, 4);
    auto k     = Input<double>::make("k", 3.0);
    auto root  = ops::ProductNode<>::make("root", {mean, k});
    EvalContext ctx;
    for (const double v : {1.0, 2.0, 4.0}) {
        x->set(v);
        root->eval(ctx);
    }
    const double meanNow = get_value<double>(mean->eval(ctx));
    const std::vector<double> g = aad::adjoints(root, {x, level, mean, k});
    EXPECT_DOUBLE_EQ(g[0], 3.0 * (1.0 / 3.0) * 0.5);   // k · 1/count · α
    EXPECT_DOUBLE_EQ(g[1], 3.0 * (1.0 / 3.0));
    EXPECT_DOUBLE_EQ(g[2], 3.0);
    EXPECT_DOUBLE_EQ(g[3], meanNow);
    const std::vector<double> t = aad::tangents({root}, {{x, 1.0}});
    EXPECT_DOUBLE_EQ(t[0], g[0]);
}

// An input that the latest sample does not reach is not followed, so a barrier
// above it is not one.
TEST(AadTimeseries, ANodeThatNamesNoInputHidesABarrierAboveIt) {
    auto x       = Input<double>::make("x", 1.0);
    auto opaque  = ComputeNode<double, double>::make("opaque", {x},
        [](const double& v) { return 2.0 * v; });
    auto delayed = ts::DelayNode<double>::make("delayed", opaque, 1);
    auto root    = ops::SumNode<>::make("root", {delayed, x});
    EvalContext ctx;
    root->eval(ctx);
    EXPECT_EQ(aad::adjoints(root, {x})[0], 1.0);

    auto through = ts::DelayNode<double>::make("through", opaque, 0);
    auto root2   = ops::SumNode<>::make("root2", {through, x});
    root2->eval(ctx);
    EXPECT_THROW(aad::adjoints(root2, {x}), std::domain_error);
}

// A pass reads the node's state and advances nothing: three passes after each
// update leave the node where a control fed the same samples is.
TEST(AadTimeseries, APassThroughAStatefulNodeDoesNotAdvanceIt) {
    auto x       = Input<double>::make("x", 1.0);
    auto ewma    = ts::EWMANode::make("ewma", x, 0.5);
    auto k       = Input<double>::make("k", 3.0);
    auto root    = ops::ProductNode<>::make("root", {ewma, k});
    auto xCtl    = Input<double>::make("xCtl", 1.0);
    auto control = ts::EWMANode::make("control", xCtl, 0.5);
    EvalContext ctx;
    bool first = true;
    for (const double v : {1.0, 2.0, 4.0, 8.0}) {
        x->set(v);
        xCtl->set(v);
        root->eval(ctx);
        control->eval(ctx);
        for (int i = 0; i < 3; ++i) EXPECT_EQ(aad::adjoints(root, {x})[0], first ? 3.0 : 1.5);
        EXPECT_EQ(get_value<double>(ewma->eval(ctx)), get_value<double>(control->eval(ctx)));
        first = false;
    }
}

// In an engine, a gradient node delivers the derivative through a stateful
// node on every cycle: 2·ewma·α for ewma², and 2·ewma for the first sample.
TEST(AadTimeseries, AGradientNodeDeliversThroughAStatefulNode) {
    async::Engine engine;
    auto s = async::AsyncInput<double>::make("s", 0.0);
    engine.addSource(s);
    auto level = ts::EWMANode::make("level", s, 0.25);
    auto sq    = ops::ProductNode<>::make("sq", {level, level});
    auto grad  = aad::GradientNode::make("grad", sq, {s});
    std::vector<double> seen;
    engine.addOutput<std::vector<double>>(grad, [&](const std::vector<double>& g) {
        seen.push_back(g[0]);
    });

    std::vector<double> expected;
    double ewma = 0.0;
    bool   held = false;
    for (const double v : {2.0, 6.0, 1.0}) {
        s->post(v);
        engine.step();
        const double alpha = held ? 0.25 : 1.0;
        ewma = held ? 0.25 * v + 0.75 * ewma : v;
        held = true;
        expected.push_back(2.0 * ewma * alpha);
    }
    EXPECT_EQ(seen, expected);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
