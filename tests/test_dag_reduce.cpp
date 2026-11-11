// =============================================================================
// flywheel-dag — A header-only C++23 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

// dag::Reduction: the nodes between some argument nodes and a root, as a
// function that is called without moving the graph.
//
// Allocations are counted by allocation_counter.cpp, which this suite links.

#include <gtest/gtest.h>
#include "aad_test_graphs.hpp"
#include "allocation_counter.hpp"
#include "flywheel/dag.hpp"
#include "flywheel/dag_aad.hpp"
#include "flywheel/dag_memoize.hpp"
#include "flywheel/dag_ops.hpp"
#include "flywheel/dag_reduce.hpp"
#include "flywheel/dag_timeseries.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numbers>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using namespace dag;
using aad_test::expectClose;
using allocation_counter::allocations_during;

namespace {

// ─────────────────────────────────────────────────────────────────────────────
// Two nodes of the suite's own that implement IReducible, so that what bind()
// does with what a node reads is held without any engine node.
// ─────────────────────────────────────────────────────────────────────────────

/// What a Counted gives a reduction: no step, a step, or a step whose partials
/// name an input the node does not have.
enum class Gives : std::uint8_t { NoStep, AStep, AStepThatMisnamesItsInput };

/// A pure function of one input, 2·a + 1, that counts each time it is computed:
/// by its own eval() and by a step alike. In is the type its input holds. Its
/// step has the one partial, 2.
template<typename In = double>
class Counted
    : public NodeBase
    , public IReducible
    , public std::enable_shared_from_this<Counted<In>>
{
public:
    static std::shared_ptr<Counted> make(std::string name, NodePtr in,
                                         Gives gives = Gives::AStep) {
        auto self = std::shared_ptr<Counted>(new Counted(std::move(name), std::move(in), gives));
        wire(self, self->inputs());
        return self;
    }

    int calls() const noexcept { return calls_; }

    ValuePtr eval(EvalContext& ctx) override {
        if (!dirty() && !ctx.forceRecompute) return cached_;
        beginEval();
        const ValuePtr v = in_->eval(ctx);
        if (skipRecompute(ctx)) { endEval(); return cached_; }
        cached_ = slot_.emit(compute(get_value<In>(v)));
        notifyDownstream();
        endEval();
        return cached_;
    }
    std::string name() const override { return name_; }
    std::vector<NodePtr> inputs() const override { return {in_}; }
    NodeKind kind() const override { return NodeKind::Compute; }

    std::unique_ptr<IReduceStep> reduceStep() override {
        if (gives_ == Gives::NoStep) return nullptr;
        return std::make_unique<Step>(this->shared_from_this());
    }

private:
    Counted(std::string name, NodePtr in, Gives gives)
        : NodeBase(InvalidationMode::Lazy), name_(std::move(name)), in_(std::move(in))
        , gives_(gives) {}

    double compute(const In& a) {
        ++calls_;
        return 2.0 * static_cast<double>(a) + 1.0;
    }

    class Step final : public IReduceStep {
    public:
        explicit Step(std::shared_ptr<Counted> node) : node_(std::move(node)) {}
        ValuePtr apply(const ReduceInputs& in) override {
            return slot_.emit(node_->compute(get_value<In>(in[0])));
        }
        bool partials(const ReduceInputs&, aad::Partials& out) override {
            out.add(node_->gives_ == Gives::AStepThatMisnamesItsInput ? 1 : 0, 2.0);
            return true;
        }
    private:
        std::shared_ptr<Counted> node_;
        ValueSlot<double>        slot_;
    };

    std::string       name_;
    NodePtr           in_;
    Gives             gives_;
    ValuePtr          cached_;
    ValueSlot<double> slot_;
    int               calls_ = 0;
};

/// Selects one of two inputs by a third: `first` while `which` holds 0, and
/// `second` otherwise. Like a ConditionNode, it pulls and reads only the input
/// it selects, and `which` decides which that is. hold(v) freezes it at v, and
/// it then reads no input at all, as a tweaked node does.
class Chooser
    : public NodeBase
    , public IReducible
    , public std::enable_shared_from_this<Chooser>
{
public:
    static std::shared_ptr<Chooser> make(std::string name, NodePtr which, NodePtr first,
                                         NodePtr second) {
        auto self = std::shared_ptr<Chooser>(
            new Chooser(std::move(name), std::move(which), std::move(first), std::move(second)));
        wire(self, self->inputs());
        return self;
    }

    void hold(double v) {
        held_   = true;
        cached_ = make_value(v);
        markDirty();
        notifyDownstream();
    }

    ValuePtr eval(EvalContext& ctx) override {
        if (held_) { markClean(); return cached_; }
        if (!dirty() && !ctx.forceRecompute) return cached_;
        beginEval();
        const ValuePtr which = which_->eval(ctx);
        cached_ = (get_value<double>(which) == 0.0 ? first_ : second_)->eval(ctx);
        notifyDownstream();
        endEval();
        return cached_;
    }
    std::string name() const override { return name_; }
    std::vector<NodePtr> inputs() const override { return {which_, first_, second_}; }
    NodeKind kind() const override { return NodeKind::Compute; }

    std::unique_ptr<IReduceStep> reduceStep() override { return std::make_unique<Step>(); }

    void reduceReads(EvalContext& ctx, ReduceReads& out) override {
        if (held_) return;
        const ValuePtr which = which_->eval(ctx);
        out.decides(0);
        out.reads(get_value<double>(which) == 0.0 ? 1 : 2);
    }

private:
    Chooser(std::string name, NodePtr which, NodePtr first, NodePtr second)
        : name_(std::move(name)), which_(std::move(which)), first_(std::move(first))
        , second_(std::move(second)) {}

    class Step final : public IReduceStep {
    public:
        ValuePtr apply(const ReduceInputs& in) override {
            return in[get_value<double>(in[0]) == 0.0 ? 1 : 2];
        }
    };

    std::string name_;
    NodePtr     which_, first_, second_;
    ValuePtr    cached_;
    bool        held_ = false;
};

/// A Black–Scholes call, from ops and differentiable nodes: d1 is one
/// differentiable node over all five inputs, d2 is ops over d1, and each normal
/// CDF is a differentiable node over one input. Seventeen nodes, five of them
/// inputs, and eight of the other twelve below vol.
struct CallPrice {
    InputPtr<double> spot   = Input<double>::make("spot", 100.0);
    InputPtr<double> strike = Input<double>::make("strike", 95.0);
    InputPtr<double> vol    = Input<double>::make("vol", 0.25);
    InputPtr<double> expiry = Input<double>::make("expiry", 0.75);
    InputPtr<double> rate   = Input<double>::make("rate", 0.03);
    NodePtr call;

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
    }
};

/// True when a generic functor is running on duals.
template<typename T>
constexpr bool isDual() { return !std::is_same_v<std::decay_t<T>, double>; }

/// ∂call/∂vol of that call, in closed form: S·φ(d1)·√T.
double vega(double s, double k, double v, double t, double r) {
    const double d1 = (std::log(s / k) + (r + 0.5 * v * v) * t) / (v * std::sqrt(t));
    return s * std::exp(-0.5 * d1 * d1) / std::sqrt(2.0 * std::numbers::pi) * std::sqrt(t);
}

void expectWithin(double actual, double expected, double relative) {
    EXPECT_NEAR(actual, expected, relative * std::max(1.0, std::abs(expected)));
}

/// f.gradient() with respect to every input of a graph, at two points, each
/// handed to check() once the graph holds that point: the point the graph holds
/// to begin with, and another one, where the gradient is taken before the inputs
/// are set there.
template<typename Graph, typename Check>
void atTwoPoints(const Graph& g, Check&& check) {
    const std::vector<InputPtr<double>> ins = g.inputs();
    EvalContext ctx;
    const ValuePtr held = g.root->eval(ctx);
    Reduction f = reduce(g.root, aad_test::asNodes(ins));

    std::vector<double> x;
    x.reserve(ins.size());
    for (const auto& in : ins) x.push_back(in->get());
    std::vector<double> gradient(ins.size(), 0.0);
    EXPECT_EQ(f.gradient(x, gradient), get_value<double>(held));
    check(gradient);

    for (double& v : x) v = 1.1 * v + 0.05;
    const double there = f.gradient(x, gradient);
    for (std::size_t i = 0; i < ins.size(); ++i) ins[i]->set(x[i]);
    const ValuePtr moved = g.root->eval(ctx);
    EXPECT_EQ(there, get_value<double>(moved));
    check(gradient);
}

/// The message of the E that fn() throws. An exception of another type leaves
/// the test, and fails it.
template<typename E, typename Fn>
std::string messageOf(Fn&& fn) {
    try {
        fn();
    } catch (const E& e) {
        return e.what();
    }
    return "nothing was thrown";
}

bool mentions(const std::string& message, const std::string& words) {
    return message.find(words) != std::string::npos;
}

/// The function of every input of a graph, against the graph itself: at the
/// point the graph holds, and at another one, which the inputs are then set to.
template<typename Graph>
void expectTheGraphsValue(const Graph& g) {
    const std::vector<InputPtr<double>> ins = g.inputs();
    EvalContext ctx;
    const ValuePtr held = g.root->eval(ctx);
    Reduction f = reduce(g.root, aad_test::asNodes(ins));
    ASSERT_EQ(f.arity(), ins.size());

    std::vector<double> x;
    x.reserve(ins.size());
    for (const auto& in : ins) x.push_back(in->get());
    EXPECT_EQ(f(x), get_value<double>(held));

    for (double& v : x) v = 1.1 * v + 0.05;
    const double there = f(x);
    EXPECT_NE(there, get_value<double>(held));
    for (std::size_t i = 0; i < ins.size(); ++i) ins[i]->set(x[i]);
    const ValuePtr moved = g.root->eval(ctx);
    EXPECT_EQ(there, get_value<double>(moved));
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Its value
// ─────────────────────────────────────────────────────────────────────────────

// To the bit: a step and its node apply one Op to the same values.
TEST(Reduction, MatchesTheGraphAtEveryPoint) {
    expectTheGraphsValue(aad_test::SumOfTerms{});
    expectTheGraphsValue(aad_test::LogTimesRoot{});
    expectTheGraphsValue(aad_test::PowerOfDifference{});
    expectTheGraphsValue(aad_test::TrigOfRatio{});
    expectTheGraphsValue(aad_test::InverseTrig{});
    expectTheGraphsValue(aad_test::PolarRoundTrip{});
}

// A sum of doubles depends on its order: (1 + 1e16) − 1e16 is 0, because
// 1 + 1e16 rounds to 1e16, and (−1e16 + 1e16) + 1 is 1.
TEST(Reduction, AnNAryStepFoldsInItsNodesOrder) {
    auto one   = Input<double>::make("one", 1.0);
    auto big   = Input<double>::make("big", 1e16);
    auto minus = Input<double>::make("minus", -1e16);
    auto root  = ops::SumNode<>::make("root", {one, big, minus});
    EvalContext ctx;
    const ValuePtr held = root->eval(ctx);
    ASSERT_EQ(get_value<double>(held), 0.0);

    Reduction f = reduce(root, {one, big, minus});
    EXPECT_EQ(f({1.0, 1e16, -1e16}), 0.0);
}

TEST(Reduction, ACallLeavesTheGraphAsItWas) {
    const aad_test::SumOfTerms g;
    EvalContext ctx;
    g.root->eval(ctx);

    // Every node of the graph, a shared one more than once, with what it holds.
    std::vector<NodePtr> nodes{g.root};
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        const std::vector<NodePtr> ins = nodes[i]->inputs();
        nodes.insert(nodes.end(), ins.begin(), ins.end());
    }
    ASSERT_EQ(nodes.size(), 8u);
    std::vector<ValuePtr> held;
    held.reserve(nodes.size());
    for (const auto& n : nodes) held.push_back(n->eval(ctx));

    Reduction f = reduce(g.root, {g.x, g.y});
    for (int i = 0; i < 100; ++i) f({1.0 + 0.01 * i, 0.5 + 0.02 * i});

    for (std::size_t i = 0; i < nodes.size(); ++i) {
        SCOPED_TRACE(nodes[i]->name());
        EXPECT_FALSE(nodes[i]->dirty());
        EXPECT_EQ(nodes[i]->eval(ctx).get(), held[i].get());
    }
    EXPECT_EQ(g.x->get(), 1.3);
    EXPECT_EQ(g.y->get(), 0.7);
}

// ─────────────────────────────────────────────────────────────────────────────
// The point it is bound to
// ─────────────────────────────────────────────────────────────────────────────

TEST(Reduction, BindReadsThePointTheGraphHolds) {
    auto x    = Input<double>::make("x", 2.0);
    auto k    = Input<double>::make("k", 3.0);
    auto root = ops::ProductNode<>::make("root", {x, k});
    EvalContext ctx;
    root->eval(ctx);
    Reduction f = reduce(root, {x});
    EXPECT_EQ(f({5.0}), 15.0);

    k->set(4.0);
    root->eval(ctx);
    EXPECT_EQ(f({5.0}), 15.0);   // the point it was bound to
    f.bind();
    EXPECT_EQ(f({5.0}), 20.0);
}

// The reduction is built before anything is evaluated, and evaluates nothing.
// The root takes `inner` or a barrier over the argument, as `which` says.
TEST(Reduction, IsUnboundUntilABindSucceeds) {
    auto x       = Input<double>::make("x", 2.0);
    auto which   = Input<double>::make("which", 0.0);
    auto inner   = Counted<>::make("inner", x);
    auto barrier = ts::EWMANode::make("barrier", x, 0.5);
    auto root    = Chooser::make("root", which, inner, barrier);

    Reduction f(root, {x});
    EXPECT_TRUE(root->dirty());
    EXPECT_TRUE(inner->dirty());
    EXPECT_EQ(inner->calls(), 0);
    EXPECT_EQ(f.size(), 0u);
    EXPECT_TRUE(mentions(messageOf<std::logic_error>([&] { f({1.0}); }), "is not bound"));

    EvalContext ctx;
    root->eval(ctx);
    f.bind();
    EXPECT_EQ(f.size(), 2u);
    EXPECT_EQ(f({1.0}), 3.0);

    // The graph takes the barrier now, so this bind throws part of the way
    // through, and the reduction is not left bound to the point before.
    which->set(1.0);
    root->eval(ctx);
    EXPECT_THROW(f.bind(), std::domain_error);
    EXPECT_EQ(f.size(), 0u);
    EXPECT_TRUE(mentions(messageOf<std::logic_error>([&] { f({1.0}); }), "is not bound"));

    which->set(0.0);
    root->eval(ctx);
    f.bind();
    EXPECT_EQ(f({1.0}), 3.0);
}

TEST(Reduction, MovesWithItsBinding) {
    auto x    = Input<double>::make("x", 2.0);
    auto k    = Input<double>::make("k", 3.0);
    auto root = ops::ProductNode<>::make("root", {x, k});
    EvalContext ctx;
    root->eval(ctx);
    Reduction f = reduce(root, {x});
    ASSERT_EQ(f({5.0}), 15.0);

    Reduction g(std::move(f));
    EXPECT_EQ(g.arity(), 1u);
    EXPECT_EQ(g.size(), 1u);
    EXPECT_EQ(g({5.0}), 15.0);

    Reduction h(root, {k, x});   // another function, and unbound
    h = std::move(g);
    EXPECT_EQ(h.arity(), 1u);
    EXPECT_EQ(h({7.0}), 21.0);
}

// ─────────────────────────────────────────────────────────────────────────────
// Its arguments
// ─────────────────────────────────────────────────────────────────────────────

TEST(Reduction, AnyNodeCanBeAnArgument) {
    auto x    = Input<double>::make("x", 1.5);
    auto ex   = ops::ExpNode<>::make("exp(x)", x);
    auto root = ops::ProductNode<>::make("root", {ex, x});
    EvalContext ctx;
    root->eval(ctx);

    // Over exp(x) it is u·x, at the x the graph holds: exp is not a step.
    Reduction ofExp = reduce(root, {ex});
    EXPECT_EQ(ofExp.size(), 1u);
    EXPECT_EQ(ofExp({2.0}), 3.0);

    // Over both it is u·v: the argument x does not move the argument exp(x).
    Reduction ofBoth = reduce(root, {ex, x});
    EXPECT_EQ(ofBoth.size(), 1u);
    EXPECT_EQ(ofBoth({2.0, 5.0}), 10.0);

    // Over the root itself it is the identity.
    Reduction ofRoot = reduce(root, {root});
    EXPECT_EQ(ofRoot.size(), 0u);
    EXPECT_EQ(ofRoot({42.0}), 42.0);
}

TEST(Reduction, AnArgumentTheRootDoesNotDependOnIsIgnored) {
    auto x     = Input<double>::make("x", 2.0);
    auto root  = ops::ExpNode<>::make("root", x);
    auto other = Input<double>::make("other", 7.0);   // a node of another graph
    EvalContext ctx;
    const ValuePtr held = root->eval(ctx);

    Reduction ofX    = reduce(root, {x});
    Reduction ofBoth = reduce(root, {x, other});
    EXPECT_EQ(ofBoth.arity(), 2u);
    EXPECT_EQ(ofBoth.size(), 1u);
    EXPECT_EQ(ofBoth({1.0, 100.0}), ofX({1.0}));
    EXPECT_EQ(ofBoth({1.0, -3.0}), ofX({1.0}));

    Reduction ofOther = reduce(root, {other});
    EXPECT_EQ(ofOther.size(), 0u);
    EXPECT_EQ(ofOther({100.0}), get_value<double>(held));
}

TEST(Reduction, ANodeNamedTwiceIsOneValue) {
    auto x      = Input<double>::make("x", 3.0);
    auto square = ops::ProductNode<>::make("x.x", {x, x});
    auto zero   = ops::DiffNode<>::make("x-x", x, x);
    EvalContext ctx;
    square->eval(ctx);
    zero->eval(ctx);

    Reduction squared = reduce(square, {x});
    EXPECT_EQ(squared({5.0}), 25.0);
    Reduction cancelled = reduce(zero, {x});
    EXPECT_EQ(cancelled({5.0}), 0.0);
}

TEST(Reduction, RejectsBadArguments) {
    auto x    = Input<double>::make("x", 2.0);
    auto root = ops::NegateNode<>::make("root", x);
    EXPECT_THROW(Reduction(nullptr, {x}), std::invalid_argument);
    EXPECT_THROW(Reduction(root, {}), std::invalid_argument);
    EXPECT_THROW(Reduction(root, {x, nullptr}), std::invalid_argument);
    EXPECT_THROW(Reduction(root, {x, x}), std::invalid_argument);

    EvalContext ctx;
    root->eval(ctx);
    Reduction f = reduce(root, {x});
    const std::vector<double> none;
    EXPECT_THROW(f(none), std::invalid_argument);
    EXPECT_THROW(f({1.0, 2.0}), std::invalid_argument);
    EXPECT_EQ(f({1.0}), -1.0);

    // A gradient takes one value and one slot per argument.
    std::array<double, 2> slots{};
    EXPECT_THROW(f.gradient({1.0}, slots), std::invalid_argument);
    EXPECT_THROW(f.gradient({1.0, 2.0}, std::span<double>(slots).first(1)), std::invalid_argument);
    EXPECT_EQ(f.gradient({1.0}, std::span<double>(slots).first(1)), -1.0);
    EXPECT_EQ(slots[0], -1.0);
}

TEST(Reduction, RejectsADirtyRoot) {
    auto x    = Input<double>::make("x", 2.0);
    auto root = ops::NegateNode<>::make("root", x);

    // Never evaluated.
    EXPECT_THROW(reduce(root, {x}), std::invalid_argument);
    EXPECT_TRUE(root->dirty());

    // Evaluated, and then an input moved.
    EvalContext ctx;
    root->eval(ctx);
    Reduction f = reduce(root, {x});
    x->set(3.0);
    EXPECT_THROW(f.bind(), std::invalid_argument);
    EXPECT_TRUE(root->dirty());
}

// An argument's value is handed to its steps as a double, so a step that reads
// an int would throw std::bad_cast at every call.
TEST(Reduction, RejectsARootOrAnArgumentThatDoesNotHoldADouble) {
    auto i = Input<int>::make("i", 2);
    EvalContext ctx;
    i->eval(ctx);
    EXPECT_THROW(reduce(i, {i}), std::invalid_argument);

    auto root = Counted<int>::make("root", i);
    const ValuePtr held = root->eval(ctx);
    ASSERT_EQ(get_value<double>(held), 5.0);
    Reduction f(root, {i});
    EXPECT_TRUE(mentions(messageOf<std::invalid_argument>([&] { f.bind(); }),
                         "argument i does not hold a double"));
    EXPECT_TRUE(mentions(messageOf<std::logic_error>([&] { f({1.0}); }), "is not bound"));
}

// ─────────────────────────────────────────────────────────────────────────────
// Its steps
// ─────────────────────────────────────────────────────────────────────────────

// x → shared → left and right → root. The value is 2·(2s + 1), with s = 2x + 1.
TEST(Reduction, RunsEachNodeOncePerCall) {
    auto x      = Input<double>::make("x", 1.0);
    auto shared = Counted<>::make("shared", x);
    auto left   = Counted<>::make("left", shared);
    auto right  = Counted<>::make("right", shared);
    auto root   = ops::SumNode<>::make("root", {left, right});
    EvalContext ctx;
    root->eval(ctx);
    ASSERT_EQ(shared->calls(), 1);

    Reduction f = reduce(root, {x});
    EXPECT_EQ(f.size(), 4u);
    EXPECT_EQ(shared->calls(), 1);   // building and binding call nothing

    EXPECT_EQ(f({2.0}), 22.0);
    EXPECT_EQ(shared->calls(), 2);
    EXPECT_EQ(left->calls(), 2);
    EXPECT_EQ(right->calls(), 2);

    EXPECT_EQ(f({3.0}), 30.0);
    EXPECT_EQ(shared->calls(), 3);
}

// ─────────────────────────────────────────────────────────────────────────────
// Compute nodes: which functors are called
// ─────────────────────────────────────────────────────────────────────────────

TEST(Reduction, ALazyComputeNodeIsCalled) {
    auto x = Input<double>::make("x", 2.0);
    auto k = Input<double>::make("k", 10.0);
    int calls = 0;
    auto scaled = ComputeNode<double, double, double>::make("scaled", {x, k},
        [&calls](const double& a, const double& b) { ++calls; return a * b + 1.0; },
        InvalidationMode::Lazy);
    EvalContext ctx;
    const ValuePtr held = scaled->eval(ctx);
    ASSERT_EQ(calls, 1);

    Reduction f = reduce(scaled, {x});
    EXPECT_EQ(f.size(), 1u);
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(f({3.0}), 31.0);
    EXPECT_EQ(calls, 2);
    EXPECT_EQ(f({4.0}), 41.0);
    EXPECT_EQ(calls, 3);

    // The node holds what it held, and is not evaluated again.
    EXPECT_FALSE(scaled->dirty());
    EXPECT_EQ(scaled->eval(ctx).get(), held.get());
    EXPECT_EQ(get_value<double>(held), 21.0);
    EXPECT_EQ(calls, 3);
}

// Eager is the default, and what a functor with captured state must be, so
// nothing says an Eager functor may run at a trial value. It does not run.
TEST(Reduction, AnEagerComputeNodeIsABarrier) {
    auto x = Input<double>::make("arg.x", 2.0);
    int calls = 0;
    auto eager = ComputeNode<double, double>::make("node.eager", {x},
        [&calls](const double& a) { ++calls; return a + 1.0; });
    EvalContext ctx;
    eager->eval(ctx);
    Reduction f(eager, {x});
    const std::string why = messageOf<std::domain_error>([&] { f.bind(); });
    EXPECT_TRUE(mentions(why, "arg.x reaches root node.eager through node.eager")) << why;
    EXPECT_TRUE(mentions(why, "is Eager")) << why;
    EXPECT_TRUE(mentions(why, "InvalidationMode::Lazy")) << why;
    EXPECT_EQ(calls, 1);

    auto inPlace = InPlaceComputeNode<std::vector<double>, double>::make("node.in-place", {x},
        [](std::vector<double>& out, const double& a) { out.assign({a, a}); });
    auto total = ComputeNode<double, std::vector<double>>::make("root.total", {inPlace},
        [](const std::vector<double>& v) { return v[0] + v[1]; }, InvalidationMode::Lazy);
    total->eval(ctx);
    Reduction g(total, {x});
    const std::string whyNot = messageOf<std::domain_error>([&] { g.bind(); });
    EXPECT_TRUE(mentions(whyNot, "arg.x reaches root root.total through node.in-place")) << whyNot;
    EXPECT_TRUE(mentions(whyNot, "is Eager")) << whyNot;
    EXPECT_TRUE(mentions(whyNot, "InvalidationMode::Lazy")) << whyNot;

    // An Eager node that no argument moves is a constant like any other.
    auto y       = Input<double>::make("y", 5.0);
    auto product = ops::ProductNode<>::make("product", {x, eager, y});
    product->eval(ctx);
    Reduction ofY = reduce(product, {y});
    EXPECT_EQ(ofY.size(), 1u);
    EXPECT_EQ(ofY({7.0}), 42.0);
    EXPECT_EQ(calls, 1);
}

// Only the arguments and the root are doubles.
TEST(Reduction, NodesBetweenHoldAnyType) {
    auto x = Input<double>::make("x", 2.0);
    auto ladder = InPlaceComputeNode<std::vector<double>, double>::make("ladder", {x},
        [](std::vector<double>& out, const double& a) { out.assign({a, 2.0 * a, 3.0 * a}); },
        InvalidationMode::Lazy);
    auto total = ComputeNode<double, std::vector<double>>::make("total", {ladder},
        [](const std::vector<double>& v) { return v[0] + v[1] + v[2]; }, InvalidationMode::Lazy);
    EvalContext ctx;
    const ValuePtr held = total->eval(ctx);
    ASSERT_EQ(get_value<double>(held), 12.0);

    Reduction f = reduce(total, {x});
    EXPECT_EQ(f.size(), 2u);
    EXPECT_EQ(f({5.0}), 30.0);
    EXPECT_EQ(f({1.0}), 6.0);
}

// An in-place functor is handed the value of its last run. The functor here
// records what it was handed, so the test can tell whose that was: the node's
// own runs are handed the node's last value, and never a call's.
TEST(Reduction, AnInPlaceStepHasAScratchOfItsOwn) {
    auto x = Input<double>::make("x", 2.0);
    std::vector<double> handed;
    auto pair = InPlaceComputeNode<std::vector<double>, double>::make("pair", {x},
        [&handed](std::vector<double>& out, const double& a) {
            handed = out;
            out.assign({a, a});
        },
        InvalidationMode::Lazy);
    auto total = ComputeNode<double, std::vector<double>>::make("total", {pair},
        [](const std::vector<double>& v) { return v[0] + v[1]; }, InvalidationMode::Lazy);
    EvalContext ctx;
    for (const double v : {2.0, 2.5, 3.0}) {   // both buffers of each slot
        x->set(v);
        total->eval(ctx);
    }
    ASSERT_EQ(handed, (std::vector<double>{2.5, 2.5}));

    Reduction f = reduce(total, {x});
    for (int i = 0; i < 3; ++i) EXPECT_EQ(f({10.0 + i}), 20.0 + 2.0 * i);
    EXPECT_EQ(handed, (std::vector<double>{11.0, 11.0}));   // the step's last run but one

    double sum = 0.0;
    EXPECT_EQ(allocations_during([&] { sum += f({7.0}) + f({8.0}); }), 0);
    EXPECT_EQ(sum, 30.0);

    // The node's next run is handed {3, 3}, the value of its own last run.
    x->set(4.0);
    double value = 0.0;
    EXPECT_EQ(allocations_during([&] {
        const ValuePtr v = total->eval(ctx);
        value = get_value<double>(v);
    }), 0);
    EXPECT_EQ(value, 8.0);
    EXPECT_EQ(handed, (std::vector<double>{3.0, 3.0}));
}

// A step that published through its node's slot would leave the reduction
// holding one of the slot's two buffers, and the node's next run would allocate.
TEST(Reduction, ACallLeavesANodesSlotAlone) {
    auto x = Input<double>::make("x", 1.0);
    auto doubled = ComputeNode<double, double>::make("doubled", {x},
        [](const double& a) { return 2.0 * a; }, InvalidationMode::Lazy);
    EvalContext ctx;
    for (const double v : {1.0, 2.0, 3.0}) {   // both buffers of its slot
        x->set(v);
        doubled->eval(ctx);
    }
    Reduction f = reduce(doubled, {x});
    for (int i = 0; i < 3; ++i) EXPECT_EQ(f({5.0 + i}), 10.0 + 2.0 * i);

    for (const double v : {4.0, 5.0}) {
        x->set(v);   // Input::set allocates, by design, so it is not counted
        EXPECT_EQ(allocations_during([&] { doubled->eval(ctx); }), 0);
    }
}

// The cache is keyed by the input values, so calls through it would fill it
// with trial values. The step calls the functor.
TEST(Reduction, AMemoizedNodeIsCalledPastItsCache) {
    using Memo = MemoizedComputeNode<double, double>;
    Memo::clearCache();
    auto x = Input<double>::make("x", 2.0);
    int calls = 0;
    auto squared = Memo::make("reduction.squared", {x},
        [&calls](const double& a) { ++calls; return a * a; });   // Eager, and called all the same
    EvalContext ctx;
    squared->eval(ctx);
    ASSERT_EQ(Memo::cacheSize(), 1u);
    ASSERT_EQ(squared->cacheMisses(), 1u);
    ASSERT_EQ(squared->cacheHits(), 0u);

    Reduction f = reduce(squared, {x});
    EXPECT_EQ(f.size(), 1u);
    for (int i = 0; i < 50; ++i) EXPECT_EQ(f({3.0 + i}), (3.0 + i) * (3.0 + i));
    EXPECT_EQ(calls, 51);
    EXPECT_EQ(Memo::cacheSize(), 1u);
    EXPECT_EQ(squared->cacheMisses(), 1u);
    EXPECT_EQ(squared->cacheHits(), 0u);
    Memo::clearCache();
}

// A tape runs a differentiable node's functor on duals. A call does not.
TEST(Reduction, ADifferentiableNodeIsCalledAtDouble) {
    auto x = Input<double>::make("x", 1.5);
    auto y = Input<double>::make("y", 0.5);
    int doubles = 0, duals = 0;
    auto product = aad::DifferentiableNode<2>::make("x.exp(y)", {x, y},
        [&doubles, &duals](const auto& a, const auto& b) {
            using std::exp;
            if constexpr (isDual<decltype(a)>()) ++duals;
            else ++doubles;
            return a * exp(b);
        });   // Eager, and called all the same
    EvalContext ctx;
    product->eval(ctx);
    ASSERT_EQ(doubles, 1);

    Reduction f = reduce(product, {x, y});
    EXPECT_EQ(f.size(), 1u);
    EXPECT_EQ(f({2.0, 0.0}), 2.0);
    for (int i = 0; i < 4; ++i) f({1.0 + i, 0.25});
    EXPECT_EQ(doubles, 6);
    EXPECT_EQ(duals, 0);
}

// To the bit, through three differentiable nodes and five ops.
TEST(Reduction, ACallPriceAsAFunctionOfVol) {
    const CallPrice c;
    EvalContext ctx;
    const ValuePtr held = c.call->eval(ctx);

    Reduction f = reduce(c.call, {c.vol});
    EXPECT_EQ(f.size(), 8u);
    EXPECT_EQ(f({0.25}), get_value<double>(held));

    const double there = f({0.30});
    c.vol->set(0.30);
    const ValuePtr moved = c.call->eval(ctx);
    EXPECT_EQ(there, get_value<double>(moved));
    EXPECT_NE(there, get_value<double>(held));
}

// ─────────────────────────────────────────────────────────────────────────────
// Selection and tweaks: the point decides what a call follows
// ─────────────────────────────────────────────────────────────────────────────

// The other branch is never pulled, so it stays dirty, and its functor, which a
// call could run, never does.
TEST(Reduction, AConditionTakesTheBranchTheGraphTook) {
    auto x    = Input<double>::make("x", 2.0);
    auto cond = Input<bool>::make("cond", true);
    int calls = 0;
    auto taken = ops::ProductNode<>::make("taken", {x, x});
    auto other = ComputeNode<double, double>::make("other", {x},
        [&calls](const double& a) { ++calls; return -a; }, InvalidationMode::Lazy);
    auto root  = ConditionNode::make("root", cond, taken, other);
    EvalContext ctx;
    const ValuePtr held = root->eval(ctx);
    ASSERT_EQ(get_value<double>(held), 4.0);
    ASSERT_TRUE(other->dirty());

    Reduction f = reduce(root, {x});
    EXPECT_EQ(f.size(), 2u);   // taken and the selection
    EXPECT_EQ(f({3.0}), 9.0);
    EXPECT_EQ(calls, 0);
    EXPECT_TRUE(other->dirty());
}

TEST(Reduction, ABranchSwitchIsSeenAtTheNextBind) {
    auto x    = Input<double>::make("x", 2.0);
    auto cond = Input<bool>::make("cond", true);
    auto root = ConditionNode::make("root", cond, ops::ProductNode<>::make("x.x", {x, x}),
                                    ops::NegateNode<>::make("-x", x));
    EvalContext ctx;
    root->eval(ctx);
    Reduction f = reduce(root, {x});
    EXPECT_EQ(f({3.0}), 9.0);

    cond->set(false);
    root->eval(ctx);
    EXPECT_EQ(f({3.0}), 9.0);   // the point it was bound to
    f.bind();
    EXPECT_EQ(f({3.0}), -3.0);
}

// A barrier is a matter of the point: an Eager node on the branch the graph did
// not take is not read, and one on the branch it took is.
TEST(Reduction, ABarrierOnTheBranchNotTakenIsNotOne) {
    auto x     = Input<double>::make("arg.x", 2.0);
    auto cond  = Input<bool>::make("cond", true);
    auto eager = ComputeNode<double, double>::make("node.eager", {x},
        [](const double& a) { return a + 1.0; });
    auto root  = ConditionNode::make("root.out", cond, ops::NegateNode<>::make("-x", x), eager);
    EvalContext ctx;
    root->eval(ctx);

    Reduction f = reduce(root, {x});
    EXPECT_EQ(f({5.0}), -5.0);

    cond->set(false);
    root->eval(ctx);
    const std::string why = messageOf<std::domain_error>([&] { f.bind(); });
    EXPECT_TRUE(mentions(why, "arg.x reaches root root.out through node.eager")) << why;
}

// |x| as a selection: x > 0 chooses between x and −x, and the argument moves it.
TEST(Reduction, AConditionAnArgumentMovesIsABarrier) {
    auto x        = Input<double>::make("arg.x", 2.0);
    auto positive = ComputeNode<bool, double>::make("node.positive", {x},
        [](const double& a) { return a > 0.0; }, InvalidationMode::Lazy);
    auto root     = ConditionNode::make("root.out", positive, x, ops::NegateNode<>::make("-x", x));
    EvalContext ctx;
    root->eval(ctx);

    Reduction f(root, {x});
    const std::string why = messageOf<std::domain_error>([&] { f.bind(); });
    EXPECT_TRUE(mentions(why, "arg.x moves node.positive, which decides what root.out reads")) << why;
}

// sel·x, where sel is k or −x. With k taken the selection reads nothing that
// the argument moves, so it is a constant, and the product is the only step.
TEST(Reduction, ABranchNoArgumentReachesIsAConstant) {
    auto x    = Input<double>::make("x", 2.0);
    auto k    = Input<double>::make("k", 10.0);
    auto cond = Input<bool>::make("cond", true);
    auto sel  = ConditionNode::make("sel", cond, k, ops::NegateNode<>::make("-x", x));
    auto root = ops::ProductNode<>::make("root", {sel, x});
    EvalContext ctx;
    root->eval(ctx);

    Reduction f = reduce(root, {x});
    EXPECT_EQ(f.size(), 1u);
    EXPECT_EQ(f({3.0}), 30.0);

    cond->set(false);
    root->eval(ctx);
    f.bind();
    EXPECT_EQ(f.size(), 3u);   // −x, the selection and the product
    EXPECT_EQ(f({3.0}), -9.0);
}

// A tweaked node holds its frozen value whatever its inputs do, so it reads
// none of them: the EWMA above it is never pulled, and is no barrier.
TEST(Reduction, ATweakedNodeIsAConstant) {
    auto x        = Input<double>::make("arg.x", 2.0);
    auto smoothed = ts::EWMANode::make("smoothed", x, 0.5);
    int calls = 0;
    auto lazy = TweakableComputeNode<double, double>::make("node.lazy", {smoothed},
        [&calls](const double& a) { ++calls; return 10.0 * a; }, InvalidationMode::Lazy);
    auto root = ops::SumNode<>::make("root.out", {lazy, x});
    lazy->tweak(100.0);
    EvalContext ctx;
    root->eval(ctx);

    Reduction f = reduce(root, {x});
    EXPECT_EQ(f.size(), 1u);
    EXPECT_EQ(f({3.0}), 103.0);
    EXPECT_EQ(calls, 0);
    EXPECT_TRUE(smoothed->dirty());
    EXPECT_FALSE(smoothed->isInitialized());

    // Untweaked and Lazy, it is called.
    auto called  = TweakableComputeNode<double, double>::make("node.called", {x},
        [&calls](const double& a) { ++calls; return 10.0 * a; }, InvalidationMode::Lazy);
    auto negated = ops::NegateNode<>::make("root.negated", called);
    negated->eval(ctx);
    ASSERT_EQ(calls, 1);
    Reduction g = reduce(negated, {x});
    EXPECT_EQ(g.size(), 2u);
    EXPECT_EQ(g({3.0}), -30.0);
    EXPECT_EQ(calls, 2);

    // Untweaked and Eager, it is a barrier, and tweaked it is a constant again.
    auto eager   = TweakableComputeNode<double, double>::make("node.eager", {x},
        [](const double& a) { return 10.0 * a; });
    auto doubled = ops::SumNode<>::make("root.doubled", {eager, eager});
    doubled->eval(ctx);
    Reduction h(doubled, {x});
    const std::string why = messageOf<std::domain_error>([&] { h.bind(); });
    EXPECT_TRUE(mentions(why, "arg.x reaches root root.doubled through node.eager")) << why;
    EXPECT_TRUE(mentions(why, "is Eager")) << why;

    eager->tweak(7.0);
    doubled->eval(ctx);
    h.bind();
    EXPECT_EQ(h.size(), 0u);
    EXPECT_EQ(h({3.0}), 14.0);
}

TEST(Reduction, ATweakIsSeenAtTheNextBind) {
    auto x    = Input<double>::make("x", 2.0);
    auto lazy = TweakableComputeNode<double, double>::make("lazy", {x},
        [](const double& a) { return 10.0 * a; }, InvalidationMode::Lazy);
    auto root = ops::SumNode<>::make("root", {lazy, x});
    EvalContext ctx;
    root->eval(ctx);
    Reduction f = reduce(root, {x});
    EXPECT_EQ(f.size(), 2u);
    EXPECT_EQ(f({3.0}), 33.0);

    lazy->tweak(100.0);
    root->eval(ctx);
    EXPECT_EQ(f({3.0}), 33.0);   // the point it was bound to
    f.bind();
    EXPECT_EQ(f.size(), 1u);
    EXPECT_EQ(f({3.0}), 103.0);

    lazy->clearTweak();
    root->eval(ctx);
    f.bind();
    EXPECT_EQ(f.size(), 2u);
    EXPECT_EQ(f({3.0}), 33.0);
}

// The selection takes a·x or b·x. Bound with a·x taken, the reduction holds a's
// value. After the switch and a bind it holds b's, and a's no more: its
// reference count is what it was before the reduction was built.
TEST(Reduction, ABindHoldsOnlyWhatItsPointReads) {
    auto x    = Input<double>::make("x", 2.0);
    auto a    = Input<double>::make("a", 10.0);
    auto b    = Input<double>::make("b", 20.0);
    auto cond = Input<bool>::make("cond", true);
    auto root = ConditionNode::make("root", cond, ops::ProductNode<>::make("a.x", {a, x}),
                                    ops::ProductNode<>::make("b.x", {b, x}));
    EvalContext ctx;
    root->eval(ctx);
    const ValuePtr heldByA = a->eval(ctx);
    const long before = heldByA.use_count();

    Reduction f = reduce(root, {x});
    EXPECT_EQ(f({3.0}), 30.0);
    EXPECT_EQ(heldByA.use_count(), before + 1);

    cond->set(false);
    root->eval(ctx);
    f.bind();
    EXPECT_EQ(f({3.0}), 60.0);
    EXPECT_EQ(heldByA.use_count(), before);
}

// ─────────────────────────────────────────────────────────────────────────────
// The gradient
// ─────────────────────────────────────────────────────────────────────────────

// To 1e-13 and not to the bit: a step and a node take their partials from one
// Derivative<Op>, inlined at two sites, which a compiler may contract apart.
TEST(Reduction, GradientMatchesATape) {
    const auto againstATape = [](const auto& g) {
        atTwoPoints(g, [&g](const std::vector<double>& gradient) {
            const std::vector<double> tape = aad::adjoints(g.root, aad_test::asNodes(g.inputs()));
            ASSERT_EQ(gradient.size(), tape.size());
            for (std::size_t i = 0; i < tape.size(); ++i) expectClose(gradient[i], tape[i]);
        });
    };
    againstATape(aad_test::SumOfTerms{});
    againstATape(aad_test::LogTimesRoot{});
    againstATape(aad_test::PowerOfDifference{});
    againstATape(aad_test::TrigOfRatio{});
    againstATape(aad_test::InverseTrig{});
    againstATape(aad_test::PolarRoundTrip{});
}

TEST(Reduction, GradientMatchesAnalyticDerivatives) {
    const auto againstTheClosedForm = [](const auto& g) {
        atTwoPoints(g, [&g](const std::vector<double>& gradient) {
            const std::vector<double> closed = g.gradient();
            ASSERT_EQ(gradient.size(), closed.size());
            for (std::size_t i = 0; i < closed.size(); ++i) expectWithin(gradient[i], closed[i], 1e-12);
        });
    };
    againstTheClosedForm(aad_test::SumOfTerms{});
    againstTheClosedForm(aad_test::LogTimesRoot{});
    againstTheClosedForm(aad_test::PowerOfDifference{});
    againstTheClosedForm(aad_test::TrigOfRatio{});
    againstTheClosedForm(aad_test::InverseTrig{});
    againstTheClosedForm(aad_test::PolarRoundTrip{});

    // A call's vega, at the vol the graph holds and at another.
    const CallPrice c;
    EvalContext ctx;
    c.call->eval(ctx);
    Reduction f = reduce(c.call, {c.vol});
    for (const double vol : {0.25, 0.30, 0.45}) {
        SCOPED_TRACE(vol);
        double slope = 0.0;
        f.gradient({vol}, {&slope, 1});
        expectWithin(slope, vega(100.0, 95.0, vol, 0.75, 0.03), 1e-11);
    }
}

// u·v over exp(x) and x: each derivative is the other argument's value, and x
// moves nothing through exp(x), which is an argument too.
TEST(Reduction, TheGradientIsInTheArgumentsOwnValues) {
    auto x    = Input<double>::make("x", 1.5);
    auto ex   = ops::ExpNode<>::make("exp(x)", x);
    auto root = ops::ProductNode<>::make("root", {ex, x});
    EvalContext ctx;
    root->eval(ctx);

    Reduction ofBoth = reduce(root, {ex, x});
    std::array<double, 2> g{9.0, 9.0};
    EXPECT_EQ(ofBoth.gradient({2.0, 5.0}, g), 10.0);
    EXPECT_EQ(g[0], 5.0);
    EXPECT_EQ(g[1], 2.0);

    // Over the root itself: the identity, whose derivative is 1.
    Reduction ofRoot = reduce(root, {root});
    double slope = 9.0;
    EXPECT_EQ(ofRoot.gradient({42.0}, {&slope, 1}), 42.0);
    EXPECT_EQ(slope, 1.0);
}

// A Lazy compute node can be called, and its functor is opaque.
TEST(Reduction, AnOpaqueStepHasNoGradient) {
    auto x      = Input<double>::make("arg.x", 2.0);
    auto opaque = ComputeNode<double, double>::make("node.opaque", {x},
        [](const double& a) { return a * a; }, InvalidationMode::Lazy);
    auto root   = ops::NegateNode<>::make("root.out", opaque);
    EvalContext ctx;
    root->eval(ctx);

    Reduction f = reduce(root, {x});
    EXPECT_EQ(f({3.0}), -9.0);
    double slope = 0.0;
    const std::string why = messageOf<std::domain_error>([&] { f.gradient({3.0}, {&slope, 1}); });
    EXPECT_TRUE(mentions(why, "arg.x reaches root root.out through node.opaque")) << why;
    EXPECT_TRUE(mentions(why, "has no partials")) << why;
    EXPECT_EQ(f({3.0}), -9.0);
}

// z·√x at z = 0 and x = 0. √'s partial is infinite there and its adjoint is 0,
// and the product is 0 for every x, so ∂/∂x is 0 and not 0·∞.
TEST(Reduction, AZeroPartialOrAdjointPropagatesNothing) {
    auto z    = Input<double>::make("z", 0.0);
    auto x    = Input<double>::make("x", 0.0);
    auto root = ops::ProductNode<>::make("root", {z, ops::SqrtNode<>::make("sqrt(x)", x)});
    EvalContext ctx;
    root->eval(ctx);

    Reduction f = reduce(root, {z, x});
    std::array<double, 2> g{9.0, 9.0};
    EXPECT_EQ(f.gradient({0.0, 0.0}, g), 0.0);
    EXPECT_EQ(g[0], 0.0);
    EXPECT_EQ(g[1], 0.0);
}

// (−2)³. With respect to the exponent the derivative is a^b·ln a, and ln(−2)
// is NaN: the true answer, and nothing multiplies it by 0.
TEST(Reduction, ATrueNaNIsKept) {
    auto a    = Input<double>::make("a", -2.0);
    auto b    = Input<double>::make("b", 3.0);
    auto root = ops::PowerNode<>::make("root", a, b);
    EvalContext ctx;
    root->eval(ctx);

    Reduction f = reduce(root, {a, b});
    std::array<double, 2> g{9.0, 9.0};
    EXPECT_EQ(f.gradient({-2.0, 3.0}, g), -8.0);
    EXPECT_EQ(g[0], 12.0);
    EXPECT_TRUE(std::isnan(g[1]));
}

TEST(Reduction, TheGradientThroughAConditionIsItsBranchs) {
    auto x     = Input<double>::make("x", 2.0);
    auto y     = Input<double>::make("y", 5.0);
    auto other = Input<double>::make("other", 1.0);   // a node of another graph
    auto cond  = Input<bool>::make("cond", true);
    auto root  = ConditionNode::make("root", cond, ops::ProductNode<>::make("x.x", {x, x}),
                                     ops::ProductNode<>::make("y.y.y", {y, y, y}));
    EvalContext ctx;
    root->eval(ctx);

    Reduction f = reduce(root, {x, y, other});
    std::array<double, 3> g{9.0, 9.0, 9.0};
    EXPECT_EQ(f.gradient({3.0, 7.0, 11.0}, g), 9.0);
    EXPECT_EQ(g[0], 6.0);   // 2x, through the selection
    EXPECT_EQ(g[1], 0.0);   // on the branch not taken
    EXPECT_EQ(g[2], 0.0);   // the root does not reach it

    cond->set(false);
    root->eval(ctx);
    f.bind();
    EXPECT_EQ(f.gradient({3.0, 7.0, 11.0}, g), 343.0);
    EXPECT_EQ(g[0], 0.0);
    EXPECT_EQ(g[1], 147.0);   // 3y²
    EXPECT_EQ(g[2], 0.0);
}

// 100 + x while the node is tweaked: the argument moves nothing through it.
// Untweaked it is 10x + x, through a functor that has no partials.
TEST(Reduction, ATweakedNodeHasDerivativeZero) {
    auto x      = Input<double>::make("x", 2.0);
    auto frozen = TweakableComputeNode<double, double>::make("frozen", {x},
        [](const double& a) { return 10.0 * a; }, InvalidationMode::Lazy);
    auto root   = ops::SumNode<>::make("root", {frozen, x});
    frozen->tweak(100.0);
    EvalContext ctx;
    root->eval(ctx);

    Reduction f = reduce(root, {x});
    double slope = 9.0;
    EXPECT_EQ(f.gradient({3.0}, {&slope, 1}), 103.0);
    EXPECT_EQ(slope, 1.0);

    frozen->clearTweak();
    root->eval(ctx);
    f.bind();
    EXPECT_EQ(f({3.0}), 33.0);
    EXPECT_THROW(f.gradient({3.0}, {&slope, 1}), std::domain_error);
}

// a·eᵇ/c as one node of three inputs, away from the point the graph holds. Each
// partial comes from the functor's one run on duals, argument i seeded with eᵢ.
TEST(Reduction, ADifferentiableNodesPartialsAreItsFunctors) {
    auto a    = Input<double>::make("a", 1.5);
    auto b    = Input<double>::make("b", 0.4);
    auto c    = Input<double>::make("c", 2.5);
    auto root = aad::DifferentiableNode<3>::make("root", {a, b, c},
        [](const auto& p, const auto& q, const auto& r) { using std::exp; return p * exp(q) / r; });
    EvalContext ctx;
    root->eval(ctx);

    Reduction f = reduce(root, {a, b, c});
    std::array<double, 3> g{};
    const double value = f.gradient({2.0, -0.3, 4.0}, g);
    expectClose(value, 2.0 * std::exp(-0.3) / 4.0);
    expectClose(g[0], std::exp(-0.3) / 4.0);
    expectClose(g[1], 2.0 * std::exp(-0.3) / 4.0);
    expectClose(g[2], -2.0 * std::exp(-0.3) / 16.0);
}

TEST(Reduction, OneDualCallPerDifferentiableNode) {
    auto x = Input<double>::make("x", 1.5);
    auto y = Input<double>::make("y", 0.5);
    int doubles = 0, duals = 0;
    auto root = aad::DifferentiableNode<2>::make("root", {x, y},
        [&doubles, &duals](const auto& a, const auto& b) {
            using std::exp;
            if constexpr (isDual<decltype(a)>()) ++duals;
            else ++doubles;
            return a * exp(b);
        });
    EvalContext ctx;
    root->eval(ctx);
    ASSERT_EQ(doubles, 1);

    Reduction f = reduce(root, {x, y});
    std::array<double, 2> g{};
    EXPECT_EQ(f.gradient({2.0, 0.0}, g), 2.0);
    EXPECT_EQ(doubles, 2);
    EXPECT_EQ(duals, 1);
    EXPECT_EQ(g[0], 1.0);
    EXPECT_EQ(g[1], 2.0);

    f.gradient({3.0, 0.0}, g);
    EXPECT_EQ(doubles, 3);
    EXPECT_EQ(duals, 2);
}

// A node of the suite's own: 2·(2s + 1) with s = 2x + 1 is 8x + 6.
TEST(Reduction, ANodeOfYourOwnHasAGradientThroughItsStep) {
    auto x      = Input<double>::make("x", 1.0);
    auto shared = Counted<>::make("shared", x);
    auto root   = ops::SumNode<>::make("root", {Counted<>::make("left", shared),
                                                Counted<>::make("right", shared)});
    EvalContext ctx;
    root->eval(ctx);

    Reduction f = reduce(root, {x});
    double slope = 0.0;
    EXPECT_EQ(f.gradient({2.0}, {&slope, 1}), 22.0);
    EXPECT_EQ(slope, 8.0);
}

// A step that names an input its node does not have is a bug in the step, and
// is refused by name.
TEST(Reduction, APartialOfAnInputTheNodeDoesNotHaveIsRefused) {
    auto x    = Input<double>::make("x", 1.0);
    auto root = Counted<>::make("node.misnamed", x, Gives::AStepThatMisnamesItsInput);
    EvalContext ctx;
    root->eval(ctx);

    Reduction f = reduce(root, {x});
    EXPECT_EQ(f({2.0}), 5.0);
    double slope = 0.0;
    const std::string why = messageOf<std::logic_error>([&] { f.gradient({2.0}, {&slope, 1}); });
    EXPECT_TRUE(mentions(why, "node.misnamed names input 1 of its 1")) << why;
}

// Newton's method on price(vol) − 12, from 0.2. The call is worth 12.34 at the
// vol the graph holds, 0.25.
TEST(Reduction, NewtonFindsAnImpliedVol) {
    const CallPrice c;
    EvalContext ctx;
    c.call->eval(ctx);
    Reduction f = reduce(c.call, {c.vol});

    const double target = 12.0;
    double vol = 0.2, slope = 0.0;
    int steps = 0;
    for (; steps < 8; ++steps) {
        const double miss = f.gradient({vol}, {&slope, 1}) - target;
        if (std::abs(miss) < 1e-12) break;
        vol -= miss / slope;
    }
    EXPECT_LT(steps, 8);
    EXPECT_GT(vol, 0.2);
    EXPECT_LT(vol, 0.25);

    // The graph agrees, once vol is set to the answer.
    c.vol->set(vol);
    const ValuePtr solved = c.call->eval(ctx);
    EXPECT_NEAR(get_value<double>(solved), target, 1e-12);
}

// ─────────────────────────────────────────────────────────────────────────────
// Constants and barriers
// ─────────────────────────────────────────────────────────────────────────────

// An EWMANode ticks each time it evaluates. Ten calls do not tick it: a control
// fed the same values holds the same level, before and after one more tick.
TEST(Reduction, ANodeNoArgumentReachesIsAConstant) {
    auto s       = Input<double>::make("s", 1.0);
    auto ewma    = ts::EWMANode::make("ewma", s, 0.5);
    auto x       = Input<double>::make("x", 2.0);
    auto root    = ops::ProductNode<>::make("root", {ewma, x});
    auto sCtl    = Input<double>::make("sCtl", 1.0);
    auto control = ts::EWMANode::make("control", sCtl, 0.5);
    EvalContext ctx;
    for (const double v : {1.0, 2.0, 4.0}) {
        s->set(v);
        sCtl->set(v);
        root->eval(ctx);
        control->eval(ctx);
    }
    const ValuePtr level = ewma->eval(ctx);

    Reduction f = reduce(root, {x});
    EXPECT_EQ(f.size(), 1u);
    for (int i = 0; i < 10; ++i) EXPECT_EQ(f({3.0 + i}), get_value<double>(level) * (3.0 + i));

    EXPECT_EQ(get_value<double>(ewma->eval(ctx)), get_value<double>(control->eval(ctx)));
    s->set(8.0);
    sCtl->set(8.0);
    root->eval(ctx);
    EXPECT_EQ(get_value<double>(ewma->eval(ctx)), get_value<double>(control->eval(ctx)));
}

// The structure is no barrier: the constructor accepts it. The point is one,
// because the argument moves a node that has no function to call.
TEST(Reduction, ANodeThatCannotBeCalledIsABarrier) {
    auto x    = Input<double>::make("arg.x", 2.0);
    auto ewma = ts::EWMANode::make("node.ewma", x, 0.5);
    auto root = ops::NegateNode<>::make("root.out", ewma);
    EvalContext ctx;
    root->eval(ctx);

    Reduction f(root, {x});
    const std::string why = messageOf<std::domain_error>([&] { f.bind(); });
    EXPECT_TRUE(mentions(why, "arg.x reaches root root.out through node.ewma")) << why;
    EXPECT_TRUE(mentions(why, "is not an IReducible")) << why;

    // An IReducible that gives no step is one too.
    auto silent  = Counted<>::make("node.silent", x, Gives::NoStep);
    auto negated = ops::NegateNode<>::make("root.negated", silent);
    negated->eval(ctx);
    Reduction g(negated, {x});
    const std::string whyNot = messageOf<std::domain_error>([&] { g.bind(); });
    EXPECT_TRUE(mentions(whyNot, "arg.x reaches root root.negated through node.silent")) << whyNot;
    EXPECT_TRUE(mentions(whyNot, "gives no step")) << whyNot;
}

// The Chooser takes `taken`, so it never pulls `other`, which stays dirty.
// `other` is a barrier over the argument, and the bind does not reach it.
TEST(Reduction, FollowsOnlyTheInputsANodeReads) {
    auto x     = Input<double>::make("x", 2.0);
    auto which = Input<double>::make("which", 0.0);
    auto taken = ops::NegateNode<>::make("taken", x);
    auto other = ts::EWMANode::make("other", x, 0.5);
    auto root  = Chooser::make("root", which, taken, other);
    EvalContext ctx;
    root->eval(ctx);
    ASSERT_TRUE(other->dirty());

    Reduction f = reduce(root, {x});
    EXPECT_EQ(f.size(), 2u);   // taken and the Chooser
    EXPECT_EQ(f({5.0}), -5.0);
    EXPECT_TRUE(other->dirty());
    EXPECT_FALSE(other->isInitialized());
}

// 0·x chooses, so the argument moves what decides which input is read.
TEST(Reduction, ADecidingInputAnArgumentMovesIsABarrier) {
    auto x     = Input<double>::make("arg.x", 2.0);
    auto zero  = Input<double>::make("zero", 0.0);
    auto which = ops::ProductNode<>::make("node.which", {zero, x});
    auto first = Input<double>::make("first", 10.0);
    auto root  = Chooser::make("root.out", which, first, Input<double>::make("second", 20.0));
    EvalContext ctx;
    root->eval(ctx);

    Reduction f(root, {x});
    const std::string why = messageOf<std::domain_error>([&] { f.bind(); });
    EXPECT_TRUE(mentions(why, "arg.x moves node.which, which decides what root.out reads")) << why;

    // With a deciding input no argument moves, the same Chooser is called.
    Reduction ofFirst = reduce(root, {first});
    EXPECT_EQ(ofFirst.size(), 1u);
    EXPECT_EQ(ofFirst({11.0}), 11.0);
}

// Held, the Chooser reads nothing, so it is a constant, although the argument
// is above it. Its inputs are never pulled, and the barrier among them is never
// reached: the EWMA is still as it was built.
TEST(Reduction, ANodeThatReadsNothingIsAConstant) {
    auto x      = Input<double>::make("x", 2.0);
    auto ewma   = ts::EWMANode::make("ewma", x, 0.5);
    auto held   = Chooser::make("held", Input<double>::make("which", 0.0), ewma, x);
    auto root   = ops::ProductNode<>::make("root", {held, x});
    held->hold(7.0);
    EvalContext ctx;
    root->eval(ctx);

    Reduction f = reduce(root, {x});
    EXPECT_EQ(f.size(), 1u);
    EXPECT_EQ(f({3.0}), 21.0);
    EXPECT_TRUE(ewma->dirty());
    EXPECT_FALSE(ewma->isInitialized());
}

// ─────────────────────────────────────────────────────────────────────────────
// What a step and a node are handed
// ─────────────────────────────────────────────────────────────────────────────

TEST(ReduceContract, InputsAreAViewOfTheReductionsValues) {
    const std::vector<ValuePtr> values{make_value(1.5), nullptr, make_value(2.5)};
    const std::vector<std::size_t> positions{2, 0, 1, 2};
    const ReduceInputs in(values, positions);
    ASSERT_EQ(in.size(), 4u);
    EXPECT_EQ(in[0].get(), values[2].get());
    EXPECT_EQ(in[1].get(), values[0].get());
    EXPECT_EQ(in[2], nullptr);
    EXPECT_EQ(in[3].get(), values[2].get());
}

TEST(ReduceContract, ReadsRecordsWhatANodeNames) {
    using Use = ReduceReads::Use;
    std::vector<Use> uses(3, Use::Unread);
    ReduceReads reads(uses);
    ASSERT_EQ(reads.size(), 3u);
    reads.reads(2);
    reads.decides(0);
    reads.reads(0);   // a deciding input stays one
    EXPECT_EQ(uses, (std::vector<Use>{Use::Decides, Use::Unread, Use::Read}));
    EXPECT_THROW(reads.reads(3), std::out_of_range);
    EXPECT_THROW(reads.decides(3), std::out_of_range);
    EXPECT_EQ(uses, (std::vector<Use>{Use::Decides, Use::Unread, Use::Read}));
}

// IReducible's default, on a node that does not override it.
TEST(ReduceContract, ANodeReadsEveryInputUnlessItSaysOtherwise) {
    using Use = ReduceReads::Use;
    auto x   = Input<double>::make("x", 1.0);
    auto sum = ops::SumNode<>::make("sum", {x, x, x});
    std::vector<Use> uses(3, Use::Unread);
    ReduceReads reads(uses);
    EvalContext ctx;
    sum->reduceReads(ctx, reads);
    EXPECT_EQ(uses, std::vector<Use>(3, Use::Read));
}

// ─────────────────────────────────────────────────────────────────────────────
// Allocation
// ─────────────────────────────────────────────────────────────────────────────

// x·y + exp(x)/y over x: an n-ary, a binary and a unary step, and y a constant.
TEST(Reduction, AWarmCallAndBindAllocateNothing) {
    const aad_test::SumOfTerms g;
    EvalContext ctx;
    g.root->eval(ctx);
    Reduction f = reduce(g.root, {g.x});
    for (int i = 0; i < 3; ++i) f({1.0 + i});   // every step has both of its buffers

    double sum = 0.0;
    const long calls = allocations_during([&] {
        for (int i = 0; i < 100; ++i) sum += f({1.0 + 0.01 * i});
    });
    EXPECT_EQ(calls, 0);
    const long binds = allocations_during([&] {
        for (int i = 0; i < 100; ++i) {
            f.bind();
            sum += f({2.0});
        }
    });
    EXPECT_EQ(binds, 0);
    EXPECT_NE(sum, 0.0);

    // The first bind of a reduction that was built without one.
    Reduction unbound(g.root, {g.x, g.y});
    EXPECT_EQ(allocations_during([&] { unbound.bind(); }), 0);
    EXPECT_EQ(unbound.size(), 4u);
}

// x·y + exp(x)/y over both: an n-ary product and an n-ary sum, whose partials
// need every input at once, and a quotient and an exponential.
TEST(Reduction, AWarmGradientAllocatesNothing) {
    const aad_test::SumOfTerms g;
    EvalContext ctx;
    g.root->eval(ctx);
    Reduction f = reduce(g.root, {g.x, g.y});
    std::array<double, 2> gradient{};
    for (int i = 0; i < 3; ++i) f.gradient({1.0 + i, 0.5}, gradient);

    double sum = 0.0;
    const long allocs = allocations_during([&] {
        for (int i = 0; i < 100; ++i)
            sum += f.gradient({1.0 + 0.01 * i, 0.7}, gradient) + gradient[0] + gradient[1];
    });
    EXPECT_EQ(allocs, 0);
    EXPECT_NE(sum, 0.0);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
