// =============================================================================
// flywheel-dag — A header-only C++17 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

// aad::GradientNode: a node whose value is a gradient, so an engine delivers
// sensitivities through addOutput (flywheel-dag#12).

#include <gtest/gtest.h>
#include "aad_test_graphs.hpp"
#include "flywheel/dag.hpp"
#include "flywheel/dag_aad.hpp"
#include "flywheel/dag_engine.hpp"
#include "flywheel/dag_ops.hpp"
#include "flywheel/dag_timeseries.hpp"
#include <cmath>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <vector>

using namespace dag;
using aad_test::expectClose;
using Gradient = std::vector<double>;

namespace {

// Checks the node's value on one graph against a pass and the analytic gradient.
template<typename Graph>
void expectTheTapesGradient(const Graph& g) {
    const std::vector<NodePtr> wrt = aad_test::asNodes(g.inputs());
    auto grad = aad::GradientNode::make("grad", g.root, wrt);
    EvalContext ctx;
    const ValuePtr v = grad->eval(ctx);
    const Gradient& value = get_value<Gradient>(v);
    EXPECT_EQ(value, aad::adjoints(g.root, wrt));
    const Gradient expected = g.gradient();
    ASSERT_EQ(value.size(), expected.size());
    for (std::size_t i = 0; i < value.size(); ++i) expectClose(value[i], expected[i]);
}

// True when a generic functor is running on duals, which it does once each
// time a tape records its node.
template<typename T>
constexpr bool isDual() { return !std::is_same_v<std::decay_t<T>, double>; }

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Its value
// ─────────────────────────────────────────────────────────────────────────────

TEST(AadGradientNode, MatchesTheTape) {
    expectTheTapesGradient(aad_test::SumOfTerms{});
    expectTheTapesGradient(aad_test::LogTimesRoot{});
    expectTheTapesGradient(aad_test::PowerOfDifference{});
    expectTheTapesGradient(aad_test::TrigOfRatio{});
    expectTheTapesGradient(aad_test::InverseTrig{});
    expectTheTapesGradient(aad_test::PolarRoundTrip{});
}

// The issue's example. x·y is 6 at (2, 3) and at (3, 2), so the root's own
// callback fires once. Its gradient moves from (3, 2) to (2, 3), and the
// node delivers both.
TEST(AadGradientNode, DeliversAGradientThatMovesWhileTheValueStandsStill) {
    async::Engine engine;
    auto x = async::AsyncInput<double>::make("x", 2.0);
    auto y = async::AsyncInput<double>::make("y", 3.0);
    engine.addSource(x);
    engine.addSource(y);
    auto xy   = ops::ProductNode<>::make("xy", {x, y});
    auto grad = aad::GradientNode::make("grad", xy, {x, y});
    std::vector<double> values;
    std::vector<Gradient> gradients;
    engine.addOutput<double>(xy, [&](const double& v) { values.push_back(v); });
    engine.addOutput<Gradient>(grad, [&](const Gradient& g) { gradients.push_back(g); });

    engine.step();
    x->post(3.0);
    y->post(2.0);
    engine.step();

    EXPECT_EQ(values, (std::vector<double>{6.0}));
    ASSERT_EQ(gradients.size(), 2u);
    EXPECT_EQ(gradients[0], (Gradient{3.0, 2.0}));
    EXPECT_EQ(gradients[1], (Gradient{2.0, 3.0}));
}

TEST(AadGradientNode, RecordsOnlyWhenSomethingUpstreamFired) {
    auto x = Input<double>::make("x", 1.5);
    auto y = Input<double>::make("y", 0.4);
    int duals = 0;
    auto f = aad::DifferentiableNode<2>::make("f", {x, y},
        [&duals](const auto& a, const auto& b) {
            using std::exp;
            if constexpr (isDual<decltype(a)>()) ++duals;
            return a * exp(b);
        });
    auto grad = aad::GradientNode::make("grad", f, {x, y});
    EvalContext ctx;
    const ValuePtr first = grad->eval(ctx);
    EXPECT_EQ(duals, 1);

    EXPECT_FALSE(grad->dirty());
    EXPECT_EQ(grad->eval(ctx).get(), first.get());
    EXPECT_EQ(duals, 1);

    x->set(2.0);
    EXPECT_TRUE(grad->dirty());
    const ValuePtr second = grad->eval(ctx);
    EXPECT_EQ(duals, 2);
    EXPECT_NE(second.get(), first.get());
    EXPECT_EQ(get_value<Gradient>(second), aad::adjoints(f, {x, y}));
}

// ─────────────────────────────────────────────────────────────────────────────
// What it publishes
// ─────────────────────────────────────────────────────────────────────────────

// x + k has gradient (1, 1) wherever x is. Each cycle that moves x records a
// tape, which the dual calls count, and the gradient is delivered once.
TEST(AadGradientNode, AnUnchangedGradientIsNotDelivered) {
    async::Engine engine;
    auto x = async::AsyncInput<double>::make("x", 2.0);
    engine.addSource(x);
    auto k = Input<double>::make("k", 3.0);
    int duals = 0;
    auto sum = aad::DifferentiableNode<2>::make("sum", {x, k},
        [&duals](const auto& a, const auto& b) {
            if constexpr (isDual<decltype(a)>()) ++duals;
            return a + b;
        });
    auto grad = aad::GradientNode::make("grad", sum, {x, k});
    std::vector<Gradient> gradients;
    engine.addOutput<Gradient>(grad, [&](const Gradient& g) { gradients.push_back(g); });

    engine.step();
    for (const double v : {5.0, 7.0, 11.0}) {
        x->post(v);
        engine.step();
    }

    EXPECT_EQ(duals, 4);
    ASSERT_EQ(gradients.size(), 1u);
    EXPECT_EQ(gradients[0], (Gradient{1.0, 1.0}));
}

// The gradient of x·y with respect to x is y. A tolerance of 0.01 keeps the
// published value through a move of 0.001, and not through a move of 0.5.
TEST(AadGradientNode, AnEqualityPolicyGatesDelivery) {
    auto x = Input<double>::make("x", 2.0);
    auto y = Input<double>::make("y", 3.0);
    auto xy = ops::ProductNode<>::make("xy", {x, y});
    auto within = std::make_shared<PredicateEqualityPolicy>(
        [](const ValuePtr& a, const ValuePtr& b) {
            if (!a || !b) return false;
            const Gradient& u = get_value<Gradient>(a);
            const Gradient& v = get_value<Gradient>(b);
            for (std::size_t i = 0; i < u.size(); ++i)
                if (std::abs(u[i] - v[i]) >= 0.01) return false;
            return true;
        });
    auto grad = aad::GradientNode::make("grad", xy, {x, y}, within);
    EvalContext ctx;
    const ValuePtr first = grad->eval(ctx);
    EXPECT_EQ(get_value<Gradient>(first), (Gradient{3.0, 2.0}));

    y->set(3.001);
    EXPECT_EQ(grad->eval(ctx).get(), first.get());

    y->set(3.5);
    const ValuePtr moved = grad->eval(ctx);
    EXPECT_NE(moved.get(), first.get());
    EXPECT_EQ(get_value<Gradient>(moved), (Gradient{3.5, 2.0}));
}

// ─────────────────────────────────────────────────────────────────────────────
// It leaves the graph as its root's eval() leaves it
// ─────────────────────────────────────────────────────────────────────────────

// The root is sel·x, where sel picks e^x or a counted functor of y. With the
// exponential taken, a move of y reaches the node through the untaken branch.
// The node records a tape, as the dual calls show, without reading that
// branch, although the branch's node is one of its wrt nodes.
TEST(AadGradientNode, EvaluatesOnlyWhatItsRootDoes) {
    auto x    = Input<double>::make("x", 2.0);
    auto y    = Input<double>::make("y", 3.0);
    auto cond = Input<bool>::make("cond", false);
    int evals = 0, duals = 0;
    auto counted = ComputeNode<double, double>::make("counted", {y},
        [&evals](const double& v) { ++evals; return v + 1.0; });
    auto ex = aad::DifferentiableNode<1>::make("exp", {x},
        [&duals](const auto& v) {
            using std::exp;
            if constexpr (isDual<decltype(v)>()) ++duals;
            return exp(v);
        });
    auto sel  = ConditionNode::make("sel", cond, ex, counted);
    auto root = ops::ProductNode<>::make("root", {sel, x});
    auto grad = aad::GradientNode::make("grad", root, {x, counted});
    EvalContext ctx;

    // counted is taken: ∂/∂x is counted's value, and ∂/∂counted is x.
    EXPECT_EQ(get_value<Gradient>(grad->eval(ctx)), (Gradient{4.0, 2.0}));
    EXPECT_EQ(evals, 1);

    // e^x is taken: ∂/∂x is (1 + x)·e^x, and counted is off the tape.
    cond->set(true);
    const ValuePtr before = grad->eval(ctx);
    const Gradient& g = get_value<Gradient>(before);
    expectClose(g[0], 3.0 * std::exp(2.0));
    EXPECT_EQ(g[1], 0.0);
    EXPECT_FALSE(counted->dirty());
    const int dualsBefore = duals;

    y->set(4.0);
    EXPECT_TRUE(grad->dirty());
    const ValuePtr after = grad->eval(ctx);
    EXPECT_EQ(duals, dualsBefore + 1);
    EXPECT_EQ(evals, 1);
    EXPECT_TRUE(counted->dirty());
    EXPECT_EQ(after.get(), before.get());   // the same gradient, so still published
}

// An EWMANode ticks each time it evaluates. As a wrt node it gets its adjoint,
// and it ticks no more often than a control fed the same values.
TEST(AadGradientNode, DoesNotAdvanceAStatefulNode) {
    auto x       = Input<double>::make("x", 1.0);
    auto ewma    = ts::EWMANode::make("ewma", x, 0.5);
    auto k       = Input<double>::make("k", 3.0);
    auto root    = ops::ProductNode<>::make("root", {ewma, k});
    auto grad    = aad::GradientNode::make("grad", root, {ewma, k});
    auto xCtl    = Input<double>::make("xCtl", 1.0);
    auto control = ts::EWMANode::make("control", xCtl, 0.5);

    EvalContext ctx;
    for (const double v : {1.0, 2.0, 4.0, 8.0}) {
        x->set(v);
        xCtl->set(v);
        const ValuePtr g = grad->eval(ctx);
        control->eval(ctx);
        const double e = get_value<double>(ewma->eval(ctx));
        EXPECT_EQ(get_value<Gradient>(g), (Gradient{3.0, e}));
        EXPECT_EQ(e, get_value<double>(control->eval(ctx)));
    }
}

// k·x·x, where k is always dirty. The tape's pull evaluates k again, which
// marks the root dirty again, so the node stays dirty with it. The move of x
// then reaches the node, although the cascade stops at the root
// (flywheel-dag#19).
TEST(AadGradientNode, StaysDirtyWhileItsRootIs) {
    auto k    = aad_test::AlwaysFiring::make(2.0);
    auto x    = Input<double>::make("x", 3.0);
    auto root = ops::ProductNode<>::make("root", {k, x, x});
    auto grad = aad::GradientNode::make("grad", root, {x});
    EvalContext ctx;
    const ValuePtr before = grad->eval(ctx);
    EXPECT_EQ(get_value<Gradient>(before), (Gradient{12.0}));
    EXPECT_TRUE(root->dirty());
    EXPECT_TRUE(grad->dirty());

    x->set(5.0);
    EXPECT_TRUE(grad->dirty());
    const ValuePtr after = grad->eval(ctx);
    EXPECT_EQ(get_value<Gradient>(after), (Gradient{20.0}));
}

// ─────────────────────────────────────────────────────────────────────────────
// Rejections
// ─────────────────────────────────────────────────────────────────────────────

// With the opaque branch taken, x reaches the root through a barrier. Once the
// condition switches, the node, still dirty, gives the gradient of e^x.
TEST(AadGradientNode, ThrowsWhatItsTapeThrows) {
    auto x      = Input<double>::make("x", 2.0);
    auto cond   = Input<bool>::make("cond", true);
    auto opaque = ComputeNode<double, double>::make("opaque", {x},
        [](const double& v) { return v * v; });
    auto ex     = ops::ExpNode<>::make("exp", x);
    auto sel    = ConditionNode::make("sel", cond, opaque, ex);
    auto grad   = aad::GradientNode::make("grad", sel, {x});
    EvalContext ctx;
    EXPECT_THROW(grad->eval(ctx), std::domain_error);
    EXPECT_TRUE(grad->dirty());

    cond->set(false);
    const ValuePtr v = grad->eval(ctx);
    ASSERT_EQ(get_value<Gradient>(v).size(), 1u);
    expectClose(get_value<Gradient>(v)[0], std::exp(2.0));
    EXPECT_FALSE(grad->dirty());

    auto i       = Input<int>::make("i", 2);
    auto ofAnInt = aad::GradientNode::make("ofAnInt", i, {i});
    EXPECT_THROW(ofAnInt->eval(ctx), std::invalid_argument);
    EXPECT_TRUE(ofAnInt->dirty());
}

TEST(AadGradientNode, RejectsANullRootOrAnEmptyOrNullWrt) {
    auto x = Input<double>::make("x", 2.0);
    EXPECT_THROW(aad::GradientNode::make("grad", nullptr, {x}), std::invalid_argument);
    EXPECT_THROW(aad::GradientNode::make("grad", x, {}), std::invalid_argument);
    EXPECT_THROW(aad::GradientNode::make("grad", x, {x, nullptr}), std::invalid_argument);
}

// ─────────────────────────────────────────────────────────────────────────────
// In a graph and an engine
// ─────────────────────────────────────────────────────────────────────────────

TEST(AadGradientNode, ItsOneInputIsItsRoot) {
    auto x    = Input<double>::make("x", 1.0);
    auto k    = Input<double>::make("k", 2.0);
    auto ewma = ts::EWMANode::make("ewma", x, 0.5);
    auto root = ops::ProductNode<>::make("root", {ewma, k});
    auto grad = aad::GradientNode::make("grad", root, {ewma, k});
    EXPECT_EQ(grad->inputs(), (std::vector<NodePtr>{root}));
    EXPECT_EQ(grad->name(), "grad");
    EXPECT_EQ(grad->kind(), NodeKind::Compute);
    EXPECT_EQ(dynamic_cast<aad::IDifferentiable*>(grad.get()), nullptr);

    async::Engine engine;
    engine.addOutput<Gradient>(grad, [](const Gradient&) {});
    const std::vector<StatefulNodePtr> found = engine.discoverStatefulNodes();
    ASSERT_EQ(found.size(), 1u);
    EXPECT_EQ(std::dynamic_pointer_cast<ts::EWMANode>(found[0]), ewma);
}

// The engine evaluates outputs in the order they were registered, so the node
// pulls the root before the engine reaches it. The engine's dirty snapshot
// still fires the root's callback on every cycle that moves it.
TEST(AadGradientNode, RegisteredBeforeItsRoot) {
    async::Engine engine;
    auto s = async::AsyncInput<double>::make("s", 1.0);
    engine.addSource(s);
    auto sq   = ops::ProductNode<>::make("sq", {s, s});
    auto grad = aad::GradientNode::make("grad", sq, {s});
    std::vector<Gradient> gradients;
    std::vector<double> values;
    engine.addOutput<Gradient>(grad, [&](const Gradient& g) { gradients.push_back(g); });
    engine.addOutput<double>(sq, [&](const double& v) { values.push_back(v); });

    for (const double v : {2.0, 3.0, 5.0}) {
        s->post(v);
        engine.step();
    }

    EXPECT_EQ(values, (std::vector<double>{4.0, 9.0, 25.0}));
    EXPECT_EQ(gradients, (std::vector<Gradient>{{4.0}, {6.0}, {10.0}}));
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
