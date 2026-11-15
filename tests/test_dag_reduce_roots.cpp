// =============================================================================
// flywheel-dag — A header-only C++23 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

// dag::Reduction of several roots: a function ℝⁿ → ℝᵐ, and its Jacobian.
//
// Allocations are counted by allocation_counter.cpp, which this suite links.

#include <gtest/gtest.h>
#include "aad_test_graphs.hpp"
#include "allocation_counter.hpp"
#include "reduce_test_nodes.hpp"
#include "flywheel/dag.hpp"
#include "flywheel/dag_aad.hpp"
#include "flywheel/dag_ops.hpp"
#include "flywheel/dag_reduce.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using namespace dag;
using aad_test::expectClose;
using allocation_counter::allocations_during;
using namespace reduce_test;

namespace {

/// Three residuals over two inputs, which share exp(x) and x·y:
///   r0 = x·y + exp(x)/y,   r1 = exp(x)·√y,   r2 = x·y − y.
struct ThreeOverTwo {
    InputPtr<double> x = Input<double>::make("x", 1.3);
    InputPtr<double> y = Input<double>::make("y", 0.7);
    NodePtr ex = ops::ExpNode<>::make("exp(x)", x);
    NodePtr xy = ops::ProductNode<>::make("x.y", {x, y});
    NodePtr r0 = ops::SumNode<>::make("r0", {xy, ops::DivideNode<>::make("exp/y", ex, y)});
    NodePtr r1 = ops::ProductNode<>::make("r1", {ex, ops::SqrtNode<>::make("sqrt(y)", y)});
    NodePtr r2 = ops::DiffNode<>::make("r2", xy, y);

    std::vector<NodePtr> roots() const { return {r0, r1, r2}; }
    std::vector<InputPtr<double>> inputs() const { return {x, y}; }
    /// Row-major, at the values the inputs hold.
    std::vector<double> jacobian() const {
        const double a = x->get(), b = y->get(), e = std::exp(a);
        return {b + e / b,        a - e / (b * b),
                e * std::sqrt(b), e / (2.0 * std::sqrt(b)),
                b,                a - 1.0};
    }
};

/// Two residuals over three inputs:  r0 = (x − y)^z,   r1 = x·y·z + exp(x)/z.
struct TwoOverThree {
    InputPtr<double> x = Input<double>::make("x", 3.5);
    InputPtr<double> y = Input<double>::make("y", 1.25);
    InputPtr<double> z = Input<double>::make("z", 1.8);
    NodePtr r0 = ops::PowerNode<>::make("r0", ops::DiffNode<>::make("x-y", x, y), z);
    NodePtr r1 = ops::SumNode<>::make("r1", {
        ops::ProductNode<>::make("x.y.z", {x, y, z}),
        ops::DivideNode<>::make("exp/z", ops::ExpNode<>::make("exp(x)", x), z)});

    std::vector<NodePtr> roots() const { return {r0, r1}; }
    std::vector<InputPtr<double>> inputs() const { return {x, y, z}; }
    std::vector<double> jacobian() const {
        const double a = x->get(), b = y->get(), c = z->get();
        const double d = a - b, e = std::exp(a);
        const double da = c * std::pow(d, c - 1.0);
        return {da,            -da,   std::pow(d, c) * std::log(d),
                b * c + e / c, a * c, a * b - e / (c * c)};
    }
};

std::vector<double> held(const std::vector<InputPtr<double>>& ins) {
    std::vector<double> x;
    x.reserve(ins.size());
    for (const auto& in : ins) x.push_back(in->get());
    return x;
}

void evalAll(const std::vector<NodePtr>& roots) {
    EvalContext ctx;
    for (const auto& r : roots) r->eval(ctx);
}

double valueOf(const NodePtr& node) {
    EvalContext ctx;
    const ValuePtr v = node->eval(ctx);
    return get_value<double>(v);
}

/// The function of every input of a graph against the graph itself, root by
/// root: at the point the graph holds, and at another, which the inputs are then
/// set to.
template<typename Graph>
void expectTheGraphsValues(const Graph& g) {
    const std::vector<NodePtr> roots = g.roots();
    const std::vector<InputPtr<double>> ins = g.inputs();
    evalAll(roots);
    Reduction f = reduce(roots, aad_test::asNodes(ins));
    ASSERT_EQ(f.roots(), roots.size());
    ASSERT_EQ(f.arity(), ins.size());

    std::vector<double> x = held(ins);
    std::vector<double> y(roots.size(), 0.0);
    f(x, y);
    for (std::size_t r = 0; r < roots.size(); ++r) EXPECT_EQ(y[r], valueOf(roots[r])) << r;

    for (double& v : x) v = 1.1 * v + 0.05;
    f(x, y);
    for (std::size_t i = 0; i < ins.size(); ++i) ins[i]->set(x[i]);
    evalAll(roots);
    for (std::size_t r = 0; r < roots.size(); ++r) EXPECT_EQ(y[r], valueOf(roots[r])) << r;
}

/// The Jacobian at the point the graph holds and at a moved one, each handed to
/// check() once the graph holds that point.
template<typename Graph, typename Check>
void jacobianAtTwoPoints(const Graph& g, Check&& check) {
    const std::vector<NodePtr> roots = g.roots();
    const std::vector<InputPtr<double>> ins = g.inputs();
    evalAll(roots);
    Reduction f = reduce(roots, aad_test::asNodes(ins));

    std::vector<double> x = held(ins);
    std::vector<double> y(roots.size(), 0.0);
    std::vector<double> j(roots.size() * ins.size(), 0.0);
    f.jacobian(x, y, j);
    for (std::size_t r = 0; r < roots.size(); ++r) EXPECT_EQ(y[r], valueOf(roots[r])) << r;
    check(j);

    for (double& v : x) v = 1.1 * v + 0.05;
    f.jacobian(x, y, j);
    for (std::size_t i = 0; i < ins.size(); ++i) ins[i]->set(x[i]);
    evalAll(roots);
    for (std::size_t r = 0; r < roots.size(); ++r) EXPECT_EQ(y[r], valueOf(roots[r])) << r;
    check(j);
}

/// x·a·b·c, as three products, with a = b = 1e200 and c = 1e-300. Its
/// derivative with respect to x is a·b·c = 1e100. A reverse sweep multiplies
/// from the root's end, c·b·a, and stays finite. A forward sweep multiplies from
/// the argument's end, a·b·c, and a·b overflows.
struct Overflowing {
    InputPtr<double> x     = Input<double>::make("x", 1e-200);
    InputPtr<double> other = Input<double>::make("other", 7.0);   // a node of another graph
    NodePtr first  = ops::ProductNode<>::make("x.a", {x, Input<double>::make("a", 1e200)});
    NodePtr second = ops::ProductNode<>::make("x.a.b", {first, Input<double>::make("b", 1e200)});
    NodePtr root   = ops::ProductNode<>::make("x.a.b.c", {second, Input<double>::make("c", 1e-300)});

    Overflowing() { evalAll({root}); }
};

constexpr double kInf = std::numeric_limits<double>::infinity();

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Its values
// ─────────────────────────────────────────────────────────────────────────────

// To the bit, root by root: a step and its node apply one Op to the same values.
TEST(ReductionRoots, MatchesTheGraphAtEveryPoint) {
    expectTheGraphsValues(ThreeOverTwo{});
    expectTheGraphsValues(TwoOverThree{});
}

// x → shared → left and right, all three of them roots, with a fourth over
// left and right. One call computes the shared node once.
TEST(ReductionRoots, ASharedNodeRunsOncePerCall) {
    auto x      = Input<double>::make("x", 1.0);
    auto shared = Counted<>::make("shared", x);
    auto left   = Counted<>::make("left", shared);
    auto right  = Counted<>::make("right", shared);
    auto sum    = ops::SumNode<>::make("sum", {left, right});
    evalAll({sum});
    ASSERT_EQ(shared->calls(), 1);

    Reduction f = reduce(std::vector<NodePtr>{shared, left, right, sum}, {x});
    EXPECT_EQ(f.roots(), 4u);
    EXPECT_EQ(f.size(), 4u);         // one step each, and the sum
    EXPECT_EQ(shared->calls(), 1);   // building and binding call nothing

    std::array<double, 4> y{};
    f({2.0}, y);
    EXPECT_EQ(y, (std::array<double, 4>{5.0, 11.0, 11.0, 22.0}));
    EXPECT_EQ(shared->calls(), 2);
    EXPECT_EQ(left->calls(), 2);
    EXPECT_EQ(right->calls(), 2);

    f({3.0}, y);
    EXPECT_EQ(y, (std::array<double, 4>{7.0, 15.0, 15.0, 30.0}));
    EXPECT_EQ(shared->calls(), 3);

    // The Jacobian asks each step once too: 2, 4, 4 and 8.
    std::array<double, 4> j{};
    f.jacobian({3.0}, y, j);
    EXPECT_EQ(j, (std::array<double, 4>{2.0, 4.0, 4.0, 8.0}));
    EXPECT_EQ(shared->calls(), 4);
}

TEST(ReductionRoots, BindReadsThePointEveryRootHolds) {
    auto x  = Input<double>::make("x", 2.0);
    auto k  = Input<double>::make("k", 3.0);
    auto r0 = ops::ProductNode<>::make("x.k", {x, k});
    auto r1 = ops::SumNode<>::make("x+k", {x, k});
    evalAll({r0, r1});
    Reduction f = reduce(std::vector<NodePtr>{r0, r1}, {x});
    std::array<double, 2> y{};
    f({5.0}, y);
    EXPECT_EQ(y, (std::array<double, 2>{15.0, 8.0}));

    k->set(4.0);
    evalAll({r0, r1});
    f({5.0}, y);
    EXPECT_EQ(y, (std::array<double, 2>{15.0, 8.0}));   // the point it was bound to
    f.bind();
    f({5.0}, y);
    EXPECT_EQ(y, (std::array<double, 2>{20.0, 9.0}));
}

// A root can be an argument, whose value is the caller's, and a node that no
// argument reaches, whose value is the graph's.
TEST(ReductionRoots, ARootCanBeAnArgumentOrAConstant) {
    auto x     = Input<double>::make("x", 2.0);
    auto k     = Input<double>::make("k", 3.0);
    auto other = Input<double>::make("other", 9.0);   // a node of another graph
    auto xk    = ops::ProductNode<>::make("x.k", {x, k});
    evalAll({xk});

    // Three roots and one argument: swept forward.
    Reduction f = reduce(std::vector<NodePtr>{x, k, xk}, {x});
    EXPECT_EQ(f.size(), 1u);
    std::array<double, 3> y{};
    std::array<double, 3> j{9.0, 9.0, 9.0};
    f.jacobian({5.0}, y, j);
    EXPECT_EQ(y, (std::array<double, 3>{5.0, 3.0, 15.0}));
    EXPECT_EQ(j, (std::array<double, 3>{1.0, 0.0, 3.0}));

    // Two roots and two arguments: swept in reverse.
    Reduction g = reduce(std::vector<NodePtr>{x, k}, {x, other});
    EXPECT_EQ(g.size(), 0u);
    std::array<double, 2> v{};
    std::array<double, 4> jg{9.0, 9.0, 9.0, 9.0};
    g.jacobian({5.0, 100.0}, v, jg);
    EXPECT_EQ(v, (std::array<double, 2>{5.0, 3.0}));
    EXPECT_EQ(jg, (std::array<double, 4>{1.0, 0.0, 0.0, 0.0}));

    // Three roots and two arguments, one of which no root depends on: forward,
    // and that argument's column is 0 without a sweep.
    Reduction h = reduce(std::vector<NodePtr>{x, k, xk}, {other, x});
    std::array<double, 6> jh{};
    jh.fill(9.0);
    h.jacobian({100.0, 5.0}, y, jh);
    EXPECT_EQ(y, (std::array<double, 3>{5.0, 3.0, 15.0}));
    EXPECT_EQ(jh, (std::array<double, 6>{0.0, 1.0, 0.0, 0.0, 0.0, 3.0}));
}

TEST(ReductionRoots, ARootNamedTwiceIsTwoValues) {
    auto x    = Input<double>::make("x", 2.0);
    auto root = ops::ProductNode<>::make("x.x", {x, x});
    evalAll({root});
    Reduction f = reduce(std::vector<NodePtr>{root, root}, {x});
    EXPECT_EQ(f.roots(), 2u);
    EXPECT_EQ(f.size(), 1u);
    std::array<double, 2> y{};
    std::array<double, 2> j{};
    f.jacobian({3.0}, y, j);
    EXPECT_EQ(y, (std::array<double, 2>{9.0, 9.0}));
    EXPECT_EQ(j, (std::array<double, 2>{6.0, 6.0}));
}

// ─────────────────────────────────────────────────────────────────────────────
// The one-root calls
// ─────────────────────────────────────────────────────────────────────────────

// On one root the two shapes of call agree to the bit, and so do the gradient
// and the Jacobian's one row: they are the same operations in the same order.
TEST(ReductionRoots, OneRootIsARowOfOne) {
    const auto agree = [](const auto& g) {
        const std::vector<InputPtr<double>> ins = g.inputs();
        evalAll({g.root});
        Reduction f = reduce(g.root, aad_test::asNodes(ins));
        ASSERT_EQ(f.roots(), 1u);
        std::vector<double> x = held(ins);
        for (int point = 0; point < 2; ++point) {
            std::vector<double> gradient(ins.size(), 0.0), row(ins.size(), 0.0);
            std::array<double, 1> y{};
            const double value = f.gradient(x, gradient);
            f.jacobian(x, y, row);
            EXPECT_EQ(y[0], value);
            EXPECT_EQ(row, gradient);
            f(x, y);
            EXPECT_EQ(y[0], f(x));
            for (double& v : x) v = 1.1 * v + 0.05;
        }
    };
    agree(aad_test::SumOfTerms{});
    agree(aad_test::LogTimesRoot{});
    agree(aad_test::PowerOfDifference{});
    agree(aad_test::TrigOfRatio{});
    agree(aad_test::InverseTrig{});
    agree(aad_test::PolarRoundTrip{});
}

TEST(ReductionRoots, TheOneRootCallsRefuseSeveralRoots) {
    const ThreeOverTwo g;
    evalAll(g.roots());
    Reduction f = reduce(g.roots(), {g.x, g.y});
    const std::string why = messageOf<std::logic_error>([&] { f({1.0, 2.0}); });
    EXPECT_TRUE(mentions(why, "roots r0, r1 and r2")) << why;
    std::array<double, 2> slots{};
    EXPECT_THROW(f.gradient({1.0, 2.0}, slots), std::logic_error);

    // And the calls that take room for the values work.
    std::array<double, 3> y{};
    f({1.3, 0.7}, y);
    EXPECT_EQ(y[0], valueOf(g.r0));
}

// ─────────────────────────────────────────────────────────────────────────────
// The Jacobian
// ─────────────────────────────────────────────────────────────────────────────

// Row i is a tape's adjoints of root i, and column k a tape's tangents along
// argument k. Three roots over two arguments are swept forward, and two roots
// over three in reverse. To 1e-13 and not to the bit: a step and a node take
// their partials from one Derivative<Op>, inlined at two sites.
TEST(ReductionRoots, TheJacobianMatchesATape) {
    const auto againstATape = [](const auto& g) {
        jacobianAtTwoPoints(g, [&g](const std::vector<double>& j) {
            const std::vector<NodePtr> roots = g.roots();
            const std::vector<NodePtr> args  = aad_test::asNodes(g.inputs());
            const aad::Tape tape(roots);
            for (std::size_t r = 0; r < roots.size(); ++r) {
                const std::vector<double> row = tape.adjoints(roots[r], args);
                for (std::size_t k = 0; k < args.size(); ++k)
                    expectClose(j[r * args.size() + k], row[k]);
            }
            for (std::size_t k = 0; k < args.size(); ++k) {
                const std::vector<double> column = tape.tangents({{args[k], 1.0}});
                for (std::size_t r = 0; r < roots.size(); ++r)
                    expectClose(j[r * args.size() + k], column[r]);
            }
        });
    };
    againstATape(ThreeOverTwo{});
    againstATape(TwoOverThree{});
}

TEST(ReductionRoots, TheJacobianMatchesAnalyticDerivatives) {
    const auto againstTheClosedForm = [](const auto& g) {
        jacobianAtTwoPoints(g, [&g](const std::vector<double>& j) {
            const std::vector<double> closed = g.jacobian();
            ASSERT_EQ(j.size(), closed.size());
            for (std::size_t i = 0; i < closed.size(); ++i)
                EXPECT_NEAR(j[i], closed[i], 1e-12 * std::max(1.0, std::abs(closed[i]))) << i;
        });
    };
    againstTheClosedForm(ThreeOverTwo{});
    againstTheClosedForm(TwoOverThree{});
}

// Which way a Jacobian was swept shows where a product of partials overflows
// from one end and not from the other: 1e100 in reverse, and inf forward.
TEST(ReductionRoots, TheJacobianSweepsTheShorterWay) {
    const Overflowing g;

    // One root, one argument: a tie, which goes to reverse.
    Reduction one = reduce(g.root, {g.x});
    std::array<double, 1> y1{};
    std::array<double, 1> j1{};
    one.jacobian({1e-200}, y1, j1);
    EXPECT_EQ(j1[0], 1e100);
    double slope = 0.0;
    one.gradient({1e-200}, {&slope, 1});
    EXPECT_EQ(slope, 1e100);          // a gradient is a reverse sweep

    // One root, two arguments: one reverse sweep.
    Reduction wide = reduce(g.root, {g.x, g.other});
    std::array<double, 2> j2{};
    wide.jacobian({1e-200, 7.0}, y1, j2);
    EXPECT_EQ(j2, (std::array<double, 2>{1e100, 0.0}));

    // Two roots, one argument: one forward sweep.
    Reduction tall = reduce(std::vector<NodePtr>{g.root, g.first}, {g.x});
    std::array<double, 2> y2{};
    std::array<double, 2> jt{};
    tall.jacobian({1e-200}, y2, jt);
    EXPECT_EQ(jt, (std::array<double, 2>{kInf, 1e200}));

    // Two roots, two arguments: a tie again, so two reverse sweeps.
    Reduction square = reduce(std::vector<NodePtr>{g.root, g.first}, {g.x, g.other});
    std::array<double, 4> js{};
    square.jacobian({1e-200, 7.0}, y2, js);
    EXPECT_EQ(js, (std::array<double, 4>{1e100, 0.0, 1e200, 0.0}));

    // Three roots, two arguments: forward, although only one argument moves anything.
    Reduction taller = reduce(std::vector<NodePtr>{g.root, g.first, g.second}, {g.x, g.other});
    std::array<double, 3> y3{};
    std::array<double, 6> j3{};
    taller.jacobian({1e-200, 7.0}, y3, j3);
    EXPECT_EQ(j3, (std::array<double, 6>{kInf, 0.0, 1e200, 0.0, kInf, 0.0}));
}

// A Jacobian asks each step for its partials once, however many sweeps it then
// makes: a differentiable node's functor runs once on duals for three roots
// over two arguments, and once for two roots over three.
TEST(ReductionRoots, TheJacobianAsksEachStepOnce) {
    auto a = Input<double>::make("a", 1.5);
    auto b = Input<double>::make("b", 0.5);
    auto c = Input<double>::make("c", 2.0);
    int doubles = 0, duals = 0;
    auto shared = aad::DifferentiableNode<3>::make("a.b.c", {a, b, c},
        [&doubles, &duals](const auto& p, const auto& q, const auto& r) {
            if constexpr (std::is_same_v<std::decay_t<decltype(p)>, double>) ++doubles;
            else ++duals;
            return p * q * r;
        });
    auto r0 = ops::NegateNode<>::make("r0", shared);
    auto r1 = ops::SumNode<>::make("r1", {shared, a});
    auto r2 = ops::ProductNode<>::make("r2", {shared, b});
    evalAll({r0, r1, r2});
    ASSERT_EQ(doubles, 1);

    Reduction forward = reduce(std::vector<NodePtr>{r0, r1, r2}, {a, b});
    std::array<double, 3> y{};
    std::array<double, 6> jf{};
    forward.jacobian({1.5, 0.5}, y, jf);
    EXPECT_EQ(doubles, 2);
    EXPECT_EQ(duals, 1);
    EXPECT_EQ(jf, (std::array<double, 6>{-1.0, -3.0, 2.0, 3.0, 0.5, 1.5 + 1.5}));

    Reduction reverse = reduce(std::vector<NodePtr>{r0, r1}, {a, b, c});
    std::array<double, 2> v{};
    std::array<double, 6> jr{};
    reverse.jacobian({1.5, 0.5, 2.0}, v, jr);
    EXPECT_EQ(doubles, 3);
    EXPECT_EQ(duals, 2);
    EXPECT_EQ(jr, (std::array<double, 6>{-1.0, -3.0, -0.75, 2.0, 3.0, 0.75}));
}

// z·√x and √(z·x) at z = 0 and x = 0: each is 0 for every x there, so ∂/∂x is
// 0 in both sweeps, although one partial on the way is infinite.
TEST(ReductionRoots, AZeroPartialOrTangentPropagatesNothing) {
    auto z  = Input<double>::make("z", 0.0);
    auto x  = Input<double>::make("x", 0.0);
    auto r0 = ops::ProductNode<>::make("z.sqrt(x)", {z, ops::SqrtNode<>::make("sqrt(x)", x)});
    auto r1 = ops::SqrtNode<>::make("sqrt(z.x)", ops::ProductNode<>::make("z.x", {z, x}));
    auto r2 = ops::SumNode<>::make("z+x", {z, x});
    evalAll({r0, r1, r2});

    // Forward: three roots over one argument.
    Reduction forward = reduce(std::vector<NodePtr>{r0, r1, r2}, {x});
    std::array<double, 3> y{};
    std::array<double, 3> jf{9.0, 9.0, 9.0};
    forward.jacobian({0.0}, y, jf);
    EXPECT_EQ(jf[0], 0.0);   // the tangent of √x is infinite, and z is 0
    EXPECT_EQ(jf[1], 0.0);   // the tangent of z·x is 0, and √'s partial is infinite
    EXPECT_EQ(jf[2], 1.0);

    // Reverse: the same three over three arguments.
    auto other = Input<double>::make("other", 1.0);
    Reduction reverse = reduce(std::vector<NodePtr>{r0, r1, r2}, {x, z, other});
    std::array<double, 9> jr{};
    jr.fill(9.0);
    reverse.jacobian({0.0, 0.0, 1.0}, y, jr);
    EXPECT_EQ(jr[0], 0.0);   // ∂r0/∂x
    EXPECT_EQ(jr[1], 0.0);   // ∂r0/∂z = √x
    EXPECT_EQ(jr[2], 0.0);
    EXPECT_EQ(jr[3], 0.0);   // ∂r1/∂x: the adjoint of z·x is infinite, and z is 0
    EXPECT_EQ(jr[4], 0.0);
    EXPECT_EQ(jr[5], 0.0);
    EXPECT_EQ(jr[6], 1.0);
    EXPECT_EQ(jr[7], 1.0);
    EXPECT_EQ(jr[8], 0.0);
}

// A step with no partials stops the Jacobian of the roots it is under. A
// reduction of the other roots alone does not run it.
TEST(ReductionRoots, AnOpaqueStepStopsTheJacobianOfTheRootsItIsUnder) {
    auto x      = Input<double>::make("arg.x", 2.0);
    auto opaque = ComputeNode<double, double>::make("node.opaque", {x},
        [](const double& a) { return a * a; }, InvalidationMode::Lazy);
    auto clear  = ops::ExpNode<>::make("root.clear", x);
    auto under  = ops::NegateNode<>::make("root.under", opaque);
    evalAll({clear, under});

    Reduction both = reduce(std::vector<NodePtr>{clear, under}, {x});
    std::array<double, 2> y{};
    std::array<double, 2> j{};
    both({3.0}, y);
    EXPECT_EQ(y[1], -9.0);
    const std::string why = messageOf<std::domain_error>([&] { both.jacobian({3.0}, y, j); });
    EXPECT_TRUE(mentions(why, "arg.x reaches roots root.clear and root.under through node.opaque"))
        << why;
    EXPECT_TRUE(mentions(why, "has no partials")) << why;

    // Without the root that is under it, nothing stands in the way.
    Reduction alone = reduce(std::vector<NodePtr>{clear}, {x});
    std::array<double, 1> ya{};
    std::array<double, 1> ja{};
    alone.jacobian({0.0}, ya, ja);
    EXPECT_EQ(ja[0], 1.0);
}

// ─────────────────────────────────────────────────────────────────────────────
// What it refuses
// ─────────────────────────────────────────────────────────────────────────────

TEST(ReductionRoots, RejectsBadRoots) {
    auto x    = Input<double>::make("x", 2.0);
    auto root = ops::NegateNode<>::make("root", x);
    EXPECT_THROW(Reduction(std::vector<NodePtr>{}, {x}), std::invalid_argument);
    EXPECT_THROW(Reduction(std::vector<NodePtr>{root, nullptr}, {x}), std::invalid_argument);
    EXPECT_THROW(Reduction(std::vector<NodePtr>{root, root}, {}), std::invalid_argument);
    EXPECT_THROW(Reduction(std::vector<NodePtr>{root, root}, {x, x}), std::invalid_argument);
    EXPECT_THROW(reduce(std::vector<NodePtr>{}, {x}), std::invalid_argument);
}

// Every root must be clean and hold a double, and the one that is not is named.
TEST(ReductionRoots, RejectsADirtyRootOrOneThatHoldsNoDouble) {
    auto x     = Input<double>::make("x", 2.0);
    auto clean = ops::NegateNode<>::make("root.clean", x);
    auto dirty = ops::ExpNode<>::make("root.dirty", x);
    evalAll({clean});
    Reduction f(std::vector<NodePtr>{clean, dirty}, {x});
    const std::string why = messageOf<std::invalid_argument>([&] { f.bind(); });
    EXPECT_TRUE(mentions(why, "root root.dirty is dirty")) << why;
    EXPECT_TRUE(dirty->dirty());
    std::array<double, 2> y{};
    EXPECT_TRUE(mentions(messageOf<std::logic_error>([&] { f({1.0}, y); }), "not bound"));

    evalAll({dirty});
    f.bind();
    f({1.0}, y);
    EXPECT_EQ(y, (std::array<double, 2>{-1.0, std::exp(1.0)}));

    auto i = Input<int>::make("root.int", 2);
    EvalContext ctx;
    i->eval(ctx);
    Reduction g(std::vector<NodePtr>{clean, i}, {x});
    const std::string whyNot = messageOf<std::invalid_argument>([&] { g.bind(); });
    EXPECT_TRUE(mentions(whyNot, "root root.int does not hold a double")) << whyNot;
}

TEST(ReductionRoots, RejectsRoomOfTheWrongSize) {
    const ThreeOverTwo g;
    evalAll(g.roots());
    Reduction f = reduce(g.roots(), {g.x, g.y});
    std::array<double, 3> y{};
    std::array<double, 6> j{};
    const std::vector<double> none;
    EXPECT_THROW(f(none, y), std::invalid_argument);
    EXPECT_THROW(f({1.0, 2.0, 3.0}, y), std::invalid_argument);
    EXPECT_THROW(f({1.0, 2.0}, std::span<double>(y).first(2)), std::invalid_argument);
    EXPECT_THROW(f.jacobian({1.0, 2.0}, std::span<double>(y).first(2), j), std::invalid_argument);
    EXPECT_THROW(f.jacobian({1.0, 2.0}, y, std::span<double>(j).first(5)), std::invalid_argument);
    EXPECT_THROW(f.jacobian({1.0}, y, j), std::invalid_argument);
    f.jacobian({1.3, 0.7}, y, j);
    EXPECT_EQ(y[2], valueOf(g.r2));
}

// ─────────────────────────────────────────────────────────────────────────────
// Allocation
// ─────────────────────────────────────────────────────────────────────────────

// Both shapes: three roots over two arguments, swept forward, and two over
// three, in reverse. The n-ary steps' partials need every input at once.
TEST(ReductionRoots, AWarmCallAndJacobianAllocateNothing) {
    const auto warm = [](const auto& g) {
        const std::vector<NodePtr> roots = g.roots();
        const std::vector<InputPtr<double>> ins = g.inputs();
        evalAll(roots);
        Reduction f = reduce(roots, aad_test::asNodes(ins));
        std::vector<double> x = held(ins);
        std::vector<double> y(roots.size(), 0.0);
        std::vector<double> j(roots.size() * ins.size(), 0.0);
        for (int i = 0; i < 3; ++i) f.jacobian(x, y, j);   // every step has both of its buffers

        double sum = 0.0;
        const long calls = allocations_during([&] {
            for (int i = 0; i < 100; ++i) {
                x[0] += 0.001;
                f(x, y);
                sum += y[0];
            }
        });
        EXPECT_EQ(calls, 0);
        const long jacobians = allocations_during([&] {
            for (int i = 0; i < 100; ++i) {
                x[0] += 0.001;
                f.jacobian(x, y, j);
                sum += j[0] + y[1];
            }
        });
        EXPECT_EQ(jacobians, 0);
        const long binds = allocations_during([&] {
            for (int i = 0; i < 100; ++i) {
                f.bind();
                f.jacobian(x, y, j);
                sum += j[1];
            }
        });
        EXPECT_EQ(binds, 0);
        EXPECT_NE(sum, 0.0);
    };
    warm(ThreeOverTwo{});
    warm(TwoOverThree{});
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
