// =============================================================================
// flywheel-dag — A header-only C++17 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

// Local partial derivatives: what each differentiable node reports through
// aad::IDifferentiable, and that reporting them evaluates nothing.

#include <gtest/gtest.h>
#include "flywheel/dag.hpp"
#include "flywheel/dag_ops.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <vector>

using namespace dag;

// An Op an application might define, with its derivative, and one without. A
// named namespace, not an anonymous one: a specialisation for a type with
// internal linkage has internal linkage too, and Clang's tooling then reports
// `defined` as an unused variable, although the op reads it.
namespace application_ops {

struct CubeOp {
    double operator()(const double& a) const { return a * a * a; }
};

struct SquareOp {
    double operator()(const double& a) const { return a * a; }
};

} // namespace application_ops

namespace dag::ops {

template<>
struct Derivative<application_ops::CubeOp> {
    static constexpr bool defined = true;
    static double d(double a) { return 3.0 * a * a; }
};

} // namespace dag::ops

namespace {

// What a pass sees: the node found through the mixin, and asked only once it is
// clean.
struct Reported {
    bool ok = false;
    std::vector<aad::Partials::Entry> entries;

    /// The partial for input i: the sum of the entries that name it.
    double at(std::size_t i) const {
        double d = 0.0;
        for (const auto& e : entries)
            if (e.input == i) d += e.d;
        return d;
    }
};

Reported partialsOf(const NodePtr& n) {
    EvalContext ctx;
    n->eval(ctx);
    auto* d = dynamic_cast<aad::IDifferentiable*>(n.get());
    if (d == nullptr) {
        ADD_FAILURE() << n->name() << " is not an aad::IDifferentiable";
        return {};
    }
    aad::Partials p;
    Reported r;
    r.ok = d->partials(ctx, p);
    r.entries = p.entries();
    return r;
}

// Compares each partial of `node` with a central difference, bumping one input
// at a time through set() and evaluating the node again. `ins` is in the order
// of the node's inputs().
void expectCentralDifferences(const NodePtr& node, const std::vector<InputPtr<double>>& ins) {
    const Reported r = partialsOf(node);
    ASSERT_TRUE(r.ok) << node->name();
    EvalContext ctx;
    for (std::size_t i = 0; i < ins.size(); ++i) {
        const double x  = ins[i]->get();
        const double h  = 1e-6 * std::max(1.0, std::abs(x));
        const double up = x + h;
        const double dn = x - h;
        ins[i]->set(up);
        const double fUp = get_value<double>(node->eval(ctx));
        ins[i]->set(dn);
        const double fDn = get_value<double>(node->eval(ctx));
        ins[i]->set(x);
        const double fd = (fUp - fDn) / (up - dn);
        EXPECT_NEAR(r.at(i), fd, 1e-6 * std::max(1.0, std::abs(fd)))
            << node->name() << ", input " << i;
    }
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// The ops
// ─────────────────────────────────────────────────────────────────────────────

TEST(AadPartials, OpsMatchTheirClosedForms) {
    auto a = Input<double>::make("a", 1.0);
    auto b = Input<double>::make("b", 1.0);
    auto c = Input<double>::make("c", 1.0);
    const NodePtr sum  = ops::SumNode<>::make("sum", {a, b, c});
    const NodePtr prod = ops::ProductNode<>::make("prod", {a, b, c});
    const NodePtr diff = ops::DiffNode<>::make("diff", a, b);
    const NodePtr div  = ops::DivideNode<>::make("div", a, b);
    const NodePtr neg  = ops::NegateNode<>::make("neg", a);
    const NodePtr ex   = ops::ExpNode<>::make("exp", a);
    const NodePtr ln   = ops::LnNode<>::make("ln", a);
    const NodePtr pw   = ops::PowerNode<>::make("pow", a, b);
    const NodePtr sq   = ops::SqrtNode<>::make("sqrt", a);

    struct Point { double x, y, z; };
    // a > 0 at both, so that ln, pow and sqrt are defined.
    for (const Point& p : {Point{2.0, 3.0, 4.0}, Point{0.25, -1.5, 8.0}}) {
        a->set(p.x);
        b->set(p.y);
        c->set(p.z);
        SCOPED_TRACE(testing::Message() << "at " << p.x << ", " << p.y << ", " << p.z);

        Reported r = partialsOf(sum);
        ASSERT_TRUE(r.ok);
        EXPECT_EQ(r.entries.size(), 3u);
        EXPECT_EQ(r.at(0), 1.0);
        EXPECT_EQ(r.at(1), 1.0);
        EXPECT_EQ(r.at(2), 1.0);

        r = partialsOf(prod);
        ASSERT_TRUE(r.ok);
        EXPECT_EQ(r.at(0), p.y * p.z);
        EXPECT_EQ(r.at(1), p.x * p.z);
        EXPECT_EQ(r.at(2), p.x * p.y);

        r = partialsOf(diff);
        ASSERT_TRUE(r.ok);
        EXPECT_EQ(r.at(0), 1.0);
        EXPECT_EQ(r.at(1), -1.0);

        r = partialsOf(div);
        ASSERT_TRUE(r.ok);
        EXPECT_DOUBLE_EQ(r.at(0), 1.0 / p.y);
        EXPECT_DOUBLE_EQ(r.at(1), -p.x / (p.y * p.y));

        r = partialsOf(neg);
        ASSERT_TRUE(r.ok);
        EXPECT_EQ(r.at(0), -1.0);

        r = partialsOf(ex);
        ASSERT_TRUE(r.ok);
        EXPECT_DOUBLE_EQ(r.at(0), std::exp(p.x));

        r = partialsOf(ln);
        ASSERT_TRUE(r.ok);
        EXPECT_DOUBLE_EQ(r.at(0), 1.0 / p.x);

        r = partialsOf(pw);
        ASSERT_TRUE(r.ok);
        EXPECT_DOUBLE_EQ(r.at(0), p.y * std::pow(p.x, p.y - 1.0));
        EXPECT_DOUBLE_EQ(r.at(1), std::pow(p.x, p.y) * std::log(p.x));

        r = partialsOf(sq);
        ASSERT_TRUE(r.ok);
        EXPECT_DOUBLE_EQ(r.at(0), 1.0 / (2.0 * std::sqrt(p.x)));
    }
}

TEST(AadPartials, OpsMatchCentralDifferences) {
    auto a = Input<double>::make("a", 1.7);
    auto b = Input<double>::make("b", -0.6);
    auto c = Input<double>::make("c", 2.3);
    expectCentralDifferences(ops::SumNode<>::make("sum", {a, b, c}), {a, b, c});
    expectCentralDifferences(ops::ProductNode<>::make("prod", {a, b, c}), {a, b, c});
    expectCentralDifferences(ops::DiffNode<>::make("diff", a, b), {a, b});
    expectCentralDifferences(ops::DivideNode<>::make("div", a, b), {a, b});
    expectCentralDifferences(ops::NegateNode<>::make("neg", a), {a});
    expectCentralDifferences(ops::ExpNode<>::make("exp", a), {a});
    expectCentralDifferences(ops::LnNode<>::make("ln", a), {a});
    expectCentralDifferences(ops::PowerNode<>::make("pow", a, b), {a, b});
    expectCentralDifferences(ops::SqrtNode<>::make("sqrt", a), {a});
}

// A factor of 0 is where product / x_i would be 0/0.
TEST(AadPartials, ProductIsExactWithZeroFactors) {
    auto a = Input<double>::make("a", 0.0);
    auto b = Input<double>::make("b", 3.0);
    auto c = Input<double>::make("c", 4.0);
    const NodePtr prod = ops::ProductNode<>::make("prod", {a, b, c});

    Reported r = partialsOf(prod);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.at(0), 12.0);
    EXPECT_EQ(r.at(1), 0.0);
    EXPECT_EQ(r.at(2), 0.0);

    b->set(0.0);
    r = partialsOf(prod);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.at(0), 0.0);
    EXPECT_EQ(r.at(1), 0.0);
    EXPECT_EQ(r.at(2), 0.0);
}

// 0^b is 0 for every b > 0, where a^b·ln a is 0·(−∞).
TEST(AadPartials, PowerIsFlatInTheExponentAtZero) {
    auto a = Input<double>::make("a", 0.0);
    auto b = Input<double>::make("b", 2.0);
    const Reported r = partialsOf(ops::PowerNode<>::make("pow", a, b));
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.at(0), 0.0);
    EXPECT_EQ(r.at(1), 0.0);
}

// a^0 is 1 for every a, where b·a^(b−1) is 0·∞ at a = 0.
TEST(AadPartials, PowerIsFlatInTheBaseWhenTheExponentIsZero) {
    auto a = Input<double>::make("a", 0.0);
    auto b = Input<double>::make("b", 0.0);
    const NodePtr pw = ops::PowerNode<>::make("pow", a, b);

    Reported r = partialsOf(pw);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.at(0), 0.0);

    a->set(-2.0);
    r = partialsOf(pw);
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.at(0), 0.0);
}

// A NaN that is the true answer is kept: a^b has no derivative in b at a < 0.
TEST(AadPartials, PowerKeepsATrueNaN) {
    auto a = Input<double>::make("a", -2.0);
    auto b = Input<double>::make("b", 3.0);
    const Reported r = partialsOf(ops::PowerNode<>::make("pow", a, b));
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.at(0), 12.0);
    EXPECT_TRUE(std::isnan(r.at(1)));
}

TEST(AadPartials, OnlyDoubleOpsHavePartials) {
    auto i = Input<int>::make("i", 2);
    const Reported ri = partialsOf(ops::SumNode<int>::make("sumInt", {i, i}));
    EXPECT_FALSE(ri.ok);
    EXPECT_TRUE(ri.entries.empty());

    auto f = Input<float>::make("f", 1.5f);
    const Reported rf = partialsOf(ops::ExpNode<float>::make("expFloat", f));
    EXPECT_FALSE(rf.ok);
    EXPECT_TRUE(rf.entries.empty());

    const Reported rs = partialsOf(ops::SinNode<float>::make("sinFloat", f));
    EXPECT_FALSE(rs.ok);
    EXPECT_TRUE(rs.entries.empty());

    auto g = Input<float>::make("g", 2.0f);
    const Reported ra = partialsOf(ops::Atan2Node<float>::make("atan2Float", f, g));
    EXPECT_FALSE(ra.ok);
    EXPECT_TRUE(ra.entries.empty());
}

TEST(AadPartials, AnApplicationOpCanSpecialiseDerivative) {
    auto x = Input<double>::make("x", 2.0);

    const Reported cube =
        partialsOf(ops::UnaryOpNode<double, application_ops::CubeOp>::make("cube", x));
    ASSERT_TRUE(cube.ok);
    EXPECT_EQ(cube.at(0), 12.0);

    const Reported square =
        partialsOf(ops::UnaryOpNode<double, application_ops::SquareOp>::make("square", x));
    EXPECT_FALSE(square.ok);
}

// One node on both sides: the two entries name it, and add up to 0.
TEST(AadPartials, AnInputNamedTwiceGetsTwoEntries) {
    auto x = Input<double>::make("x", 2.0);
    const Reported r = partialsOf(ops::DiffNode<>::make("x-x", x, x));
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.entries.size(), 2u);
    EXPECT_EQ(r.entries[0].d + r.entries[1].d, 0.0);
}

// ─────────────────────────────────────────────────────────────────────────────
// The trigonometric ops
// ─────────────────────────────────────────────────────────────────────────────

// The textbook forms, to 4 ulp. For tan the check is 1/cos² a, not the op's
// 1 + tan² a, so it does not test the formula against itself.
TEST(AadPartials, TrigOpsMatchTheirClosedForms) {
    auto a = Input<double>::make("a", 0.0);
    auto b = Input<double>::make("b", 1.0);
    const NodePtr sn  = ops::SinNode<>::make("sin", a);
    const NodePtr cs  = ops::CosNode<>::make("cos", a);
    const NodePtr tn  = ops::TanNode<>::make("tan", a);
    const NodePtr as  = ops::AsinNode<>::make("asin", a);
    const NodePtr ac  = ops::AcosNode<>::make("acos", a);
    const NodePtr at  = ops::AtanNode<>::make("atan", a);
    const NodePtr at2 = ops::Atan2Node<>::make("atan2", a, b);

    struct Point { double x, y; };
    // |x| < 1 at both, so that asin and acos are defined.
    for (const Point& p : {Point{0.3, -1.7}, Point{-0.8, 2.4}}) {
        a->set(p.x);
        b->set(p.y);
        SCOPED_TRACE(testing::Message() << "at " << p.x << ", " << p.y);

        Reported r = partialsOf(sn);
        ASSERT_TRUE(r.ok);
        EXPECT_DOUBLE_EQ(r.at(0), std::cos(p.x));

        r = partialsOf(cs);
        ASSERT_TRUE(r.ok);
        EXPECT_DOUBLE_EQ(r.at(0), -std::sin(p.x));

        r = partialsOf(tn);
        ASSERT_TRUE(r.ok);
        EXPECT_DOUBLE_EQ(r.at(0), 1.0 / (std::cos(p.x) * std::cos(p.x)));

        r = partialsOf(as);
        ASSERT_TRUE(r.ok);
        EXPECT_DOUBLE_EQ(r.at(0), 1.0 / std::sqrt(1.0 - p.x * p.x));

        r = partialsOf(ac);
        ASSERT_TRUE(r.ok);
        EXPECT_DOUBLE_EQ(r.at(0), -1.0 / std::sqrt(1.0 - p.x * p.x));

        r = partialsOf(at);
        ASSERT_TRUE(r.ok);
        EXPECT_DOUBLE_EQ(r.at(0), 1.0 / (1.0 + p.x * p.x));

        r = partialsOf(at2);
        ASSERT_TRUE(r.ok);
        const double r2 = p.x * p.x + p.y * p.y;
        EXPECT_DOUBLE_EQ(r.at(0), p.y / r2);
        EXPECT_DOUBLE_EQ(r.at(1), -p.x / r2);
    }
}

TEST(AadPartials, TrigOpsMatchCentralDifferences) {
    auto a = Input<double>::make("a", 0.45);
    auto b = Input<double>::make("b", -1.3);
    expectCentralDifferences(ops::SinNode<>::make("sin", a), {a});
    expectCentralDifferences(ops::CosNode<>::make("cos", a), {a});
    expectCentralDifferences(ops::TanNode<>::make("tan", a), {a});
    expectCentralDifferences(ops::AsinNode<>::make("asin", a), {a});
    expectCentralDifferences(ops::AcosNode<>::make("acos", a), {a});
    expectCentralDifferences(ops::AtanNode<>::make("atan", a), {a});
    expectCentralDifferences(ops::Atan2Node<>::make("atan2", a, b), {a, b});
}

// Next to |a| = 1, 1 − a² cancels: at a = 1 − 2⁻²⁷ it reads 2⁻²⁶ where the exact
// value is 2⁻²⁶ − 2⁻⁵⁴, and the textbook partial is 1.9e-9 too small. The
// textbook form passes here only where the compiler fuses 1 − a·a into one FMA,
// as Apple Clang does on arm64.
TEST(AadPartials, AsinAndAcosAreAccurateNextToTheirEnds) {
    const double nearOne = 1.0 - std::ldexp(1.0, -27);
    // 1/√(2⁻²⁶ − 2⁻⁵⁴), rounded twice: 1 − 2⁻²⁸ is exact.
    const double exact = std::ldexp(1.0, 13) / std::sqrt(1.0 - std::ldexp(1.0, -28));
    auto a = Input<double>::make("a", nearOne);
    const NodePtr as = ops::AsinNode<>::make("asin", a);
    const NodePtr ac = ops::AcosNode<>::make("acos", a);
    for (const double x : {nearOne, -nearOne}) {
        a->set(x);
        SCOPED_TRACE(testing::Message() << "at " << x);
        const Reported rs = partialsOf(as);
        ASSERT_TRUE(rs.ok);
        EXPECT_DOUBLE_EQ(rs.at(0), exact);
        const Reported rc = partialsOf(ac);
        ASSERT_TRUE(rc.ok);
        EXPECT_DOUBLE_EQ(rc.at(0), -exact);
    }
}

// At |a| = 1 the slope from inside is infinite, as √'s is at 0. Beyond, the
// value is NaN, and so are the partials.
TEST(AadPartials, AsinAndAcosKeepTheirTrueInfinities) {
    const double inf = std::numeric_limits<double>::infinity();
    auto a = Input<double>::make("a", 1.0);
    const NodePtr as = ops::AsinNode<>::make("asin", a);
    const NodePtr ac = ops::AcosNode<>::make("acos", a);
    for (const double x : {1.0, -1.0}) {
        a->set(x);
        EXPECT_EQ(partialsOf(as).at(0), inf) << "at " << x;
        EXPECT_EQ(partialsOf(ac).at(0), -inf) << "at " << x;
    }

    a->set(1.5);
    EXPECT_TRUE(std::isnan(partialsOf(as).at(0)));
    EXPECT_TRUE(std::isnan(partialsOf(ac).at(0)));
}

// a² + b² underflows to 0 at the first point, and overflows at the second. The
// exact partials are b/(a² + b²) and −a/(a² + b²): (4/25, −3/25)·2⁶⁰⁰ at
// (3, 4)·2⁻⁶⁰⁰, and (4/25, −3/25)·2⁻⁶⁰⁰ at (3, 4)·2⁶⁰⁰.
TEST(AadPartials, Atan2NeitherOverflowsNorUnderflows) {
    auto a = Input<double>::make("a", 0.0);
    auto b = Input<double>::make("b", 1.0);
    const NodePtr at2 = ops::Atan2Node<>::make("atan2", a, b);
    for (const int e : {-600, 600}) {
        a->set(std::ldexp(3.0, e));
        b->set(std::ldexp(4.0, e));
        SCOPED_TRACE(testing::Message() << "at (3, 4) * 2^" << e);
        const Reported r = partialsOf(at2);
        ASSERT_TRUE(r.ok);
        EXPECT_DOUBLE_EQ(r.at(0), std::ldexp(4.0 / 25.0, -e));
        EXPECT_DOUBLE_EQ(r.at(1), std::ldexp(-3.0 / 25.0, -e));
    }
}

// The angle jumps at the origin, so it has no derivative there: 0/0.
TEST(AadPartials, Atan2HasNoDerivativeAtTheOrigin) {
    auto a = Input<double>::make("a", 0.0);
    auto b = Input<double>::make("b", 0.0);
    const Reported r = partialsOf(ops::Atan2Node<>::make("atan2", a, b));
    ASSERT_TRUE(r.ok);
    EXPECT_TRUE(std::isnan(r.at(0)));
    EXPECT_TRUE(std::isnan(r.at(1)));
}

// ─────────────────────────────────────────────────────────────────────────────
// ConditionNode
// ─────────────────────────────────────────────────────────────────────────────

TEST(AadPartials, ConditionNamesOnlyTheTakenBranch) {
    auto cond = Input<bool>::make("cond", true);
    auto x    = Input<double>::make("x", 2.0);
    auto y    = Input<double>::make("y", 5.0);
    const NodePtr sel = ConditionNode::make("sel", cond, x, y);

    Reported r = partialsOf(sel);
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.entries.size(), 1u);
    EXPECT_EQ(r.entries[0].input, 1u);
    EXPECT_EQ(r.entries[0].d, 1.0);

    cond->set(false);
    r = partialsOf(sel);
    ASSERT_TRUE(r.ok);
    ASSERT_EQ(r.entries.size(), 1u);
    EXPECT_EQ(r.entries[0].input, 2u);
    EXPECT_EQ(r.entries[0].d, 1.0);
}

// eval() never pulls the branch it does not take, and neither may partials():
// the branch can be stale, and pulling it would evaluate it.
TEST(AadPartials, ConditionNeverReadsTheOtherBranch) {
    auto cond = Input<bool>::make("cond", true);
    auto x    = Input<double>::make("x", 2.0);
    int otherEvals = 0;
    auto other = ComputeNode<double, double>::make("other", {x},
        [&otherEvals](const double& v) { ++otherEvals; return v * 3.0; });
    const NodePtr sel = ConditionNode::make("sel", cond, x, other);

    EvalContext ctx;
    sel->eval(ctx);
    x->set(3.0);
    sel->eval(ctx);

    auto* d = dynamic_cast<aad::IDifferentiable*>(sel.get());
    ASSERT_NE(d, nullptr);
    aad::Partials p;
    ASSERT_TRUE(d->partials(ctx, p));
    EXPECT_EQ(otherEvals, 0);
    EXPECT_TRUE(other->dirty());
}

// ─────────────────────────────────────────────────────────────────────────────
// TweakableComputeNode
// ─────────────────────────────────────────────────────────────────────────────

TEST(AadPartials, ATweakedNodeIsAConstant) {
    auto x = Input<double>::make("x", 2.0);
    auto t = TweakableComputeNode<double, double>::make("t", {x},
        [](const double& v) { return v * v; });

    t->tweak(5.0);
    Reported r = partialsOf(t);
    EXPECT_TRUE(r.ok);
    EXPECT_TRUE(r.entries.empty());

    t->clearTweak();
    r = partialsOf(t);
    EXPECT_FALSE(r.ok);
    EXPECT_TRUE(r.entries.empty());
}

// ─────────────────────────────────────────────────────────────────────────────
// Reading partials leaves the graph alone
// ─────────────────────────────────────────────────────────────────────────────

TEST(AadPartials, ReadingPartialsEvaluatesNothing) {
    auto x    = Input<double>::make("x", 2.0);
    auto y    = Input<double>::make("y", 3.0);
    auto cond = Input<bool>::make("cond", true);
    int evals = 0;
    auto counted = ComputeNode<double, double>::make("counted", {x},
        [&evals](const double& v) { ++evals; return v + 1.0; });
    auto frozen = TweakableComputeNode<double, double>::make("frozen", {counted},
        [](const double& v) { return v * 2.0; });
    frozen->tweak(7.0);

    const std::vector<NodePtr> nodes = {
        x, y, cond, counted, frozen,
        ops::SumNode<>::make("sum", {x, y, counted}),
        ops::ProductNode<>::make("prod", {x, counted, frozen}),
        ops::DivideNode<>::make("div", counted, y),
        ops::ExpNode<>::make("exp", counted),
        ConditionNode::make("sel", cond, counted, y),
    };

    EvalContext ctx;
    std::vector<ValuePtr> before;
    before.reserve(nodes.size());
    for (const auto& n : nodes) before.push_back(n->eval(ctx));
    const int evalsBefore = evals;

    for (const auto& n : nodes) {
        if (auto* d = dynamic_cast<aad::IDifferentiable*>(n.get())) {
            aad::Partials p;
            EXPECT_TRUE(d->partials(ctx, p)) << n->name();
        }
    }

    for (std::size_t i = 0; i < nodes.size(); ++i) {
        EXPECT_FALSE(nodes[i]->dirty()) << nodes[i]->name();
        EXPECT_EQ(nodes[i]->eval(ctx).get(), before[i].get()) << nodes[i]->name();
    }
    EXPECT_EQ(evals, evalsBefore);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
