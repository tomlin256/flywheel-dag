// =============================================================================
// flywheel-dag — A header-only C++23 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

// A tape that is kept: recorded again at each point, swept into a caller's
// span, and, once it has met the nodes a point reaches, without allocating.
//
// Allocations are counted by allocation_counter.cpp, which this suite links.

#include <gtest/gtest.h>
#include "aad_test_graphs.hpp"
#include "allocation_counter.hpp"
#include "flywheel/dag.hpp"
#include "flywheel/dag_aad.hpp"
#include "flywheel/dag_ops.hpp"
#include "flywheel/dag_timeseries.hpp"
#include <array>
#include <cmath>
#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

using namespace dag;
using allocation_counter::allocations_during;
using Sensitivities = std::vector<double>;

namespace {

/// A Black–Scholes call, from ops and differentiable nodes: seventeen nodes,
/// five of them inputs.
struct CallPrice {
    InputPtr<double> spot   = Input<double>::make("spot", 100.0);
    InputPtr<double> strike = Input<double>::make("strike", 95.0);
    InputPtr<double> vol    = Input<double>::make("vol", 0.25);
    InputPtr<double> expiry = Input<double>::make("expiry", 0.75);
    InputPtr<double> rate   = Input<double>::make("rate", 0.03);
    NodePtr root;

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
        root = ops::DiffNode<>::make("call",
            ops::ProductNode<>::make("spot.N(d1)", {spot, nd1}),
            ops::ProductNode<>::make("strike.discount.N(d2)", {strike, discount, nd2}));
    }

    std::vector<InputPtr<double>> inputs() const { return {spot, strike, vol, expiry, rate}; }
};

/// x·y + x while the flag is set, and exp(x)/y + x while it is not. The two
/// branches share no node but the inputs.
struct Branching {
    InputPtr<bool>   flag = Input<bool>::make("flag", true);
    InputPtr<double> x    = Input<double>::make("x", 1.5);
    InputPtr<double> y    = Input<double>::make("y", 0.5);
    NodePtr onTrue  = ops::ProductNode<>::make("x.y", {x, y});
    NodePtr onFalse = ops::DivideNode<>::make("exp(x)/y", ops::ExpNode<>::make("exp(x)", x), y);
    NodePtr root    = ops::SumNode<>::make("root",
        {ConditionNode::make("pick", flag, onTrue, onFalse), x});

    std::vector<InputPtr<double>> inputs() const { return {x, y}; }
};

/// The gradient and, for a seed of 1 on each input in turn, the tangents, by the
/// sweeps that write into a span.
struct Swept {
    Sensitivities              adjoints;
    std::vector<Sensitivities> tangents;   ///< one per input: the roots' tangents
    bool operator==(const Swept&) const = default;
};

Swept sweepKept(aad::Tape& tape, const std::vector<NodePtr>& roots,
                const std::vector<NodePtr>& wrt) {
    Swept s;
    s.adjoints.assign(wrt.size(), -1.0);
    tape.adjoints(roots.front(), wrt, s.adjoints);
    for (const auto& w : wrt) {
        const std::array<aad::Seed, 1> seed{aad::Seed{w, 1.0}};
        Sensitivities t(roots.size(), -1.0);
        tape.tangents(seed, t);
        s.tangents.push_back(t);
    }
    return s;
}

/// The same from a new tape, by the sweeps that return a vector.
Swept sweepNew(const std::vector<NodePtr>& roots, const std::vector<NodePtr>& wrt) {
    const aad::Tape tape(roots);
    Swept s;
    s.adjoints = tape.adjoints(roots.front(), wrt);
    for (const auto& w : wrt) s.tangents.push_back(tape.tangents({{w, 1.0}}));
    return s;
}

/// A kept tape against a new one, at the point the graph holds and at three
/// more, each recorded again once the inputs are there.
template<typename Graph>
void expectANewTapesSweeps(const Graph& g) {
    const std::vector<InputPtr<double>> ins = g.inputs();
    const std::vector<NodePtr> wrt   = aad_test::asNodes(ins);
    const std::vector<NodePtr> roots = {g.root};
    EvalContext ctx;
    g.root->eval(ctx);
    aad::Tape tape(roots);
    EXPECT_EQ(sweepKept(tape, roots, wrt), sweepNew(roots, wrt));

    for (int step = 1; step <= 3; ++step) {
        SCOPED_TRACE("step " + std::to_string(step));
        for (const auto& in : ins) in->set(in->get() * (1.0 + 0.01 * step) + 0.001);
        g.root->eval(ctx);
        tape.record();
        const Swept made = sweepNew(roots, wrt);
        EXPECT_EQ(sweepKept(tape, roots, wrt), made);
        EXPECT_EQ(tape.size(), aad::Tape(roots).size());
        // And the sweeps that return a vector read the new recording too.
        EXPECT_EQ(tape.adjoints(g.root, wrt), made.adjoints);
    }
}

std::string messageOf(const std::function<void()>& fn) {
    try {
        fn();
    } catch (const std::exception& e) {
        return e.what();
    }
    return "nothing was thrown";
}

/// A node of the suite's own, 3·a, that counts the calls to its inputs().
class CountsItsInputsCalls
    : public NodeBase
    , public std::enable_shared_from_this<CountsItsInputsCalls>
{
public:
    static std::shared_ptr<CountsItsInputsCalls> make(NodePtr in) {
        auto self = std::shared_ptr<CountsItsInputsCalls>(new CountsItsInputsCalls(std::move(in)));
        wire(self, self->inputs());
        return self;
    }
    int inputsCalls() const noexcept { return *inputsCalls_; }

    ValuePtr eval(EvalContext& ctx) override {
        if (!dirty() && !ctx.forceRecompute) return cached_;
        beginEval();
        const ValuePtr v = in_->eval(ctx);
        cached_ = slot_.emit(3.0 * get_value<double>(v));
        notifyDownstream();
        endEval();
        return cached_;
    }
    std::string name() const override { return "3a"; }
    std::vector<NodePtr> inputs() const override {
        ++*inputsCalls_;
        return {in_};
    }
    NodeKind kind() const override { return NodeKind::Compute; }
    bool partials(EvalContext&, aad::Partials& out) override {
        out.add(0, 3.0);
        return true;
    }

private:
    explicit CountsItsInputsCalls(NodePtr in) : in_(std::move(in)) {}

    NodePtr           in_;
    ValuePtr          cached_;
    ValueSlot<double> slot_;
    /// Held through a pointer, so that inputs(), which is const, can count.
    std::shared_ptr<int> inputsCalls_ = std::make_shared<int>(0);
};

/// 2·a, whose partials name its one input, or, once told to, an input it does
/// not have: a record that reaches it then throws part of the way through.
class Doubles : public NodeBase, public std::enable_shared_from_this<Doubles> {
public:
    static std::shared_ptr<Doubles> make(NodePtr in) {
        auto self = std::shared_ptr<Doubles>(new Doubles(std::move(in)));
        wire(self, self->inputs());
        return self;
    }
    void misname(bool on) noexcept { misnames_ = on; }

    ValuePtr eval(EvalContext& ctx) override {
        if (!dirty() && !ctx.forceRecompute) return cached_;
        beginEval();
        const ValuePtr v = in_->eval(ctx);
        cached_ = slot_.emit(2.0 * get_value<double>(v));
        notifyDownstream();
        endEval();
        return cached_;
    }
    std::string name() const override { return "2a"; }
    std::vector<NodePtr> inputs() const override { return {in_}; }
    NodeKind kind() const override { return NodeKind::Compute; }
    bool partials(EvalContext&, aad::Partials& out) override {
        out.add(misnames_ ? 7 : 0, 2.0);
        return true;
    }

private:
    explicit Doubles(NodePtr in) : in_(std::move(in)) {}

    NodePtr           in_;
    ValuePtr          cached_;
    ValueSlot<double> slot_;
    bool              misnames_ = false;
};

/// A float as a double, with a partial of 1, as a node that derives INode and
/// the IDifferentiable mixin and no NodeBase. It keeps the dirty protocol by
/// hand. No node of the engine puts a non-double on a tape, or gives its
/// partials through the mixin alone, but an application's could do both.
class Widen : public INode, public aad::IDifferentiable,
              public std::enable_shared_from_this<Widen> {
public:
    static std::shared_ptr<Widen> make(NodePtr in) {
        auto self = std::shared_ptr<Widen>(new Widen(std::move(in)));
        self->in_->addDownstream(self);
        return self;
    }
    ValuePtr eval(EvalContext& ctx) override {
        if (!dirty_ && !ctx.forceRecompute) return cached_;
        const ValuePtr v = in_->eval(ctx);
        cached_ = make_value<double>(get_value<float>(v));
        dirty_  = false;
        for (auto& w : downstream_) if (auto n = w.lock()) n->invalidate();
        return cached_;
    }
    std::string name() const override { return "widen"; }
    std::vector<NodePtr> inputs() const override { return {in_}; }
    void invalidate() override { mark(); }
    void invalidateMaybe() override { mark(); }
    bool dirty() const override { return dirty_; }
    void addDownstream(std::weak_ptr<INode> n) override { downstream_.push_back(std::move(n)); }
    NodeKind kind() const override { return NodeKind::Compute; }
    bool partials(EvalContext&, aad::Partials& out) override {
        out.add(0, 1.0);
        return true;
    }

private:
    explicit Widen(NodePtr in) : in_(std::move(in)) {}

    void mark() {
        if (dirty_) return;
        dirty_ = true;
        for (auto& w : downstream_) if (auto n = w.lock()) n->invalidateMaybe();
    }

    NodePtr                           in_;
    ValuePtr                          cached_;
    bool                              dirty_ = true;
    std::vector<std::weak_ptr<INode>> downstream_;
};

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// A tape recorded again
// ─────────────────────────────────────────────────────────────────────────────

// Bit for bit: a kept tape walks the roots as a new one does, so its entries
// are in the same order and its sums are taken in the same order.
TEST(AadTape, ARecordGivesWhatANewTapeGives) {
    expectANewTapesSweeps(aad_test::SumOfTerms{});
    expectANewTapesSweeps(aad_test::LogTimesRoot{});
    expectANewTapesSweeps(aad_test::PowerOfDifference{});
    expectANewTapesSweeps(aad_test::TrigOfRatio{});
    expectANewTapesSweeps(aad_test::InverseTrig{});
    expectANewTapesSweeps(aad_test::PolarRoundTrip{});
    expectANewTapesSweeps(CallPrice{});
}

TEST(AadTape, SeveralRootsAreRecordedAgainInTheirOrder) {
    aad_test::SumOfTerms s;
    aad_test::LogTimesRoot l;
    auto both = ops::ProductNode<>::make("both", {s.root, l.root});
    EvalContext ctx;
    both->eval(ctx);
    const std::vector<NodePtr> roots = {s.root, both};
    const std::vector<NodePtr> wrt   = {s.x, s.y, l.x, l.y};
    aad::Tape tape(roots);

    s.x->set(0.9);
    l.y->set(2.2);
    both->eval(ctx);
    tape.record();
    const aad::Tape made(roots);
    Sensitivities gs(wrt.size()), gb(wrt.size());
    tape.adjoints(s.root, wrt, gs);
    tape.adjoints(both, wrt, gb);
    EXPECT_EQ(gs, made.adjoints(s.root, wrt));
    EXPECT_EQ(gb, made.adjoints(both, wrt));
    EXPECT_EQ(gs[2], 0.0);   // the first root does not reach the second graph
    EXPECT_EQ(gs[3], 0.0);
    const std::vector<aad::Seed> seeds = {{s.x, 1.0}, {l.y, -2.0}};
    Sensitivities t(roots.size());
    tape.tangents(seeds, t);
    EXPECT_EQ(t, made.tangents(seeds));
}

// The walk follows what each node's partials name at the new point.
TEST(AadTape, ABranchSwitchIsFollowedByTheNextRecord) {
    const Branching g;
    const std::vector<NodePtr> wrt = {g.x, g.y};
    EvalContext ctx;
    g.root->eval(ctx);
    aad::Tape tape({g.root});
    Sensitivities grad(2);
    tape.adjoints(g.root, wrt, grad);
    EXPECT_EQ(grad, (Sensitivities{0.5 + 1.0, 1.5}));
    const std::size_t onTrue = tape.size();
    // The branch taken is on the tape: the root moves one for one with it.
    const std::vector<NodePtr> wrtBranch = {g.onTrue};
    const std::vector<aad::Seed> seedBranch = {{g.onTrue, 1.0}};
    Sensitivities one(1);
    tape.adjoints(g.root, wrtBranch, one);
    EXPECT_EQ(one[0], 1.0);
    tape.tangents(seedBranch, one);
    EXPECT_EQ(one[0], 1.0);

    g.flag->set(false);
    g.root->eval(ctx);
    tape.record();
    tape.adjoints(g.root, wrt, grad);
    EXPECT_EQ(grad, aad::adjoints(g.root, wrt));
    EXPECT_DOUBLE_EQ(grad[0], std::exp(1.5) / 0.5 + 1.0);
    EXPECT_DOUBLE_EQ(grad[1], -std::exp(1.5) / 0.25);
    EXPECT_EQ(tape.size(), onTrue + 1);   // exp(x) and exp(x)/y in place of x·y
    // The branch not taken has left the tape, though the tape has met it: the
    // root does not depend on it here.
    tape.adjoints(g.root, wrtBranch, one);
    EXPECT_EQ(one[0], 0.0);
    tape.tangents(seedBranch, one);
    EXPECT_EQ(one[0], 0.0);

    g.flag->set(true);
    g.root->eval(ctx);
    tape.record();
    tape.adjoints(g.root, wrt, grad);
    EXPECT_EQ(grad, (Sensitivities{0.5 + 1.0, 1.5}));
    EXPECT_EQ(tape.size(), onTrue);
}

// A tweaked node names no input, and an untweaked one is a barrier.
TEST(AadTape, ATweakIsFollowedByTheNextRecord) {
    auto x      = Input<double>::make("x", 2.0);
    auto k      = Input<double>::make("k", 3.0);
    auto opaque = TweakableComputeNode<double, double>::make("opaque", {x},
        [](const double& v) { return v * v; });
    auto root   = ops::ProductNode<>::make("root", {opaque, k});
    const std::vector<NodePtr> wrt = {x, k};
    EvalContext ctx;
    root->eval(ctx);
    aad::Tape tape({root});
    Sensitivities grad(2);
    EXPECT_THROW(tape.adjoints(root, wrt, grad), std::domain_error);

    opaque->tweak(10.0);
    root->eval(ctx);
    tape.record();
    tape.adjoints(root, wrt, grad);
    EXPECT_EQ(grad, (Sensitivities{0.0, 10.0}));

    opaque->clearTweak();
    root->eval(ctx);
    tape.record();
    EXPECT_THROW(tape.adjoints(root, wrt, grad), std::domain_error);
    const std::vector<NodePtr> onlyK = {k};
    Sensitivities one(1);
    tape.adjoints(root, onlyK, one);
    EXPECT_EQ(one, (Sensitivities{4.0}));
}

// A record holds partials, and a stateful node's partial is its last update's.
TEST(AadTape, AStatefulNodeIsRecordedAtItsCurrentState) {
    auto x     = Input<double>::make("x", 2.0, std::make_shared<AlwaysChangedPolicy>());
    auto level = ts::EWMANode::make("level", x, 0.25);
    auto root  = ops::ProductNode<>::make("root", {level, level});
    const std::vector<NodePtr> wrt = {x};
    EvalContext ctx;
    root->eval(ctx);
    aad::Tape tape({root});
    Sensitivities grad(1);
    tape.adjoints(root, wrt, grad);
    EXPECT_EQ(grad[0], 2.0 * 2.0 * 1.0);   // the first sample is taken whole

    x->set(6.0);
    root->eval(ctx);
    tape.record();
    tape.adjoints(root, wrt, grad);
    EXPECT_EQ(grad[0], 2.0 * 3.0 * 0.25);
}

// A graph is not rewired once it is built, so a tape reads a node's inputs()
// the first time it meets the node, and not again.
TEST(AadTape, ANodesInputsAreReadOnce) {
    auto x     = Input<double>::make("x", 2.0);
    auto node  = CountsItsInputsCalls::make(x);
    auto above = ComputeNode<double, double>::make("opaque", {node},
        [](const double& v) { return v + 1.0; });
    auto root  = ops::SumNode<>::make("root", {node, above});
    const std::vector<NodePtr> wrt = {root};
    EvalContext ctx;
    root->eval(ctx);
    const int before = node->inputsCalls();
    aad::Tape tape({root});
    Sensitivities one(1);
    for (int i = 0; i < 5; ++i) {
        x->set(3.0 + i);
        root->eval(ctx);
        tape.record();
        tape.adjoints(root, wrt, one);   // the barrier check walks above `above`, to the node
        EXPECT_EQ(one[0], 1.0);
    }
    EXPECT_EQ(node->inputsCalls(), before + 1);
}

// A tape holds places, not pointers into itself, so a copy is a tape of its own.
TEST(AadTape, ACopyIsATapeOfItsOwn) {
    const aad_test::SumOfTerms g;
    const std::vector<NodePtr> wrt = aad_test::asNodes(g.inputs());
    EvalContext ctx;
    g.root->eval(ctx);
    aad::Tape first({g.root});
    const Sensitivities atFirst = first.adjoints(g.root, wrt);

    aad::Tape second = first;
    g.x->set(2.0);
    g.root->eval(ctx);
    second.record();
    Sensitivities kept(wrt.size());
    first.adjoints(g.root, wrt, kept);
    EXPECT_EQ(kept, atFirst);   // the first still describes the point it recorded
    second.adjoints(g.root, wrt, kept);
    EXPECT_EQ(kept, aad::adjoints(g.root, wrt));
    EXPECT_NE(kept, atFirst);
}

// ─────────────────────────────────────────────────────────────────────────────
// Rejections
// ─────────────────────────────────────────────────────────────────────────────

TEST(AadTape, ARecordThatThrowsLeavesNoRecording) {
    const aad_test::SumOfTerms g;
    const std::vector<NodePtr> wrt = aad_test::asNodes(g.inputs());
    const std::vector<aad::Seed> seeds = {{g.x, 1.0}};
    EvalContext ctx;
    g.root->eval(ctx);
    aad::Tape tape({g.root});
    Sensitivities grad(2), tangent(1);

    g.x->set(2.0);
    ASSERT_TRUE(g.root->dirty());
    EXPECT_THROW(tape.record(), std::invalid_argument);
    EXPECT_TRUE(g.root->dirty());   // a record evaluates no root
    EXPECT_THROW(tape.adjoints(g.root, wrt, grad), std::logic_error);
    EXPECT_THROW(tape.adjoints(g.root, wrt), std::logic_error);
    EXPECT_THROW(tape.tangents(seeds, tangent), std::logic_error);
    EXPECT_THROW(tape.tangents(seeds), std::logic_error);

    g.root->eval(ctx);
    tape.record();
    tape.adjoints(g.root, wrt, grad);
    EXPECT_EQ(grad, aad::adjoints(g.root, wrt));
    tape.tangents(seeds, tangent);
    EXPECT_EQ(tangent, aad::tangents({g.root}, seeds));
}

// A later root that is dirty leaves the tape without a recording too, though
// the first root was recorded.
TEST(AadTape, EveryRootMustBeCleanForARecord) {
    auto x = Input<double>::make("x", 2.0);
    auto y = Input<double>::make("y", 3.0);
    auto a = ops::ProductNode<>::make("a", {x, x});
    auto b = ops::ProductNode<>::make("b", {y, y});
    const std::vector<NodePtr> wrt = {x, y};
    EvalContext ctx;
    a->eval(ctx);
    b->eval(ctx);
    aad::Tape tape({a, b});
    y->set(4.0);
    EXPECT_THROW(tape.record(), std::invalid_argument);
    Sensitivities grad(2);
    EXPECT_THROW(tape.adjoints(a, wrt, grad), std::logic_error);
    b->eval(ctx);
    tape.record();
    tape.adjoints(b, wrt, grad);
    EXPECT_EQ(grad, (Sensitivities{0.0, 8.0}));
}

TEST(AadTape, ASpanSweepWantsOneSlotPerResult) {
    const aad_test::SumOfTerms g;
    const std::vector<NodePtr> wrt = aad_test::asNodes(g.inputs());
    const std::vector<aad::Seed> seeds = {{g.x, 1.0}};
    EvalContext ctx;
    g.root->eval(ctx);
    aad::Tape tape({g.root});
    Sensitivities one(1), two(2), three(3);
    EXPECT_THROW(tape.adjoints(g.root, wrt, one), std::invalid_argument);
    EXPECT_THROW(tape.adjoints(g.root, wrt, three), std::invalid_argument);
    EXPECT_NO_THROW(tape.adjoints(g.root, wrt, two));
    EXPECT_THROW(tape.tangents(seeds, two), std::invalid_argument);
    EXPECT_NO_THROW(tape.tangents(seeds, one));
    // No wrt node and no seed are no error, as for the sweeps that return a vector.
    EXPECT_NO_THROW(tape.adjoints(g.root, std::span<const NodePtr>{}, std::span<double>{}));
    EXPECT_NO_THROW(tape.tangents(std::span<const aad::Seed>{}, one));
    EXPECT_EQ(one[0], 0.0);
}

// The span sweeps refuse what the sweeps that return a vector refuse, in the
// same words.
TEST(AadTape, ASpanSweepRefusesWhatAVectorSweepRefuses) {
    auto x      = Input<double>::make("x", 2.0);
    auto y      = Input<double>::make("y", 3.0);
    auto f      = Input<float>::make("f", 1.5f);
    auto opaque = ComputeNode<double, double>::make("opaque", {x},
        [](const double& v) { return v * v; });
    auto widen  = Widen::make(f);
    auto root   = ops::SumNode<>::make("root", {opaque, y, widen});
    auto beside = ops::ProductNode<>::make("beside", {y, y});
    EvalContext ctx;
    root->eval(ctx);
    beside->eval(ctx);
    aad::Tape tape({root, beside});
    const aad::Tape& sameTape = tape;
    Sensitivities one(1), two(2);

    // A wrt or seed node above a barrier the sweep reaches.
    const std::vector<NodePtr> wrtX = {x};
    const std::string reverse = messageOf([&] { sameTape.adjoints(root, wrtX); });
    EXPECT_NE(reverse.find("x reaches root root through opaque, which has no partials"),
              std::string::npos) << reverse;
    EXPECT_EQ(messageOf([&] { tape.adjoints(root, wrtX, one); }), reverse);
    EXPECT_THROW(tape.adjoints(root, wrtX, one), std::domain_error);

    const std::vector<aad::Seed> seedX = {{x, 1.0}};
    const std::string forward = messageOf([&] { sameTape.tangents(seedX); });
    EXPECT_NE(forward.find("x reaches a root through opaque, which has no partials"),
              std::string::npos) << forward;
    EXPECT_EQ(messageOf([&] { tape.tangents(seedX, two); }), forward);
    EXPECT_THROW(tape.tangents(seedX, two), std::domain_error);

    // A reverse sweep counts only the barriers its root reaches: `beside`
    // reaches none.
    tape.adjoints(beside, wrtX, one);
    EXPECT_EQ(one[0], 0.0);
    // A barrier with no wrt node above it is a constant, and a barrier can be
    // a wrt node itself.
    const std::vector<NodePtr> wrtBarrier = {opaque, y};
    tape.adjoints(root, wrtBarrier, two);
    EXPECT_EQ(two, (Sensitivities{1.0, 1.0}));

    // A null node, a node on the tape that holds no double, and a root of
    // another tape.
    const std::vector<NodePtr> wrtNull = {nullptr};
    EXPECT_EQ(messageOf([&] { tape.adjoints(root, wrtNull, one); }),
              messageOf([&] { sameTape.adjoints(root, wrtNull); }));
    EXPECT_THROW(tape.adjoints(root, wrtNull, one), std::invalid_argument);
    const std::vector<NodePtr> wrtFloat = {f};
    EXPECT_EQ(messageOf([&] { tape.adjoints(root, wrtFloat, one); }),
              messageOf([&] { sameTape.adjoints(root, wrtFloat); }));
    EXPECT_THROW(tape.adjoints(root, wrtFloat, one), std::invalid_argument);
    // The node above it gives its partial through the mixin, and is on the tape.
    const std::vector<NodePtr> wrtWiden = {widen, y};
    tape.adjoints(root, wrtWiden, two);
    EXPECT_EQ(two, (Sensitivities{1.0, 1.0}));
    EXPECT_EQ(messageOf([&] { tape.adjoints(opaque, wrtX, one); }),
              messageOf([&] { sameTape.adjoints(opaque, wrtX); }));
    EXPECT_THROW(tape.adjoints(opaque, wrtX, one), std::invalid_argument);
}

// The check walks up from a barrier through nodes that are on no tape, and
// reads the inputs() of each the first time it comes to it.
TEST(AadTape, ABarrierCheckWalksEveryNodeAboveABarrier) {
    auto x      = Input<double>::make("x", 0.5);
    auto y      = Input<double>::make("y", 3.0);
    auto ex     = ops::ExpNode<>::make("exp(x)", x);
    auto minus  = ops::NegateNode<>::make("-exp(x)", ex);
    auto opaque = ComputeNode<double, double>::make("opaque", {minus},
        [](const double& v) { return v * v; });
    auto root   = ops::SumNode<>::make("root", {opaque, y});
    EvalContext ctx;
    root->eval(ctx);
    aad::Tape tape({root});
    const aad::Tape& sameTape = tape;
    Sensitivities one(1);

    for (const NodePtr& above : {NodePtr(x), NodePtr(ex), NodePtr(minus)}) {
        SCOPED_TRACE(above->name());
        const std::vector<NodePtr> wrt = {above};
        const std::string reverse = messageOf([&] { sameTape.adjoints(root, wrt); });
        EXPECT_NE(reverse.find("reaches root root through opaque, which has no partials"),
                  std::string::npos) << reverse;
        EXPECT_EQ(messageOf([&] { tape.adjoints(root, wrt, one); }), reverse);
        const std::vector<aad::Seed> seeds = {{above, 1.0}};
        const std::string forward = messageOf([&] { sameTape.tangents(seeds); });
        EXPECT_NE(forward.find("reaches a root through opaque, which has no partials"),
                  std::string::npos) << forward;
        EXPECT_EQ(messageOf([&] { tape.tangents(seeds, one); }), forward);
    }
    const std::vector<NodePtr> wrtY = {y};
    tape.adjoints(root, wrtY, one);
    EXPECT_EQ(one[0], 1.0);
}

// A record that throws part of the way through, with nodes still on the walk's
// stack, leaves nothing behind for the next one.
TEST(AadTape, ARecordThatThrowsPartWayIsForgotten) {
    auto x       = Input<double>::make("x", 2.0);
    auto y       = Input<double>::make("y", 3.0);
    auto doubles = Doubles::make(x);
    auto root    = ops::ProductNode<>::make("root", {doubles, y});
    const std::vector<NodePtr> wrt = {x, y};
    EvalContext ctx;
    root->eval(ctx);
    aad::Tape tape({root});
    Sensitivities grad(2);
    tape.adjoints(root, wrt, grad);
    EXPECT_EQ(grad, (Sensitivities{6.0, 4.0}));

    doubles->misname(true);
    EXPECT_THROW(tape.record(), std::logic_error);
    EXPECT_THROW(tape.adjoints(root, wrt, grad), std::logic_error);
    EXPECT_THROW(tape.record(), std::logic_error);   // and again, from where it stopped

    doubles->misname(false);
    x->set(5.0);
    y->set(7.0);
    root->eval(ctx);
    tape.record();
    EXPECT_EQ(tape.size(), aad::Tape({root}).size());
    tape.adjoints(root, wrt, grad);
    EXPECT_EQ(grad, (Sensitivities{14.0, 10.0}));
    EXPECT_EQ(grad, aad::adjoints(root, wrt));
}

// A partial times an adjoint or a tangent counts as 0 when either one is 0.
TEST(AadTape, AZeroPartialOrAdjointPropagatesNothing) {
    auto z    = Input<double>::make("z", 0.0);
    auto x    = Input<double>::make("x", 0.0);
    auto root = ops::ProductNode<>::make("root", {z, ops::SqrtNode<>::make("sqrt", x)});
    const std::vector<NodePtr> wrt = {z, x};
    EvalContext ctx;
    root->eval(ctx);
    aad::Tape tape({root});
    Sensitivities grad(2), tangent(1);
    tape.adjoints(root, wrt, grad);
    EXPECT_EQ(grad, (Sensitivities{0.0, 0.0}));
    const std::vector<aad::Seed> seeds = {{x, 1.0}};
    tape.tangents(seeds, tangent);
    EXPECT_EQ(tangent[0], 0.0);
}

// ─────────────────────────────────────────────────────────────────────────────
// Allocation
// ─────────────────────────────────────────────────────────────────────────────

namespace {

/// The allocations of `rounds` passes over a tape that has met its nodes: a
/// record(), a reverse sweep for every wrt node and a forward sweep for a seed
/// on each, with the inputs moved before each pass. Moving an Input allocates,
/// so only the pass is counted.
template<typename Move>
long allocationsOfWarmPasses(const std::vector<NodePtr>& roots, const std::vector<NodePtr>& wrt,
                             int rounds, Move&& move) {
    EvalContext ctx;
    for (const auto& r : roots) r->eval(ctx);
    aad::Tape tape(roots);
    std::vector<aad::Seed> seeds;
    seeds.reserve(wrt.size());
    for (const auto& w : wrt) seeds.push_back({w, 1.0});
    Sensitivities grad(wrt.size()), tangent(roots.size());
    double sum = 0.0;
    const auto pass = [&] {
        tape.record();
        tape.adjoints(roots.front(), wrt, grad);
        tape.tangents(seeds, tangent);
        sum += grad.front() + tangent.front();
    };
    pass();   // the working storage reaches its size
    long allocations = 0;
    for (int i = 0; i < rounds; ++i) {
        move(i);
        for (const auto& r : roots) r->eval(ctx);
        allocations += allocations_during(pass);
    }
    EXPECT_NE(sum, 0.0);
    return allocations;
}

} // namespace

TEST(AadTape, AWarmRecordAndItsSweepsAllocateNothing) {
    {
        SCOPED_TRACE("a call price: seventeen nodes, n-ary products, differentiable nodes");
        const CallPrice g;
        EXPECT_EQ(allocationsOfWarmPasses({g.root}, aad_test::asNodes(g.inputs()), 50,
                      [&](int i) { g.spot->set(100.0 + i); g.vol->set(0.25 + 0.001 * i); }), 0);
    }
    {
        SCOPED_TRACE("an n-ary product and an n-ary sum");
        const aad_test::SumOfTerms g;
        EXPECT_EQ(allocationsOfWarmPasses({g.root}, aad_test::asNodes(g.inputs()), 50,
                      [&](int i) { g.x->set(1.3 + 0.01 * i); }), 0);
    }
    {
        SCOPED_TRACE("a condition, on the branch it took");
        const Branching g;
        EXPECT_EQ(allocationsOfWarmPasses({g.root}, aad_test::asNodes(g.inputs()), 50,
                      [&](int i) { g.y->set(0.5 + 0.01 * i); }), 0);
    }
    {
        SCOPED_TRACE("a tweaked node");
        auto x      = Input<double>::make("x", 2.0);
        auto k      = Input<double>::make("k", 3.0);
        auto opaque = TweakableComputeNode<double, double>::make("opaque", {x},
            [](const double& v) { return v * v; });
        auto root   = ops::ProductNode<>::make("root", {opaque, k});
        opaque->tweak(10.0);
        EXPECT_EQ(allocationsOfWarmPasses({root}, {x, k}, 50,
                      [&](int i) { k->set(3.0 + i); }), 0);
    }
    {
        SCOPED_TRACE("a stateful node");
        auto x     = Input<double>::make("x", 2.0);
        auto level = ts::EWMANode::make("level", x, 0.25);
        auto root  = ops::ProductNode<>::make("root", {level, level});
        EXPECT_EQ(allocationsOfWarmPasses({root}, {x}, 50,
                      [&](int i) { x->set(3.0 + i); }), 0);
    }
    {
        SCOPED_TRACE("two roots, and a barrier with nothing asked for above it");
        auto x      = Input<double>::make("x", 2.0);
        auto y      = Input<double>::make("y", 3.0);
        auto opaque = ComputeNode<double, double>::make("opaque", {x},
            [](const double& v) { return v * v; });
        auto first  = ops::SumNode<>::make("first", {opaque, y});
        auto second = ops::ProductNode<>::make("second", {first, y});
        EXPECT_EQ(allocationsOfWarmPasses({first, second}, {y, opaque}, 50,
                      [&](int i) { y->set(3.0 + i); }), 0);
    }
}

// A node the tape has not met costs it an allocation, once: its place, and its
// inputs() the first time a walk comes to it.
TEST(AadTape, TheFirstRecordThatMeetsNewNodesAllocatesAndTheNextDoesNot) {
    const Branching g;
    const std::vector<NodePtr> wrt = {g.x, g.y};
    EvalContext ctx;
    g.root->eval(ctx);
    aad::Tape tape({g.root});
    Sensitivities grad(2);
    const auto pass = [&] {
        tape.record();
        tape.adjoints(g.root, wrt, grad);
    };
    pass();
    EXPECT_EQ(allocations_during(pass), 0);

    g.flag->set(false);   // exp(x) and exp(x)/y: two nodes the tape has not met
    g.root->eval(ctx);
    EXPECT_GT(allocations_during(pass), 0);
    EXPECT_EQ(allocations_during(pass), 0);
    EXPECT_EQ(grad, aad::adjoints(g.root, wrt));

    g.flag->set(true);    // back on a branch it has met
    g.root->eval(ctx);
    EXPECT_EQ(allocations_during(pass), 0);
    g.flag->set(false);
    g.root->eval(ctx);
    EXPECT_EQ(allocations_during(pass), 0);
}

// A wrt node the tape does not hold is given a place the first time a span
// sweep names it, so that the barrier check can tell it from the nodes it
// walks.
TEST(AadTape, AWrtNodeOffTheTapeIsMetOnce) {
    const aad_test::SumOfTerms g;
    auto elsewhere = Input<double>::make("elsewhere", 1.0);
    const std::vector<NodePtr> wrt = {g.x, elsewhere};
    EvalContext ctx;
    g.root->eval(ctx);
    aad::Tape tape({g.root});
    Sensitivities grad(2);
    tape.adjoints(g.root, wrt, grad);
    EXPECT_EQ(grad[1], 0.0);
    EXPECT_EQ(allocations_during([&] { tape.adjoints(g.root, wrt, grad); }), 0);
    EXPECT_EQ(grad, aad::adjoints(g.root, wrt));
}

// An n-ary node's partials need every input's value at once. They are worked
// out in the vectors the pass lends, so a Partials that has met a list this
// long is asked again for nothing.
TEST(AadTape, AnNAryNodesPartialsUseThePassesStorage) {
    auto a = Input<double>::make("a", 2.0);
    auto b = Input<double>::make("b", 3.0);
    auto c = Input<double>::make("c", 5.0);
    auto product = ops::ProductNode<>::make("product", {a, b, c});
    auto sum     = ops::SumNode<>::make("sum", {a, b, c});
    EvalContext ctx;
    product->eval(ctx);
    sum->eval(ctx);
    aad::Partials out;
    ASSERT_TRUE(product->partials(ctx, out));
    out.clear();
    const long allocations = allocations_during([&] {
        for (int i = 0; i < 100; ++i) {
            out.clear();
            product->partials(ctx, out);
            out.clear();
            sum->partials(ctx, out);
        }
    });
    EXPECT_EQ(allocations, 0);
    out.clear();
    ASSERT_TRUE(product->partials(ctx, out));
    ASSERT_EQ(out.entries().size(), 3u);
    EXPECT_EQ(out.entries()[0].d, 15.0);
    EXPECT_EQ(out.entries()[1].d, 10.0);
    EXPECT_EQ(out.entries()[2].d, 6.0);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
