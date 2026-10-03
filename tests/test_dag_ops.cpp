// =============================================================================
// flywheel-dag — A header-only C++17 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

#include <gtest/gtest.h>
#include "flywheel/dag.hpp"
#include "flywheel/dag_ops.hpp"
#include <cmath>
#include <limits>
#include <vector>

using namespace dag;

// ── Helper function ──────────────────────────────────────────────────────────
// Not named get_value, so it does not overload dag::get_value<T>(ValuePtr): this
// takes the node and evaluates it first.
template<typename T, typename Node>
T evalAs(const std::shared_ptr<Node>& n) {
    EvalContext ctx;
    return get_value<T>(n->eval(ctx));
}

// ─────────────────────────────────────────────────────────────────────────────
// SumNode<T>
// ─────────────────────────────────────────────────────────────────────────────

TEST(DagOpsTests, SumBasic) {
    auto a = Input<double>::make("a", 1.0);
    auto b = Input<double>::make("b", 2.0);
    auto c = Input<double>::make("c", 3.0);

    auto sum = ops::SumNode<double>::make("sum", {a, b, c});

    EXPECT_EQ(evalAs<double>(sum), 6.0); // 1+2+3
}

TEST(DagOpsTests, SumRecomputesOnInputChange) {
    auto a = Input<double>::make("a", 1.0);
    auto b = Input<double>::make("b", 2.0);
    auto c = Input<double>::make("c", 3.0);

    auto sum = ops::SumNode<double>::make("sum", {a, b, c});

    EXPECT_EQ(evalAs<double>(sum), 6.0);
    b->set(10.0);
    EXPECT_EQ(evalAs<double>(sum), 14.0); // 1+10+3
}

TEST(DagOpsTests, SumEmptyInputsIsZero) {
    auto sum = ops::SumNode<double>::make("sum", {});
    EXPECT_EQ(evalAs<double>(sum), 0.0);
}

TEST(DagOpsTests, SumSingleInputPassesThrough) {
    auto a = Input<double>::make("a", 42.0);
    auto sum = ops::SumNode<double>::make("sum", {a});
    EXPECT_EQ(evalAs<double>(sum), 42.0);
}

TEST(DagOpsTests, SumIntType) {
    auto a = Input<int>::make("a", 1);
    auto b = Input<int>::make("b", 2);
    auto c = Input<int>::make("c", 3);

    auto sum = ops::SumNode<int>::make("sum", {a, b, c});

    EXPECT_EQ(evalAs<int>(sum), 6);
}

TEST(DagOpsTests, SumDefaultTemplateArgIsDouble) {
    auto a = Input<double>::make("a", 1.5);
    auto b = Input<double>::make("b", 2.5);

    auto sum = ops::SumNode<>::make("sum", {a, b});

    EXPECT_EQ(evalAs<double>(sum), 4.0);
}

// Observed by pointer identity: the default TypedEqualityPolicy<T> keeps the
// cached ValuePtr when the total comes out equal. A downstream recompute count
// cannot show it under an Eager consumer, which recomputes anyway; a Lazy one
// skips, as in
// LazyInvalidation.Case1_ConstantIntermediateStopsDownstreamRecomputing.
TEST(DagOpsTests, SumPreservesCachedValuePtrWhenTotalUnchanged) {
    auto a = Input<double>::make("a", 1.0);
    auto b = Input<double>::make("b", 2.0);
    auto sum = ops::SumNode<double>::make("sum", {a, b});

    EvalContext ctx;
    auto before = sum->eval(ctx); // settle: total = 3.0
    EXPECT_EQ(get_value<double>(before), 3.0);

    // a+1, b-1 leaves the total at 3.0: SumNode recomputes, but the default
    // equality policy keeps the cached ValuePtr rather than replacing it with an
    // equal one.
    a->set(2.0);
    b->set(1.0);
    auto after = sum->eval(ctx);
    EXPECT_EQ(before.get(), after.get());
    EXPECT_EQ(get_value<double>(after), 3.0);

    a->set(5.0); // total actually changes -> cached ValuePtr is replaced
    auto changed = sum->eval(ctx);
    EXPECT_NE(after.get(), changed.get());
    EXPECT_EQ(get_value<double>(changed), 6.0);
}

TEST(DagOpsTests, SumKindIsCompute) {
    auto a = Input<double>::make("a", 1.0);
    auto sum = ops::SumNode<double>::make("sum", {a});
    EXPECT_EQ(sum->kind(), NodeKind::Compute);
}

// ─────────────────────────────────────────────────────────────────────────────
// ProductNode<T>
// ─────────────────────────────────────────────────────────────────────────────

TEST(DagOpsTests, ProductBasic) {
    auto a = Input<double>::make("a", 2.0);
    auto b = Input<double>::make("b", 3.0);
    auto c = Input<double>::make("c", 4.0);

    auto product = ops::ProductNode<double>::make("product", {a, b, c});

    EXPECT_EQ(evalAs<double>(product), 24.0); // 2*3*4
}

TEST(DagOpsTests, ProductRecomputesOnInputChange) {
    auto a = Input<double>::make("a", 2.0);
    auto b = Input<double>::make("b", 3.0);
    auto product = ops::ProductNode<double>::make("product", {a, b});

    EXPECT_EQ(evalAs<double>(product), 6.0);
    b->set(5.0);
    EXPECT_EQ(evalAs<double>(product), 10.0);
}

TEST(DagOpsTests, ProductEmptyInputsIsOne) {
    auto product = ops::ProductNode<double>::make("product", {});
    EXPECT_EQ(evalAs<double>(product), 1.0);
}

TEST(DagOpsTests, ProductSingleInputPassesThrough) {
    auto a = Input<double>::make("a", 7.0);
    auto product = ops::ProductNode<double>::make("product", {a});
    EXPECT_EQ(evalAs<double>(product), 7.0);
}

TEST(DagOpsTests, ProductWithZeroInputIsZero) {
    auto a = Input<double>::make("a", 2.0);
    auto b = Input<double>::make("b", 0.0);
    auto c = Input<double>::make("c", 4.0);

    auto product = ops::ProductNode<double>::make("product", {a, b, c});

    EXPECT_EQ(evalAs<double>(product), 0.0);
}

// See SumPreservesCachedValuePtrWhenTotalUnchanged for why this checks
// cached-pointer identity rather than a downstream recompute count.
TEST(DagOpsTests, ProductPreservesCachedValuePtrWhenTotalUnchanged) {
    auto a = Input<double>::make("a", 2.0);
    auto b = Input<double>::make("b", 3.0);
    auto product = ops::ProductNode<double>::make("product", {a, b});

    EvalContext ctx;
    auto before = product->eval(ctx); // settle: total = 6.0
    EXPECT_EQ(get_value<double>(before), 6.0);

    // a*2, b/2 leaves the total (6.0) unchanged.
    a->set(4.0);
    b->set(1.5);
    auto after = product->eval(ctx);
    EXPECT_EQ(before.get(), after.get());
    EXPECT_EQ(get_value<double>(after), 6.0);

    a->set(10.0); // total actually changes -> cached ValuePtr is replaced
    auto changed = product->eval(ctx);
    EXPECT_NE(after.get(), changed.get());
    EXPECT_EQ(get_value<double>(changed), 15.0);
}

TEST(DagOpsTests, ChainSumAndProduct) {
    auto a = Input<double>::make("a", 1.0);
    auto b = Input<double>::make("b", 2.0);
    auto c = Input<double>::make("c", 3.0);
    auto d = Input<double>::make("d", 4.0);

    auto sumAB = ops::SumNode<double>::make("sumAB", {a, b});   // a+b
    auto sumCD = ops::SumNode<double>::make("sumCD", {c, d});   // c+d
    auto product = ops::ProductNode<double>::make("product", {sumAB, sumCD}); // (a+b)*(c+d)

    EXPECT_EQ(evalAs<double>(product), 21.0); // 3*7

    a->set(5.0); // sumAB = 7 -> product = 7*7 = 49
    EXPECT_EQ(evalAs<double>(product), 49.0);
}

// ─────────────────────────────────────────────────────────────────────────────
// DiffNode<T>
// ─────────────────────────────────────────────────────────────────────────────

TEST(DagOpsTests, DiffBasic) {
    auto a = Input<double>::make("a", 5.0);
    auto b = Input<double>::make("b", 3.0);
    auto diff = ops::DiffNode<double>::make("diff", a, b);
    EXPECT_EQ(evalAs<double>(diff), 2.0);
}

TEST(DagOpsTests, DiffRecomputesOnEitherInputChange) {
    auto a = Input<double>::make("a", 5.0);
    auto b = Input<double>::make("b", 3.0);
    auto diff = ops::DiffNode<double>::make("diff", a, b);

    EXPECT_EQ(evalAs<double>(diff), 2.0);
    a->set(10.0);
    EXPECT_EQ(evalAs<double>(diff), 7.0);
    b->set(1.0);
    EXPECT_EQ(evalAs<double>(diff), 9.0);
}

TEST(DagOpsTests, DiffNegativeResult) {
    auto a = Input<double>::make("a", 1.0);
    auto b = Input<double>::make("b", 5.0);
    auto diff = ops::DiffNode<double>::make("diff", a, b);
    EXPECT_EQ(evalAs<double>(diff), -4.0);
}

// ─────────────────────────────────────────────────────────────────────────────
// DivideNode<T>
// ─────────────────────────────────────────────────────────────────────────────

TEST(DagOpsTests, DivideBasic) {
    auto a = Input<double>::make("a", 10.0);
    auto b = Input<double>::make("b", 4.0);
    auto divide = ops::DivideNode<double>::make("divide", a, b);
    EXPECT_EQ(evalAs<double>(divide), 2.5);
}

TEST(DagOpsTests, DivideRecomputesOnEitherInputChange) {
    auto a = Input<double>::make("a", 10.0);
    auto b = Input<double>::make("b", 4.0);
    auto divide = ops::DivideNode<double>::make("divide", a, b);

    EXPECT_EQ(evalAs<double>(divide), 2.5);
    a->set(20.0);
    EXPECT_EQ(evalAs<double>(divide), 5.0);
    b->set(2.0);
    EXPECT_EQ(evalAs<double>(divide), 10.0);
}

TEST(DagOpsTests, DivideByZeroProducesInf) {
    auto a = Input<double>::make("a", 1.0);
    auto b = Input<double>::make("b", 0.0);
    auto divide = ops::DivideNode<double>::make("divide", a, b);
    EXPECT_TRUE(std::isinf(evalAs<double>(divide)));
}

TEST(DagOpsTests, DivideZeroOverZeroProducesNan) {
    auto a = Input<double>::make("a", 0.0);
    auto b = Input<double>::make("b", 0.0);
    auto divide = ops::DivideNode<double>::make("divide", a, b);
    EXPECT_TRUE(std::isnan(evalAs<double>(divide)));
}

// ─────────────────────────────────────────────────────────────────────────────
// NegateNode<T>
// ─────────────────────────────────────────────────────────────────────────────

TEST(DagOpsTests, NegateBasic) {
    auto a = Input<double>::make("a", 4.0);
    auto negate = ops::NegateNode<double>::make("negate", a);
    EXPECT_EQ(evalAs<double>(negate), -4.0);
}

TEST(DagOpsTests, NegateRecomputesOnInputChange) {
    auto a = Input<double>::make("a", 4.0);
    auto negate = ops::NegateNode<double>::make("negate", a);

    EXPECT_EQ(evalAs<double>(negate), -4.0);
    a->set(-7.0);
    EXPECT_EQ(evalAs<double>(negate), 7.0);
}

TEST(DagOpsTests, NegateDoubleNegationRoundTrips) {
    auto a = Input<double>::make("a", 4.0);
    auto negate1 = ops::NegateNode<double>::make("negate1", a);
    auto negate2 = ops::NegateNode<double>::make("negate2", negate1);
    EXPECT_EQ(evalAs<double>(negate2), 4.0);
}

// ─────────────────────────────────────────────────────────────────────────────
// ExpNode<T>
// ─────────────────────────────────────────────────────────────────────────────

TEST(DagOpsTests, ExpBasic) {
    auto a = Input<double>::make("a", 0.0);
    auto expNode = ops::ExpNode<double>::make("exp", a);
    EXPECT_EQ(evalAs<double>(expNode), 1.0); // exp(0) == 1
}

TEST(DagOpsTests, ExpRecomputesOnInputChange) {
    auto a = Input<double>::make("a", 1.0);
    auto expNode = ops::ExpNode<double>::make("exp", a);

    EXPECT_DOUBLE_EQ(evalAs<double>(expNode), std::exp(1.0));
    a->set(2.0);
    EXPECT_DOUBLE_EQ(evalAs<double>(expNode), std::exp(2.0));
}

TEST(DagOpsTests, ExpOverflowProducesInf) {
    auto a = Input<double>::make("a", 1000.0);
    auto expNode = ops::ExpNode<double>::make("exp", a);
    EXPECT_TRUE(std::isinf(evalAs<double>(expNode)));
}

// ─────────────────────────────────────────────────────────────────────────────
// LnNode<T>
// ─────────────────────────────────────────────────────────────────────────────

TEST(DagOpsTests, LnBasic) {
    auto a = Input<double>::make("a", 1.0);
    auto ln = ops::LnNode<double>::make("ln", a);
    EXPECT_EQ(evalAs<double>(ln), 0.0); // ln(1) == 0
}

TEST(DagOpsTests, LnRecomputesOnInputChange) {
    auto a = Input<double>::make("a", 2.0);
    auto ln = ops::LnNode<double>::make("ln", a);

    EXPECT_DOUBLE_EQ(evalAs<double>(ln), std::log(2.0));
    a->set(10.0);
    EXPECT_DOUBLE_EQ(evalAs<double>(ln), std::log(10.0));
}

TEST(DagOpsTests, LnOfZeroIsNegativeInfinity) {
    auto a = Input<double>::make("a", 0.0);
    auto ln = ops::LnNode<double>::make("ln", a);
    double result = evalAs<double>(ln);
    EXPECT_TRUE(std::isinf(result));
    EXPECT_LT(result, 0.0);
}

TEST(DagOpsTests, LnOfNegativeIsNan) {
    auto a = Input<double>::make("a", -1.0);
    auto ln = ops::LnNode<double>::make("ln", a);
    EXPECT_TRUE(std::isnan(evalAs<double>(ln)));
}

// ─────────────────────────────────────────────────────────────────────────────
// PowerNode<T>
// ─────────────────────────────────────────────────────────────────────────────

TEST(DagOpsTests, PowerBasic) {
    auto a = Input<double>::make("a", 2.0);
    auto b = Input<double>::make("b", 3.0);
    auto power = ops::PowerNode<double>::make("power", a, b);
    EXPECT_EQ(evalAs<double>(power), 8.0); // 2^3
}

TEST(DagOpsTests, PowerRecomputesOnEitherInputChange) {
    auto a = Input<double>::make("a", 2.0);
    auto b = Input<double>::make("b", 3.0);
    auto power = ops::PowerNode<double>::make("power", a, b);

    EXPECT_EQ(evalAs<double>(power), 8.0);
    a->set(3.0);
    EXPECT_EQ(evalAs<double>(power), 27.0);
    b->set(2.0);
    EXPECT_EQ(evalAs<double>(power), 9.0);
}

TEST(DagOpsTests, PowerZeroExponentIsOne) {
    auto a = Input<double>::make("a", 5.0);
    auto b = Input<double>::make("b", 0.0);
    auto power = ops::PowerNode<double>::make("power", a, b);
    EXPECT_EQ(evalAs<double>(power), 1.0);
}

TEST(DagOpsTests, PowerNegativeBaseFractionalExponentIsNan) {
    auto a = Input<double>::make("a", -1.0);
    auto b = Input<double>::make("b", 0.5);
    auto power = ops::PowerNode<double>::make("power", a, b);
    EXPECT_TRUE(std::isnan(evalAs<double>(power)));
}

TEST(DagOpsTests, PowerOverflowProducesInf) {
    auto a = Input<double>::make("a", 10.0);
    auto b = Input<double>::make("b", 1000.0);
    auto power = ops::PowerNode<double>::make("power", a, b);
    EXPECT_TRUE(std::isinf(evalAs<double>(power)));
}

// ─────────────────────────────────────────────────────────────────────────────
// SqrtNode<T>
// ─────────────────────────────────────────────────────────────────────────────

TEST(DagOpsTests, SqrtBasic) {
    auto a = Input<double>::make("a", 9.0);
    auto sqrtNode = ops::SqrtNode<double>::make("sqrt", a);
    EXPECT_EQ(evalAs<double>(sqrtNode), 3.0);
}

TEST(DagOpsTests, SqrtRecomputesOnInputChange) {
    auto a = Input<double>::make("a", 9.0);
    auto sqrtNode = ops::SqrtNode<double>::make("sqrt", a);

    EXPECT_EQ(evalAs<double>(sqrtNode), 3.0);
    a->set(16.0);
    EXPECT_EQ(evalAs<double>(sqrtNode), 4.0);
}

TEST(DagOpsTests, SqrtOfZeroIsZero) {
    auto a = Input<double>::make("a", 0.0);
    auto sqrtNode = ops::SqrtNode<double>::make("sqrt", a);
    EXPECT_EQ(evalAs<double>(sqrtNode), 0.0);
}

TEST(DagOpsTests, SqrtOfNegativeIsNan) {
    auto a = Input<double>::make("a", -4.0);
    auto sqrtNode = ops::SqrtNode<double>::make("sqrt", a);
    EXPECT_TRUE(std::isnan(evalAs<double>(sqrtNode)));
}

// ─────────────────────────────────────────────────────────────────────────────
// Trigonometric ops: SinNode, CosNode, TanNode, AsinNode, AcosNode, AtanNode,
// Atan2Node
// ─────────────────────────────────────────────────────────────────────────────

namespace {

// M_PI is POSIX, not standard C++.
const double kPi = std::acos(-1.0);

} // namespace

// Each node calls its std:: function, but a test's own call can be folded at
// compile time: GCC evaluates a constant std::sin(1.2) correctly rounded, and
// the library the node calls at run time need not be. Hence 4 ulp, not exact.
TEST(DagOpsTests, TrigOpsMatchStd) {
    // Inside asin's and acos's domain, and in three of atan2's quadrants.
    struct Point { double a, b; };
    for (const Point& p : {Point{-0.8, -1.7}, Point{0.3, 2.2}, Point{0.9, -0.4}}) {
        SCOPED_TRACE(testing::Message() << "at " << p.a << ", " << p.b);
        auto a = Input<double>::make("a", p.a);
        auto b = Input<double>::make("b", p.b);
        auto sinNode   = ops::SinNode<double>::make("sin", a);
        auto cosNode   = ops::CosNode<double>::make("cos", a);
        auto tanNode   = ops::TanNode<double>::make("tan", a);
        auto asinNode  = ops::AsinNode<double>::make("asin", a);
        auto acosNode  = ops::AcosNode<double>::make("acos", a);
        auto atanNode  = ops::AtanNode<double>::make("atan", a);
        auto atan2Node = ops::Atan2Node<double>::make("atan2", a, b);

        EXPECT_DOUBLE_EQ(evalAs<double>(sinNode), std::sin(p.a));
        EXPECT_DOUBLE_EQ(evalAs<double>(cosNode), std::cos(p.a));
        EXPECT_DOUBLE_EQ(evalAs<double>(tanNode), std::tan(p.a));
        EXPECT_DOUBLE_EQ(evalAs<double>(asinNode), std::asin(p.a));
        EXPECT_DOUBLE_EQ(evalAs<double>(acosNode), std::acos(p.a));
        EXPECT_DOUBLE_EQ(evalAs<double>(atanNode), std::atan(p.a));
        EXPECT_DOUBLE_EQ(evalAs<double>(atan2Node), std::atan2(p.a, p.b));
    }
}

TEST(DagOpsTests, TrigOpsRecomputeOnInputChange) {
    auto a = Input<double>::make("a", 0.25);
    auto b = Input<double>::make("b", 2.0);
    const std::vector<NodePtr> unary = {
        ops::SinNode<double>::make("sin", a),   ops::CosNode<double>::make("cos", a),
        ops::TanNode<double>::make("tan", a),   ops::AsinNode<double>::make("asin", a),
        ops::AcosNode<double>::make("acos", a), ops::AtanNode<double>::make("atan", a),
    };
    auto atan2Node = ops::Atan2Node<double>::make("atan2", a, b);

    for (const double x : {0.25, -0.5}) {
        a->set(x);
        const double want[] = {std::sin(x),  std::cos(x),  std::tan(x),
                               std::asin(x), std::acos(x), std::atan(x)};
        for (std::size_t i = 0; i < unary.size(); ++i)
            EXPECT_DOUBLE_EQ(evalAs<double>(unary[i]), want[i]) << unary[i]->name() << " at " << x;
        EXPECT_DOUBLE_EQ(evalAs<double>(atan2Node), std::atan2(x, 2.0)) << "at " << x;
    }

    // b moves atan2 alone.
    b->set(-3.0);
    for (const auto& n : unary) EXPECT_FALSE(n->dirty()) << n->name();
    EXPECT_DOUBLE_EQ(evalAs<double>(atan2Node), std::atan2(-0.5, -3.0));
}

// y first, in std::atan2's order. Swapped, the answers at (1, −1) and (−1, 1)
// would trade places.
TEST(DagOpsTests, Atan2TakesYThenX) {
    auto y = Input<double>::make("y", 1.0);
    auto x = Input<double>::make("x", 1.0);
    auto angle = ops::Atan2Node<double>::make("angle", y, x);

    struct Case { double y, x, angle; };
    for (const Case& c : {Case{1.0, 1.0, kPi / 4}, Case{1.0, -1.0, 3 * kPi / 4},
                          Case{-1.0, -1.0, -3 * kPi / 4}, Case{-1.0, 1.0, -kPi / 4}}) {
        y->set(c.y);
        x->set(c.x);
        EXPECT_DOUBLE_EQ(evalAs<double>(angle), c.angle) << "at y = " << c.y << ", x = " << c.x;
    }
}

TEST(DagOpsTests, AsinAndAcosOutsideTheirDomainAreNan) {
    auto a = Input<double>::make("a", 1.5);
    auto asinNode = ops::AsinNode<double>::make("asin", a);
    auto acosNode = ops::AcosNode<double>::make("acos", a);
    EXPECT_TRUE(std::isnan(evalAs<double>(asinNode)));
    EXPECT_TRUE(std::isnan(evalAs<double>(acosNode)));

    a->set(-1.5);
    EXPECT_TRUE(std::isnan(evalAs<double>(asinNode)));
    EXPECT_TRUE(std::isnan(evalAs<double>(acosNode)));

    // The edges themselves are inside.
    a->set(1.0);
    EXPECT_DOUBLE_EQ(evalAs<double>(asinNode), kPi / 2);
    EXPECT_EQ(evalAs<double>(acosNode), 0.0);
    a->set(-1.0);
    EXPECT_DOUBLE_EQ(evalAs<double>(asinNode), -kPi / 2);
    EXPECT_DOUBLE_EQ(evalAs<double>(acosNode), kPi);
}

TEST(DagOpsTests, SinCosTanOfInfinityAreNan) {
    const double inf = std::numeric_limits<double>::infinity();
    auto a = Input<double>::make("a", inf);
    auto sinNode = ops::SinNode<double>::make("sin", a);
    auto cosNode = ops::CosNode<double>::make("cos", a);
    auto tanNode = ops::TanNode<double>::make("tan", a);
    for (const double x : {inf, -inf}) {
        a->set(x);
        EXPECT_TRUE(std::isnan(evalAs<double>(sinNode))) << "at " << x;
        EXPECT_TRUE(std::isnan(evalAs<double>(cosNode))) << "at " << x;
        EXPECT_TRUE(std::isnan(evalAs<double>(tanNode))) << "at " << x;
    }
}

// π/2 is not a double, so no input is a pole: next to it, tan is large but
// finite.
TEST(DagOpsTests, TanIsFiniteAtTheDoubleNearestHalfPi) {
    auto a = Input<double>::make("a", kPi / 2);
    auto tanNode = ops::TanNode<double>::make("tan", a);
    const double t = evalAs<double>(tanNode);
    EXPECT_TRUE(std::isfinite(t));
    EXPECT_GT(t, 1.6e16);
}

TEST(DagOpsTests, AsinSinRoundTrip) {
    auto a = Input<double>::make("a", 0.7);
    auto sinNode = ops::SinNode<double>::make("sin", a);
    auto asinNode = ops::AsinNode<double>::make("asin", sinNode);
    EXPECT_NEAR(evalAs<double>(asinNode), 0.7, 1e-15); // asin(sin(x)) == x for |x| <= π/2
}

// The second quadrant, which the atan of a ratio cannot reach.
TEST(DagOpsTests, Atan2SinCosRoundTrip) {
    auto a = Input<double>::make("a", 2.5);
    auto sinNode = ops::SinNode<double>::make("sin", a);
    auto cosNode = ops::CosNode<double>::make("cos", a);
    auto angle = ops::Atan2Node<double>::make("angle", sinNode, cosNode);
    EXPECT_NEAR(evalAs<double>(angle), 2.5, 1e-15);
}

TEST(DagOpsTests, TrigOpsWorkAtFloat) {
    auto a = Input<float>::make("a", 0.5f);
    auto b = Input<float>::make("b", -2.0f);
    auto sinNode = ops::SinNode<float>::make("sin", a);
    auto angle = ops::Atan2Node<float>::make("angle", a, b);
    EXPECT_FLOAT_EQ(evalAs<float>(sinNode), std::sin(0.5f));
    EXPECT_FLOAT_EQ(evalAs<float>(angle), std::atan2(0.5f, -2.0f));
}

// ─────────────────────────────────────────────────────────────────────────────
// Cross-op composition + inputs() round-trip
// ─────────────────────────────────────────────────────────────────────────────

TEST(DagOpsTests, ChainDiffDivideNegate) {
    auto a = Input<double>::make("a", 10.0);
    auto b = Input<double>::make("b", 4.0);
    auto c = Input<double>::make("c", 3.0);

    auto diff = ops::DiffNode<double>::make("diff", a, b);          // a-b = 6
    auto divide = ops::DivideNode<double>::make("divide", diff, c); // (a-b)/c = 2
    auto negate = ops::NegateNode<double>::make("negate", divide);  // -(a-b)/c = -2

    EXPECT_EQ(evalAs<double>(negate), -2.0);

    b->set(0.0); // a-b=10, /c=10/3, negate=-10/3
    EXPECT_DOUBLE_EQ(evalAs<double>(negate), -10.0 / 3.0);
}

TEST(DagOpsTests, ExpLnRoundTrip) {
    auto a = Input<double>::make("a", 3.0);
    auto expNode = ops::ExpNode<double>::make("exp", a);
    auto ln = ops::LnNode<double>::make("ln", expNode);
    EXPECT_DOUBLE_EQ(evalAs<double>(ln), 3.0); // ln(exp(x)) == x
}

TEST(DagOpsTests, SqrtPowerRoundTrip) {
    auto a = Input<double>::make("a", 7.0);
    auto sqrtNode = ops::SqrtNode<double>::make("sqrt", a);
    auto two = Input<double>::make("two", 2.0);
    auto power = ops::PowerNode<double>::make("power", sqrtNode, two);
    EXPECT_DOUBLE_EQ(evalAs<double>(power), 7.0); // sqrt(x)^2 == x
}

TEST(DagOpsTests, AllOpsInputsReturnUpstreamNodes) {
    auto a = Input<double>::make("a", 1.0);
    auto b = Input<double>::make("b", 2.0);
    auto c = Input<double>::make("c", 3.0);

    std::vector<NodePtr> ab{a, b};
    std::vector<NodePtr> abc{a, b, c};
    std::vector<NodePtr> aOnly{a};

    auto sum = ops::SumNode<double>::make("sum", {a, b, c});
    EXPECT_EQ(sum->inputs(), abc);

    auto product = ops::ProductNode<double>::make("product", {a, b, c});
    EXPECT_EQ(product->inputs(), abc);

    auto diff = ops::DiffNode<double>::make("diff", a, b);
    EXPECT_EQ(diff->inputs(), ab);

    auto divide = ops::DivideNode<double>::make("divide", a, b);
    EXPECT_EQ(divide->inputs(), ab);

    auto negate = ops::NegateNode<double>::make("negate", a);
    EXPECT_EQ(negate->inputs(), aOnly);

    auto expNode = ops::ExpNode<double>::make("exp", a);
    EXPECT_EQ(expNode->inputs(), aOnly);

    auto ln = ops::LnNode<double>::make("ln", a);
    EXPECT_EQ(ln->inputs(), aOnly);

    auto power = ops::PowerNode<double>::make("power", a, b);
    EXPECT_EQ(power->inputs(), ab);

    auto sqrtNode = ops::SqrtNode<double>::make("sqrt", a);
    EXPECT_EQ(sqrtNode->inputs(), aOnly);

    auto sinNode = ops::SinNode<double>::make("sin", a);
    EXPECT_EQ(sinNode->inputs(), aOnly);

    auto cosNode = ops::CosNode<double>::make("cos", a);
    EXPECT_EQ(cosNode->inputs(), aOnly);

    auto tanNode = ops::TanNode<double>::make("tan", a);
    EXPECT_EQ(tanNode->inputs(), aOnly);

    auto asinNode = ops::AsinNode<double>::make("asin", a);
    EXPECT_EQ(asinNode->inputs(), aOnly);

    auto acosNode = ops::AcosNode<double>::make("acos", a);
    EXPECT_EQ(acosNode->inputs(), aOnly);

    auto atanNode = ops::AtanNode<double>::make("atan", a);
    EXPECT_EQ(atanNode->inputs(), aOnly);

    auto atan2Node = ops::Atan2Node<double>::make("atan2", a, b);
    EXPECT_EQ(atan2Node->inputs(), ab);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
