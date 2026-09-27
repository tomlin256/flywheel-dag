// =============================================================================
// flywheel-dag — A header-only C++17 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

// test_window_status_integration.cpp — per-node IWindowed integration tests

#include <gtest/gtest.h>

#include <memory>
#include <vector>

#include "flywheel/dag.hpp"
#include "flywheel/dag_engine.hpp"
#include "flywheel/dag_timeseries.hpp"
#include "flywheel/dag_window_status.hpp"

using namespace dag;
using namespace dag::async;
using namespace dag::ts;

// ─────────────────────────────────────────────────────────────────────────────
// Helper: evaluate a node and return the typed value
// ─────────────────────────────────────────────────────────────────────────────
template<typename T, typename Node>
T evalNode(const std::shared_ptr<Node>& n)
{
    EvalContext ctx;
    return get_value<T>(n->eval(ctx));
}

// ─────────────────────────────────────────────────────────────────────────────
// RollingStats
// ─────────────────────────────────────────────────────────────────────────────

TEST(WindowStatusIntegration, RollingStatsCapacityFilled)
{
    auto inp   = Input<double>::make("x", 1.0);
    auto stats = RollingStats::make("rs",
        std::static_pointer_cast<INode>(inp), 5);

    EvalContext ctx;
    stats->eval(ctx);
    EXPECT_EQ(stats->capacity(), 5u);
    EXPECT_EQ(stats->filled(),   1u);

    inp->set(2.0); EvalContext c2; stats->eval(c2);
    inp->set(3.0); EvalContext c3; stats->eval(c3);

    EXPECT_EQ(stats->capacity(), 5u);
    EXPECT_EQ(stats->filled(),   3u);
}

TEST(WindowStatusIntegration, RollingStatsStatusNodeReflectsFill)
{
    auto inp   = Input<double>::make("x", 0.0);
    auto stats = RollingStats::make("rs",
        std::static_pointer_cast<INode>(inp), 5);
    auto statusNode = stats->windowStatusNode();

    std::vector<WindowStatus> seen;
    Engine engine;
    engine.addOutput<WindowStatus>(statusNode, [&](const WindowStatus& s) {
        seen.push_back(s);
    });

    for (int i = 1; i <= 5; ++i) {
        inp->set(static_cast<double>(i));
        engine.step();
    }

    ASSERT_EQ(seen.size(), 5u);
    EXPECT_EQ(seen[0].filled, 1u);
    EXPECT_EQ(seen[1].filled, 2u);
    EXPECT_EQ(seen[4].filled, 5u);
    EXPECT_TRUE(seen[4].full());
}

TEST(WindowStatusIntegration, RollingStatsStatusNodeIdentityWhileHeld)
{
    auto inp   = Input<double>::make("x", 1.0);
    auto stats = RollingStats::make("rs",
        std::static_pointer_cast<INode>(inp), 3);

    auto first  = stats->windowStatusNode();
    // second call while first is still alive — must return the same node
    auto second = stats->windowStatusNode();
    EXPECT_EQ(first.get(), second.get());
}

TEST(WindowStatusIntegration, RollingStatsStatusNodeRebuildsWhenReleased)
{
    auto inp   = Input<double>::make("x", 1.0);
    auto stats = RollingStats::make("rs",
        std::static_pointer_cast<INode>(inp), 3);

    {
        auto tmp = stats->windowStatusNode();
        // tmp released at end of block
    }
    auto fresh = stats->windowStatusNode();
    EXPECT_NE(fresh, nullptr);
}

TEST(WindowStatusIntegration, RollingStatsStatusNodeNoCycle)
{
    auto inp = Input<double>::make("x", 1.0);

    std::weak_ptr<RollingStats> weakStats;
    {
        auto stats = RollingStats::make("rs",
            std::static_pointer_cast<INode>(inp), 3);
        weakStats = stats;

        // Call windowStatusNode() but immediately release the returned ptr.
        { auto _ = stats->windowStatusNode(); }
        // stats goes out of scope — no external strong ref should keep it alive.
    }
    EXPECT_TRUE(weakStats.expired());
}

TEST(WindowStatusIntegration, RollingStatsStatusDirtyPropagation)
{
    auto inp   = Input<double>::make("x", 0.0);
    auto stats = RollingStats::make("rs",
        std::static_pointer_cast<INode>(inp), 3);
    auto statusNode = stats->windowStatusNode();

    int callCount = 0;
    Engine engine;
    engine.addOutput<WindowStatus>(statusNode, [&](const WindowStatus&) {
        ++callCount;
    });

    // Push 3 values to fill the window.
    for (int i = 1; i <= 3; ++i) {
        inp->set(static_cast<double>(i));
        engine.step();
    }
    int countAfterFull = callCount;

    // Push one more — window is full; status should NOT change (equality suppresses it).
    inp->set(4.0);
    engine.step();

    EXPECT_EQ(callCount, countAfterFull);  // no extra downstream fire
}

// ─────────────────────────────────────────────────────────────────────────────
// DelayNode<T>
// ─────────────────────────────────────────────────────────────────────────────

TEST(WindowStatusIntegration, DelayNodeCapacityFilled)
{
    auto inp = Input<double>::make("x", 1.0);
    auto dly = DelayNode<double>::make("d",
        std::static_pointer_cast<INode>(inp), 3);

    EvalContext ctx;
    dly->eval(ctx);
    EXPECT_EQ(dly->capacity(), 3u);
    EXPECT_EQ(dly->filled(),   1u);

    inp->set(2.0); EvalContext c2; dly->eval(c2);
    EXPECT_EQ(dly->filled(), 2u);
}

TEST(WindowStatusIntegration, DelayNodeStatusNodeFiresOncePerTick)
{
    auto inp = Input<double>::make("x", 0.0);
    auto dly = DelayNode<double>::make("d",
        std::static_pointer_cast<INode>(inp), 3);
    auto statusNode = dly->windowStatusNode();

    int callCount = 0;
    Engine engine;
    engine.addOutput<WindowStatus>(statusNode, [&](const WindowStatus&) {
        ++callCount;
    });

    for (int i = 1; i <= 3; ++i) {
        inp->set(static_cast<double>(i));
        engine.step();
    }
    // Status changes on each of the 3 fill ticks.
    EXPECT_EQ(callCount, 3);
}

// ─────────────────────────────────────────────────────────────────────────────
// WindowNode<T>
// ─────────────────────────────────────────────────────────────────────────────

TEST(WindowStatusIntegration, WindowNodeCapacityFilled)
{
    auto inp = Input<double>::make("x", 1.0);
    auto win = WindowNode<double>::make("w",
        std::static_pointer_cast<INode>(inp), 4);

    EvalContext ctx;
    win->eval(ctx);
    EXPECT_EQ(win->capacity(), 4u);
    EXPECT_EQ(win->filled(),   1u);

    for (int i = 2; i <= 4; ++i) {
        inp->set(static_cast<double>(i));
        EvalContext c; win->eval(c);
    }
    EXPECT_EQ(win->filled(), 4u);
    EXPECT_TRUE(win->full());  // WindowNode::full() delegate still works
}

// ─────────────────────────────────────────────────────────────────────────────
// RollingMinMaxNode
// ─────────────────────────────────────────────────────────────────────────────

TEST(WindowStatusIntegration, RollingMinMaxCapacityFilled)
{
    auto inp = Input<double>::make("x", 1.0);
    auto mmx = RollingMinMaxNode::make("mm",
        std::static_pointer_cast<INode>(inp), 6);

    EvalContext ctx;
    mmx->eval(ctx);
    EXPECT_EQ(mmx->capacity(), 6u);
    EXPECT_EQ(mmx->filled(),   1u);
    EXPECT_FALSE(mmx->filled() >= mmx->capacity());

    for (int i = 2; i <= 3; ++i) {
        inp->set(static_cast<double>(i));
        EvalContext c; mmx->eval(c);
    }
    EXPECT_EQ(mmx->filled(), 3u);
    EXPECT_FALSE(mmx->filled() >= mmx->capacity());
}

// ─────────────────────────────────────────────────────────────────────────────
// Main
// ─────────────────────────────────────────────────────────────────────────────
int main(int argc, char** argv)
{
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
