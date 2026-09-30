// =============================================================================
// flywheel-dag — A header-only C++17 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

// aad::TangentNode: a node whose value is a forward sweep's tangents, so an
// engine delivers several roots' derivatives in one direction through
// addOutput (flywheel-dag#17).

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
using Tangents = std::vector<double>;

namespace {

// Checks the node's value on one graph, seeded on two inputs at a time,
// against a pass and the analytic directional derivative.
template<typename Graph>
void expectTheTapesTangents(const Graph& g) {
    const std::vector<InputPtr<double>> ins = g.inputs();
    const std::vector<double> gradient = g.gradient();
    for (std::size_t i = 0; i < ins.size(); ++i) {
        const std::size_t j = (i + 1) % ins.size();
        const std::vector<aad::Seed> seeds = {{ins[i], 1.0}, {ins[j], 0.5}};
        auto node = aad::TangentNode::make("tangents", {g.root}, seeds);
        EvalContext ctx;
        const ValuePtr v = node->eval(ctx);
        const Tangents& value = get_value<Tangents>(v);
        EXPECT_EQ(value, aad::tangents({g.root}, seeds));
        ASSERT_EQ(value.size(), 1u);
        expectClose(value[0], gradient[i] + 0.5 * gradient[j]);
    }
}

// True when a generic functor is running on duals, which it does once each
// time a tape records its node.
template<typename T>
constexpr bool isDual() { return !std::is_same_v<std::decay_t<T>, double>; }

// The graph of flywheel-dag#18. sel takes e^x, so b, on the branch it did not
// take, is pulled only by root2. When b moves, root2's pull evaluates it, b
// tells sel, and root1 is dirty again after its own pull.
struct StaleBranch {
    InputPtr<bool>   cond  = Input<bool>::make("cond", true);
    InputPtr<double> x     = Input<double>::make("x", 1.0);
    InputPtr<double> y     = Input<double>::make("y", 4.0);
    NodePtr          b     = ops::SqrtNode<>::make("b", y);
    NodePtr          sel   = ConditionNode::make("sel", cond, ops::ExpNode<>::make("a", x), b);
    NodePtr          root1 = ops::ProductNode<>::make("root1", {sel, Input<double>::make("two", 2.0)});
    NodePtr          root2 = ops::SumNode<>::make("root2", {b, Input<double>::make("one", 1.0)});
};

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Its value
// ─────────────────────────────────────────────────────────────────────────────

TEST(AadTangentNode, MatchesTheTape) {
    expectTheTapesTangents(aad_test::SumOfTerms{});
    expectTheTapesTangents(aad_test::LogTimesRoot{});
    expectTheTapesTangents(aad_test::PowerOfDifference{});
    expectTheTapesTangents(aad_test::TrigOfRatio{});
    expectTheTapesTangents(aad_test::InverseTrig{});
    expectTheTapesTangents(aad_test::PolarRoundTrip{});
}

// x·y and x + y are 6 and 5 at (2, 3) and at (3, 2), so x·y's own callback
// fires once. Their derivatives along x move from (3, 1) to (2, 1), and the
// node delivers both.
TEST(AadTangentNode, DeliversTangentsThatMoveWhileTheValuesStandStill) {
    async::Engine engine;
    auto x = async::AsyncInput<double>::make("x", 2.0);
    auto y = async::AsyncInput<double>::make("y", 3.0);
    engine.addSource(x);
    engine.addSource(y);
    auto xy       = ops::ProductNode<>::make("xy", {x, y});
    auto sum      = ops::SumNode<>::make("x+y", {x, y});
    auto tangents = aad::TangentNode::make("d/dx", {xy, sum}, {{x, 1.0}});
    std::vector<double> values;
    std::vector<Tangents> delivered;
    engine.addOutput<double>(xy, [&](const double& v) { values.push_back(v); });
    engine.addOutput<Tangents>(tangents, [&](const Tangents& t) { delivered.push_back(t); });

    engine.step();
    x->post(3.0);
    y->post(2.0);
    engine.step();

    EXPECT_EQ(values, (std::vector<double>{6.0}));
    ASSERT_EQ(delivered.size(), 2u);
    EXPECT_EQ(delivered[0], (Tangents{3.0, 1.0}));
    EXPECT_EQ(delivered[1], (Tangents{2.0, 1.0}));
}

// Both roots read e^x through one DifferentiableNode. One tape records it
// once, so its functor runs on duals once.
TEST(AadTangentNode, OneTapeServesEveryRoot) {
    auto x = Input<double>::make("x", 1.5);
    int duals = 0;
    auto ex = aad::DifferentiableNode<1>::make("exp", {x},
        [&duals](const auto& v) {
            using std::exp;
            if constexpr (isDual<decltype(v)>()) ++duals;
            return exp(v);
        });
    auto product  = ops::ProductNode<>::make("e^x.x", {ex, x});
    auto sum      = ops::SumNode<>::make("e^x+x", {ex, x});
    auto tangents = aad::TangentNode::make("d/dx", {product, sum}, {{x, 1.0}});
    EvalContext ctx;
    const ValuePtr v = tangents->eval(ctx);
    EXPECT_EQ(duals, 1);
    const Tangents& t = get_value<Tangents>(v);
    ASSERT_EQ(t.size(), 2u);
    expectClose(t[0], std::exp(1.5) * 2.5);
    expectClose(t[1], std::exp(1.5) + 1.0);
}

TEST(AadTangentNode, RecordsOnlyWhenSomethingUpstreamFired) {
    auto x = Input<double>::make("x", 1.5);
    auto y = Input<double>::make("y", 0.4);
    int duals = 0;
    auto f = aad::DifferentiableNode<2>::make("f", {x, y},
        [&duals](const auto& a, const auto& b) {
            using std::exp;
            if constexpr (isDual<decltype(a)>()) ++duals;
            return a * exp(b);
        });
    auto tangents = aad::TangentNode::make("d/dx", {f}, {{x, 1.0}});
    EvalContext ctx;
    const ValuePtr first = tangents->eval(ctx);
    EXPECT_EQ(duals, 1);

    EXPECT_FALSE(tangents->dirty());
    EXPECT_EQ(tangents->eval(ctx).get(), first.get());
    EXPECT_EQ(duals, 1);

    y->set(0.9);
    EXPECT_TRUE(tangents->dirty());
    const ValuePtr second = tangents->eval(ctx);
    EXPECT_EQ(duals, 2);
    EXPECT_NE(second.get(), first.get());
    EXPECT_EQ(get_value<Tangents>(second), aad::tangents({f}, {{x, 1.0}}));
}

// ─────────────────────────────────────────────────────────────────────────────
// What it publishes
// ─────────────────────────────────────────────────────────────────────────────

// x + k and x − k both move one for one with x. Each cycle that moves x
// records a tape, which the dual calls count, and the tangents are delivered
// once.
TEST(AadTangentNode, AnUnchangedTangentIsNotDelivered) {
    async::Engine engine;
    auto x = async::AsyncInput<double>::make("x", 2.0);
    engine.addSource(x);
    auto k = Input<double>::make("k", 3.0);
    int duals = 0;
    auto sum = aad::DifferentiableNode<2>::make("x+k", {x, k},
        [&duals](const auto& a, const auto& b) {
            if constexpr (isDual<decltype(a)>()) ++duals;
            return a + b;
        });
    auto diff     = ops::DiffNode<>::make("x-k", x, k);
    auto tangents = aad::TangentNode::make("d/dx", {sum, diff}, {{x, 1.0}});
    std::vector<Tangents> delivered;
    engine.addOutput<Tangents>(tangents, [&](const Tangents& t) { delivered.push_back(t); });

    engine.step();
    for (const double v : {5.0, 7.0, 11.0}) {
        x->post(v);
        engine.step();
    }

    EXPECT_EQ(duals, 4);
    ASSERT_EQ(delivered.size(), 1u);
    EXPECT_EQ(delivered[0], (Tangents{1.0, 1.0}));
}

// ─────────────────────────────────────────────────────────────────────────────
// Roots that leave each other dirty
// ─────────────────────────────────────────────────────────────────────────────

// After y moves, the pull of root2 leaves root1 dirty. The node recorded root1
// before that, so its tangents are right, and it stays dirty with root1. The
// later move of x stops at sel, which is still dirty, but the node is dirty
// already, so it recomputes. A tape recorded after both pulls throws.
TEST(AadTangentNode, ARootLeftDirtyByAnotherKeepsTheNodeDirty) {
    StaleBranch g;
    auto tangents = aad::TangentNode::make("tangents", {g.root1, g.root2}, {{g.x, 1.0}, {g.y, 1.0}});
    EvalContext ctx;
    tangents->eval(ctx);
    EXPECT_TRUE(tangents->dirty());   // b is new, and its first pull told sel
    tangents->eval(ctx);
    EXPECT_FALSE(tangents->dirty());

    g.y->set(9.0);
    const ValuePtr afterY = tangents->eval(ctx);
    EXPECT_TRUE(g.root1->dirty());
    EXPECT_TRUE(tangents->dirty());
    const Tangents& t = get_value<Tangents>(afterY);
    ASSERT_EQ(t.size(), 2u);
    expectClose(t[0], 2.0 * std::exp(1.0));   // root1 = 2·e^x
    expectClose(t[1], 1.0 / 6.0);             // root2 = √y + 1, at y = 9

    g.x->set(2.0);
    const ValuePtr afterX = tangents->eval(ctx);
    expectClose(get_value<Tangents>(afterX)[0], 2.0 * std::exp(2.0));
    EXPECT_FALSE(tangents->dirty());

    StaleBranch h;
    h.root1->eval(ctx);
    h.root2->eval(ctx);
    h.y->set(9.0);
    h.root1->eval(ctx);
    h.root2->eval(ctx);
    EXPECT_TRUE(h.root1->dirty());
    EXPECT_THROW(aad::Tape({h.root1, h.root2}), std::invalid_argument);
}

// k is always dirty, and the tape's pulls evaluate it again, so the roots end
// every evaluation dirty, and the node with them. It neither hangs nor throws,
// and the move of x still reaches it.
TEST(AadTangentNode, AnAlwaysDirtyNodeKeepsItDirty) {
    auto k        = aad_test::AlwaysFiring::make(2.0);
    auto x        = Input<double>::make("x", 3.0);
    auto kx       = ops::ProductNode<>::make("k.x", {k, x});
    auto kxx      = ops::ProductNode<>::make("k.x.x", {k, x, x});
    auto tangents = aad::TangentNode::make("d/dx", {kx, kxx}, {{x, 1.0}});
    EvalContext ctx;
    const ValuePtr before = tangents->eval(ctx);
    EXPECT_EQ(get_value<Tangents>(before), (Tangents{2.0, 12.0}));
    EXPECT_TRUE(tangents->dirty());

    x->set(5.0);
    EXPECT_TRUE(tangents->dirty());
    const ValuePtr after = tangents->eval(ctx);
    EXPECT_EQ(get_value<Tangents>(after), (Tangents{2.0, 20.0}));
    EXPECT_TRUE(tangents->dirty());
}

// ─────────────────────────────────────────────────────────────────────────────
// It leaves the graph as its roots' eval() leaves it
// ─────────────────────────────────────────────────────────────────────────────

// The first root is sel·x, where sel picks e^x or a counted functor of y. With
// the exponential taken, a move of y reaches the node through the untaken
// branch. The node records a tape, as the dual calls show, without reading
// that branch, although a seed sits on the branch's node.
TEST(AadTangentNode, EvaluatesOnlyWhatItsRootsDo) {
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
    auto sel      = ConditionNode::make("sel", cond, ex, counted);
    auto root     = ops::ProductNode<>::make("root", {sel, x});
    auto square   = ops::ProductNode<>::make("x.x", {x, x});
    auto tangents = aad::TangentNode::make("tangents", {root, square},
                                           {{x, 1.0}, {counted, 1.0}});
    EvalContext ctx;

    // counted is taken: along x, root moves by counted's value and square by
    // 2x. Along counted, root moves by x.
    EXPECT_EQ(get_value<Tangents>(tangents->eval(ctx)), (Tangents{4.0 + 2.0, 4.0}));
    EXPECT_EQ(evals, 1);

    // e^x is taken: root moves by (1 + x)·e^x, and counted is off the tape.
    cond->set(true);
    const ValuePtr before = tangents->eval(ctx);
    const Tangents& t = get_value<Tangents>(before);
    expectClose(t[0], 3.0 * std::exp(2.0));
    EXPECT_EQ(t[1], 4.0);
    EXPECT_FALSE(counted->dirty());
    const int dualsBefore = duals;

    y->set(4.0);
    EXPECT_TRUE(tangents->dirty());
    const ValuePtr after = tangents->eval(ctx);
    EXPECT_EQ(duals, dualsBefore + 1);
    EXPECT_EQ(evals, 1);
    EXPECT_TRUE(counted->dirty());
    EXPECT_EQ(after.get(), before.get());   // the same tangents, so still published
}

// ─────────────────────────────────────────────────────────────────────────────
// Rejections
// ─────────────────────────────────────────────────────────────────────────────

// With the opaque branch taken, the seed on x sits above a barrier. Once the
// condition switches, the node, still dirty, gives e^x.
TEST(AadTangentNode, ThrowsWhatItsTapeThrows) {
    auto x        = Input<double>::make("x", 2.0);
    auto cond     = Input<bool>::make("cond", true);
    auto opaque   = ComputeNode<double, double>::make("opaque", {x},
        [](const double& v) { return v * v; });
    auto ex       = ops::ExpNode<>::make("exp", x);
    auto sel      = ConditionNode::make("sel", cond, opaque, ex);
    auto tangents = aad::TangentNode::make("tangents", {sel}, {{x, 1.0}});
    EvalContext ctx;
    EXPECT_THROW(tangents->eval(ctx), std::domain_error);
    EXPECT_TRUE(tangents->dirty());

    cond->set(false);
    const ValuePtr v = tangents->eval(ctx);
    ASSERT_EQ(get_value<Tangents>(v).size(), 1u);
    expectClose(get_value<Tangents>(v)[0], std::exp(2.0));
    EXPECT_FALSE(tangents->dirty());

    auto i       = Input<int>::make("i", 2);
    auto ofAnInt = aad::TangentNode::make("ofAnInt", {i}, {{i, 1.0}});
    EXPECT_THROW(ofAnInt->eval(ctx), std::invalid_argument);
    EXPECT_TRUE(ofAnInt->dirty());
}

TEST(AadTangentNode, RejectsEmptyOrNullRootsOrSeeds) {
    auto x = Input<double>::make("x", 2.0);
    EXPECT_THROW(aad::TangentNode::make("t", {}, {{x, 1.0}}), std::invalid_argument);
    EXPECT_THROW(aad::TangentNode::make("t", {x, nullptr}, {{x, 1.0}}), std::invalid_argument);
    EXPECT_THROW(aad::TangentNode::make("t", {x}, {}), std::invalid_argument);
    EXPECT_THROW(aad::TangentNode::make("t", {x}, {{x, 1.0}, {nullptr, 1.0}}),
                 std::invalid_argument);
}

// ─────────────────────────────────────────────────────────────────────────────
// In a graph and an engine
// ─────────────────────────────────────────────────────────────────────────────

TEST(AadTangentNode, ItsInputsAreItsRoots) {
    auto x        = Input<double>::make("x", 1.0);
    auto k        = Input<double>::make("k", 2.0);
    auto ewma     = ts::EWMANode::make("ewma", x, 0.5);
    auto product  = ops::ProductNode<>::make("ewma.k", {ewma, k});
    auto sum      = ops::SumNode<>::make("x+k", {x, k});
    auto tangents = aad::TangentNode::make("tangents", {product, sum}, {{k, 1.0}});
    EXPECT_EQ(tangents->inputs(), (std::vector<NodePtr>{product, sum}));
    EXPECT_EQ(tangents->name(), "tangents");
    EXPECT_EQ(tangents->kind(), NodeKind::Compute);
    EXPECT_EQ(dynamic_cast<aad::IDifferentiable*>(tangents.get()), nullptr);

    async::Engine engine;
    engine.addOutput<Tangents>(tangents, [](const Tangents&) {});
    const std::vector<StatefulNodePtr> found = engine.discoverStatefulNodes();
    ASSERT_EQ(found.size(), 1u);
    EXPECT_EQ(std::dynamic_pointer_cast<ts::EWMANode>(found[0]), ewma);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
