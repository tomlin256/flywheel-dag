// =============================================================================
// flywheel-dag — A header-only C++17 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

// test_clean_inputs.cpp — a node is clean only when every input it read still
// is (flywheel-dag#18).
//
// NodeBase::propagate() stops a cascade at a node that is already dirty. That
// is safe only while a clean node's inputs are clean. An evaluation that ends
// clean over an input it read, which went dirty again before the evaluation
// ended, breaks that, and every later change then stops at the input.
//
// Coverage
// ────────
//  UntakenBranch — a ConditionNode hears only the branch it took, so the
//                  branch it did not take can no longer make an input dirty
//                  again under a consumer's evaluation.

#include <gtest/gtest.h>
#include "flywheel/dag.hpp"
#include "flywheel/dag_async.hpp"
#include "flywheel/dag_engine.hpp"
#include "flywheel/dag_ops.hpp"
#include <cmath>
#include <memory>
#include <vector>

using namespace dag;

namespace {

const double e  = std::exp(1.0);
const double e2 = std::exp(2.0);

// The graph of flywheel-dag#18, over inputs of type In. sel takes a = e^x, so
// b = √y, on the branch it did not take, is pulled only by root2. c pulls root1
// before root2.
template<template<typename> class In>
struct IssueGraph {
    std::shared_ptr<In<double>> x = In<double>::make("x", 1.0);
    std::shared_ptr<In<double>> y = In<double>::make("y", 4.0);
    InputPtr<bool> cond = Input<bool>::make("cond", true);
    NodePtr b     = ops::SqrtNode<>::make("b", y);
    NodePtr sel   = ConditionNode::make("sel", cond, ops::ExpNode<>::make("a", x), b);
    NodePtr root1 = ops::ProductNode<>::make("root1", {sel, Input<double>::make("two", 2.0)});
    NodePtr root2 = ops::SumNode<>::make("root2", {b, Input<double>::make("one", 1.0)});
    ComputeNodePtr<double, double, double> c = ComputeNode<double, double, double>::make(
        "c", {root1, root2}, [](const double& p, const double& q) { return p + q; });
};

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// UntakenBranch
// ─────────────────────────────────────────────────────────────────────────────

// The issue's sequence. Before the fix, root2's pull evaluated b after c had
// read root1, b told sel, and c ended clean over a dirty root1. x's move then
// stopped at sel, and c kept 2e + 4.
TEST(UntakenBranch, TheIssuesSequenceSeesTheLaterChange) {
    IssueGraph<Input> g;
    EvalContext ctx;
    g.c->eval(ctx);

    g.y->set(9.0);
    EXPECT_DOUBLE_EQ(get_value<double>(g.c->eval(ctx)), 2.0 * e + 4.0);
    EXPECT_FALSE(g.root1->dirty());

    g.x->set(2.0);
    EXPECT_DOUBLE_EQ(get_value<double>(g.c->eval(ctx)), 2.0 * e2 + 4.0);
}

// The same through an engine, which delivered nothing for x = 2.
TEST(UntakenBranch, AnEngineDeliversTheLaterChange) {
    async::Engine engine;
    IssueGraph<async::AsyncInput> g;
    engine.addSource(g.x);
    engine.addSource(g.y);
    std::vector<double> values;
    engine.addOutput<double>(g.c, [&](const double& v) { values.push_back(v); });

    engine.step();
    g.y->post(9.0);
    engine.step();
    g.x->post(2.0);
    engine.step();

    ASSERT_EQ(values.size(), 3u);
    EXPECT_DOUBLE_EQ(values[0], 2.0 * e + 3.0);
    EXPECT_DOUBLE_EQ(values[1], 2.0 * e + 4.0);
    EXPECT_DOUBLE_EQ(values[2], 2.0 * e2 + 4.0);
}

// b has been evaluated, so it is clean, and its move cascades. sel no longer
// hears it while it takes a, so neither sel nor the Eager node below it has
// anything to do. A switch to b still reads b's new value.
TEST(UntakenBranch, ItsMoveReachesNoConsumer) {
    auto cond = Input<bool>::make("cond", true);
    auto x    = Input<double>::make("x", 1.0);
    auto y    = Input<double>::make("y", 4.0);
    auto sel  = ConditionNode::make("sel", cond, ops::ExpNode<>::make("a", x),
                                    ops::SqrtNode<>::make("b", y));
    int runs = 0;
    auto below = ComputeNode<double, double>::make("below", {sel},
        [&runs](const double& v) { ++runs; return v; });
    EvalContext ctx;
    below->eval(ctx);
    cond->set(false);
    below->eval(ctx);   // b's first evaluation, which leaves it clean
    cond->set(true);
    below->eval(ctx);
    const int before = runs;

    y->set(9.0);
    EXPECT_FALSE(sel->dirty());
    EXPECT_FALSE(below->dirty());
    below->eval(ctx);
    EXPECT_EQ(runs, before);

    cond->set(false);
    EXPECT_DOUBLE_EQ(get_value<double>(below->eval(ctx)), 3.0);
}

// One node on both branches has a listener for each, and whichever branch is
// taken passes its move on.
TEST(UntakenBranch, OneNodeOnBothBranchesIsHeardEitherWay) {
    auto cond = Input<bool>::make("cond", true);
    auto x    = Input<double>::make("x", 1.0);
    NodePtr a = ops::NegateNode<>::make("a", x);
    auto sel  = ConditionNode::make("sel", cond, a, a);
    EvalContext ctx;
    EXPECT_DOUBLE_EQ(get_value<double>(sel->eval(ctx)), -1.0);

    x->set(2.0);
    EXPECT_DOUBLE_EQ(get_value<double>(sel->eval(ctx)), -2.0);

    cond->set(false);
    EXPECT_DOUBLE_EQ(get_value<double>(sel->eval(ctx)), -2.0);
    x->set(3.0);
    EXPECT_DOUBLE_EQ(get_value<double>(sel->eval(ctx)), -3.0);
}

// The node owns its listeners, and a listener only watches its node. Released,
// the node expires, and a later move of its branch reaches listeners that are
// gone.
TEST(UntakenBranch, ItsListenersDoNotKeepItAlive) {
    auto cond = Input<bool>::make("cond", true);
    auto x    = Input<double>::make("x", 1.0);
    std::weak_ptr<ConditionNode> watch;
    {
        auto sel = ConditionNode::make("sel", cond, x, x);
        EvalContext ctx;
        sel->eval(ctx);
        watch = sel;
    }
    EXPECT_TRUE(watch.expired());
    x->set(2.0);
    cond->set(false);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
