// =============================================================================
// flywheel-dag — A header-only C++17 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

// Sweeping a tape forward, and that it is the dual of the reverse sweep
// (flywheel-dag#10).

#include <gtest/gtest.h>
#include "aad_test_graphs.hpp"
#include "flywheel/dag.hpp"
#include "flywheel/dag_aad.hpp"
#include "flywheel/dag_ops.hpp"
#include <cmath>
#include <stdexcept>
#include <string>
#include <vector>

using namespace dag;
using aad_test::expectClose;
using aad_test::LogTimesRoot;
using aad_test::PowerOfDifference;
using aad_test::SumOfTerms;

namespace {

// Seeds one input at a time, and checks each tangent against the analytic
// gradient.
template<typename Graph>
void expectRightTangents(const Graph& g) {
    EvalContext ctx;
    g.root->eval(ctx);
    const aad::Tape tape({g.root});
    const auto ins = g.inputs();
    const std::vector<double> expected = g.gradient();
    for (std::size_t i = 0; i < ins.size(); ++i)
        expectClose(tape.tangents({{ins[i], 1.0}})[0], expected[i]);
}

// A weighted seed's tangent is the same weighted sum of adjoints.
template<typename Graph>
void expectDuality(const Graph& g) {
    EvalContext ctx;
    g.root->eval(ctx);
    const aad::Tape tape({g.root});
    const auto ins = g.inputs();
    const std::vector<double> adj = tape.adjoints(g.root, aad_test::asNodes(ins));
    const double weights[] = {0.3, -1.7, 2.9};
    std::vector<aad::Seed> seeds;
    double dot = 0.0;
    for (std::size_t i = 0; i < ins.size(); ++i) {
        seeds.push_back({ins[i], weights[i]});
        dot += weights[i] * adj[i];
    }
    expectClose(tape.tangents(seeds)[0], dot);
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Values
// ─────────────────────────────────────────────────────────────────────────────

TEST(AadForward, MatchesAnalyticDerivatives) {
    expectRightTangents(SumOfTerms{});
    expectRightTangents(LogTimesRoot{});
    expectRightTangents(PowerOfDifference{});
}

TEST(AadForward, OneSweepServesEveryRoot) {
    auto x = Input<double>::make("x", 0.9);
    auto y = Input<double>::make("y", 1.6);
    NodePtr xy   = ops::ProductNode<>::make("xy", {x, y});
    NodePtr ex   = ops::ExpNode<>::make("exp", x);
    NodePtr xOvY = ops::DivideNode<>::make("x/y", x, y);
    EvalContext ctx;
    for (const auto& r : {xy, ex, xOvY}) r->eval(ctx);

    const std::vector<double> t = aad::tangents({xy, ex, xOvY}, {{x, 1.0}});
    ASSERT_EQ(t.size(), 3u);
    expectClose(t[0], 1.6);
    expectClose(t[1], std::exp(0.9));
    expectClose(t[2], 1.0 / 1.6);
}

TEST(AadForward, IsTheDualOfReverse) {
    expectDuality(SumOfTerms{});
    expectDuality(LogTimesRoot{});
    expectDuality(PowerOfDifference{});
}

// f = (x·y·z, exp(x) − z/y): 3 forward sweeps and 2 reverse sweeps of one tape.
TEST(AadForward, JacobianBothWays) {
    auto x = Input<double>::make("x", 0.4);
    auto y = Input<double>::make("y", 2.2);
    auto z = Input<double>::make("z", -1.1);
    NodePtr f0 = ops::ProductNode<>::make("f0", {x, y, z});
    NodePtr f1 = ops::DiffNode<>::make("f1", ops::ExpNode<>::make("exp", x),
                                       ops::DivideNode<>::make("z/y", z, y));
    EvalContext ctx;
    f0->eval(ctx);
    f1->eval(ctx);

    const aad::Tape tape({f0, f1});
    const std::vector<NodePtr> ins = {x, y, z};
    const std::vector<double> rows[] = {tape.adjoints(f0, ins), tape.adjoints(f1, ins)};
    for (std::size_t j = 0; j < ins.size(); ++j) {
        const std::vector<double> column = tape.tangents({{ins[j], 1.0}});
        expectClose(column[0], rows[0][j]);
        expectClose(column[1], rows[1][j]);
    }
    expectClose(rows[0][0], 2.2 * -1.1);
    expectClose(rows[1][1], -1.1 / (2.2 * 2.2));
}

// x·eˣ·eˣ. A seed on eˣ alone gives ∂root/∂(eˣ). Seeded with x as well, it adds
// to the tangent that x sends through eˣ.
TEST(AadForward, ASeedOnAnIntermediateNodeAdds) {
    auto x    = Input<double>::make("x", 0.8);
    auto ex   = ops::ExpNode<>::make("exp", x);
    auto root = ops::ProductNode<>::make("root", {x, ex, ex});
    EvalContext ctx;
    root->eval(ctx);
    const aad::Tape tape({root});

    const double viaEx = 2.0 * 0.8 * std::exp(0.8);
    const double viaX  = std::exp(1.6) * (1.0 + 2.0 * 0.8);
    expectClose(tape.tangents({{ex, 1.0}})[0], viaEx);
    expectClose(tape.tangents({{x, 1.0}, {ex, 1.0}})[0], viaX + viaEx);
}

TEST(AadForward, AnUnreachedSeedMovesNothing) {
    auto cond  = Input<bool>::make("cond", true);
    auto x     = Input<double>::make("x", 2.0);
    auto y     = Input<double>::make("y", 3.0);
    auto other = Input<double>::make("other", 5.0);
    auto sel   = ConditionNode::make("sel", cond, x, y);
    EvalContext ctx;
    sel->eval(ctx);
    EXPECT_EQ(aad::tangents({sel}, {{y, 1.0}, {other, 1.0}})[0], 0.0);
    EXPECT_EQ(aad::tangents({sel}, {{x, 2.5}})[0], 2.5);
}

// ─────────────────────────────────────────────────────────────────────────────
// Rejections and barriers
// ─────────────────────────────────────────────────────────────────────────────

TEST(AadForward, RejectsANullSeed) {
    SumOfTerms s;
    EvalContext ctx;
    s.root->eval(ctx);
    EXPECT_THROW(aad::tangents({s.root}, {{nullptr, 1.0}}), std::invalid_argument);
}

TEST(AadForward, ABarrierWithASeedUpstreamThrows) {
    auto x      = Input<double>::make("x", 2.0);
    auto y      = Input<double>::make("y", 5.0);
    auto opaque = ComputeNode<double, double>::make("opaque", {x},
        [](const double& v) { return v * v; });
    auto root   = ops::ProductNode<>::make("root", {opaque, y});
    EvalContext ctx;
    root->eval(ctx);
    try {
        aad::tangents({root}, {{x, 1.0}});
        FAIL() << "expected std::domain_error";
    } catch (const std::domain_error& e) {
        const std::string what = e.what();
        EXPECT_NE(what.find("x reaches"), std::string::npos) << what;
        EXPECT_NE(what.find("through opaque"), std::string::npos) << what;
    }
    EXPECT_EQ(aad::tangents({root}, {{opaque, 1.0}})[0], 5.0);
}

// ─────────────────────────────────────────────────────────────────────────────
// Zeros and NaNs
// ─────────────────────────────────────────────────────────────────────────────

// The exponent is not seeded, so its tangent is 0, and its NaN partial would
// make ∂root/∂a a NaN if 0·NaN counted.
TEST(AadForward, AZeroTangentPropagatesNothing) {
    auto a = Input<double>::make("a", -2.0);
    auto n = Input<double>::make("n", 3.0);
    auto p = ops::PowerNode<>::make("pow", a, n);
    EvalContext ctx;
    p->eval(ctx);
    EXPECT_EQ(aad::tangents({p}, {{a, 1.0}})[0], 12.0);
    EXPECT_TRUE(std::isnan(aad::tangents({p}, {{n, 1.0}})[0]));
}

// z·√x at z = 0 and x = 0. Seeded on x, √x's tangent is ∞, and the product's
// partial for √x is z = 0. z·√x is 0 for every x while z is 0, so the tangent
// is 0, as the reverse sweep's adjoint is.
TEST(AadForward, AZeroPartialPropagatesNothing) {
    auto z    = Input<double>::make("z", 0.0);
    auto x    = Input<double>::make("x", 0.0);
    auto root = ops::ProductNode<>::make("root", {z, ops::SqrtNode<>::make("sqrt", x)});
    EvalContext ctx;
    root->eval(ctx);
    EXPECT_EQ(aad::tangents({root}, {{x, 1.0}})[0], 0.0);
    EXPECT_EQ(aad::adjoints(root, {x})[0], 0.0);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
