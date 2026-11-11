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
#include <cmath>
#include <cstddef>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using namespace dag;
using allocation_counter::allocations_during;

namespace {

// ─────────────────────────────────────────────────────────────────────────────
// Two nodes of the suite's own that implement IReducible, so that what bind()
// does with what a node reads is held without any engine node.
// ─────────────────────────────────────────────────────────────────────────────

/// Whether a Counted gives a step.
enum class Callable : bool { No, Yes };

/// A pure function of one input, 2·a + 1, that counts each time it is computed:
/// by its own eval() and by a step alike. In is the type its input holds.
template<typename In = double>
class Counted
    : public NodeBase
    , public IReducible
    , public std::enable_shared_from_this<Counted<In>>
{
public:
    static std::shared_ptr<Counted> make(std::string name, NodePtr in,
                                         Callable callable = Callable::Yes) {
        auto self = std::shared_ptr<Counted>(new Counted(std::move(name), std::move(in), callable));
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
        if (callable_ == Callable::No) return nullptr;
        return std::make_unique<Step>(this->shared_from_this());
    }

private:
    Counted(std::string name, NodePtr in, Callable callable)
        : NodeBase(InvalidationMode::Lazy), name_(std::move(name)), in_(std::move(in))
        , callable_(callable) {}

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
    private:
        std::shared_ptr<Counted> node_;
        ValueSlot<double>        slot_;
    };

    std::string       name_;
    NodePtr           in_;
    Callable          callable_;
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
    auto silent  = Counted<>::make("node.silent", x, Callable::No);
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

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
