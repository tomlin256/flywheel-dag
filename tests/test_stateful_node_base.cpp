// =============================================================================
// flywheel-dag — A header-only C++17 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

// test_stateful_node_base.cpp — Unit tests for StatefulNodeBase scaffolding
//
// Uses a toy SumNode (running sum) to exercise the base independently of any
// real time-series node.
//
// Coverage:
//   1. eval() accumulates correctly over N ticks
//   2. eval() returns cached value on second call (dirty guard)
//   3. saveState / restoreState round-trip preserves state
//   4. restoreState marks the node dirty (invalidate() called)
//   5. eval() after restoreState produces the correct next output
//   6. Downstream is not notified when output value is unchanged (equality policy)

#include <gtest/gtest.h>
#include "flywheel/dag.hpp"
#include "flywheel/dag_timeseries.hpp"
#include "flywheel/dag_state_store.hpp"

using namespace dag;
using namespace dag::ts;

// ─────────────────────────────────────────────────────────────────────────────
// Toy node: running sum of all inputs seen so far.
// ─────────────────────────────────────────────────────────────────────────────
struct SumNodeState { double sum = 0.0; };

class SumNode : public StatefulNodeBase<SumNode, double, double, SumNodeState> {
public:
    using State = SumNodeState;

    static std::shared_ptr<SumNode> make(std::string name, NodePtr upstream) {
        auto self = std::shared_ptr<SumNode>(new SumNode(std::move(name), upstream));
        wire(self, self->inputs());
        return self;
    }

    double doCompute(const double& x, State& s) {
        s.sum += x;
        return s.sum;
    }

    void doSaveState(INodeState& s, const State& st) const {
        s["sum"] = st.sum;
    }

    void doRestoreState(const INodeState& s, State& st) {
        st.sum = s["sum"].as<double>();
    }

private:
    explicit SumNode(std::string n, NodePtr up)
        : StatefulNodeBase<SumNode, double, double, SumNodeState>(std::move(n), up)
    {}
};

// ─────────────────────────────────────────────────────────────────────────────
// Helper: push a value through the DAG and return the sum node's output.
// ─────────────────────────────────────────────────────────────────────────────
static double evalSum(const std::shared_ptr<Input<double>>& inp,
                      const std::shared_ptr<SumNode>& node,
                      double value)
{
    inp->set(value);
    EvalContext ctx;
    return get_value<double>(node->eval(ctx));
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 1 — eval() accumulates correctly over N ticks
// ─────────────────────────────────────────────────────────────────────────────
TEST(StatefulNodeBase, AccumulatesCorrectly) {
    auto inp  = Input<double>::make("inp", 0.0);
    auto node = SumNode::make("sum", inp);

    EXPECT_DOUBLE_EQ(evalSum(inp, node, 1.0), 1.0);
    EXPECT_DOUBLE_EQ(evalSum(inp, node, 2.0), 3.0);
    EXPECT_DOUBLE_EQ(evalSum(inp, node, 3.0), 6.0);
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 2 — eval() returns cached value on second call without a new tick
// ─────────────────────────────────────────────────────────────────────────────
TEST(StatefulNodeBase, DirtyGuardCachesResult) {
    auto inp  = Input<double>::make("inp", 0.0);
    auto node = SumNode::make("sum", inp);

    inp->set(5.0);
    EvalContext ctx;
    double first  = get_value<double>(node->eval(ctx));
    double second = get_value<double>(node->eval(ctx)); // no new set(), dirty=false

    EXPECT_DOUBLE_EQ(first,  5.0);
    EXPECT_DOUBLE_EQ(second, 5.0);

    // Verify state was not recomputed: pushing 0 produces sum = 5
    inp->set(0.0);
    double third = get_value<double>(node->eval(ctx));
    EXPECT_DOUBLE_EQ(third, 5.0); // 5 + 0 = 5
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 3 — saveState / restoreState round-trip preserves state
// ─────────────────────────────────────────────────────────────────────────────
TEST(StatefulNodeBase, SaveRestoreRoundTrip) {
    auto inp  = Input<double>::make("inp", 0.0);
    auto node = SumNode::make("mysum", inp);

    evalSum(inp, node, 10.0);
    evalSum(inp, node, 20.0); // sum = 30

    InMemoryStateStore store;
    store.save({node});

    // Restore into a fresh node with the same name
    auto inp2  = Input<double>::make("inp2", 0.0);
    auto node2 = SumNode::make("mysum", inp2);
    store.restore({node2});

    inp2->set(5.0);
    EvalContext ctx;
    EXPECT_DOUBLE_EQ(get_value<double>(node2->eval(ctx)), 35.0); // 30 + 5
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 4 — restoreState marks the node dirty (invalidate() is called)
// ─────────────────────────────────────────────────────────────────────────────
TEST(StatefulNodeBase, RestoreMarksNodeDirty) {
    auto inp  = Input<double>::make("inp", 0.0);
    auto node = SumNode::make("mysum", inp);

    evalSum(inp, node, 1.0); // clears dirty
    EXPECT_FALSE(node->dirty());

    InMemoryStateStore store;
    store.save({node});
    store.restore({node}); // restore into the same node

    EXPECT_TRUE(node->dirty());
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 5 — eval() after restoreState produces the correct next output
// ─────────────────────────────────────────────────────────────────────────────
TEST(StatefulNodeBase, EvalAfterRestoreIsCorrect) {
    auto inp  = Input<double>::make("inp", 0.0);
    auto node = SumNode::make("mysum", inp);

    evalSum(inp, node, 100.0); // sum = 100

    InMemoryStateStore store;
    store.save({node});

    auto inp2  = Input<double>::make("inp2", 0.0);
    auto node2 = SumNode::make("mysum", inp2);
    store.restore({node2});

    EXPECT_DOUBLE_EQ(evalSum(inp2, node2,  7.0), 107.0);
    EXPECT_DOUBLE_EQ(evalSum(inp2, node2,  3.0), 110.0);
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 6 — Downstream is NOT notified when output value is unchanged
//
// SumNode uses TypedEqualityPolicy<double>. When the base's eval() produces
// the same output twice, notifyDownstream should NOT invalidate downstream.
//
// We use forceRecompute to re-run the computation without a dirty cascade so
// that the downstream node starts clean and we can observe the suppression.
// ─────────────────────────────────────────────────────────────────────────────
TEST(StatefulNodeBase, EqualityPolicySuppressesNotification) {
    // SumNode with sum = 0 as upstream (constant zero input)
    auto inp  = Input<double>::make("inp", 0.0);
    auto sum1 = SumNode::make("sum1", inp);
    auto sum2 = SumNode::make("sum2", sum1);

    // Push 0.0 — sum1 stays at 0.0, downstream is dirty from cascade
    inp->set(0.0);
    EvalContext ctx;
    sum1->eval(ctx);

    // Evaluate sum2 to clear its dirty flag
    sum2->eval(ctx);
    EXPECT_FALSE(sum2->dirty());

    // Force recompute sum1 without changing inp (no new cascade).
    // sum1 re-runs doCompute(0.0) → state.sum stays 0.0 (same as cached).
    // TypedEqualityPolicy<double> sees 0.0 == 0.0 → sum2 NOT invalidated.
    EvalContext forceCtx;
    forceCtx.forceRecompute = true;
    sum1->eval(forceCtx);

    EXPECT_FALSE(sum2->dirty()); // equality policy suppressed the notification
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
