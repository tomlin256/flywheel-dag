// =============================================================================
// flywheel-dag — A header-only C++17 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

// test_lazy_invalidation.cpp — the Dirtiness{Clean, Maybe, Dirty} protocol.
//
// These tests are about the CASCADE SHAPE, not about what any node computes.
// The engine's other suites already pin the values; what is new here is which
// nodes a source's invalidation reaches, in which state, and how often.

#include <gtest/gtest.h>
#include "flywheel/dag.hpp"
#include "flywheel/dag_ops.hpp"
#include "flywheel/dag_timeseries.hpp"

#include <memory>
#include <string>
#include <vector>

using namespace dag;

namespace {

// ─────────────────────────────────────────────────────────────────────────────
// ProbeNode — the smallest thing that can hold the protocol still and be looked
// at. It exposes state(), counts how many times propagate() reached it, and
// counts eval()s, none of which a production node offers.
//
// It derives from NodeBase and overrides propagate() only to count — the
// behaviour under test is entirely NodeBase's.
// ─────────────────────────────────────────────────────────────────────────────
class ProbeNode : public NodeBase, public std::enable_shared_from_this<ProbeNode> {
public:
    static std::shared_ptr<ProbeNode> make(std::string name,
                                           std::vector<NodePtr> ins = {}) {
        auto self = std::shared_ptr<ProbeNode>(
            new ProbeNode(std::move(name), std::move(ins)));
        wire(self, self->inputs());
        return self;
    }

    ValuePtr eval(EvalContext& ctx) override {
        ++evals_;
        for (auto& in : inputs_) in->eval(ctx);
        markClean();
        return cached_;
    }
    std::string name() const override { return name_; }
    std::vector<NodePtr> inputs() const override { return inputs_; }
    NodeKind kind() const override { return NodeKind::Compute; }

    Dirtiness state() const noexcept { return NodeBase::state(); }
    int propagations() const noexcept { return propagations_; }
    int evals() const noexcept { return evals_; }

    /// Put the node back in the Clean state without evaluating anything.
    void settle() noexcept { markClean(); }
    /// "My value changed" — the one hop a real eval() makes on a change.
    void publish() { notifyDownstream(); }

protected:
    void propagate(Dirtiness incoming) override {
        ++propagations_;
        NodeBase::propagate(incoming);
    }

private:
    ProbeNode(std::string n, std::vector<NodePtr> ins)
        : name_(std::move(n)), inputs_(std::move(ins)) {}

    std::string          name_;
    std::vector<NodePtr> inputs_;
    ValuePtr             cached_;
    int                  propagations_ = 0;
    int                  evals_        = 0;
};

using ProbePtr = std::shared_ptr<ProbeNode>;

/// A chain src → mid → down, every node Clean and ready to be invalidated.
struct Chain {
    ProbePtr src, mid, down;
    Chain() {
        src  = ProbeNode::make("src");
        mid  = ProbeNode::make("mid",  { src });
        down = ProbeNode::make("down", { mid });
        settle();
    }
    void settle() { src->settle(); mid->settle(); down->settle(); }
};

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// The three states, and what dirty() means now.
// ─────────────────────────────────────────────────────────────────────────────

TEST(LazyInvalidation, ANodeStartsDirtySoItsFirstEvalAlwaysRuns) {
    auto n = ProbeNode::make("n");
    EXPECT_EQ(n->state(), Dirtiness::Dirty);
    EXPECT_TRUE(n->dirty());
}

TEST(LazyInvalidation, DirtyIsTrueInBothNonCleanStates) {
    Chain c;
    ASSERT_FALSE(c.mid->dirty());
    ASSERT_EQ(c.mid->state(), Dirtiness::Clean);

    c.mid->invalidateMaybe();
    EXPECT_EQ(c.mid->state(), Dirtiness::Maybe);
    EXPECT_TRUE(c.mid->dirty()) << "Maybe must read as dirty — the engine's "
                                   "pre-eval snapshot depends on it";

    c.settle();
    c.mid->invalidate();
    EXPECT_EQ(c.mid->state(), Dirtiness::Dirty);
    EXPECT_TRUE(c.mid->dirty());
}

// ─────────────────────────────────────────────────────────────────────────────
// invalidate() — one hop Dirty, then a Maybe cascade.
// ─────────────────────────────────────────────────────────────────────────────

TEST(LazyInvalidation, InvalidateOnCleanNodeCascadesMaybeOnward) {
    Chain c;
    c.mid->invalidate();

    EXPECT_EQ(c.mid->state(),  Dirtiness::Dirty)
        << "the node told 'your input changed' is Dirty, not Maybe";
    EXPECT_EQ(c.down->state(), Dirtiness::Maybe)
        << "everything below it is only MAYBE dirty — nothing has been compared yet";
    EXPECT_EQ(c.src->state(),  Dirtiness::Clean)
        << "invalidation never travels upstream";
}

TEST(LazyInvalidation, InvalidateOnMaybeNodeUpgradesWithoutReCascading) {
    Chain c;
    c.mid->invalidateMaybe();
    ASSERT_EQ(c.mid->state(), Dirtiness::Maybe);
    const int downPropagationsAfterMaybe = c.down->propagations();

    c.mid->invalidate();

    EXPECT_EQ(c.mid->state(), Dirtiness::Dirty) << "Maybe upgrades to Dirty";
    EXPECT_EQ(c.down->propagations(), downPropagationsAfterMaybe)
        << "the maybe-cascade already went through; walking it again is pure cost";
}

TEST(LazyInvalidation, InvalidateOnAlreadyDirtyNodeIsANoOp) {
    Chain c;
    c.mid->invalidate();
    const int downPropagations = c.down->propagations();

    c.mid->invalidate();

    EXPECT_EQ(c.down->propagations(), downPropagations);
}

// ─────────────────────────────────────────────────────────────────────────────
// invalidateMaybe() — the transitive cascade, and its visited guard.
// ─────────────────────────────────────────────────────────────────────────────

TEST(LazyInvalidation, InvalidateMaybeSetsMaybeAndStopsAtTheVisitedGuard) {
    Chain c;
    c.src->invalidateMaybe();

    EXPECT_EQ(c.src->state(),  Dirtiness::Maybe);
    EXPECT_EQ(c.mid->state(),  Dirtiness::Maybe);
    EXPECT_EQ(c.down->state(), Dirtiness::Maybe);

    const int midPropagations  = c.mid->propagations();
    const int downPropagations = c.down->propagations();

    c.src->invalidateMaybe();   // src is no longer Clean

    EXPECT_EQ(c.mid->propagations(),  midPropagations)
        << "a second cascade from an already-dirty source must stop immediately";
    EXPECT_EQ(c.down->propagations(), downPropagations);
}

TEST(LazyInvalidation, DiamondLowerHalfIsWalkedOnceNotOncePerPath) {
    //        src
    //        / \        .
    //     left right
    //        \ /
    //        join
    //         |
    //        tail
    auto src   = ProbeNode::make("src");
    auto left  = ProbeNode::make("left",  { src });
    auto right = ProbeNode::make("right", { src });
    auto join  = ProbeNode::make("join",  { left, right });
    auto tail  = ProbeNode::make("tail",  { join });
    for (auto& n : { src, left, right, join, tail }) n->settle();

    src->invalidate();

    EXPECT_EQ(join->propagations(), 2)
        << "join is reached down both arms — the guard is inside it, not before it";
    EXPECT_EQ(tail->propagations(), 1)
        << "but join only cascades onward once; that is what the guard buys";
    EXPECT_EQ(tail->state(), Dirtiness::Maybe);
}

// ─────────────────────────────────────────────────────────────────────────────
// The absorbing override must hold on BOTH entry points.
//
// RateLimiterNode no longer absorbs, so TweakableComputeNode is the only
// absorber left and carries this on its own.
//
// This is the failure the single propagate() override point makes
// unrepresentable:
// a node that overrode invalidate() alone would keep its behaviour on the direct
// hop and lose it on the transitive cascade — which is the path that carries
// almost every invalidation in a real graph. Overriding propagate() cannot fail
// that way, and these two tests are what says so.
// ─────────────────────────────────────────────────────────────────────────────

TEST(LazyInvalidation, TweakedNodeAbsorbsTheTransitiveCascadeNotJustTheDirectHop) {
    auto src = Input<double>::make("src", 1.0);
    auto mid = ComputeNode<double, double>::make(
        "mid", std::make_tuple(std::static_pointer_cast<INode>(src)),
        [](const double& v) { return v * 2.0; });
    auto frozen = TweakableComputeNode<double, double>::make(
        "frozen", std::make_tuple(std::static_pointer_cast<INode>(mid)),
        [](const double& v) { return v + 1.0; });
    auto consumer = ProbeNode::make("consumer", { frozen });

    EvalContext ctx;
    consumer->eval(ctx);
    consumer->settle();
    frozen->tweak(99.0);
    consumer->settle();

    // src → mid is the DIRECT hop; mid → frozen is the transitive cascade, which
    // is where an invalidate()-only override would have leaked.
    src->set(2.0);

    EXPECT_FALSE(frozen->dirty())
        << "a frozen node's value does not depend on its inputs";
    EXPECT_EQ(consumer->state(), Dirtiness::Clean)
        << "and nothing below it has any reason to re-pull";
}

// ─────────────────────────────────────────────────────────────────────────────
// Case 8 — RateLimiterNode, which has NO invalidation override at all.
//
// It used to absorb: mark self dirty, tell downstream nothing. That is what the
// tri-state now does properly — mark self Dirty, tell downstream MAYBE — and
// "maybe" is exactly the suppression flag a limiter needs. The
// consumer is told something moved and finds out whether it matters by pulling;
// if the limiter does not emit, a Lazy consumer skips.
//
// Absorbing was not merely redundant, it was a bug: a consumer that was
// never dirtied never pulled the limiter, so the limiter never reached the
// eval() that would have released, and a limiter wired mid-graph never
// propagated its release at all.
// ─────────────────────────────────────────────────────────────────────────────
TEST(LazyInvalidation, Case8_RateLimiterForwardsMaybeAndItsLazyConsumerSkips) {
    auto src = Input<double>::make("src", 0.0);
    auto mid = ComputeNode<double, double>::make(
        "mid", std::make_tuple(std::static_pointer_cast<INode>(src)),
        [](const double& v) { return v; });
    auto limiter = ts::RateLimiterNode<double>::make("limiter", mid, 10.0);

    int consumerRuns = 0;
    auto consumer = ComputeNode<double, double>::make(
        "consumer", std::make_tuple(std::static_pointer_cast<INode>(limiter)),
        [&consumerRuns](const double& v) { ++consumerRuns; return v; },
        InvalidationMode::Lazy);

    EvalContext ctx;
    consumer->eval(ctx);
    const int primed = consumerRuns;

    src->set(0.5);   // far below minDelta

    EXPECT_TRUE(limiter->dirty())
        << "the limiter itself still has to look at the new value";
    EXPECT_TRUE(consumer->dirty())
        << "and it tells its consumer something moved — where the old absorbing "
           "override told it nothing, which is precisely why a mid-graph limiter "
           "was never pulled and never released";

    consumer->eval(ctx);
    EXPECT_EQ(consumerRuns, primed)
        << "the limiter did not emit, so the consumer resolved with no work — "
           "suppression measured on the functor, not on a flag";
    EXPECT_FALSE(consumer->dirty()) << "and it is clean again afterwards";

    src->set(50.0);  // clears minDelta
    consumer->eval(ctx);
    EXPECT_EQ(consumerRuns, primed + 1) << "and the release gets through";
}

// ─────────────────────────────────────────────────────────────────────────────
// A node whose value changed says so with invalidate(), and the transitive part
// of that is a maybe-cascade. This is the shape every eval() uses.
// ─────────────────────────────────────────────────────────────────────────────

TEST(LazyInvalidation, PublishingAChangeMarksTheDirectConsumerDirtyAndTheRestMaybe) {
    Chain c;
    c.src->publish();

    EXPECT_EQ(c.mid->state(),  Dirtiness::Dirty)
        << "mid's declared input changed — that is not a maybe";
    EXPECT_EQ(c.down->state(), Dirtiness::Maybe)
        << "down's input is mid, and mid has not been evaluated yet";
}

// ─────────────────────────────────────────────────────────────────────────────
// Everything still defaults to Eager: the skip path exists only for a node
// that opts in to Lazy.
// ─────────────────────────────────────────────────────────────────────────────

TEST(LazyInvalidation, EveryNodeConstructsEagerByDefault) {
    auto in = Input<double>::make("in", 1.0);
    auto compute = ComputeNode<double, double>::make(
        "compute", std::make_tuple(std::static_pointer_cast<INode>(in)),
        [](const double& v) { return v; });
    auto probe = ProbeNode::make("probe");

    EXPECT_EQ(in->invalidationMode(),      InvalidationMode::Eager);
    EXPECT_EQ(compute->invalidationMode(), InvalidationMode::Eager);
    EXPECT_EQ(probe->invalidationMode(),   InvalidationMode::Eager);
}

// ═════════════════════════════════════════════════════════════════════════════
// The orderings that have to hold once a node is Lazy.
//
// Every one of these counts FUNCTOR EXECUTIONS, not dirty flags. A dirty flag
// can be right for the wrong reason; the functor either ran or it did not, and
// that is what the feature is about.
//
// Case 8 (RateLimiterNode wired mid-graph) is with the protocol tests above:
// its requirement is that the limiter's release PROPAGATES.
// ═════════════════════════════════════════════════════════════════════════════

namespace {

NodePtr asNode(const std::shared_ptr<Input<double>>& n) {
    return std::static_pointer_cast<INode>(n);
}
template<typename T>
NodePtr asNode(const std::shared_ptr<T>& n) {
    return std::static_pointer_cast<INode>(n);
}

} // namespace

// ── Case 1 — the whole point of the feature: the Eager test, inverted ────────
TEST(LazyInvalidation, Case1_ConstantIntermediateStopsDownstreamRecomputing) {
    auto src = Input<double>::make("src", 0.0);
    int midRuns = 0, downRuns = 0;

    // mid's value never changes, however much src moves.
    auto mid = ComputeNode<double, double>::make(
        "mid", std::make_tuple(asNode(src)),
        [&midRuns](const double&) { ++midRuns; return 42.0; },
        InvalidationMode::Lazy);
    auto down = ComputeNode<double, double>::make(
        "down", std::make_tuple(asNode(mid)),
        [&downRuns](const double& v) { ++downRuns; return v; },
        InvalidationMode::Lazy);

    EvalContext ctx;
    down->eval(ctx);
    ASSERT_EQ(midRuns,  1);
    ASSERT_EQ(downRuns, 1);

    src->set(1.0);  down->eval(ctx);
    src->set(2.0);  down->eval(ctx);
    src->set(3.0);  down->eval(ctx);

    EXPECT_EQ(midRuns, 4) << "mid is downstream of src and must reconsider every time";
    EXPECT_EQ(downRuns, 1)
        << "mid's value never changed, so down had nothing to recompute from. "
           "This is the assertion the old protocol made impossible.";
    EXPECT_DOUBLE_EQ(get_value<double>(down->eval(ctx)), 42.0);
}

// ── Case 2 — a change reaches EVERY consumer, not just the one that pulled ───
TEST(LazyInvalidation, Case2_AChangedNodeRecomputesBothItsConsumers) {
    auto src = Input<double>::make("src", 0.0);
    int c1Runs = 0, c2Runs = 0;

    auto x = ComputeNode<double, double>::make(
        "x", std::make_tuple(asNode(src)),
        [](const double& v) { return v * 10.0; }, InvalidationMode::Lazy);
    auto c1 = ComputeNode<double, double>::make(
        "c1", std::make_tuple(asNode(x)),
        [&c1Runs](const double& v) { ++c1Runs; return v; }, InvalidationMode::Lazy);
    auto c2 = ComputeNode<double, double>::make(
        "c2", std::make_tuple(asNode(x)),
        [&c2Runs](const double& v) { ++c2Runs; return v; }, InvalidationMode::Lazy);

    EvalContext ctx;
    c1->eval(ctx); c2->eval(ctx);
    ASSERT_EQ(c1Runs, 1); ASSERT_EQ(c2Runs, 1);

    src->set(5.0);
    c1->eval(ctx);   // c1 pulls x; x changes and notifies BOTH consumers
    c2->eval(ctx);

    EXPECT_EQ(c1Runs, 2);
    EXPECT_EQ(c2Runs, 2) << "c2 did not pull x itself — notifyDownstream() has to reach it";
    EXPECT_DOUBLE_EQ(get_value<double>(c2->eval(ctx)), 50.0);
}

// ── Case 3 — and an unchanged node stops both, in EITHER evaluation order ────
class LazyFanOutOrder : public ::testing::TestWithParam<bool> {};

TEST_P(LazyFanOutOrder, Case3_AnUnchangedNodeStopsEveryConsumer) {
    auto src = Input<double>::make("src", 0.0);
    int c1Runs = 0, c2Runs = 0;

    auto x = ComputeNode<double, double>::make(
        "x", std::make_tuple(asNode(src)),
        [](const double&) { return 7.0; }, InvalidationMode::Lazy);
    auto c1 = ComputeNode<double, double>::make(
        "c1", std::make_tuple(asNode(x)),
        [&c1Runs](const double& v) { ++c1Runs; return v; }, InvalidationMode::Lazy);
    auto c2 = ComputeNode<double, double>::make(
        "c2", std::make_tuple(asNode(x)),
        [&c2Runs](const double& v) { ++c2Runs; return v; }, InvalidationMode::Lazy);

    EvalContext ctx;
    c1->eval(ctx); c2->eval(ctx);
    ASSERT_EQ(c1Runs, 1); ASSERT_EQ(c2Runs, 1);

    src->set(5.0);
    if (GetParam()) { c1->eval(ctx); c2->eval(ctx); }
    else            { c2->eval(ctx); c1->eval(ctx); }

    EXPECT_EQ(c1Runs, 1);
    EXPECT_EQ(c2Runs, 1) << "whichever consumer resolves x first, neither has work to do";
}

INSTANTIATE_TEST_SUITE_P(BothOrders, LazyFanOutOrder, ::testing::Bool(),
                         [](const auto& i) { return i.param ? "C1First" : "C2First"; });

// ── Case 4 — one changed input is enough, even when the other did not move ───
TEST(LazyInvalidation, Case4_DiamondRecomputesWhenOnlyOneArmChanged) {
    auto src = Input<double>::make("src", 0.0);
    int dRuns = 0;

    // a passes src through (changes); b is constant (never changes).
    auto a = ComputeNode<double, double>::make(
        "a", std::make_tuple(asNode(src)),
        [](const double& v) { return v; }, InvalidationMode::Lazy);
    auto b = ComputeNode<double, double>::make(
        "b", std::make_tuple(asNode(a)),
        [](const double&) { return 1.0; }, InvalidationMode::Lazy);
    auto d = ComputeNode<double, double, double>::make(
        "d", std::make_tuple(asNode(a), asNode(b)),
        [&dRuns](const double& x, const double& y) { ++dRuns; return x + y; },
        InvalidationMode::Lazy);

    EvalContext ctx;
    d->eval(ctx);
    ASSERT_EQ(dRuns, 1);

    src->set(4.0);
    EXPECT_DOUBLE_EQ(get_value<double>(d->eval(ctx)), 5.0);
    EXPECT_EQ(dRuns, 2) << "b never moved, but a did — that is enough";
}

// ── Case 5 — the semantic the mode exists to protect ─────────────────────────
TEST(LazyInvalidation, Case5_StatefulNodeStillTicksUnderAnUnchangingLazyInput) {
    auto src = Input<double>::make("src", 0.0);

    // mid's value is constant, so under Lazy it stops telling anyone anything.
    auto mid = ComputeNode<bool, double>::make(
        "mid", std::make_tuple(asNode(src)),
        [](const double&) { return true; }, InvalidationMode::Lazy);

    // The debounce needs three consecutive true TICKS. It can only get there by
    // being evaluated three times — its output depends on HOW OFTEN it ran, not
    // only on what it ran with. dag::ts nodes are Eager and offer no way to say
    // otherwise, which is exactly what this asserts.
    auto deb = ts::DebounceCountNode::make("deb", mid, 3);

    EvalContext ctx;
    EXPECT_FALSE(get_value<bool>(deb->eval(ctx))) << "tick 1 of 3";
    src->set(1.0);
    EXPECT_FALSE(get_value<bool>(deb->eval(ctx))) << "tick 2 of 3";
    src->set(2.0);
    EXPECT_TRUE(get_value<bool>(deb->eval(ctx)))
        << "the stateful node has to advance on every source change even though "
           "its input's VALUE never moved — a Lazy dag::ts node would sit at tick 1 forever";
}

// ── Case 6 — ConditionNode's untaken branch ─────────────────────────────────
TEST(LazyInvalidation, Case6_ConditionNodeNeverServesAStaleUntakenBranch) {
    auto src  = Input<double>::make("src", 1.0);
    auto flag = Input<bool>::make("flag", true);

    auto whenTrue = ComputeNode<double, double>::make(
        "whenTrue", std::make_tuple(asNode(src)),
        [](const double& v) { return v; }, InvalidationMode::Lazy);
    auto whenFalse = ComputeNode<double, double>::make(
        "whenFalse", std::make_tuple(asNode(src)),
        [](const double& v) { return v * 100.0; }, InvalidationMode::Lazy);
    auto pick = ConditionNode::make("pick", flag, whenTrue, whenFalse);

    EvalContext ctx;
    ASSERT_DOUBLE_EQ(get_value<double>(pick->eval(ctx)), 1.0);

    // src moves while whenFalse is the UNTAKEN branch, so nothing evaluates it.
    src->set(3.0);
    ASSERT_DOUBLE_EQ(get_value<double>(pick->eval(ctx)), 3.0);

    // Now switch to it. The branch is pulled before its value is read, so the
    // value is fresh — 300, not the 100 it would have held had it been stale.
    flag->set(false);
    EXPECT_DOUBLE_EQ(get_value<double>(pick->eval(ctx)), 300.0)
        << "the untaken branch is evaluated on the way past, never served stale";
}

TEST(LazyInvalidation, Case6_ConditionNodeMaySpuriouslyRecomputeAndThatIsAcceptable) {
    auto flag = Input<bool>::make("flag", true);
    auto src  = Input<double>::make("src", 1.0);
    int  takenRuns = 0;

    auto whenTrue = ComputeNode<double, double>::make(
        "whenTrue", std::make_tuple(asNode(src)),
        [&takenRuns](const double& v) { ++takenRuns; return v; },
        InvalidationMode::Lazy);
    auto whenFalse = ComputeNode<double, double>::make(
        "whenFalse", std::make_tuple(asNode(src)),
        [](const double& v) { return v; }, InvalidationMode::Lazy);
    auto pick = ConditionNode::make("pick", flag, whenTrue, whenFalse);

    EvalContext ctx;
    pick->eval(ctx);
    const int afterFirst = takenRuns;

    // Toggle away and back without src moving. whenTrue was left Dirty by the
    // cascade while it was untaken, so it recomputes on the way back even though
    // nothing it depends on changed.
    flag->set(false); pick->eval(ctx);
    flag->set(true);  pick->eval(ctx);

    EXPECT_GE(takenRuns, afterFirst)
        << "a spurious recompute is allowed here — what is NOT allowed is a "
           "stale value, which the sibling test pins. ConditionNode is the one "
           "node that does not pull every declared input, and this asymmetry "
           "predates lazy invalidation.";
}

// ── Case 7 — forceRecompute still forces ────────────────────────────────────
TEST(LazyInvalidation, Case7_ForceRecomputeRunsTheFunctorThroughALazyNode) {
    auto src = Input<double>::make("src", 0.0);
    int runs = 0;
    auto mid = ComputeNode<double, double>::make(
        "mid", std::make_tuple(asNode(src)),
        [&runs](const double&) { ++runs; return 42.0; }, InvalidationMode::Lazy);

    EvalContext ctx;
    mid->eval(ctx);
    ASSERT_EQ(runs, 1);

    mid->eval(ctx);
    ASSERT_EQ(runs, 1) << "clean and not forced — nothing to do";

    EvalContext forced;
    forced.forceRecompute = true;
    mid->eval(forced);
    EXPECT_EQ(runs, 2) << "forceRecompute outranks both the clean check and the resolve";

    src->set(1.0);
    mid->eval(forced);
    EXPECT_EQ(runs, 3);
}

// ── The families whose answer is fixed, and where it is fixed ────────────────
//
// Neither of these is a per-node question, so neither is offered at make():
//
//   dag::ops    STRUCTURALLY PURE. eval() computes Op{}(...), default-constructing
//               the functor at every evaluation, so it cannot carry state between
//               calls even if an author wanted it to. Fixed in OpNodeImpl.
//   ConditionNode  Pure selection over three inputs, with no functor at all.
//                  Fixed in the class.
//
// dag::ts is the counterexample and stays Eager for the same kind of reason
// inverted: a stateful node's output depends on how often it ran.
// ─────────────────────────────────────────────────────────────────────────────
TEST(LazyInvalidation, Step5aFixesTheStructurallyPureFamiliesLazy) {
    auto a = Input<double>::make("a", 1.0);
    auto b = Input<double>::make("b", 2.0);
    auto sum  = ops::SumNode<double>::make("sum", { asNode(a), asNode(b) });
    auto diff = ops::DiffNode<double>::make("diff", asNode(a), asNode(b));
    auto neg  = ops::NegateNode<double>::make("neg", asNode(a));
    auto flag = Input<bool>::make("flag", true);
    auto pick = ConditionNode::make("pick", flag, a, b);
    auto ewma = ts::EWMANode::make("ewma", a, 0.5);

    EXPECT_EQ(sum->invalidationMode(),  InvalidationMode::Lazy);
    EXPECT_EQ(diff->invalidationMode(), InvalidationMode::Lazy);
    EXPECT_EQ(neg->invalidationMode(),  InvalidationMode::Lazy);
    EXPECT_EQ(pick->invalidationMode(), InvalidationMode::Lazy);
    EXPECT_EQ(ewma->invalidationMode(), InvalidationMode::Eager)
        << "a stateful node's output depends on how many times it was evaluated";
}

TEST(LazyInvalidation, Step5aStatefulNodeBelowALazyOpStillTicks) {
    auto src = Input<double>::make("src", 0.0);
    // A constant fed through an op: the op's value never moves, so under Lazy it
    // stops telling anyone anything — and the debounce below it must tick anyway.
    auto constant = ComputeNode<double, double>::make(
        "constant", std::make_tuple(asNode(src)),
        [](const double&) { return 1.0; }, InvalidationMode::Lazy);
    auto viaOp = ops::SumNode<double>::make("viaOp", { asNode(constant) });
    auto isOne = ComputeNode<bool, double>::make(
        "isOne", std::make_tuple(asNode(viaOp)),
        [](const double& v) { return v == 1.0; }, InvalidationMode::Lazy);
    auto deb = ts::DebounceCountNode::make("deb", isOne, 3);

    EvalContext ctx;
    EXPECT_FALSE(get_value<bool>(deb->eval(ctx))) << "tick 1 of 3";
    src->set(1.0);
    EXPECT_FALSE(get_value<bool>(deb->eval(ctx))) << "tick 2 of 3";
    src->set(2.0);
    EXPECT_TRUE(get_value<bool>(deb->eval(ctx)))
        << "the op and the node above it both skipped, and the stateful node "
           "below them still advanced on every source change";
}

// ── The op nodes' skip path, now on by default ───────────────────────────────
TEST(LazyInvalidation, OpNodesSkipWhenTheirInputsDidNotMove) {
    auto src = Input<double>::make("src", 0.0);
    int tailRuns = 0;

    auto constant = ComputeNode<double, double>::make(
        "constant", std::make_tuple(asNode(src)),
        [](const double&) { return 2.0; }, InvalidationMode::Lazy);
    auto doubled = ops::SumNode<double>::make(
        "doubled", { asNode(constant), asNode(constant) });
    auto tail = ComputeNode<double, double>::make(
        "tail", std::make_tuple(asNode(doubled)),
        [&tailRuns](const double& v) { ++tailRuns; return v; },
        InvalidationMode::Lazy);

    EvalContext ctx;
    ASSERT_DOUBLE_EQ(get_value<double>(tail->eval(ctx)), 4.0);
    ASSERT_EQ(tailRuns, 1);

    src->set(1.0);
    src->set(2.0);
    EXPECT_DOUBLE_EQ(get_value<double>(tail->eval(ctx)), 4.0);
    EXPECT_EQ(tailRuns, 1) << "the n-ary op's inputs never moved, so neither it "
                              "nor anything below it had work";
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
