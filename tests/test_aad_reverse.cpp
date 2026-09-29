// =============================================================================
// flywheel-dag — A header-only C++17 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

// Recording a tape and sweeping it in reverse (flywheel-dag#10).

#include <gtest/gtest.h>
#include "flywheel/dag.hpp"
#include "flywheel/dag_aad.hpp"
#include "flywheel/dag_engine.hpp"
#include "flywheel/dag_ops.hpp"
#include "flywheel/dag_timeseries.hpp"
#include <algorithm>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace dag;

namespace {

void expectClose(double actual, double expected) {
    EXPECT_NEAR(actual, expected, 1e-13 * std::max(1.0, std::abs(expected)));
}

// Compares each adjoint of `root` with a central difference, bumping one input
// at a time through set() and evaluating the root again.
void expectCentralDifferences(const NodePtr& root, const std::vector<InputPtr<double>>& ins) {
    EvalContext ctx;
    root->eval(ctx);
    const std::vector<NodePtr> wrt(ins.begin(), ins.end());
    const std::vector<double> adj = aad::adjoints(root, wrt);
    for (std::size_t i = 0; i < ins.size(); ++i) {
        const double x  = ins[i]->get();
        const double h  = 1e-6 * std::max(1.0, std::abs(x));
        const double up = x + h;
        const double dn = x - h;
        ins[i]->set(up);
        const double fUp = get_value<double>(root->eval(ctx));
        ins[i]->set(dn);
        const double fDn = get_value<double>(root->eval(ctx));
        ins[i]->set(x);
        root->eval(ctx);
        const double fd = (fUp - fDn) / (up - dn);
        EXPECT_NEAR(adj[i], fd, 1e-6 * std::max(1.0, std::abs(fd)))
            << root->name() << ", input " << i;
    }
}

// x·y + exp(x)/y
struct SumOfTerms {
    InputPtr<double> x = Input<double>::make("x", 1.3);
    InputPtr<double> y = Input<double>::make("y", 0.7);
    NodePtr root = ops::SumNode<>::make("root", {
        ops::ProductNode<>::make("xy", {x, y}),
        ops::DivideNode<>::make("exp/y", ops::ExpNode<>::make("exp", x), y)});
};

// ln(x)·√y
struct LogTimesRoot {
    InputPtr<double> x = Input<double>::make("x", 2.5);
    InputPtr<double> y = Input<double>::make("y", 3.0);
    NodePtr root = ops::ProductNode<>::make("root", {
        ops::LnNode<>::make("ln", x), ops::SqrtNode<>::make("sqrt", y)});
};

// (x − y)^z
struct PowerOfDifference {
    InputPtr<double> x = Input<double>::make("x", 3.5);
    InputPtr<double> y = Input<double>::make("y", 1.25);
    InputPtr<double> z = Input<double>::make("z", 1.8);
    NodePtr root = ops::PowerNode<>::make("root", ops::DiffNode<>::make("x-y", x, y), z);
};

// A node that turns a float into a double and says so, with a partial of 1. No
// node in the engine puts a non-double on a tape, but an application's could.
class Widen : public NodeBase, public aad::IDifferentiable,
              public std::enable_shared_from_this<Widen> {
public:
    static std::shared_ptr<Widen> make(NodePtr in) {
        auto self = std::shared_ptr<Widen>(new Widen(std::move(in)));
        wire(self, self->inputs());
        return self;
    }
    ValuePtr eval(EvalContext& ctx) override {
        if (!dirty() && !ctx.forceRecompute) return cached_;
        const ValuePtr v = in_->eval(ctx);
        cached_ = make_value<double>(get_value<float>(v));
        notifyDownstream();
        markClean();
        return cached_;
    }
    std::string name() const override { return "widen"; }
    std::vector<NodePtr> inputs() const override { return {in_}; }
    NodeKind kind() const override { return NodeKind::Compute; }
    bool partials(EvalContext&, aad::Partials& out) override {
        out.add(0, 1.0);
        return true;
    }

private:
    explicit Widen(NodePtr in) : in_(std::move(in)) {}
    NodePtr  in_;
    ValuePtr cached_;
};

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Values
// ─────────────────────────────────────────────────────────────────────────────

TEST(AadReverse, MatchesAnalyticGradients) {
    EvalContext ctx;

    SumOfTerms s;
    s.root->eval(ctx);
    std::vector<double> g = aad::adjoints(s.root, {s.x, s.y});
    const double x = 1.3, y = 0.7;
    expectClose(g[0], y + std::exp(x) / y);
    expectClose(g[1], x - std::exp(x) / (y * y));

    LogTimesRoot l;
    l.root->eval(ctx);
    g = aad::adjoints(l.root, {l.x, l.y});
    expectClose(g[0], std::sqrt(3.0) / 2.5);
    expectClose(g[1], std::log(2.5) / (2.0 * std::sqrt(3.0)));

    PowerOfDifference p;
    p.root->eval(ctx);
    g = aad::adjoints(p.root, {p.x, p.y, p.z});
    const double d = 3.5 - 1.25, z = 1.8;
    expectClose(g[0], z * std::pow(d, z - 1.0));
    expectClose(g[1], -z * std::pow(d, z - 1.0));
    expectClose(g[2], std::pow(d, z) * std::log(d));
}

TEST(AadReverse, MatchesCentralDifferences) {
    SumOfTerms s;
    expectCentralDifferences(s.root, {s.x, s.y});
    LogTimesRoot l;
    expectCentralDifferences(l.root, {l.x, l.y});
    PowerOfDifference p;
    expectCentralDifferences(p.root, {p.x, p.y, p.z});
}

// x·eˣ·eˣ: eˣ feeds the product twice, and x feeds it directly and through eˣ.
TEST(AadReverse, SharedNodesAccumulate) {
    auto x    = Input<double>::make("x", 0.8);
    auto ex   = ops::ExpNode<>::make("exp", x);
    auto root = ops::ProductNode<>::make("root", {x, ex, ex});
    EvalContext ctx;
    root->eval(ctx);
    expectClose(aad::adjoints(root, {x})[0], std::exp(1.6) * (1.0 + 2.0 * 0.8));
}

TEST(AadReverse, RecordsEachNodeOnce) {
    auto x    = Input<double>::make("x", 4.0);
    auto root = ops::ProductNode<>::make("root", {
        ops::ExpNode<>::make("exp", x), ops::SqrtNode<>::make("sqrt", x)});
    EvalContext ctx;
    root->eval(ctx);
    EXPECT_EQ(aad::Tape({root}).size(), 4u);
}

TEST(AadReverse, AnIntermediateNodeCanBeWrt) {
    auto x    = Input<double>::make("x", 0.8);
    auto ex   = ops::ExpNode<>::make("exp", x);
    auto root = ops::ProductNode<>::make("root", {x, ex, ex});
    EvalContext ctx;
    root->eval(ctx);
    expectClose(aad::adjoints(root, {ex})[0], 2.0 * 0.8 * std::exp(0.8));
}

TEST(AadReverse, AnUnreachedNodeHasDerivativeZero) {
    auto cond  = Input<bool>::make("cond", true);
    auto x     = Input<double>::make("x", 2.0);
    auto y     = Input<double>::make("y", 3.0);
    auto other = Input<double>::make("other", 5.0);
    auto sel   = ConditionNode::make("sel", cond, x, y);
    EvalContext ctx;
    sel->eval(ctx);
    const std::vector<double> g = aad::adjoints(sel, {x, y, other});
    EXPECT_EQ(g[0], 1.0);
    EXPECT_EQ(g[1], 0.0);
    EXPECT_EQ(g[2], 0.0);
}

TEST(AadReverse, ARootCanBeALeaf) {
    auto x = Input<double>::make("x", 2.0);
    EvalContext ctx;
    x->eval(ctx);
    EXPECT_EQ(aad::adjoints(x, {x})[0], 1.0);
    EXPECT_EQ(aad::Tape({x}).size(), 1u);
}

// ─────────────────────────────────────────────────────────────────────────────
// Rejections
// ─────────────────────────────────────────────────────────────────────────────

TEST(AadReverse, RejectsADirtyRoot) {
    SumOfTerms s;
    EvalContext ctx;
    s.root->eval(ctx);
    s.x->set(2.0);
    EXPECT_THROW(aad::adjoints(s.root, {s.x}), std::invalid_argument);
    EXPECT_THROW(aad::adjoints(nullptr, {s.x}), std::invalid_argument);
}

TEST(AadReverse, RejectsARootOrWrtThatIsNotADouble) {
    auto i = Input<int>::make("i", 2);
    EvalContext ctx;
    i->eval(ctx);
    EXPECT_THROW(aad::adjoints(i, {i}), std::invalid_argument);

    auto f = Input<float>::make("f", 1.5f);
    auto w = Widen::make(f);
    w->eval(ctx);
    EXPECT_EQ(aad::adjoints(w, {w})[0], 1.0);
    EXPECT_THROW(aad::adjoints(w, {f}), std::invalid_argument);
    EXPECT_THROW(aad::adjoints(w, {nullptr}), std::invalid_argument);
}

TEST(AadReverse, RejectsARootThatIsNotOneOfTheTapes) {
    SumOfTerms s;
    EvalContext ctx;
    s.root->eval(ctx);
    const aad::Tape tape({s.root});
    EXPECT_THROW(tape.adjoints(s.x, {s.x}), std::invalid_argument);
}

// ─────────────────────────────────────────────────────────────────────────────
// Barriers
// ─────────────────────────────────────────────────────────────────────────────

TEST(AadReverse, ABarrierWithAWrtUpstreamThrows) {
    auto x      = Input<double>::make("x", 2.0);
    auto y      = Input<double>::make("y", 5.0);
    auto opaque = ComputeNode<double, double>::make("opaque", {x},
        [](const double& v) { return v * v; });
    auto root   = ops::ProductNode<>::make("root", {opaque, y});
    EvalContext ctx;
    root->eval(ctx);
    try {
        aad::adjoints(root, {x});
        FAIL() << "expected std::domain_error";
    } catch (const std::domain_error& e) {
        const std::string what = e.what();
        EXPECT_NE(what.find("x reaches"), std::string::npos) << what;
        EXPECT_NE(what.find("through opaque"), std::string::npos) << what;
    }
}

TEST(AadReverse, ABarrierIsAConstantOrAWrt) {
    auto x      = Input<double>::make("x", 2.0);
    auto y      = Input<double>::make("y", 5.0);
    auto opaque = ComputeNode<double, double>::make("opaque", {x},
        [](const double& v) { return v * v; });
    auto root   = ops::ProductNode<>::make("root", {opaque, y});
    EvalContext ctx;
    root->eval(ctx);
    const std::vector<double> g = aad::adjoints(root, {y, opaque});
    EXPECT_EQ(g[0], 4.0);
    EXPECT_EQ(g[1], 5.0);
}

TEST(AadReverse, OnlyBarriersUpstreamOfTheRootCount) {
    auto x      = Input<double>::make("x", 2.0);
    auto y      = Input<double>::make("y", 5.0);
    auto opaque = ComputeNode<double, double>::make("opaque", {x},
        [](const double& v) { return v * v; });
    auto plain  = ops::ProductNode<>::make("plain", {x, y});
    auto shaded = ops::ProductNode<>::make("shaded", {opaque, y});
    EvalContext ctx;
    plain->eval(ctx);
    shaded->eval(ctx);
    const aad::Tape tape({plain, shaded});
    EXPECT_EQ(tape.adjoints(plain, {x})[0], 5.0);
    EXPECT_THROW(tape.adjoints(shaded, {x}), std::domain_error);
}

// ─────────────────────────────────────────────────────────────────────────────
// Zeros and NaNs
// ─────────────────────────────────────────────────────────────────────────────

// z·√x at z = 0 and x = 0. The product passes √x an adjoint of 0, and √x's
// partial is ∞, so 0·∞ would make ∂/∂x a NaN. z·√x is 0 for every x while z is
// 0, so the answer is 0.
TEST(AadReverse, AZeroAdjointPropagatesNothing) {
    auto z    = Input<double>::make("z", 0.0);
    auto x    = Input<double>::make("x", 0.0);
    auto root = ops::ProductNode<>::make("root", {z, ops::SqrtNode<>::make("sqrt", x)});
    EvalContext ctx;
    root->eval(ctx);
    const std::vector<double> g = aad::adjoints(root, {x, z});
    EXPECT_EQ(g[0], 0.0);
    EXPECT_EQ(g[1], 0.0);
}

TEST(AadReverse, ATrueNaNIsKept) {
    auto a = Input<double>::make("a", -2.0);
    auto n = Input<double>::make("n", 3.0);
    auto p = ops::PowerNode<>::make("pow", a, n);
    EvalContext ctx;
    p->eval(ctx);
    const std::vector<double> g = aad::adjoints(p, {a, n});
    EXPECT_EQ(g[0], 12.0);
    EXPECT_TRUE(std::isnan(g[1]));
}

// ─────────────────────────────────────────────────────────────────────────────
// A pass leaves the graph alone
// ─────────────────────────────────────────────────────────────────────────────

TEST(AadReverse, APassEvaluatesNothing) {
    auto x    = Input<double>::make("x", 2.0);
    auto y    = Input<double>::make("y", 3.0);
    auto cond = Input<bool>::make("cond", true);
    int evals = 0;
    auto counted = ComputeNode<double, double>::make("counted", {y},
        [&evals](const double& v) { ++evals; return v + 1.0; });
    auto ex   = ops::ExpNode<>::make("exp", x);
    auto sel  = ConditionNode::make("sel", cond, ex, counted);
    auto root = ops::ProductNode<>::make("root", {sel, counted, x});
    const std::vector<NodePtr> nodes = {x, y, cond, counted, ex, sel, root};

    EvalContext ctx;
    root->eval(ctx);
    std::vector<ValuePtr> before;
    before.reserve(nodes.size());
    for (const auto& n : nodes) before.push_back(n->eval(ctx));
    const int evalsBefore = evals;

    const aad::Tape tape({root});
    tape.adjoints(root, {x, counted});

    for (std::size_t i = 0; i < nodes.size(); ++i) {
        EXPECT_FALSE(nodes[i]->dirty()) << nodes[i]->name();
        EXPECT_EQ(nodes[i]->eval(ctx).get(), before[i].get()) << nodes[i]->name();
    }
    EXPECT_EQ(evals, evalsBefore);
}

// An EWMANode ticks each time it evaluates, so a pass that evaluated one would
// move it. As a wrt node it is a barrier with nothing asked for above it.
TEST(AadReverse, APassDoesNotAdvanceAStatefulNode) {
    auto x       = Input<double>::make("x", 1.0);
    auto ewma    = ts::EWMANode::make("ewma", x, 0.5);
    auto k       = Input<double>::make("k", 3.0);
    auto root    = ops::ProductNode<>::make("root", {ewma, k});
    auto xCtl    = Input<double>::make("xCtl", 1.0);
    auto control = ts::EWMANode::make("control", xCtl, 0.5);

    EvalContext ctx;
    for (const double v : {1.0, 2.0, 4.0, 8.0}) {
        x->set(v);
        xCtl->set(v);
        root->eval(ctx);
        control->eval(ctx);
        const double e = get_value<double>(ewma->eval(ctx));
        const std::vector<double> g = aad::adjoints(root, {ewma, k});
        EXPECT_EQ(g[0], 3.0);
        EXPECT_EQ(g[1], e);
        EXPECT_EQ(get_value<double>(ewma->eval(ctx)), get_value<double>(control->eval(ctx)));
    }
    EXPECT_THROW(aad::adjoints(root, {x}), std::domain_error);
}

TEST(AadReverse, InsideAnEngineCallback) {
    async::Engine engine;
    auto s = async::AsyncInput<double>::make("s", 1.0);
    engine.addSource(s);
    auto sq = ops::ProductNode<>::make("sq", {s, s});
    std::vector<double> seen;
    engine.addOutput<double>(sq, [&](const double&) {
        seen.push_back(aad::adjoints(sq, {s})[0]);
    });
    for (const double v : {1.0, 2.0, 5.0}) {
        s->post(v);
        engine.step();
    }
    ASSERT_EQ(seen.size(), 3u);
    EXPECT_EQ(seen[0], 2.0);
    EXPECT_EQ(seen[1], 4.0);
    EXPECT_EQ(seen[2], 10.0);
}

// ─────────────────────────────────────────────────────────────────────────────
// One tape, many sweeps
// ─────────────────────────────────────────────────────────────────────────────

TEST(AadReverse, ATapeSweepsManyTimes) {
    SumOfTerms s;
    LogTimesRoot l;
    auto both = ops::ProductNode<>::make("both", {s.root, l.root});
    EvalContext ctx;
    both->eval(ctx);

    const aad::Tape tape({s.root, both});
    const std::vector<NodePtr> wrt = {s.x, s.y, l.x, l.y};
    const std::vector<double> gs = tape.adjoints(s.root, wrt);
    const std::vector<double> gb = tape.adjoints(both, wrt);
    const std::vector<double> gsAlone = aad::adjoints(s.root, wrt);
    const std::vector<double> gbAlone = aad::adjoints(both, wrt);
    for (std::size_t i = 0; i < wrt.size(); ++i) {
        expectClose(gs[i], gsAlone[i]);
        expectClose(gb[i], gbAlone[i]);
    }
    EXPECT_EQ(gs[2], 0.0);
    EXPECT_EQ(gs[3], 0.0);
    EXPECT_EQ(tape.adjoints(both, wrt), gb);   // a sweep reads nothing new
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
