// =============================================================================
// flywheel-dag — A header-only C++23 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

// dag::ReductionNode, a node that owns a reduction and binds it each time it
// recomputes, and Reduction::argument().
//
// Allocations are counted by allocation_counter.cpp, which this suite links.

#include <gtest/gtest.h>
#include "allocation_counter.hpp"
#include "reduce_test_nodes.hpp"
#include "test_nodes.hpp"
#include "flywheel/dag.hpp"
#include "flywheel/dag_aad.hpp"
#include "flywheel/dag_async.hpp"
#include "flywheel/dag_engine.hpp"
#include "flywheel/dag_ops.hpp"
#include "flywheel/dag_reduce.hpp"
#include "flywheel/dag_timeseries.hpp"
#include <array>
#include <cmath>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace dag;
using allocation_counter::allocations_during;
using namespace reduce_test;

namespace {

/// Newton's method on f(vol) − target, from `start`.
double impliedVol(Reduction& f, double target, double start) {
    double vol = start, slope = 0.0;
    for (int i = 0; i < 20; ++i) {
        const double miss = f.gradient({vol}, {&slope, 1}) - target;
        if (std::abs(miss) < 1e-12) break;
        vol -= miss / slope;
    }
    return vol;
}

double valueOf(const NodePtr& node) {
    EvalContext ctx;
    const ValuePtr v = node->eval(ctx);
    return get_value<double>(v);
}

/// x > level, as a Lazy compute node that holds a bool.
NodePtr above(std::string name, const NodePtr& x, double level) {
    return ComputeNode<bool, double>::make(std::move(name), {x},
        [level](const double& a) { return a > level; }, InvalidationMode::Lazy);
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// In an engine
// ─────────────────────────────────────────────────────────────────────────────

// The vol at which the call meets the quote: the node's root is the price, its
// argument the vol, and its one other input the quote, which its functor is
// handed.
TEST(ReductionNode, ASolverInAnEngine) {
    async::Engine engine;
    auto spot  = async::AsyncInput<double>::make("spot", 100.0);
    auto quote = async::AsyncInput<double>::make("quote", 12.0);
    engine.addSource(spot);
    engine.addSource(quote);
    const CallPrice c(spot);

    int solves = 0;
    auto implied = ReductionNode<double, double>::make("implied vol", c.call, {c.vol}, {quote},
        [&solves](Reduction& f, const double& target) {
            ++solves;
            return impliedVol(f, target, 0.2);
        });
    std::vector<double> delivered;
    engine.addOutput<double>(implied, [&](const double& v) { delivered.push_back(v); });

    // The price at a vol, from a reduction of the test's own, bound between
    // cycles to what the last one left.
    Reduction price(c.call, {c.vol});
    const auto priceAt = [&price](double vol) {
        price.bind();
        return price({vol});
    };

    engine.step();
    ASSERT_EQ(delivered.size(), 1u);
    EXPECT_EQ(solves, 1);
    EXPECT_NEAR(priceAt(delivered[0]), 12.0, 1e-11);

    spot->post(101.0);   // the same price from a higher spot takes less vol
    engine.step();
    ASSERT_EQ(delivered.size(), 2u);
    EXPECT_EQ(solves, 2);
    EXPECT_LT(delivered[1], delivered[0]);
    EXPECT_NEAR(priceAt(delivered[1]), 12.0, 1e-11);

    quote->post(12.5);
    engine.step();
    ASSERT_EQ(delivered.size(), 3u);
    EXPECT_EQ(solves, 3);
    EXPECT_GT(delivered[2], delivered[1]);
    EXPECT_NEAR(priceAt(delivered[2]), 12.5, 1e-11);

    engine.step();   // nothing moved
    EXPECT_EQ(delivered.size(), 3u);
    EXPECT_EQ(solves, 3);
}

// a·x + b, and the x at which it is 0, by one Newton step from the x the graph
// holds, 2. Moving (a, b) from (2, −2) to (4, −6) leaves the root at 2 and moves
// its zero from 1 to 1.5. The node is Eager, so it solves again.
TEST(ReductionNode, SeesAPointThatMovesWhileTheRootStandsStill) {
    async::Engine engine;
    auto a = async::AsyncInput<double>::make("a", 2.0);
    auto b = async::AsyncInput<double>::make("b", -2.0);
    engine.addSource(a);
    engine.addSource(b);
    auto x    = Input<double>::make("x", 2.0);
    auto root = ops::SumNode<>::make("a.x+b", {ops::ProductNode<>::make("a.x", {a, x}), b});

    auto zero = ReductionNode<double>::make("zero", root, {x}, {},
        [](Reduction& f) {
            double slope = 0.0;
            const double start = f.argument(0);
            const double value = f.gradient({start}, {&slope, 1});
            return start - value / slope;
        });
    EXPECT_EQ(zero->invalidationMode(), InvalidationMode::Eager);
    std::vector<double> roots, zeros;
    engine.addOutput<double>(root, [&](const double& v) { roots.push_back(v); });
    engine.addOutput<double>(zero, [&](const double& v) { zeros.push_back(v); });

    engine.step();
    a->post(4.0);
    b->post(-6.0);
    engine.step();

    EXPECT_EQ(roots, (std::vector<double>{2.0}));
    EXPECT_EQ(zeros, (std::vector<double>{1.0, 1.5}));
}

// The engine evaluates outputs in the order they were registered, so the node
// pulls the root before the engine reaches it. The engine's dirty snapshot
// still fires the root's callback on every cycle that moves it.
TEST(ReductionNode, RegisteredBeforeItsRoot) {
    async::Engine engine;
    auto s = async::AsyncInput<double>::make("s", 1.0);
    engine.addSource(s);
    auto x    = Input<double>::make("x", 1.0);
    auto root = ops::ProductNode<>::make("s.x", {s, x});
    auto at3  = ReductionNode<double>::make("at 3", root, {x}, {},
        [](Reduction& f) { return f({3.0}); });
    std::vector<double> values, trials;
    engine.addOutput<double>(at3, [&](const double& v) { trials.push_back(v); });
    engine.addOutput<double>(root, [&](const double& v) { values.push_back(v); });

    for (const double v : {2.0, 3.0, 5.0}) {
        s->post(v);
        engine.step();
    }
    EXPECT_EQ(values, (std::vector<double>{2.0, 3.0, 5.0}));
    EXPECT_EQ(trials, (std::vector<double>{6.0, 9.0, 15.0}));
}

// The functor always returns 1, so the node solves on every cycle that moves
// its root, and delivers once.
TEST(ReductionNode, AnEqualityPolicyGatesDelivery) {
    async::Engine engine;
    auto s = async::AsyncInput<double>::make("s", 1.0);
    engine.addSource(s);
    auto x    = Input<double>::make("x", 1.0);
    auto root = ops::ProductNode<>::make("s.x", {s, x});
    int runs = 0;
    auto one = ReductionNode<double>::make("one", root, {x}, {},
        [&runs](Reduction& f) { ++runs; return f({2.0}) / f({2.0}); });
    int delivered = 0;
    engine.addOutput<double>(one, [&](const double&) { ++delivered; });
    for (const double v : {2.0, 3.0, 5.0}) {
        s->post(v);
        engine.step();
    }
    EXPECT_EQ(runs, 3);
    EXPECT_EQ(delivered, 1);

    // With a policy that calls every value a change, it delivers each time.
    auto always = ReductionNode<double>::make("always", root, {x}, {},
        [](Reduction& f) { return f({2.0}) / f({2.0}); }, std::make_shared<AlwaysChangedPolicy>());
    int each = 0;
    engine.addOutput<double>(always, [&](const double&) { ++each; });
    for (const double v : {7.0, 11.0}) {
        s->post(v);
        engine.step();
    }
    EXPECT_EQ(each, 2);
}

// ─────────────────────────────────────────────────────────────────────────────
// What it is in a graph
// ─────────────────────────────────────────────────────────────────────────────

// Its inputs are its roots, and then the inputs its functor is handed. Its
// arguments are not inputs. An engine finds the stateful nodes above its roots
// through them.
TEST(ReductionNode, ItsInputsAreItsRootsThenItsOtherInputs) {
    auto s     = Input<double>::make("s", 1.0);
    auto ewma  = ts::EWMANode::make("ewma", s, 0.5);
    auto x     = Input<double>::make("x", 2.0);
    auto r0    = ops::ProductNode<>::make("r0", {ewma, x});
    auto r1    = ops::SumNode<>::make("r1", {ewma, x});
    auto quote = Input<double>::make("quote", 5.0);
    auto level = Input<int>::make("level", 3);
    auto node  = ReductionNode<double, double, int>::make(
        "node", std::vector<NodePtr>{r0, r1}, {x}, {quote, level},
        [](Reduction& f, const double& q, const int& l) {
            std::array<double, 2> y{};
            f({q}, y);
            return y[0] + y[1] + l;
        });
    EXPECT_EQ(node->inputs(), (std::vector<NodePtr>{r0, r1, quote, level}));
    EXPECT_EQ(node->name(), "node");
    EXPECT_EQ(node->kind(), NodeKind::Compute);
    EXPECT_EQ(node->invalidationMode(), InvalidationMode::Eager);
    EXPECT_EQ(node->reduceStep(), nullptr);
    EvalContext ctx;
    aad::Partials partials;
    EXPECT_FALSE(node->partials(ctx, partials));

    // ewma is 1 after its first sample: 1·5 + (1 + 5) + 3.
    EXPECT_EQ(valueOf(node), 14.0);

    async::Engine engine;
    engine.addOutput<double>(node, [](const double&) {});
    const std::vector<StatefulNodePtr> found = engine.discoverStatefulNodes();
    ASSERT_EQ(found.size(), 1u);
    EXPECT_EQ(std::dynamic_pointer_cast<ts::EWMANode>(found[0]), ewma);

    // One root, and no other input.
    auto alone = ReductionNode<double>::make("alone", r0, {x}, {},
        [](Reduction& f) { return f({1.0}); });
    EXPECT_EQ(alone->inputs(), (std::vector<NodePtr>{r0}));
}

// An input whose value owns heap is handed to the functor by reference, held
// alive for the call: here the trial points, as a vector.
TEST(ReductionNode, AnInputThatOwnsHeapIsHandedByReference) {
    auto x      = Input<double>::make("x", 2.0);
    auto k      = Input<double>::make("k", 3.0);
    auto root   = ops::ProductNode<>::make("x.k", {x, k});
    auto trials = Input<std::vector<double>>::make("trials", {1.0, 2.0, 4.0});
    auto total  = ReductionNode<double, std::vector<double>>::make(
        "total", root, {x}, {trials},
        [](Reduction& f, const std::vector<double>& at) {
            double sum = 0.0;
            for (const double v : at) sum += f({v});
            return sum;
        });
    EXPECT_EQ(total->inputs(), (std::vector<NodePtr>{root, trials}));
    EXPECT_EQ(valueOf(total), 21.0);
    trials->set({10.0});
    EXPECT_TRUE(total->dirty());
    EXPECT_EQ(valueOf(total), 30.0);
    k->set(5.0);
    EXPECT_EQ(valueOf(total), 50.0);
}

// A forced evaluation reaches the roots and the other inputs, as it reaches a
// compute node's inputs: the node pulls them with the context it was given.
TEST(ReductionNode, AForcedEvaluationReachesItsInputs) {
    auto x = Input<double>::make("x", 2.0);
    int roots = 0, others = 0;
    auto root = ComputeNode<double, double>::make("root", {x},
        [&roots](const double& a) { ++roots; return 3.0 * a; }, InvalidationMode::Lazy);
    auto other = ComputeNode<double, double>::make("other", {x},
        [&others](const double& a) { ++others; return a + 1.0; }, InvalidationMode::Lazy);
    int runs = 0;
    auto node = ReductionNode<double, double>::make("node", root, {x}, {other},
        [&runs](Reduction& f, const double& o) { ++runs; return f({10.0}) + o; });
    EvalContext ctx;
    const ValuePtr first = node->eval(ctx);
    EXPECT_EQ(get_value<double>(first), 33.0);
    EXPECT_EQ(roots, 2);    // the root's own evaluation, and the call
    EXPECT_EQ(others, 1);
    EXPECT_EQ(runs, 1);

    node->eval(ctx);        // clean: nothing runs
    EXPECT_EQ(runs, 1);
    EXPECT_EQ(roots, 2);

    EvalContext forced;
    forced.forceRecompute = true;
    node->eval(forced);
    EXPECT_EQ(runs, 2);
    EXPECT_EQ(roots, 4);    // evaluated again, and called again
    EXPECT_EQ(others, 2);
}

TEST(ReductionNode, RejectsWhatItsReductionRejects) {
    auto x    = Input<double>::make("x", 2.0);
    auto root = ops::NegateNode<>::make("root", x);
    const auto fn = [](Reduction& f) { return f({1.0}); };
    EXPECT_THROW(ReductionNode<double>::make("n", NodePtr{}, {x}, {}, fn), std::invalid_argument);
    EXPECT_THROW(ReductionNode<double>::make("n", root, {}, {}, fn), std::invalid_argument);
    EXPECT_THROW(ReductionNode<double>::make("n", root, {x, x}, {}, fn), std::invalid_argument);
    EXPECT_THROW(ReductionNode<double>::make("n", std::vector<NodePtr>{}, {x}, {}, fn),
                 std::invalid_argument);
    EXPECT_THROW(ReductionNode<double>::make("n", std::vector<NodePtr>{root, nullptr}, {x}, {}, fn),
                 std::invalid_argument);
}

// ─────────────────────────────────────────────────────────────────────────────
// Always-dirty nodes, and throws
// ─────────────────────────────────────────────────────────────────────────────

// k·x·x, where k is always dirty. The bind's pull evaluates k again, which
// marks the root dirty again, so the node stays dirty with it. The move of x
// then reaches the node, although the cascade stops at the root.
TEST(ReductionNode, StaysDirtyWhileItsRootIs) {
    auto k    = test_nodes::AlwaysFiring::make(2.0);
    auto x    = Input<double>::make("x", 3.0);
    auto root = ops::ProductNode<>::make("root", {k, x, x});
    auto slope = ReductionNode<double>::make("slope", root, {x}, {},
        [](Reduction& f) {
            double d = 0.0;
            f.gradient({f.argument(0)}, {&d, 1});
            return d;
        });
    EXPECT_EQ(valueOf(slope), 12.0);
    EXPECT_TRUE(root->dirty());
    EXPECT_TRUE(slope->dirty());

    x->set(5.0);
    EXPECT_TRUE(slope->dirty());
    EXPECT_EQ(valueOf(slope), 20.0);
}

// k·x·x − (k + 1), where k is always dirty and reaches the root by two paths.
// The root's own evaluation pulls k twice, and the second pull leaves k·x·x
// dirty again, so the root stays dirty after its pull. The node binds it
// anyway, where Reduction::bind() refuses a dirty root.
TEST(ReductionNode, BindsARootThatStaysDirty) {
    auto k    = test_nodes::AlwaysFiring::make(2.0);
    auto x    = Input<double>::make("x", 3.0);
    auto root = ops::DiffNode<>::make("root", ops::ProductNode<>::make("k.x.x", {k, x, x}),
                                      ops::SumNode<>::make("k+1", {k, Input<double>::make("one", 1.0)}));
    auto slope = ReductionNode<double>::make("slope", root, {x}, {},
        [](Reduction& f) {
            double d = 0.0;
            f.gradient({f.argument(0)}, {&d, 1});
            return d;
        });
    EXPECT_EQ(valueOf(slope), 12.0);
    EXPECT_TRUE(root->dirty());
    EXPECT_TRUE(slope->dirty());

    x->set(5.0);
    EXPECT_EQ(valueOf(slope), 20.0);

    // The public bind refuses the same root.
    Reduction f(root, {x});
    EvalContext ctx;
    root->eval(ctx);
    EXPECT_THROW(f.bind(), std::invalid_argument);
}

// An always-dirty node above two roots: the pull of the second leaves the
// first dirty again. The node binds both, and stays dirty.
TEST(ReductionNode, BindsTwoRootsOverAnAlwaysDirtyNode) {
    auto k  = test_nodes::AlwaysFiring::make(2.0);
    auto x  = Input<double>::make("x", 3.0);
    auto r0 = ops::ProductNode<>::make("k.x", {k, x});
    auto r1 = ops::SumNode<>::make("k+x", {k, x});
    auto both = ReductionNode<double>::make("both", std::vector<NodePtr>{r0, r1}, {x}, {},
        [](Reduction& f) {
            std::array<double, 2> y{};
            f({10.0}, y);
            return y[0] + y[1];
        });
    EXPECT_EQ(valueOf(both), 32.0);
    EXPECT_TRUE(both->dirty());
}

// A functor that throws, and a bind that throws: the node stays dirty and keeps
// the value it had, and the next evaluation tries again.
TEST(ReductionNode, AThrowLeavesItDirtyAndItsValueAsItWas) {
    auto x    = Input<double>::make("x", 2.0);
    auto k    = Input<double>::make("k", 3.0);
    auto root = ops::ProductNode<>::make("x.k", {x, k});
    bool fail = false;
    auto node = ReductionNode<double>::make("node", root, {x}, {},
        [&fail](Reduction& f) {
            if (fail) throw std::runtime_error("did not converge");
            return f({10.0});
        });
    EvalContext ctx;
    const ValuePtr before = node->eval(ctx);
    EXPECT_EQ(get_value<double>(before), 30.0);
    EXPECT_FALSE(node->dirty());

    fail = true;
    k->set(4.0);
    EXPECT_TRUE(mentions(messageOf<std::runtime_error>([&] { node->eval(ctx); }), "did not converge"));
    EXPECT_TRUE(node->dirty());
    fail = false;
    const ValuePtr after = node->eval(ctx);
    EXPECT_EQ(get_value<double>(after), 40.0);
    EXPECT_EQ(get_value<double>(before), 30.0);   // the value it had is still the one it was
    EXPECT_FALSE(node->dirty());

    // A bind that throws: with the Eager branch taken, the argument reaches the
    // root through a barrier. Once the condition switches, the node, still
    // dirty, gives its value.
    auto cond   = Input<bool>::make("cond", true);
    auto opaque = ComputeNode<double, double>::make("node.opaque", {x},
        [](const double& v) { return v * v; });
    auto sel    = ConditionNode::make("sel", cond, opaque, ops::NegateNode<>::make("-x", x));
    auto over   = ReductionNode<double>::make("over", sel, {x}, {},
        [](Reduction& f) { return f({10.0}); });
    EXPECT_THROW(over->eval(ctx), std::domain_error);
    EXPECT_TRUE(over->dirty());
    cond->set(false);
    EXPECT_EQ(valueOf(over), -10.0);
    EXPECT_FALSE(over->dirty());

    // A root that holds no double.
    auto i       = Input<int>::make("i", 2);
    auto ofAnInt = ReductionNode<double>::make("ofAnInt", i, {x}, {},
        [](Reduction& f) { return f({1.0}); });
    EXPECT_THROW(ofAnInt->eval(ctx), std::invalid_argument);
    EXPECT_TRUE(ofAnInt->dirty());
}

// ─────────────────────────────────────────────────────────────────────────────
// Several roots, and a value that owns heap
// ─────────────────────────────────────────────────────────────────────────────

/// The residuals of a line through three points, a·tᵢ + b − yᵢ, as three roots
/// over (a, b). The fit is one Gauss–Newton step, which is exact for a line:
/// (a, b) −= (JᵀJ)⁻¹ Jᵀr.
struct LineFit {
    InputPtr<double> a = Input<double>::make("a", 0.0);
    InputPtr<double> b = Input<double>::make("b", 0.0);
    std::array<double, 3> t{1.0, 2.0, 4.0};
    std::vector<std::shared_ptr<async::AsyncInput<double>>> y;
    std::vector<NodePtr> residuals;

    LineFit() {
        for (std::size_t i = 0; i < t.size(); ++i) {
            y.push_back(async::AsyncInput<double>::make("y" + std::to_string(i), 0.0));
            residuals.push_back(ops::DiffNode<>::make("r" + std::to_string(i),
                ops::SumNode<>::make("a.t+b", {
                    ops::ProductNode<>::make("a.t", {a, Input<double>::make("t", t[i])}), b}),
                y[i]));
        }
    }

    /// One Gauss–Newton step from (a, b)'s own values, written to out.
    static void step(std::vector<double>& out, Reduction& f) {
        std::array<double, 3> r{};
        std::array<double, 6> j{};
        const std::array<double, 2> at{f.argument(0), f.argument(1)};
        f.jacobian(at, r, j);
        double jtj00 = 0.0, jtj01 = 0.0, jtj11 = 0.0, jtr0 = 0.0, jtr1 = 0.0;
        for (std::size_t i = 0; i < 3; ++i) {
            jtj00 += j[2 * i] * j[2 * i];
            jtj01 += j[2 * i] * j[2 * i + 1];
            jtj11 += j[2 * i + 1] * j[2 * i + 1];
            jtr0  += j[2 * i] * r[i];
            jtr1  += j[2 * i + 1] * r[i];
        }
        const double det = jtj00 * jtj11 - jtj01 * jtj01;
        out.resize(2);
        out[0] = at[0] - (jtj11 * jtr0 - jtj01 * jtr1) / det;
        out[1] = at[1] - (jtj00 * jtr1 - jtj01 * jtr0) / det;
    }
};

// A fit as a node: three roots, their Jacobian, and a std::vector<double> for
// the solution, written in place. Once warm, a cycle that moves a point and
// fits again allocates nothing.
TEST(ReductionNode, AFitInPlaceAllocatesNothing) {
    async::Engine engine;
    const LineFit g;
    for (const auto& source : g.y) engine.addSource(source);
    auto fit = ReductionNode<std::vector<double>>::makeInPlace(
        "fit", g.residuals, {g.a, g.b}, {},
        [](std::vector<double>& out, Reduction& f) { LineFit::step(out, f); });
    double slope = 0.0, intercept = 0.0;
    long delivered = 0;
    engine.addOutput<std::vector<double>>(fit, [&](const std::vector<double>& v) {
        slope = v[0];
        intercept = v[1];
        ++delivered;
    });

    // Points on 3t − 1: the fit is exact.
    for (std::size_t i = 0; i < 3; ++i) g.y[i]->post(3.0 * g.t[i] - 1.0);
    engine.step();
    EXPECT_NEAR(slope, 3.0, 1e-12);
    EXPECT_NEAR(intercept, -1.0, 1e-12);

    for (int i = 0; i < 10; ++i) {   // every buffer of every slot
        g.y[static_cast<std::size_t>(i) % 3]->post(1.0 + i);
        engine.step();
    }
    delivered = 0;
    const long allocs = allocations_during([&] {
        for (int i = 0; i < 200; ++i) {
            g.y[static_cast<std::size_t>(i) % 3]->post(2.0 + (i % 7));
            engine.step();
        }
    });
    EXPECT_EQ(allocs, 0);
    EXPECT_EQ(delivered, 200);

    // The same fit from a functor that returns its vector gives the same
    // values, and pays for the vector on each evaluation.
    async::Engine other;
    const LineFit h;
    for (const auto& source : h.y) other.addSource(source);
    auto returned = ReductionNode<std::vector<double>>::make(
        "fit", h.residuals, {h.a, h.b}, {},
        [](Reduction& f) {
            std::vector<double> out;
            LineFit::step(out, f);
            return out;
        });
    double slope2 = 0.0, intercept2 = 0.0;
    other.addOutput<std::vector<double>>(returned, [&](const std::vector<double>& v) {
        slope2 = v[0];
        intercept2 = v[1];
    });
    for (int i = 0; i < 10; ++i) {
        h.y[static_cast<std::size_t>(i) % 3]->post(1.0 + i);
        other.step();
    }
    const long returnedAllocs = allocations_during([&] {
        for (int i = 0; i < 200; ++i) {
            h.y[static_cast<std::size_t>(i) % 3]->post(2.0 + (i % 7));
            other.step();
        }
    });
    EXPECT_EQ(returnedAllocs, 200);
    EXPECT_EQ(slope2, slope);
    EXPECT_EQ(intercept2, intercept);
}

// A solver over one root, in an engine cycle: the reduction is bound and run
// six times, and nothing is allocated.
TEST(ReductionNode, ASolvingCycleAllocatesNothing) {
    async::Engine engine;
    auto a = async::AsyncInput<double>::make("a", 2.0);
    engine.addSource(a);
    auto x        = Input<double>::make("x", 1.0);
    auto residual = ops::DiffNode<>::make("x.x-a", ops::ProductNode<>::make("x.x", {x, x}), a);
    auto root = ReductionNode<double>::make("sqrt(a)", residual, {x}, {},
        [](Reduction& f) {
            double guess = f.argument(0), slope = 0.0;
            for (int i = 0; i < 6; ++i) {
                const double miss = f.gradient({guess}, {&slope, 1});
                guess -= miss / slope;
            }
            return guess;
        });
    double last = 0.0;
    long delivered = 0;
    engine.addOutput<double>(root, [&](const double& v) { last = v; ++delivered; });
    for (int i = 0; i < 10; ++i) {
        a->post(2.0 + i);
        engine.step();
    }
    delivered = 0;
    const long allocs = allocations_during([&] {
        for (int i = 0; i < 200; ++i) {
            a->post(3.0 + (i % 7));
            engine.step();
        }
    });
    EXPECT_EQ(allocs, 0);
    EXPECT_EQ(delivered, 200);
    EXPECT_NEAR(last * last, 6.0, 1e-9);
}

// ─────────────────────────────────────────────────────────────────────────────
// Across a selection
// ─────────────────────────────────────────────────────────────────────────────

// x² above zero and −3x below, and the x below zero at which the function is
// 12, by bisection from the point the graph holds, which is above zero. The
// node takes both branches as inputs, so it pulls the one its root did not
// take, and it hears a constant of that branch move.
TEST(ReductionNode, SolvesAcrossASelectionWhoseBranchesItIsGiven) {
    auto x      = Input<double>::make("arg.x", 2.0);
    auto factor = Input<double>::make("factor", -3.0);
    auto upper  = ops::ProductNode<>::make("branch.upper", {x, x});
    auto lower  = ops::ProductNode<>::make("branch.lower", {x, factor});
    auto root   = ConditionNode::make("root.out", above("node.positive", x, 0.0), upper, lower);

    const auto bisect = [](Reduction& f) {
        double lo = -100.0, hi = 0.0;   // the function falls from 300 to 0 here
        for (int i = 0; i < 60; ++i) {
            const double mid = 0.5 * (lo + hi);
            (f({mid}) > 12.0 ? lo : hi) = mid;
        }
        return 0.5 * (lo + hi);
    };
    int solves = 0;
    auto solver = ReductionNode<double, double, double>::make(
        "solver", root, {x}, {upper, lower},
        [&bisect, &solves](Reduction& f, const double&, const double&) {
            ++solves;
            return bisect(f);
        });
    EXPECT_NEAR(valueOf(solver), -4.0, 1e-12);
    EXPECT_FALSE(solver->dirty());
    EXPECT_FALSE(lower->dirty());   // the node pulled the branch its root did not take

    // A constant of the branch not taken moves. The root does not hear it, and
    // the node does, through the branch.
    factor->set(-6.0);
    EXPECT_FALSE(root->dirty());
    EXPECT_TRUE(solver->dirty());
    EXPECT_NEAR(valueOf(solver), -2.0, 1e-12);
    EXPECT_EQ(solves, 2);

    // Without the branches as inputs, the bind refuses the stale one, and the
    // node stays dirty.
    factor->set(-3.0);
    auto blind = ReductionNode<double>::make("blind", root, {x}, {},
        [&bisect](Reduction& f) { return bisect(f); });
    EvalContext ctx;
    const std::string why = messageOf<std::domain_error>([&] { blind->eval(ctx); });
    EXPECT_TRUE(mentions(why, "branch.lower, which the graph did not read at this point, is dirty"))
        << why;
    EXPECT_TRUE(blind->dirty());
}

// The node is not given the branch its root did not take. Something else has
// evaluated that branch, so a reduction can follow it, and the node would then
// not hear the branch move, because its root does not. The node refuses.
TEST(ReductionNode, MustBeGivenTheBranchesItFollows) {
    auto x      = Input<double>::make("arg.x", 2.0);
    auto factor = Input<double>::make("factor", -3.0);
    auto upper  = ops::ProductNode<>::make("branch.upper", {x, x});
    auto lower  = ops::ProductNode<>::make("branch.lower", {x, factor});
    auto root   = ConditionNode::make("root.out", above("node.positive", x, 0.0), upper, lower);
    auto blind  = ReductionNode<double>::make("node.blind", root, {x}, {},
        [](Reduction& f) { return f({-4.0}); });
    EvalContext ctx;
    lower->eval(ctx);   // something else evaluates the branch
    const std::string why = messageOf<std::domain_error>([&] { blind->eval(ctx); });
    EXPECT_TRUE(mentions(why, "the function reads branch.lower")) << why;
    EXPECT_TRUE(mentions(why, "node.blind is not given it as an input")) << why;
    EXPECT_TRUE(blind->dirty());

    // A reduction bound by hand follows it: hearing the branch is its caller's
    // to see to.
    root->eval(ctx);
    Reduction f = reduce(root, {x});
    EXPECT_EQ(f({-4.0}), 12.0);

    // Given the branch the graph did not take, the node follows it, and hears it.
    auto given = ReductionNode<double, double>::make("node.given", root, {x}, {lower},
        [](Reduction& g, const double&) { return g({-4.0}); });
    EXPECT_EQ(valueOf(given), 12.0);
    factor->set(-5.0);
    EXPECT_FALSE(root->dirty());
    EXPECT_TRUE(given->dirty());
    EXPECT_EQ(valueOf(given), 20.0);
}

// |x| as x > 0 ? x : −x, bound below zero. The branch the graph did not take is
// the argument, whose value a call supplies, so the node need not be given it.
TEST(ReductionNode, AnArgumentItFollowsNeedsNoHearing) {
    auto x    = Input<double>::make("arg.x", -2.0);
    auto root = ConditionNode::make("root.out", above("node.positive", x, 0.0), x,
                                    ops::NegateNode<>::make("-x", x));
    auto node = ReductionNode<double>::make("node", root, {x}, {},
        [](Reduction& f) { return f({3.0}) + f({-5.0}); });
    EXPECT_EQ(valueOf(node), 8.0);
}

// ─────────────────────────────────────────────────────────────────────────────
// Reduction::argument()
// ─────────────────────────────────────────────────────────────────────────────

// The value each argument itself holds at the bound point, which is where a
// solver starts, and not the last value a call was given.
TEST(ReductionArgument, IsTheValueTheGraphHolds) {
    const CallPrice c;
    EvalContext ctx;
    c.call->eval(ctx);
    Reduction f = reduce(c.call, {c.vol, c.spot});
    EXPECT_EQ(f.argument(0), 0.25);
    EXPECT_EQ(f.argument(1), 100.0);
    f({0.40, 120.0});
    EXPECT_EQ(f.argument(0), 0.25);
    EXPECT_EQ(f.argument(1), 100.0);

    // At the arguments' own values the function is the root's value.
    EXPECT_EQ(f({f.argument(0), f.argument(1)}), valueOf(c.call));

    c.vol->set(0.30);
    c.call->eval(ctx);
    EXPECT_EQ(f.argument(0), 0.25);   // the point it was bound to
    f.bind();
    EXPECT_EQ(f.argument(0), 0.30);

    // An intermediate node as an argument: its own value, too.
    auto x    = Input<double>::make("x", 1.5);
    auto ex   = ops::ExpNode<>::make("exp(x)", x);
    auto root = ops::ProductNode<>::make("root", {ex, x});
    root->eval(ctx);
    Reduction g = reduce(root, {ex, x});
    EXPECT_EQ(g.argument(0), valueOf(ex));
    EXPECT_EQ(g.argument(1), 1.5);
}

TEST(ReductionArgument, RefusesWhatItDidNotRead) {
    auto x     = Input<double>::make("arg.x", 2.0);
    auto other = Input<double>::make("arg.other", 7.0);   // a node of another graph
    auto root  = ops::NegateNode<>::make("root.out", x);
    Reduction f(root, {x, other});
    EXPECT_TRUE(mentions(messageOf<std::logic_error>([&] { f.argument(0); }), "is not bound"));

    EvalContext ctx;
    root->eval(ctx);
    f.bind();
    EXPECT_EQ(f.argument(0), 2.0);
    EXPECT_THROW(f.argument(2), std::out_of_range);
    const std::string why = messageOf<std::domain_error>([&] { f.argument(1); });
    EXPECT_TRUE(mentions(why, "root root.out does not depend on argument arg.other")) << why;
    EXPECT_TRUE(other->dirty());   // and it was not evaluated to find out
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
