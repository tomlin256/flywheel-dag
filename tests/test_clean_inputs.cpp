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
//  CleanInputs   — every node that pulls stays dirty, and tells its
//                  consumers, when an input it read goes dirty again before
//                  its evaluation ends. An always-dirty node reached by two
//                  paths still does that, as an application's clock-driven
//                  node can. Each node kind is a test parameter.

#include <gtest/gtest.h>
#include "flywheel/dag.hpp"
#include "flywheel/dag_aad.hpp"
#include "flywheel/dag_async.hpp"
#include "flywheel/dag_engine.hpp"
#include "flywheel/dag_memoize.hpp"
#include "flywheel/dag_ops.hpp"
#include "flywheel/dag_timeseries.hpp"
#include "test_nodes.hpp"
#include <cmath>
#include <functional>
#include <memory>
#include <ostream>
#include <string>
#include <vector>

using namespace dag;

namespace {

const double e  = std::exp(1.0);
const double e2 = std::exp(2.0);

double sum(const double& p, const double& q) { return p + q; }

/// A node's value, read through a ValuePtr held while it is read.
double valueOf(const NodePtr& node, EvalContext& ctx) {
    const ValuePtr v = node->eval(ctx);
    return get_value<double>(v);
}

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

// ─────────────────────────────────────────────────────────────────────────────
// CleanInputs
// ─────────────────────────────────────────────────────────────────────────────

namespace {

// Two paths: an always-dirty k reaches c through j = −(k·x) and through k + 1,
// so c is −2x + 3. Every evaluation of c pulls k twice, and the second pull
// tells k·x, which leaves j dirty again after c has read it.
template<template<typename> class In>
struct TwoPaths {
    std::shared_ptr<test_nodes::AlwaysFiring> k = test_nodes::AlwaysFiring::make(2.0);
    std::shared_ptr<In<double>> x = In<double>::make("x", 3.0);
    NodePtr j     = ops::NegateNode<>::make("j", ops::ProductNode<>::make("k.x", {k, x}));
    NodePtr kPlus = ops::SumNode<>::make("k+1", {k, Input<double>::make("one", 1.0)});
};

// One shot: the same shape over a tripwire t, with k + 1 replaced by t plus a
// constant of w, so c is still −2x + 3. Once c has settled, trip() moves w,
// which reaches c without changing a value c reads, and arms t to fire on its
// next pull. c then reads j clean, and its pull of the second path fires t,
// which leaves j dirty again. Nothing c reads moves, so a Lazy c skips.
struct OneShot {
    std::shared_ptr<test_nodes::Tripwire> t = test_nodes::Tripwire::make(2.0);
    InputPtr<double> x = Input<double>::make("x", 3.0);
    InputPtr<double> w = Input<double>::make("w", 0.0);
    NodePtr j     = ops::NegateNode<>::make("j", ops::ProductNode<>::make("t.x", {t, x}));
    NodePtr tPlus = ops::SumNode<>::make("t+1", {t, ComputeNode<double, double>::make(
        "one", {w}, [](const double&) { return 1.0; }, InvalidationMode::Lazy)});

    void trip() {
        w->set(w->get() + 1.0);
        t->arm(1);
    }
};

double minusTwoXPlusThree(const InputPtr<double>& x) { return -2.0 * x->get() + 3.0; }

// ── The kinds of node under test ─────────────────────────────────────────────

/// A node over two inputs, p and q, whose value is p + q.
struct PairCase {
    std::string name;
    std::function<NodePtr(const NodePtr& p, const NodePtr& q)> make;
};

void PrintTo(const PairCase& c, std::ostream* os) { *os << c.name; }

/// A node over one input, c.
struct TopCase {
    std::string name;
    std::function<NodePtr(const NodePtr& c)> make;
    /// The node's value as a function of c's, or empty where it is not one.
    std::function<double(double)> of;
};

void PrintTo(const TopCase& c, std::ostream* os) { *os << c.name; }

template<typename Case>
std::string caseName(const ::testing::TestParamInfo<Case>& info) { return info.param.name; }

std::vector<PairCase> pairCases() {
    using Compute   = ComputeNode<double, double, double>;
    using InPlace   = InPlaceComputeNode<double, double, double>;
    using Tweakable = TweakableComputeNode<double, double, double>;
    using Memoized  = MemoizedComputeNode<double, double, double>;
    using Dual      = aad::DifferentiableNode<2>;
    const auto sumInto = [](double& out, const double& p, const double& q) { out = p + q; };
    const auto dualSum = [](const auto& p, const auto& q) { return p + q; };
    const auto lazy = InvalidationMode::Lazy;
    return {
        {"ComputeEager", [](const NodePtr& p, const NodePtr& q) -> NodePtr {
            return Compute::make("c", {p, q}, sum); }},
        {"ComputeLazy", [=](const NodePtr& p, const NodePtr& q) -> NodePtr {
            return Compute::make("c", {p, q}, sum, lazy); }},
        {"InPlaceEager", [=](const NodePtr& p, const NodePtr& q) -> NodePtr {
            return InPlace::make("c", {p, q}, sumInto); }},
        {"InPlaceLazy", [=](const NodePtr& p, const NodePtr& q) -> NodePtr {
            return InPlace::make("c", {p, q}, sumInto, lazy); }},
        {"TweakableEager", [](const NodePtr& p, const NodePtr& q) -> NodePtr {
            return Tweakable::make("c", {p, q}, sum); }},
        {"TweakableLazy", [=](const NodePtr& p, const NodePtr& q) -> NodePtr {
            return Tweakable::make("c", {p, q}, sum, lazy); }},
        {"MemoizedEager", [](const NodePtr& p, const NodePtr& q) -> NodePtr {
            return Memoized::make("c", {p, q}, sum); }},
        {"MemoizedLazy", [=](const NodePtr& p, const NodePtr& q) -> NodePtr {
            return Memoized::make("c", {p, q}, sum, lazy); }},
        {"DifferentiableEager", [=](const NodePtr& p, const NodePtr& q) -> NodePtr {
            return Dual::make("c", {p, q}, dualSum); }},
        {"DifferentiableLazy", [=](const NodePtr& p, const NodePtr& q) -> NodePtr {
            return Dual::make("c", {p, q}, dualSum, lazy); }},
        // p − (−q), so that the binary op's value is p + q too.
        {"BinaryOp", [](const NodePtr& p, const NodePtr& q) -> NodePtr {
            return ops::DiffNode<>::make("c", p, ops::NegateNode<>::make("-q", q)); }},
        {"NAryOp", [](const NodePtr& p, const NodePtr& q) -> NodePtr {
            return ops::SumNode<>::make("c", {p, q}); }},
    };
}

// The pair kinds that can skip: every Lazy one, and the ops. Then ComputeNode,
// InPlaceComputeNode and TweakableComputeNode again, over an input that is not
// trivially copyable, which takes their other pull path.
std::vector<PairCase> lazyPairCases() {
    std::vector<PairCase> cases;
    for (const PairCase& c : pairCases())
        if (c.name.find("Eager") == std::string::npos) cases.push_back(c);

    using Vector = std::vector<double>;
    const auto asVector = [](const NodePtr& q) -> NodePtr {
        return ComputeNode<Vector, double>::make("[q]", {q},
            [](const double& v) { return Vector{v}; });
    };
    const auto sumOf = [](const double& p, const Vector& q) { return p + q[0]; };
    const auto sumOfInto = [](double& out, const double& p, const Vector& q) { out = p + q[0]; };
    const auto lazy = InvalidationMode::Lazy;
    cases.push_back({"ComputeLazyRef", [=](const NodePtr& p, const NodePtr& q) -> NodePtr {
        return ComputeNode<double, double, Vector>::make("c", {p, asVector(q)}, sumOf, lazy); }});
    cases.push_back({"InPlaceLazyRef", [=](const NodePtr& p, const NodePtr& q) -> NodePtr {
        return InPlaceComputeNode<double, double, Vector>::make(
            "c", {p, asVector(q)}, sumOfInto, lazy); }});
    cases.push_back({"TweakableLazyRef", [=](const NodePtr& p, const NodePtr& q) -> NodePtr {
        return TweakableComputeNode<double, double, Vector>::make(
            "c", {p, asVector(q)}, sumOf, lazy); }});
    return cases;
}

std::vector<TopCase> topCases() {
    const auto same = [](double v) { return v; };
    return {
        {"UnaryOp", [](const NodePtr& c) -> NodePtr { return ops::NegateNode<>::make("top", c); },
            [](double v) { return -v; }},
        {"Condition", [](const NodePtr& c) -> NodePtr {
            return ConditionNode::make("top", Input<bool>::make("take-c", true), c,
                                       Input<double>::make("other", -1.0)); },
            same},
        // α = 1: the latest value.
        {"StatefulNode", [](const NodePtr& c) -> NodePtr { return ts::EWMANode::make("top", c, 1.0); },
            same},
        {"ZScore", [](const NodePtr& c) -> NodePtr { return ts::ZScoreNode::make("top", c, 4); },
            nullptr},
        // A threshold no z-score reaches, so it passes c through.
        {"OutlierGate", [](const NodePtr& c) -> NodePtr {
            return ts::OutlierGateNode::make("top", c, 4, 1e9); },
            same},
    };
}

// The top kinds that can skip. The other three are time-series nodes, which
// are Eager.
std::vector<TopCase> lazyTopCases() {
    std::vector<TopCase> cases;
    for (const TopCase& c : topCases())
        if (c.name == "UnaryOp" || c.name == "Condition") cases.push_back(c);
    return cases;
}

class PairKind     : public ::testing::TestWithParam<PairCase> {};
class LazyPairKind : public ::testing::TestWithParam<PairCase> {};
class TopKind      : public ::testing::TestWithParam<TopCase> {};
class LazyTopKind  : public ::testing::TestWithParam<TopCase> {};

} // namespace

// ── Nodes over two inputs ────────────────────────────────────────────────────

// Two paths: c stays dirty with j, and x's later moves reach it.
TEST_P(PairKind, StaysDirtyWhileAnInputItReadIs) {
    TwoPaths<Input> g;
    const NodePtr c = GetParam().make(g.j, g.kPlus);
    EvalContext ctx;
    EXPECT_DOUBLE_EQ(valueOf(c, ctx), minusTwoXPlusThree(g.x));
    EXPECT_TRUE(g.j->dirty());
    EXPECT_TRUE(c->dirty());

    g.x->set(5.0);
    EXPECT_DOUBLE_EQ(valueOf(c, ctx), minusTwoXPlusThree(g.x));
    g.x->set(7.0);
    EXPECT_DOUBLE_EQ(valueOf(c, ctx), minusTwoXPlusThree(g.x));
}

// One shot: c skips, if it can, and stays dirty with j. x's move reaches it.
TEST_P(LazyPairKind, StaysDirtyWhenItSkips) {
    OneShot g;
    const NodePtr c = GetParam().make(g.j, g.tPlus);
    EvalContext ctx;
    c->eval(ctx);
    ASSERT_FALSE(c->dirty());

    g.trip();
    EXPECT_DOUBLE_EQ(valueOf(c, ctx), minusTwoXPlusThree(g.x));
    EXPECT_TRUE(g.j->dirty());
    EXPECT_TRUE(c->dirty());

    g.x->set(5.0);
    EXPECT_DOUBLE_EQ(valueOf(c, ctx), minusTwoXPlusThree(g.x));
}

// A diamond: x reaches c through −x and through e^x, so c hears "maybe" twice
// before it is pulled. Its evaluation ends clean all the same.
TEST_P(PairKind, AnOrdinaryEvaluationEndsClean) {
    auto x = Input<double>::make("x", 1.0);
    const NodePtr c = GetParam().make(ops::NegateNode<>::make("-x", x),
                                      ops::ExpNode<>::make("e^x", x));
    EvalContext ctx;
    c->eval(ctx);

    x->set(2.0);
    EXPECT_DOUBLE_EQ(valueOf(c, ctx), -2.0 + e2);
    EXPECT_FALSE(c->dirty());
}

INSTANTIATE_TEST_SUITE_P(CleanInputs, PairKind, ::testing::ValuesIn(pairCases()),
                         caseName<PairCase>);
INSTANTIATE_TEST_SUITE_P(CleanInputs, LazyPairKind, ::testing::ValuesIn(lazyPairCases()),
                         caseName<PairCase>);

// ── Nodes over one input ─────────────────────────────────────────────────────

// Two paths under a ComputeNode c, which stays dirty. The node over c stays
// dirty with it, and x's move reaches it.
TEST_P(TopKind, StaysDirtyWithItsInput) {
    TwoPaths<Input> g;
    const NodePtr c = ComputeNode<double, double, double>::make("c", {g.j, g.kPlus}, sum);
    const NodePtr top = GetParam().make(c);
    EvalContext ctx;
    const double before = valueOf(top, ctx);
    if (GetParam().of) {
        EXPECT_DOUBLE_EQ(before, GetParam().of(minusTwoXPlusThree(g.x)));
    }
    EXPECT_TRUE(c->dirty());
    EXPECT_TRUE(top->dirty());

    g.x->set(5.0);
    const double after = valueOf(top, ctx);
    if (GetParam().of) {
        EXPECT_DOUBLE_EQ(after, GetParam().of(minusTwoXPlusThree(g.x)));
    }
    EXPECT_TRUE(top->dirty());
}

// One shot under a ComputeNode c. c stays dirty, and the node over it, which
// reads nothing that moved, skips and stays dirty with it.
TEST_P(LazyTopKind, StaysDirtyWithItsInputWhenNothingMoved) {
    OneShot g;
    const NodePtr c = ComputeNode<double, double, double>::make("c", {g.j, g.tPlus}, sum);
    const NodePtr top = GetParam().make(c);
    EvalContext ctx;
    top->eval(ctx);
    ASSERT_FALSE(top->dirty());

    g.trip();
    top->eval(ctx);
    EXPECT_TRUE(c->dirty());
    EXPECT_TRUE(top->dirty());

    g.x->set(5.0);
    EXPECT_DOUBLE_EQ(valueOf(top, ctx), GetParam().of(minusTwoXPlusThree(g.x)));
}

// A diamond below c. The node over c ends clean.
TEST_P(TopKind, AnOrdinaryEvaluationEndsClean) {
    auto x = Input<double>::make("x", 1.0);
    const NodePtr c = ops::SumNode<>::make("c", {ops::NegateNode<>::make("-x", x),
                                                 ops::ExpNode<>::make("e^x", x)});
    const NodePtr top = GetParam().make(c);
    EvalContext ctx;
    top->eval(ctx);

    x->set(2.0);
    top->eval(ctx);
    EXPECT_FALSE(top->dirty());
}

INSTANTIATE_TEST_SUITE_P(CleanInputs, TopKind, ::testing::ValuesIn(topCases()),
                         caseName<TopCase>);
INSTANTIATE_TEST_SUITE_P(CleanInputs, LazyTopKind, ::testing::ValuesIn(lazyTopCases()),
                         caseName<TopCase>);

// ── Through an engine, a sensitivity node and a forced evaluation ────────────

// Two paths with x an AsyncInput and c an output: every move of x is delivered.
TEST(CleanInputs, AnEngineDeliversTheLaterChange) {
    async::Engine engine;
    TwoPaths<async::AsyncInput> g;
    engine.addSource(g.x);
    auto c = ComputeNode<double, double, double>::make("c", {g.j, g.kPlus}, sum);
    std::vector<double> values;
    engine.addOutput<double>(c, [&](const double& v) { values.push_back(v); });

    engine.step();
    g.x->post(5.0);
    engine.step();
    g.x->post(7.0);
    engine.step();

    EXPECT_EQ(values, (std::vector<double>{-3.0, -7.0, -11.0}));
}

// The graph of AadGradientNode.StaysDirtyWhileItsRootIs, with a node over a
// gradient node and one over a tangent node. Each sensitivity node stays dirty
// with its root, and the node over it stays dirty too, so x's move reaches
// both. flywheel-dag#19 kept the sensitivity nodes dirty, not the nodes over
// them.
TEST(CleanInputs, ASensitivityNodesConsumerStaysDirtyWithIt) {
    auto k    = test_nodes::AlwaysFiring::make(2.0);
    auto x    = Input<double>::make("x", 3.0);
    auto root = ops::ProductNode<>::make("k.x.x", {k, x, x});
    const auto first = [](const std::vector<double>& d) { return d[0]; };
    auto overGradient = ComputeNode<double, std::vector<double>>::make(
        "over-gradient", {aad::GradientNode::make("gradient", root, {x})}, first);
    auto overTangent = ComputeNode<double, std::vector<double>>::make(
        "over-tangent", {aad::TangentNode::make("tangent", {root}, {{x, 1.0}})}, first);
    EvalContext ctx;
    EXPECT_DOUBLE_EQ(valueOf(overGradient, ctx), 12.0);
    EXPECT_DOUBLE_EQ(valueOf(overTangent, ctx), 12.0);
    EXPECT_TRUE(overGradient->dirty());
    EXPECT_TRUE(overTangent->dirty());

    x->set(5.0);
    EXPECT_DOUBLE_EQ(valueOf(overGradient, ctx), 20.0);
    EXPECT_DOUBLE_EQ(valueOf(overTangent, ctx), 20.0);
}

// A forced evaluation starts clean, and a "maybe" during it must keep c dirty
// all the same. Forced, an n-ary op pulls once, so the pull of t·x is t's first
// and the pull of the second path its second: armed for two, t fires after c
// has read j.
TEST(CleanInputs, AForcedEvaluationStaysDirtyWhileAnInputItReadIs) {
    OneShot g;
    auto c = ComputeNode<double, double, double>::make("c", {g.j, g.tPlus}, sum);
    EvalContext ctx;
    c->eval(ctx);
    ASSERT_FALSE(c->dirty());

    EvalContext forced;
    forced.forceRecompute = true;
    g.t->arm(2);
    c->eval(forced);
    EXPECT_TRUE(g.j->dirty());
    EXPECT_TRUE(c->dirty());

    g.x->set(5.0);
    EXPECT_DOUBLE_EQ(valueOf(c, ctx), minusTwoXPlusThree(g.x));
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
