// =============================================================================
// flywheel-dag — A header-only C++17 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

// DifferentiableNode<N>: a compute node whose functor a tape can differentiate,
// through dual numbers.
//
// The allocation counter must be defined before the flywheel headers.

#include <atomic>
#include <cstdlib>
#include <new>

namespace {
std::atomic<long> g_allocs{0};
bool              g_counting = false;
}  // namespace

void* operator new(std::size_t n) {
    if (g_counting) g_allocs.fetch_add(1, std::memory_order_relaxed);
    void* p = std::malloc(n);
    if (!p) throw std::bad_alloc();
    return p;
}
// GCC bug 103993 (https://gcc.gnu.org/PR103993): inlined into a caller, these
// show GCC free() given memory from operator new, and -Wmismatched-new-delete
// fires. It does not look through the replacement above to its malloc(). The
// pairing is correct, so the warning is off here, for GCC (11 on) only.
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 11
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif
void operator delete(void* p) noexcept              { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 11
#pragma GCC diagnostic pop
#endif

#include <gtest/gtest.h>
#include "aad_test_graphs.hpp"
#include "flywheel/dag.hpp"
#include "flywheel/dag_aad.hpp"
#include "flywheel/dag_ops.hpp"
#include <cmath>
#include <memory>
#include <type_traits>
#include <vector>

using namespace dag;
using aad_test::expectClose;

namespace {

/// Counts allocations across fn(), which must not itself allocate for reasons
/// unrelated to the graph (no string building, no EXPECT inside).
template <typename Fn>
long allocationsDuring(Fn&& fn) {
    g_allocs   = 0;
    g_counting = true;
    fn();
    g_counting = false;
    return g_allocs.load();
}

// M_PI is POSIX, not standard C++.
const double kPi = std::acos(-1.0);

double normPdf(double x) { return std::exp(-0.5 * x * x) / std::sqrt(2.0 * kPi); }
double normCdf(double x) { return 0.5 * std::erfc(-x / std::sqrt(2.0)); }

// A Black–Scholes call, from ops and differentiable nodes: d1 is one
// differentiable node over all five inputs, d2 is ops over d1, and each normal
// CDF is a differentiable node over one input.
struct CallPrice {
    InputPtr<double> spot   = Input<double>::make("spot", 100.0);
    InputPtr<double> strike = Input<double>::make("strike", 95.0);
    InputPtr<double> vol    = Input<double>::make("vol", 0.25);
    InputPtr<double> expiry = Input<double>::make("expiry", 0.75);
    InputPtr<double> rate   = Input<double>::make("rate", 0.03);
    NodePtr call;
    NodePtr delta;   ///< N(d1)

    CallPrice() {
        auto d1 = aad::DifferentiableNode<5>::make("d1", {spot, strike, vol, expiry, rate},
            [](const auto& s, const auto& k, const auto& v, const auto& t, const auto& r) {
                using std::log; using std::sqrt;
                return (log(s / k) + (r + 0.5 * v * v) * t) / (v * sqrt(t));
            },
            InvalidationMode::Lazy);
        auto volRootT = ops::ProductNode<>::make("vol.sqrt(expiry)",
            {vol, ops::SqrtNode<>::make("sqrt(expiry)", expiry)});
        auto d2 = ops::DiffNode<>::make("d2", d1, volRootT);
        const auto cdf = [](const auto& x) {
            using std::erfc; using std::sqrt;
            return 0.5 * erfc(-x / sqrt(2.0));
        };
        auto nd1 = aad::DifferentiableNode<1>::make("N(d1)", {d1}, cdf, InvalidationMode::Lazy);
        auto nd2 = aad::DifferentiableNode<1>::make("N(d2)", {d2}, cdf, InvalidationMode::Lazy);
        auto discount = ops::ExpNode<>::make("discount", ops::NegateNode<>::make("-rate.expiry",
            ops::ProductNode<>::make("rate.expiry", {rate, expiry})));
        call = ops::DiffNode<>::make("call",
            ops::ProductNode<>::make("spot.N(d1)", {spot, nd1}),
            ops::ProductNode<>::make("strike.discount.N(d2)", {strike, discount, nd2}));
        delta = nd1;
    }

    std::vector<NodePtr> inputs() const { return {spot, strike, vol, expiry, rate}; }
};

// The closed form, and its derivatives with respect to spot, strike, vol,
// expiry and rate, in that order.
struct Greeks {
    double price;
    std::vector<double> sensitivities;
};

Greeks closedForm(double s, double k, double v, double t, double r) {
    const double d1 = (std::log(s / k) + (r + 0.5 * v * v) * t) / (v * std::sqrt(t));
    const double d2 = d1 - v * std::sqrt(t);
    const double df = std::exp(-r * t);
    return {s * normCdf(d1) - k * df * normCdf(d2),
            {normCdf(d1),
             -df * normCdf(d2),
             s * normPdf(d1) * std::sqrt(t),
             s * normPdf(d1) * v / (2.0 * std::sqrt(t)) + r * k * df * normCdf(d2),
             k * t * df * normCdf(d2)}};
}

// N(d1)'s derivatives with respect to the same five: φ(d1)·∂d1/∂x.
std::vector<double> deltaSensitivities(double s, double k, double v, double t, double r) {
    const double rootT = std::sqrt(t);
    const double d1 = (std::log(s / k) + (r + 0.5 * v * v) * t) / (v * rootT);
    const double d2 = d1 - v * rootT;
    const double pdf = normPdf(d1);
    return {pdf / (s * v * rootT),
            -pdf / (k * v * rootT),
            -pdf * d2 / v,
            pdf * ((r + 0.5 * v * v) / (v * rootT) - d1 / (2.0 * t)),
            pdf * rootT / v};
}

void expectWithin(double actual, double expected, double relative) {
    EXPECT_NEAR(actual, expected, relative * std::max(1.0, std::abs(expected)));
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// As a compute node
// ─────────────────────────────────────────────────────────────────────────────

TEST(AadNode, EvaluatesLikeAComputeNode) {
    auto x = Input<double>::make("x", 2.0);
    auto y = Input<double>::make("y", 3.0);
    auto f = aad::DifferentiableNode<2>::make("f", {x, y},
        [](const auto& a, const auto& b) { using std::exp; return a * exp(b); });
    EvalContext ctx;
    EXPECT_EQ(get_value<double>(f->eval(ctx)), 2.0 * std::exp(3.0));
    x->set(4.0);
    EXPECT_TRUE(f->dirty());
    EXPECT_EQ(get_value<double>(f->eval(ctx)), 4.0 * std::exp(3.0));
    EXPECT_EQ(f->kind(), NodeKind::Compute);
    EXPECT_EQ(f->inputs().size(), 2u);
    EXPECT_EQ(f->name(), "f");

    // An equality policy gates as a ComputeNode's does: a move inside the
    // tolerance keeps the published value.
    auto g = aad::DifferentiableNode<1>::make("g", {x},
        [](const auto& a) { return 2.0 * a; }, std::make_shared<EpsilonPolicy<double>>(0.5));
    const ValuePtr before = g->eval(ctx);
    x->set(4.1);
    EXPECT_EQ(g->eval(ctx).get(), before.get());
    x->set(5.0);
    EXPECT_NE(g->eval(ctx).get(), before.get());
}

// `flat` does not move when x does, so the Lazy consumer skips its functor and
// the Eager one runs it.
TEST(AadNode, LazySkipsAndEagerReruns) {
    auto x    = Input<double>::make("x", 2.0);
    auto zero = Input<double>::make("zero", 0.0);
    auto flat = ops::ProductNode<>::make("flat", {x, zero});
    int lazyCalls = 0, eagerCalls = 0;
    auto lazy = aad::DifferentiableNode<1>::make("lazy", {flat},
        [&lazyCalls](const auto& v) { ++lazyCalls; return v + 1.0; }, InvalidationMode::Lazy);
    auto eager = aad::DifferentiableNode<1>::make("eager", {flat},
        [&eagerCalls](const auto& v) { ++eagerCalls; return v + 1.0; },
        nullptr, InvalidationMode::Eager);
    EvalContext ctx;
    lazy->eval(ctx);
    eager->eval(ctx);
    EXPECT_EQ(lazyCalls, 1);
    EXPECT_EQ(eagerCalls, 1);

    x->set(3.0);
    lazy->eval(ctx);
    eager->eval(ctx);
    EXPECT_EQ(lazyCalls, 1);
    EXPECT_EQ(eagerCalls, 2);
}

TEST(AadNode, SteadyStateAllocatesNothing) {
    auto x = Input<double>::make("x", 1.0);
    auto y = Input<double>::make("y", 2.0);
    auto f = aad::DifferentiableNode<2>::make("f", {x, y},
        [](const auto& a, const auto& b) { using std::sqrt; return sqrt(a * a + b * b); });
    EvalContext ctx;
    for (const double v : {1.5, 2.0, 2.5}) {   // warm both of the slot's buffers
        x->set(v);
        f->eval(ctx);
    }
    long allocs = 0;
    for (int i = 0; i < 100; ++i) {
        x->set(1.0 + 0.25 * i);   // Input::set allocates, by design, so it is not counted
        allocs += allocationsDuring([&] { f->eval(ctx); });
    }
    EXPECT_EQ(allocs, 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// Its partials
// ─────────────────────────────────────────────────────────────────────────────

TEST(AadNode, PartialsComeFromOneDualCall) {
    auto x = Input<double>::make("x", 1.5);
    auto y = Input<double>::make("y", 0.4);
    auto z = Input<double>::make("z", 2.5);
    int doubles = 0, duals = 0;
    auto f = aad::DifferentiableNode<3>::make("f", {x, y, z},
        [&doubles, &duals](const auto& a, const auto& b, const auto& c) {
            using std::exp;
            if constexpr (std::is_same_v<std::decay_t<decltype(a)>, double>) ++doubles;
            else ++duals;
            return a * exp(b) / c;
        });
    EvalContext ctx;
    f->eval(ctx);
    EXPECT_EQ(doubles, 1);
    EXPECT_EQ(duals, 0);

    const std::vector<double> g = aad::adjoints(f, {x, y, z});
    EXPECT_EQ(doubles, 1);
    EXPECT_EQ(duals, 1);
    expectClose(g[0], std::exp(0.4) / 2.5);
    expectClose(g[1], 1.5 * std::exp(0.4) / 2.5);
    expectClose(g[2], -1.5 * std::exp(0.4) / (2.5 * 2.5));

    aad::Partials p;
    ASSERT_TRUE(f->partials(ctx, p));
    EXPECT_EQ(p.entries().size(), 3u);
    EXPECT_EQ(doubles, 1);
    EXPECT_EQ(duals, 2);
}

// A closed-form formula, built from ops and differentiable nodes, differentiated
// in both modes.
TEST(AadNode, ACallPriceMatchesItsClosedFormGreeks) {
    const CallPrice c;
    EvalContext ctx;
    const double price = get_value<double>(c.call->eval(ctx));
    const Greeks g = closedForm(100.0, 95.0, 0.25, 0.75, 0.03);
    expectWithin(price, g.price, 1e-12);

    const aad::Tape tape({c.call});
    const std::vector<NodePtr> ins = c.inputs();
    const std::vector<double> adj = tape.adjoints(c.call, ins);
    for (std::size_t i = 0; i < ins.size(); ++i) {
        SCOPED_TRACE(ins[i]->name());
        expectWithin(adj[i], g.sensitivities[i], 1e-11);
        const double tangent = tape.tangents({{ins[i], 1.0}})[0];
        expectWithin(tangent, g.sensitivities[i], 1e-11);
        expectWithin(tangent, adj[i], 1e-13);
    }
}

// The price cannot see d1's partials: its derivative through d1 is 0, because
// S·φ(d1) = K·e^(−rT)·φ(d2). That identity is why delta is N(d1). N(d1) itself
// can: its derivative with respect to spot is gamma, φ(d1)·∂d1/∂spot, and every
// other one passes through d1's partials too.
TEST(AadNode, ItsDeltaMatchesItsClosedFormDerivatives) {
    const CallPrice c;
    EvalContext ctx;
    c.call->eval(ctx);
    const std::vector<double> expected = deltaSensitivities(100.0, 95.0, 0.25, 0.75, 0.03);

    const aad::Tape tape({c.call, c.delta});
    const std::vector<NodePtr> ins = c.inputs();
    const std::vector<double> adj = tape.adjoints(c.delta, ins);
    for (std::size_t i = 0; i < ins.size(); ++i) {
        SCOPED_TRACE(ins[i]->name());
        expectWithin(adj[i], expected[i], 1e-11);
        const std::vector<double> tangents = tape.tangents({{ins[i], 1.0}});
        expectWithin(tangents[1], expected[i], 1e-11);
        expectWithin(tangents[1], adj[i], 1e-13);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// The trigonometric functions
// ─────────────────────────────────────────────────────────────────────────────

// One formula over the seven, as a DifferentiableNode and as a graph of ops.
// They agree to 1e-13, not bit for bit: the compiler may fuse the lambda's
// arithmetic, which a chain of nodes cannot. The lambda calls the functions
// unqualified, with a double and an int on either side of atan2.
TEST(AadNode, ATrigFunctorAgreesWithItsOpGraph) {
    auto u     = Input<double>::make("u", 0.6);
    auto v     = Input<double>::make("v", 1.1);
    auto one   = Input<double>::make("one", 1.0);
    auto two   = Input<double>::make("two", 2.0);
    auto three = Input<double>::make("three", 3.0);

    const NodePtr fromOps = ops::SumNode<>::make("fromOps", {
        ops::ProductNode<>::make("atan2(v,u).sin(u)", {ops::Atan2Node<>::make("atan2(v,u)", v, u),
                                                       ops::SinNode<>::make("sin(u)", u)}),
        ops::CosNode<>::make("cos(v)", v),
        ops::NegateNode<>::make("-tan(u.v)",
            ops::TanNode<>::make("tan(u.v)", ops::ProductNode<>::make("u.v", {u, v}))),
        ops::ProductNode<>::make("asin(u/2).acos(v/3)", {
            ops::AsinNode<>::make("asin(u/2)", ops::DivideNode<>::make("u/2", u, two)),
            ops::AcosNode<>::make("acos(v/3)", ops::DivideNode<>::make("v/3", v, three))}),
        ops::AtanNode<>::make("atan(u)", u),
        ops::Atan2Node<>::make("atan2(v,1)", v, one),
        ops::Atan2Node<>::make("atan2(2,u)", two, u)});

    const NodePtr fromFunctor = aad::DifferentiableNode<2>::make("fromFunctor", {u, v},
        [](const auto& x, const auto& y) {
            using std::sin; using std::cos; using std::tan; using std::asin; using std::acos;
            using std::atan; using std::atan2;
            return atan2(y, x) * sin(x) + cos(y) - tan(x * y) + asin(x / 2.0) * acos(y / 3.0)
                   + atan(x) + atan2(y, 1.0) + atan2(2, x);
        },
        InvalidationMode::Lazy);

    EvalContext ctx;
    const ValuePtr opsValue     = fromOps->eval(ctx);
    const ValuePtr functorValue = fromFunctor->eval(ctx);
    expectWithin(get_value<double>(functorValue), get_value<double>(opsValue), 1e-13);

    const aad::Tape tape({fromOps, fromFunctor});
    const std::vector<NodePtr> ins = {u, v};
    const std::vector<double> opsAdj     = tape.adjoints(fromOps, ins);
    const std::vector<double> functorAdj = tape.adjoints(fromFunctor, ins);
    for (std::size_t i = 0; i < ins.size(); ++i) {
        SCOPED_TRACE(ins[i]->name());
        expectWithin(functorAdj[i], opsAdj[i], 1e-13);
        const std::vector<double> t = tape.tangents({{ins[i], 1.0}});
        expectWithin(t[0], opsAdj[i], 1e-13);
        expectWithin(t[1], opsAdj[i], 1e-13);
    }
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
