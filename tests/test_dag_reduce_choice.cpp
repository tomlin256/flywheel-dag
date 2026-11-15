// =============================================================================
// flywheel-dag — A header-only C++23 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

// dag::Reduction through a node whose choice an argument moves: a
// ConditionNode whose condition the argument reaches.
//
// Allocations are counted by allocation_counter.cpp, which this suite links.

#include <gtest/gtest.h>
#include "allocation_counter.hpp"
#include "reduce_test_nodes.hpp"
#include "test_nodes.hpp"
#include "flywheel/dag.hpp"
#include "flywheel/dag_aad.hpp"
#include "flywheel/dag_ops.hpp"
#include "flywheel/dag_reduce.hpp"
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

/// x > level, as a Lazy compute node that holds a bool: it can be called, and
/// has no partials.
NodePtr above(std::string name, const NodePtr& x, double level) {
    return ComputeNode<bool, double>::make(std::move(name), {x},
        [level](const double& a) { return a > level; }, InvalidationMode::Lazy);
}

double valueOf(const NodePtr& node) {
    EvalContext ctx;
    const ValuePtr v = node->eval(ctx);
    return get_value<double>(v);
}

void evaluate(const NodePtr& node) {
    EvalContext ctx;
    node->eval(ctx);
}

/// x·exp(x) where x > 0, and ln(1 + x²) elsewhere: a selection whose condition
/// the argument moves, with a constant on each branch.
struct Piecewise {
    InputPtr<double> x        = Input<double>::make("arg.x", 2.0);
    InputPtr<double> scale    = Input<double>::make("scale", 1.5);
    InputPtr<double> offset   = Input<double>::make("offset", 0.25);
    NodePtr positive = above("node.positive", x, 0.0);
    NodePtr whenPositive = ops::ProductNode<>::make("branch.positive",
        {x, ops::ExpNode<>::make("exp(x)", x), scale});
    NodePtr otherwise = ops::SumNode<>::make("branch.otherwise", {
        ops::LnNode<>::make("ln(1+x.x)", ops::SumNode<>::make("1+x.x",
            {Input<double>::make("one", 1.0), ops::ProductNode<>::make("x.x", {x, x})})),
        offset});
    NodePtr root = ConditionNode::make("root.out", positive, whenPositive, otherwise);

    double closed(double v) const {
        return v > 0.0 ? v * std::exp(v) * scale->get() : std::log(1.0 + v * v) + offset->get();
    }
    double slope(double v) const {
        return v > 0.0 ? (1.0 + v) * std::exp(v) * scale->get() : 2.0 * v / (1.0 + v * v);
    }
};

/// Its value is its first input's. It reads both inputs, and its partials name
/// the first alone: the second moves nothing, though the node reads it.
class FirstOfTwo : public NodeBase, public std::enable_shared_from_this<FirstOfTwo> {
public:
    static std::shared_ptr<FirstOfTwo> make(std::string name, NodePtr first, NodePtr second) {
        auto self = std::shared_ptr<FirstOfTwo>(
            new FirstOfTwo(std::move(name), std::move(first), std::move(second)));
        wire(self, self->inputs());
        return self;
    }

    ValuePtr eval(EvalContext& ctx) override {
        if (!dirty() && !ctx.forceRecompute) return cached_;
        beginEval();
        const ValuePtr a = first_->eval(ctx);
        second_->eval(ctx);
        cached_ = slot_.emit(get_value<double>(a));
        notifyDownstream();
        endEval();
        return cached_;
    }
    std::string name() const override { return name_; }
    std::vector<NodePtr> inputs() const override { return {first_, second_}; }
    NodeKind kind() const override { return NodeKind::Compute; }

    std::unique_ptr<IReduceStep> reduceStep() override { return std::make_unique<Step>(); }

private:
    FirstOfTwo(std::string name, NodePtr first, NodePtr second)
        : NodeBase(InvalidationMode::Lazy), name_(std::move(name)), first_(std::move(first))
        , second_(std::move(second)) {}

    class Step final : public IReduceStep {
    public:
        ValuePtr apply(const ReduceInputs& in) override {
            return slot_.emit(get_value<double>(in[0]));
        }
        bool partials(const ReduceInputs&, aad::Partials& out) override {
            out.add(0, 1.0);
            return true;
        }
    private:
        ValueSlot<double> slot_;
    };

    std::string       name_;
    NodePtr           first_, second_;
    ValuePtr          cached_;
    ValueSlot<double> slot_;
};

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Following the choice
// ─────────────────────────────────────────────────────────────────────────────

// With both branches evaluated, the function is the graph's on both sides of
// the flip, to the bit: at each trial value it is what the root holds once the
// argument is set there.
TEST(ReductionChoice, FollowsAConditionTheArgumentMoves) {
    const Piecewise g;
    evaluate(g.root);
    evaluate(g.otherwise);   // the branch the graph did not take
    Reduction f = reduce(g.root, {g.x});
    EXPECT_EQ(f({2.0}), valueOf(g.root));

    const std::vector<double> trials = {3.0, -4.0, 0.5, -0.5, 0.0, 1e-300, -1e-300};
    std::vector<double> there;
    there.reserve(trials.size());
    for (const double v : trials) there.push_back(f({v}));
    for (std::size_t i = 0; i < trials.size(); ++i) {
        SCOPED_TRACE(trials[i]);
        g.x->set(trials[i]);
        EXPECT_EQ(there[i], valueOf(g.root));
        EXPECT_NEAR(there[i], g.closed(trials[i]), 1e-12);
    }
}

// Bound on the other side of the flip, the function is the same one.
TEST(ReductionChoice, IsTheSameFunctionFromEitherSide) {
    const Piecewise g;
    evaluate(g.root);
    evaluate(g.otherwise);
    Reduction fromAbove = reduce(g.root, {g.x});
    const double up = fromAbove({3.0});
    const double down = fromAbove({-4.0});

    g.x->set(-1.0);
    evaluate(g.root);           // takes the other branch now
    evaluate(g.whenPositive);   // so this is the one the graph did not take
    Reduction fromBelow = reduce(g.root, {g.x});
    EXPECT_EQ(fromBelow({3.0}), up);
    EXPECT_EQ(fromBelow({-4.0}), down);
    EXPECT_EQ(fromBelow.size(), fromAbove.size());
}

// A call runs the steps of both branches, as the graph evaluates both at its
// own point, and the selection picks one.
TEST(ReductionChoice, BothBranchesRunAtEachCall) {
    auto x = Input<double>::make("x", 2.0);
    int squares = 0, negations = 0;
    auto squared = ComputeNode<double, double>::make("squared", {x},
        [&squares](const double& a) { ++squares; return a * a; }, InvalidationMode::Lazy);
    auto negated = ComputeNode<double, double>::make("negated", {x},
        [&negations](const double& a) { ++negations; return -a; }, InvalidationMode::Lazy);
    auto root = ConditionNode::make("root", above("positive", x, 0.0), squared, negated);
    evaluate(root);
    evaluate(negated);
    ASSERT_EQ(squares, 1);
    ASSERT_EQ(negations, 1);

    Reduction f = reduce(root, {x});
    EXPECT_EQ(f.size(), 4u);   // the comparison, both branches and the selection
    EXPECT_EQ(squares, 1);     // binding calls nothing
    EXPECT_EQ(negations, 1);
    EXPECT_EQ(f({3.0}), 9.0);
    EXPECT_EQ(f({-3.0}), 3.0);
    EXPECT_EQ(squares, 3);
    EXPECT_EQ(negations, 3);
    EXPECT_FALSE(negated->dirty());   // a call moves nothing in the graph
}

// A branch the selection does not pick can hold a NaN, and it is not the value.
TEST(ReductionChoice, ANaNOnTheBranchNotSelectedIsNotTheValue) {
    auto x    = Input<double>::make("x", 4.0);
    auto root = ConditionNode::make("root", above("positive", x, 0.0),
        ops::SqrtNode<>::make("sqrt(x)", x),
        ops::SqrtNode<>::make("sqrt(-x)", ops::NegateNode<>::make("-x", x)));
    evaluate(root);
    const NodePtr other = root->inputs()[2];
    evaluate(other);
    ASSERT_TRUE(std::isnan(valueOf(other)));

    Reduction f = reduce(root, {x});
    double slope = 0.0;
    EXPECT_EQ(f.gradient({9.0}, {&slope, 1}), 3.0);
    EXPECT_EQ(slope, 1.0 / 6.0);
    EXPECT_EQ(f.gradient({-16.0}, {&slope, 1}), 4.0);
    EXPECT_EQ(slope, -1.0 / 8.0);
}

// ─────────────────────────────────────────────────────────────────────────────
// What it needs: the branch not taken, evaluated
// ─────────────────────────────────────────────────────────────────────────────

// A reduction evaluates nothing, so it follows the choice only where the graph
// has evaluated the branch it did not take. The refusal names it.
TEST(ReductionChoice, ABranchNothingEvaluatedIsRefused) {
    const Piecewise g;
    evaluate(g.root);
    ASSERT_TRUE(g.otherwise->dirty());

    Reduction f(g.root, {g.x});
    const std::string why = messageOf<std::domain_error>([&] { f.bind(); });
    EXPECT_TRUE(mentions(why, "arg.x moves node.positive, which decides what root.out reads")) << why;
    EXPECT_TRUE(mentions(why, "branch.otherwise, which the graph did not read at this point, is dirty"))
        << why;
    EXPECT_TRUE(mentions(why, "evaluate branch.otherwise")) << why;
    EXPECT_TRUE(g.otherwise->dirty());   // and the bind did not evaluate it
    EXPECT_EQ(f.size(), 0u);
    EXPECT_TRUE(mentions(messageOf<std::logic_error>([&] { f({1.0}); }), "is not bound"));

    evaluate(g.otherwise);
    f.bind();
    EXPECT_EQ(f({-1.0}), g.closed(-1.0));

    // A constant of that branch moves: the branch is stale again, and nothing
    // of the graph hears it for the root, which does not read it.
    g.offset->set(0.5);
    EXPECT_FALSE(g.root->dirty());
    EXPECT_THROW(f.bind(), std::domain_error);
    evaluate(g.otherwise);
    f.bind();
    EXPECT_EQ(f({-1.0}), g.closed(-1.0));
}

// A node that is always dirty is never evaluated "at this point".
TEST(ReductionChoice, AnAlwaysDirtyBranchIsRefused) {
    auto x      = Input<double>::make("arg.x", 2.0);
    auto firing = test_nodes::AlwaysFiring::make(7.0);
    auto root   = ConditionNode::make("root.out", above("node.positive", x, 0.0),
                                      ops::ProductNode<>::make("x.x", {x, x}), firing);
    evaluate(root);
    evaluate(firing);
    Reduction f(root, {x});
    const std::string why = messageOf<std::domain_error>([&] { f.bind(); });
    EXPECT_TRUE(mentions(why, "always-firing, which the graph did not read at this point, is dirty"))
        << why;
}

// The branch the graph did not take is part of the function now, so a node on
// it that cannot be called, with the argument above it, is a barrier.
TEST(ReductionChoice, ABarrierOnTheOtherBranchIsOne) {
    auto x     = Input<double>::make("arg.x", 2.0);
    auto eager = ComputeNode<double, double>::make("node.eager", {x},
        [](const double& a) { return a + 1.0; });
    auto root  = ConditionNode::make("root.out", above("node.positive", x, 0.0),
                                     ops::NegateNode<>::make("-x", x), eager);
    evaluate(root);
    evaluate(eager);
    Reduction f(root, {x});
    const std::string why = messageOf<std::domain_error>([&] { f.bind(); });
    EXPECT_TRUE(mentions(why, "arg.x reaches root root.out through node.eager")) << why;
    EXPECT_TRUE(mentions(why, "is Eager")) << why;

    // With no argument above it, the same node is a constant on that branch.
    auto k        = Input<double>::make("k", 5.0);
    auto constant = ComputeNode<double, double>::make("node.constant", {k},
        [](const double& a) { return a + 1.0; });
    auto other    = ConditionNode::make("root.other", above("positive", x, 0.0),
                                        ops::NegateNode<>::make("-x", x), constant);
    evaluate(other);
    evaluate(constant);
    Reduction g = reduce(other, {x});
    EXPECT_EQ(g({3.0}), -3.0);
    EXPECT_EQ(g({-3.0}), 6.0);
}

// |x| as x > 0 ? x : −x. Below zero the graph takes −x, and the branch it did
// not take is the argument itself, which the comparison has evaluated: nothing
// more is needed. Above zero the branch not taken is −x, which nothing has.
TEST(ReductionChoice, TheOtherBranchCanBeTheArgument) {
    auto x    = Input<double>::make("arg.x", -2.0);
    auto root = ConditionNode::make("root.out", above("node.positive", x, 0.0), x,
                                    ops::NegateNode<>::make("-x", x));
    evaluate(root);
    Reduction f = reduce(root, {x});
    EXPECT_EQ(f({-3.0}), 3.0);
    EXPECT_EQ(f({3.0}), 3.0);

    x->set(2.0);
    evaluate(root);
    const std::string why = messageOf<std::domain_error>([&] { f.bind(); });
    EXPECT_TRUE(mentions(why, "arg.x moves node.positive, which decides what root.out reads")) << why;
    EXPECT_TRUE(mentions(why, "-x, which the graph did not read at this point, is dirty")) << why;
}

// ─────────────────────────────────────────────────────────────────────────────
// Choices within choices
// ─────────────────────────────────────────────────────────────────────────────

// Four regimes of x, by three selections, each on the branch the one before did
// not take at the point the graph holds. Each reading of the point finds one
// more of them, so the bind reads it four times.
TEST(ReductionChoice, FollowsChoicesWithinChoices) {
    auto x = Input<double>::make("x", 3.0);
    const auto times = [&x](const char* name, double k) {
        return ops::ProductNode<>::make(name, {x, Input<double>::make("k", k)});
    };
    auto a      = times("a", 1.0);
    auto b      = times("b", 10.0);
    auto c      = times("c", 100.0);
    auto d      = times("d", 1000.0);
    auto lowest = ConditionNode::make("lowest", above("x>-2", x, -2.0), c, d);
    auto lower  = ConditionNode::make("lower", above("x>-1", x, -1.0), b, lowest);
    auto root   = ConditionNode::make("root", above("x>0", x, 0.0), a, lower);
    evaluate(root);     // a
    evaluate(lower);    // b, and not `lowest`
    Reduction f(root, {x});
    EXPECT_THROW(f.bind(), std::domain_error);   // `lowest` is dirty
    evaluate(lowest);   // c, and not d
    EXPECT_THROW(f.bind(), std::domain_error);   // d is dirty
    evaluate(d);
    f.bind();

    EXPECT_EQ(f({2.0}), 2.0);
    EXPECT_EQ(f({-0.5}), -5.0);
    EXPECT_EQ(f({-1.5}), -150.0);
    EXPECT_EQ(f({-3.0}), -3000.0);
    for (const double v : {2.0, -0.5, -1.5, -3.0}) {
        const double there = f({v});
        x->set(v);
        EXPECT_EQ(there, valueOf(root)) << v;
    }
}

// Two selections on one path are both found by the first reading.
TEST(ReductionChoice, TwoChoicesOnOnePathAreFollowed) {
    auto x     = Input<double>::make("x", 3.0);
    auto half  = ops::ProductNode<>::make("x/2", {x, Input<double>::make("half", 0.5)});
    auto twice = ops::ProductNode<>::make("2x", {x, Input<double>::make("two", 2.0)});
    auto inner = ConditionNode::make("inner", above("x>1", x, 1.0), twice, half);
    auto minus = ops::NegateNode<>::make("-x", x);
    auto root  = ConditionNode::make("root", above("x>0", x, 0.0), inner, minus);
    evaluate(root);
    evaluate(half);
    evaluate(minus);
    Reduction f = reduce(root, {x});
    EXPECT_EQ(f({3.0}), 6.0);
    EXPECT_EQ(f({0.5}), 0.25);
    EXPECT_EQ(f({-4.0}), 4.0);
}

// ─────────────────────────────────────────────────────────────────────────────
// A choosing node of the suites' own
// ─────────────────────────────────────────────────────────────────────────────

// The rule is NodeBase::reduceReads()'s, not ConditionNode's: any node that
// names a deciding input chooses when an argument moves that input.
TEST(ReductionChoice, ANodeOfYourOwnThatDecidesIsFollowed) {
    auto x      = Input<double>::make("x", 3.0);
    auto which  = ComputeNode<double, double>::make("which", {x},
        [](const double& a) { return a > 5.0 ? 1.0 : 0.0; }, InvalidationMode::Lazy);
    auto first  = ops::NegateNode<>::make("first", x);
    auto second = ops::ProductNode<>::make("second", {x, x});
    auto root   = Chooser::make("root", which, first, second);
    evaluate(root);
    Reduction f(root, {x});
    EXPECT_THROW(f.bind(), std::domain_error);
    // This node hears every input, where a ConditionNode hears only the branch
    // it took: evaluating the other one for the first time publishes a value,
    // which marks the node dirty, so it is evaluated again.
    evaluate(second);
    EXPECT_TRUE(root->dirty());
    evaluate(root);
    f.bind();
    EXPECT_EQ(f.size(), 4u);
    EXPECT_EQ(f({3.0}), -3.0);
    EXPECT_EQ(f({7.0}), 49.0);

    // The argument itself can be what decides.
    auto pick   = Input<double>::make("pick", 0.0);
    auto direct = Chooser::make("direct", pick, first, second);
    evaluate(direct);
    Reduction g = reduce(direct, {pick, x});
    EXPECT_EQ(g({0.0, 4.0}), -4.0);
    EXPECT_EQ(g({1.0, 4.0}), 16.0);
}

// ─────────────────────────────────────────────────────────────────────────────
// The derivatives
// ─────────────────────────────────────────────────────────────────────────────

// The derivative is the selected branch's, on each side of the flip. The
// comparison is a step with no partials, and it is not asked: no derivative
// passes through a condition.
TEST(ReductionChoice, TheGradientIsTheSelectedBranchs) {
    const Piecewise g;
    evaluate(g.root);
    evaluate(g.otherwise);
    Reduction f = reduce(g.root, {g.x, g.scale, g.offset});
    for (const double v : {3.0, 0.5, -0.5, -4.0}) {
        SCOPED_TRACE(v);
        std::array<double, 3> gradient{9.0, 9.0, 9.0};
        const double value = f.gradient({v, 1.5, 0.25}, gradient);
        EXPECT_NEAR(value, g.closed(v), 1e-12);
        EXPECT_NEAR(gradient[0], g.slope(v), 1e-12);
        EXPECT_NEAR(gradient[1], v > 0.0 ? v * std::exp(v) : 0.0, 1e-12);   // the scale's branch
        EXPECT_EQ(gradient[2], v > 0.0 ? 0.0 : 1.0);                        // the offset's
    }

    // A tape at the point the graph holds agrees: it gives the branch taken.
    g.x->set(-0.75);
    evaluate(g.root);
    evaluate(g.whenPositive);
    f.bind();
    std::array<double, 3> gradient{};
    f.gradient({g.x->get(), g.scale->get(), g.offset->get()}, gradient);
    const std::vector<double> tape = aad::adjoints(g.root, {g.x, g.scale, g.offset});
    for (std::size_t i = 0; i < 3; ++i) EXPECT_NEAR(gradient[i], tape[i], 1e-13) << i;
}

// A step with no partials on a branch stops the gradient only at a call that
// selects that branch.
TEST(ReductionChoice, AnOpaqueBranchStopsTheGradientOnlyWhereItIsSelected) {
    auto x      = Input<double>::make("arg.x", 2.0);
    auto opaque = ComputeNode<double, double>::make("node.opaque", {x},
        [](const double& a) { return 3.0 * a; }, InvalidationMode::Lazy);
    auto root   = ConditionNode::make("root.out", above("node.positive", x, 0.0),
                                      ops::ProductNode<>::make("x.x", {x, x}), opaque);
    evaluate(root);
    evaluate(opaque);
    Reduction f = reduce(root, {x});
    double slope = 0.0;
    EXPECT_EQ(f.gradient({3.0}, {&slope, 1}), 9.0);
    EXPECT_EQ(slope, 6.0);
    EXPECT_EQ(f({-3.0}), -9.0);
    const std::string why = messageOf<std::domain_error>([&] { f.gradient({-3.0}, {&slope, 1}); });
    EXPECT_TRUE(mentions(why, "arg.x reaches root root.out through node.opaque")) << why;
    EXPECT_TRUE(mentions(why, "has no partials")) << why;
}

// Two roots over one selection: the Jacobian, swept forward, across the flip.
TEST(ReductionChoice, TheJacobianFollowsTheChoice) {
    const Piecewise g;
    auto doubled = ops::SumNode<>::make("root.doubled", {g.root, g.root});
    evaluate(doubled);
    evaluate(g.otherwise);
    Reduction f = reduce(std::vector<NodePtr>{g.root, doubled}, {g.x});
    for (const double v : {3.0, -4.0}) {
        SCOPED_TRACE(v);
        std::array<double, 2> y{};
        std::array<double, 2> j{};
        f.jacobian({v}, y, j);
        EXPECT_NEAR(y[0], g.closed(v), 1e-12);
        EXPECT_NEAR(j[0], g.slope(v), 1e-12);
        EXPECT_NEAR(j[1], 2.0 * g.slope(v), 1e-12);
    }
}

// x, an opaque Lazy node over x, and a node that reads both and depends on x
// alone, with no choosing node anywhere. The opaque node runs at each call, and
// no partial names it, so the derivative does not pass through it.
TEST(ReductionChoice, AStepTheDerivativeDoesNotPassThroughIsNotAsked) {
    auto x      = Input<double>::make("x", 2.0);
    auto opaque = ComputeNode<double, double>::make("opaque", {x},
        [](const double& v) { return 2.0 * v; }, InvalidationMode::Lazy);
    auto root   = FirstOfTwo::make("root", x, opaque);
    evaluate(root);

    Reduction f = reduce(root, {x});
    EXPECT_EQ(f.size(), 2u);
    std::vector<double> g(1, 0.0);
    EXPECT_EQ(f.gradient({3.0}, g), 3.0);
    EXPECT_EQ(g[0], 1.0);
    // And again: the bound point keeps to the marked sweep.
    EXPECT_EQ(f.gradient({5.0}, g), 5.0);
    EXPECT_EQ(g[0], 1.0);

    // A root that is the opaque node itself has no gradient.
    Reduction opaqueAlone = reduce(opaque, {x});
    EXPECT_THROW(opaqueAlone.gradient({3.0}, g), std::domain_error);
}

// ─────────────────────────────────────────────────────────────────────────────
// Allocation
// ─────────────────────────────────────────────────────────────────────────────

TEST(ReductionChoice, AWarmCallGradientAndBindAllocateNothing) {
    const Piecewise g;
    evaluate(g.root);
    evaluate(g.otherwise);
    Reduction f = reduce(g.root, {g.x});
    double slope = 0.0;
    for (int i = 0; i < 4; ++i) f.gradient({i % 2 == 0 ? 1.0 + i : -1.0 - i}, {&slope, 1});

    double sum = 0.0;
    const long allocs = allocations_during([&] {
        for (int i = 0; i < 100; ++i) {
            const double v = (i % 2 == 0 ? 1.0 : -1.0) * (0.5 + 0.01 * i);
            sum += f({v}) + f.gradient({v}, {&slope, 1}) + slope;
        }
    });
    EXPECT_EQ(allocs, 0);
    const long binds = allocations_during([&] {
        for (int i = 0; i < 100; ++i) {
            f.bind();
            sum += f({-2.0});
        }
    });
    EXPECT_EQ(binds, 0);
    EXPECT_NE(sum, 0.0);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
