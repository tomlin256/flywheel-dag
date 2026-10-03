// =============================================================================
// flywheel-dag — A header-only C++17 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

#include <gtest/gtest.h>
#include <sstream>
#include <cmath>
#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <typeinfo>
#include <utility>
#include <vector>
#include "flywheel/dag.hpp"
#include "flywheel/dag_engine.hpp"
#include "flywheel/dag_ops.hpp"

using namespace dag;

// ── Helper function ──────────────────────────────────────────────────────────
template<typename Node>
double get_value(const std::shared_ptr<Node>& n) {
    EvalContext ctx;
    return get_value<double>(n->eval(ctx));
}

// ─────────────────────────────────────────────────────────────────────────────
// DAG Example Tests
// ─────────────────────────────────────────────────────────────────────────────

// Test: Basic lazy computation chain
// ─────────────────────────────────────────────────────────────────────────────
TEST(DAGTests, BasicLazyChain) {
    auto a = Input<double>::make("a", 3.0);
    auto b = Input<double>::make("b", 4.0);

    // add = a + b  (TypedEqualityPolicy<double> by default)
    auto add = ComputeNode<double, double, double>::make(
        "add",
        std::make_tuple(std::static_pointer_cast<INode>(a),
                        std::static_pointer_cast<INode>(b)),
        [](const double& x, const double& y) { return x + y; }
    );

    // square = add^2
    auto square = ComputeNode<double, double>::make(
        "square",
        std::make_tuple(std::static_pointer_cast<INode>(add)),
        [](const double& v) { return v * v; }
    );

    EvalContext ctx;
    EXPECT_EQ(get_value<double>(square->eval(ctx)), 49.0);   // (3+4)^2 = 49
    EXPECT_EQ(get_value<double>(square->eval(ctx)), 49.0);   // cached

    a->set(5.0); // 5+4=9, 9^2=81
    EXPECT_EQ(get_value<double>(square->eval(ctx)), 81.0);   // recomputed

    a->set(5.0); // same value — TypedEqualityPolicy prevents recompute
    EXPECT_EQ(get_value<double>(square->eval(ctx)), 81.0);   // cached
}

// Test: ConditionNode with short-circuit branch evaluation
// ─────────────────────────────────────────────────────────────────────────────
TEST(DAGTests, ConditionalNode) {
    auto x = Input<double>::make("x", 9.0);

    auto isPositive = ComputeNode<bool, double>::make(
        "isPositive",
        std::make_tuple(std::static_pointer_cast<INode>(x)),
        [](const double& v) { return v > 0.0; }
    );

    auto sqrtX = ComputeNode<double, double>::make(
        "sqrt(x)",
        std::make_tuple(std::static_pointer_cast<INode>(x)),
        [](const double& v) { return std::sqrt(v); }
    );

    auto zero = Input<double>::make("zero", 0.0);

    // Only the active branch is evaluated
    auto safe_sqrt = ConditionNode::make("safe_sqrt", isPositive, sqrtX, zero);

    EvalContext ctx;
    EXPECT_EQ(get_value<double>(safe_sqrt->eval(ctx)), 3.0); // sqrt(9) = 3.0
    
    x->set(-4.0);
    EXPECT_EQ(get_value<double>(safe_sqrt->eval(ctx)), 0.0); // returns 0.0 for negative
}

// Test: Custom IEqualityPolicy — epsilon-based float comparison
// ─────────────────────────────────────────────────────────────────────────────
TEST(DAGTests, CustomEqualityPolicy) {
    // Input itself uses EpsilonPolicy: only propagates when change > 0.01
    auto sensor = Input<double>::make("sensor", 1.0, std::make_shared<EpsilonPolicy<double>>(0.01));

    int recomputeCount = 0;
    auto process = ComputeNode<double, double>::make(
        "process",
        std::make_tuple(std::static_pointer_cast<INode>(sensor)),
        [&](const double& v) { ++recomputeCount; return v * 2.0; }
    );

    EvalContext ctx;
    process->eval(ctx);
    EXPECT_EQ(recomputeCount, 1); // Initial evaluation

    sensor->set(1.001); // within epsilon — no downstream invalidation
    process->eval(ctx);
    EXPECT_EQ(recomputeCount, 1); // No recompute due to epsilon policy

    sensor->set(1.05);  // outside epsilon — recomputes
    process->eval(ctx);
    EXPECT_EQ(recomputeCount, 2); // Recomputed due to large change
}

// Test: library SumNode (dag::ops) composed into the DAG like any other node.
// ─────────────────────────────────────────────────────────────────────────────
TEST(DAGTests, LibrarySumNode) {
    auto a = Input<double>::make("a", 1.0);
    auto b = Input<double>::make("b", 2.0);
    auto c = Input<double>::make("c", 3.0);

    auto sum = dag::ops::SumNode<double>::make("sum", {a, b, c});

    EvalContext ctx;
    EXPECT_EQ(get_value<double>(sum->eval(ctx)), 6.0); // 1+2+3

    b->set(10.0);
    EXPECT_EQ(get_value<double>(sum->eval(ctx)), 14.0); // 1+10+3
}

// ─────────────────────────────────────────────────────────────────────────────
// DAG Tweaking Tests
// ─────────────────────────────────────────────────────────────────────────────

// Test 1: Basic tweak and clear on a single node
// ─────────────────────────────────────────────────────────────────────────────
TEST(TweakExampleTest, BasicTweakAndClear) {
    auto a = Input<double>::make("a", 5.0);

    auto double_a = TweakableComputeNode<double, double>::make(
        "double_a",
        std::make_tuple(std::static_pointer_cast<INode>(a)),
        [](const double& x) { return x * 2.0; }
    );

    // Normal computation: a=5, double_a = 5*2 = 10
    EXPECT_EQ(get_value(double_a), 10.0);
    
    // Tweak double_a = 99
    double_a->tweak(99.0);
    EXPECT_TRUE(double_a->isTweaked());
    EXPECT_EQ(get_value(double_a), 99.0);

    // Upstream change while tweaked — absorbed
    a->set(20.0);
    EXPECT_EQ(get_value(double_a), 99.0); // Still tweaked at 99

    // clearTweak — resumes normal computation
    double_a->clearTweak();
    EXPECT_FALSE(double_a->isTweaked());
    EXPECT_EQ(get_value(double_a), 40.0); // a=20, so 20*2 = 40
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 2: Tweak mid-graph — downstream still lazily recomputes correctly
// ─────────────────────────────────────────────────────────────────────────────
TEST(TweakExampleTest, DownstreamLaziness) {
    auto a = Input<double>::make("a", 3.0);
    auto b = Input<double>::make("b", 4.0);

    auto sum = TweakableComputeNode<double, double, double>::make(
        "sum",
        std::make_tuple(
            std::static_pointer_cast<INode>(a),
            std::static_pointer_cast<INode>(b)),
        [](const double& x, const double& y) { return x + y; }
    );

    int squareEvals = 0;
    auto square = ComputeNode<double, double>::make(
        "square",
        std::make_tuple(std::static_pointer_cast<INode>(sum)),
        [&](const double& v) { ++squareEvals; return v * v; }
    );

    // Normal: sum=3+4=7, square=49
    EXPECT_EQ(get_value(sum), 7.0);
    EXPECT_EQ(get_value(square), 49.0);

    // Tweak sum=10 — square sees 100
    sum->tweak(10.0);
    EXPECT_EQ(get_value(sum), 10.0);
    EXPECT_EQ(get_value(square), 100.0);
    int evalsBefore = squareEvals;

    // Change a and b while sum is tweaked — square NOT recomputed
    a->set(100.0);
    b->set(200.0);
    EXPECT_EQ(get_value(square), 100.0); // sum still tweaked at 10
    EXPECT_EQ(squareEvals, evalsBefore); // No new evals (lazy)

    // clearTweak — square picks up a=100+b=200=300, then 90000
    sum->clearTweak();
    EXPECT_EQ(get_value(sum), 300.0);
    EXPECT_EQ(get_value(square), 90000.0);
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 3: Tweak value equals computed value — no spurious downstream churn
// ─────────────────────────────────────────────────────────────────────────────
TEST(TweakExampleTest, EqualityPolicy) {
    auto a = Input<double>::make("a", 3.0);

    auto node = TweakableComputeNode<double, double>::make(
        "x2",
        std::make_tuple(std::static_pointer_cast<INode>(a)),
        [](const double& x) { return x * 2.0; }
    );

    int downstreamEvals = 0;
    auto sink = ComputeNode<double, double>::make(
        "sink",
        std::make_tuple(std::static_pointer_cast<INode>(node)),
        [&](const double& v) { ++downstreamEvals; return v; }
    );

    // Establish baseline: a=3, node=6
    EvalContext ctx;
    node->eval(ctx);
    sink->eval(ctx);
    int base = downstreamEvals;

    // Tweak to 6.0 — same as computed value (3×2), no downstream churn
    node->tweak(6.0);
    sink->eval(ctx);
    EXPECT_EQ(downstreamEvals, base); // No additional evals

    // Tweak to 99.0 — different, downstream notified
    base = downstreamEvals;
    node->tweak(99.0);
    sink->eval(ctx);
    EXPECT_GT(downstreamEvals, base); // At least one new eval

    // clearTweak — node is now dirty, downstream notified
    base = downstreamEvals;
    node->clearTweak();
    sink->eval(ctx); // pulls through: node recomputes a=3 → 6
    EXPECT_GT(downstreamEvals, base);
    EXPECT_EQ(get_value<double>(sink->eval(ctx)), 6.0);
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 4: Multiple tweakable nodes — independent freeze/unfreeze
// ─────────────────────────────────────────────────────────────────────────────
TEST(TweakExampleTest, MultipleTweaks) {
    auto a = Input<double>::make("a", 2.0);
    auto b = Input<double>::make("b", 3.0);

    auto mul2 = TweakableComputeNode<double, double>::make(
        "a×2", std::make_tuple(std::static_pointer_cast<INode>(a)),
        [](const double& x) { return x * 2.0; });

    auto mul3 = TweakableComputeNode<double, double>::make(
        "b×3", std::make_tuple(std::static_pointer_cast<INode>(b)),
        [](const double& x) { return x * 3.0; });

    auto total = ComputeNode<double, double, double>::make(
        "total",
        std::make_tuple(
            std::static_pointer_cast<INode>(mul2),
            std::static_pointer_cast<INode>(mul3)),
        [](const double& x, const double& y) { return x + y; });

    auto getState = [&]() {
        EvalContext ctx;
        return std::make_tuple(
            get_value<double>(mul2->eval(ctx)),
            get_value<double>(mul3->eval(ctx)),
            get_value<double>(total->eval(ctx))
        );
    };

    // a=2,b=3 — normal: 4+9=13
    auto [m2, m3, t] = getState();
    EXPECT_EQ(m2, 4.0);
    EXPECT_EQ(m3, 9.0);
    EXPECT_EQ(t, 13.0);

    // mul2 tweaked=100: 100+9=109
    mul2->tweak(100.0);
    std::tie(m2, m3, t) = getState();
    EXPECT_EQ(m2, 100.0);
    EXPECT_EQ(m3, 9.0);
    EXPECT_EQ(t, 109.0);

    // mul3 tweaked=200: 100+200=300
    mul3->tweak(200.0);
    std::tie(m2, m3, t) = getState();
    EXPECT_EQ(m2, 100.0);
    EXPECT_EQ(m3, 200.0);
    EXPECT_EQ(t, 300.0);

    // a=50,b=50 — both tweaks absorb changes: still 100+200=300
    a->set(50.0);
    b->set(50.0);
    std::tie(m2, m3, t) = getState();
    EXPECT_EQ(t, 300.0);

    // mul2 cleared (a=50 → 100): 100+200=300
    mul2->clearTweak();
    std::tie(m2, m3, t) = getState();
    EXPECT_EQ(m2, 100.0);
    EXPECT_EQ(m3, 200.0);
    EXPECT_EQ(t, 300.0);

    // mul3 cleared (b=50 → 150): 100+150=250
    mul3->clearTweak();
    std::tie(m2, m3, t) = getState();
    EXPECT_EQ(m2, 100.0);
    EXPECT_EQ(m3, 150.0);
    EXPECT_EQ(t, 250.0);
}

// ─────────────────────────────────────────────────────────────────────────────
// Test 5: Re-tweaking — change the frozen value without clearing first
// ─────────────────────────────────────────────────────────────────────────────
TEST(TweakExampleTest, Retweaking) {
    auto a = Input<double>::make("a", 1.0);

    auto node = TweakableComputeNode<double, double>::make(
        "node", std::make_tuple(std::static_pointer_cast<INode>(a)),
        [](const double& x) { return x * 10.0; });

    int sinkEvals = 0;
    auto sink = ComputeNode<double, double>::make(
        "sink", std::make_tuple(std::static_pointer_cast<INode>(node)),
        [&](const double& v) { ++sinkEvals; return v; });

    EvalContext ctx;
    node->eval(ctx);
    sink->eval(ctx);
    int base = sinkEvals;

    // Tweak to 42
    node->tweak(42.0);
    EXPECT_EQ(get_value<double>(sink->eval(ctx)), 42.0);
    EXPECT_GT(sinkEvals, base);

    // Re-tweak to same value — should not notify downstream
    base = sinkEvals;
    node->tweak(42.0);
    EXPECT_EQ(get_value<double>(sink->eval(ctx)), 42.0);
    EXPECT_EQ(sinkEvals, base); // No new evals

    // Re-tweak to different value while already tweaked
    base = sinkEvals;
    node->tweak(77.0);
    EXPECT_EQ(get_value<double>(sink->eval(ctx)), 77.0);
    EXPECT_GT(sinkEvals, base); // New eval from different value

    // Verify tweakValue is set
    EXPECT_TRUE(node->tweakValue().has_value());
    EXPECT_EQ(node->tweakValue().value(), 77.0);

    // Clear and verify resume normal computation
    node->clearTweak();
    EXPECT_FALSE(node->tweakValue().has_value());
    EXPECT_EQ(get_value<double>(node->eval(ctx)), 10.0); // a=1 → 10
}

// ─────────────────────────────────────────────────────────────────────────────
// A tweak and the engine
//
// Both halves concern what a registered output sees. An equal tweak keeps the
// published value's identity, so after clearTweak() the engine does not deliver
// the unchanged value again. And a changed tweak reaches the node's own output:
// tweak() leaves the node dirty, so the engine evaluates it once while frozen,
// though propagate() absorbs every invalidation.
// ─────────────────────────────────────────────────────────────────────────────
namespace {

/// Input (2) → TweakableComputeNode (x * 10), registered as an engine output.
struct TweakedOutput {
    std::shared_ptr<Input<double>> in = Input<double>::make("in", 2.0);
    std::shared_ptr<TweakableComputeNode<double, double>> node =
        TweakableComputeNode<double, double>::make(
            "x10", std::make_tuple(std::static_pointer_cast<INode>(in)),
            [](const double& x) { return x * 10.0; });
    async::Engine engine;
    std::vector<double> delivered;

    TweakedOutput() {
        engine.addOutput<double>(node, [this](const double& v) { delivered.push_back(v); });
        engine.step();   // delivers the computed 20
    }

    /// Everything delivered since the last call.
    std::vector<double> take() { return std::exchange(delivered, {}); }
};

}  // namespace

TEST(TweakExampleTest, EqualTweakKeepsTheCachedPointer) {
    auto a    = Input<double>::make("a", 2.0);
    auto node = TweakableComputeNode<double, double>::make(
        "x10", std::make_tuple(std::static_pointer_cast<INode>(a)),
        [](const double& x) { return x * 10.0; });

    EvalContext ctx;
    const ValuePtr before = node->eval(ctx);   // 20
    node->tweak(20.0);                         // equal: freeze only
    EXPECT_TRUE(node->isTweaked());
    EXPECT_EQ(node->eval(ctx).get(), before.get())
        << "an equal tweak must keep the cached pointer; the engine reads a new one "
           "as a change";
}

TEST(TweakExampleTest, EqualTweakThenClearFiresNoExtraCallback) {
    TweakedOutput t;
    ASSERT_EQ(t.take(), (std::vector<double>{20.0}));

    t.node->tweak(20.0);    // equal to what the output already has
    t.engine.step();
    t.node->clearTweak();   // recomputes 20
    t.engine.step();

    EXPECT_TRUE(t.take().empty()) << "the output's value never changed";
}

TEST(TweakExampleTest, TweakedOutputDeliversItsValueOnce) {
    TweakedOutput t;
    ASSERT_EQ(t.take(), (std::vector<double>{20.0}));

    t.node->tweak(99.0);
    t.engine.step();
    EXPECT_EQ(t.take(), (std::vector<double>{99.0}))
        << "the node's own output must see its tweak";

    t.engine.step();
    EXPECT_TRUE(t.take().empty()) << "and see it once";

    t.node->clearTweak();
    t.engine.step();
    EXPECT_EQ(t.take(), (std::vector<double>{20.0})) << "clearing resumes the computed value";
}

// An equal re-tweak must not cancel a delivery still pending from the first
// tweak. Marking the node clean there would lose the 99 altogether.
TEST(TweakExampleTest, EqualRetweakKeepsAPendingDelivery) {
    TweakedOutput t;
    ASSERT_EQ(t.take(), (std::vector<double>{20.0}));

    t.node->tweak(99.0);
    t.node->tweak(99.0);
    t.engine.step();
    EXPECT_EQ(t.take(), (std::vector<double>{99.0}));
}

TEST(TweakExampleTest, TweakedOutputAndItsConsumerDeliverInOneCycle) {
    TweakedOutput t;
    auto plusOne = ComputeNode<double, double>::make(
        "plus_one", std::make_tuple(std::static_pointer_cast<INode>(t.node)),
        [](const double& v) { return v + 1.0; });
    std::vector<double> consumer;
    t.engine.addOutput<double>(plusOne, [&](const double& v) { consumer.push_back(v); });
    t.engine.step();        // delivers the consumer's initial 21
    t.take();
    consumer.clear();

    t.node->tweak(99.0);
    t.engine.step();
    EXPECT_EQ(t.take(), (std::vector<double>{99.0}));
    EXPECT_EQ(consumer, (std::vector<double>{100.0}));
}

// ─────────────────────────────────────────────────────────────────────────────
// Clearing a tweak
//
// clearTweak() marks the node Dirty and its consumers Maybe. The node's own
// eval() says "changed" only if its value did, so a Lazy consumer skips when the
// node recomputes the very value it was frozen at.
// ─────────────────────────────────────────────────────────────────────────────
namespace {

/// Input (2) → TweakableComputeNode (x * 10) → Lazy consumer (v + 1), counting
/// the consumer's functor runs.
struct TweakedChain {
    std::shared_ptr<Input<double>> in = Input<double>::make("in", 2.0);
    std::shared_ptr<TweakableComputeNode<double, double>> node;
    std::shared_ptr<ComputeNode<double, double>> consumer;
    int runs = 0;

    explicit TweakedChain(InvalidationMode nodeMode = InvalidationMode::Eager) {
        node = TweakableComputeNode<double, double>::make(
            "x10", std::make_tuple(std::static_pointer_cast<INode>(in)),
            [](const double& x) { return x * 10.0; }, nodeMode);
        consumer = ComputeNode<double, double>::make(
            "plus_one", std::make_tuple(std::static_pointer_cast<INode>(node)),
            [this](const double& v) { ++runs; return v + 1.0; },
            InvalidationMode::Lazy);
    }

    double pull() {
        EvalContext ctx;
        return get_value<double>(consumer->eval(ctx));
    }
};

}  // namespace

TEST(TweakExampleTest, ClearingAnEqualTweakSkipsALazyConsumer) {
    TweakedChain c;
    ASSERT_EQ(c.pull(), 21.0);
    c.node->tweak(20.0);    // equal: freeze only
    ASSERT_EQ(c.pull(), 21.0);
    const int before = c.runs;

    c.node->clearTweak();   // recomputes 20, the value it was frozen at
    EXPECT_EQ(c.pull(), 21.0);
    EXPECT_EQ(c.runs, before)
        << "nothing the consumer depends on changed, so a Lazy consumer must skip";
}

TEST(TweakExampleTest, ClearingAnEqualTweakSkipsALazyOutputThroughTheEngine) {
    TweakedChain c;
    async::Engine engine;
    int callbacks = 0;
    engine.addOutput<double>(c.consumer, [&](const double&) { ++callbacks; });
    engine.step();          // the first delivery: 21
    const int runsBefore = c.runs;
    callbacks = 0;

    c.node->tweak(20.0);
    engine.step();
    c.node->clearTweak();
    engine.step();

    EXPECT_EQ(c.runs, runsBefore) << "the consumer's functor must not rerun";
    EXPECT_EQ(callbacks, 0);
}

TEST(TweakExampleTest, ClearingAChangedTweakStillReachesALazyConsumer) {
    TweakedChain c;
    ASSERT_EQ(c.pull(), 21.0);
    c.node->tweak(99.0);
    ASSERT_EQ(c.pull(), 100.0);
    const int before = c.runs;

    c.node->clearTweak();   // recomputes 20: a real change
    EXPECT_EQ(c.pull(), 21.0);
    EXPECT_EQ(c.runs, before + 1);
}

// The node itself must go Dirty, not Maybe. Left Maybe, a Lazy tweakable node
// would resolve against inputs that never called its invalidate(), skip, and
// keep returning the value it was frozen at.
TEST(TweakExampleTest, ClearingRecomputesALazyTweakableNode) {
    TweakedChain c(InvalidationMode::Lazy);
    ASSERT_EQ(c.pull(), 21.0);
    c.node->tweak(99.0);
    ASSERT_EQ(c.pull(), 100.0);

    c.node->clearTweak();
    EvalContext ctx;
    EXPECT_EQ(get_value<double>(c.node->eval(ctx)), 20.0);
    EXPECT_EQ(c.pull(), 21.0);
}

// With no pull between the tweak and the clear, the node is still Dirty from
// the tweak, so clearing cascades nothing: the tweak already told the consumer
// Dirty. The consumer recomputes, conservatively (an Input going A→B→A between
// pulls does the same), and must read the recomputed value, not the tweak.
TEST(TweakExampleTest, ClearingBeforeTheTweakIsPulledGivesTheRecomputedValue) {
    TweakedChain c;
    ASSERT_EQ(c.pull(), 21.0);

    c.node->tweak(99.0);
    c.node->clearTweak();
    EXPECT_EQ(c.pull(), 21.0);
}

// ─────────────────────────────────────────────────────────────────────────────
// Input<T> — wake-hook behaviour (engine integration)
// ─────────────────────────────────────────────────────────────────────────────

TEST(Input, InitialValue) {
    auto inp = Input<double>::make("t", 3.14);
    EXPECT_DOUBLE_EQ(inp->get(), 3.14);
}

TEST(Input, SetUpdatesValue) {
    auto inp = Input<double>::make("t", 0.0);
    inp->set(7.0);
    EXPECT_DOUBLE_EQ(inp->get(), 7.0);
}

TEST(Input, SetSameValueNoDownstreamInvalidation) {
    auto inp = Input<double>::make("t", 5.0);
    int evals = 0;
    auto ds = ComputeNode<double, double>::make(
        "ds",
        std::make_tuple(std::static_pointer_cast<INode>(inp)),
        [&](const double& v) { ++evals; return v; });

    EvalContext ctx;
    ds->eval(ctx);
    int base = evals;

    inp->set(5.0);
    ds->eval(ctx);
    EXPECT_EQ(evals, base);  // no re-eval; value unchanged
}

TEST(Input, SetDifferentValueInvalidatesDownstream) {
    auto inp = Input<double>::make("t", 5.0);
    int evals = 0;
    auto ds = ComputeNode<double, double>::make(
        "ds",
        std::make_tuple(std::static_pointer_cast<INode>(inp)),
        [&](const double& v) { ++evals; return v; });

    EvalContext ctx;
    ds->eval(ctx);

    inp->set(9.0);
    EXPECT_TRUE(ds->dirty());
    EXPECT_EQ(get_value<double>(ds->eval(ctx)), 9.0);
}

TEST(Input, WakeHookCalledOnChange) {
    auto inp = Input<double>::make("t", 0.0);
    std::atomic<int> hookCount{0};
    inp->setWakeHook([&] { ++hookCount; });

    inp->set(1.0);
    EXPECT_EQ(hookCount.load(), 1);

    inp->set(2.0);
    EXPECT_EQ(hookCount.load(), 2);
}

TEST(Input, WakeHookNotCalledForSameValue) {
    auto inp = Input<double>::make("t", 5.0);
    std::atomic<int> hookCount{0};
    inp->setWakeHook([&] { ++hookCount; });

    inp->set(5.0);
    EXPECT_EQ(hookCount.load(), 0);
}

TEST(Input, EvalClearsDirtyFlag) {
    auto inp = Input<double>::make("t", 0.0);
    inp->set(1.0);
    EXPECT_TRUE(inp->dirty());
    EvalContext ctx;
    inp->eval(ctx);
    EXPECT_FALSE(inp->dirty());
}

TEST(Input, NameReturned) {
    auto inp = Input<int>::make("myinput", 0);
    EXPECT_EQ(inp->name(), "myinput");
}

// An equality policy on an intermediate node whose consumers are all Eager
// suppresses nothing: an Eager consumer recomputes whenever anything upstream
// fired, so the comparison buys nothing. See IEqualityPolicy in dag.hpp.
//
// `mid` here returns a constant, so TypedEqualityPolicy answers "unchanged" on
// every single evaluation — the most favourable case a suppressing policy could
// possibly get. The downstream node still evaluates every time, and swapping in
// AlwaysChangedPolicy changes neither count.
TEST(DAGTests, EqualityPolicyOnIntermediateNodeDoesNotSuppressDownstreamEval) {
    const auto run = [](EqualityPolicyPtr midPolicy) {
        auto src = Input<double>::make("src", 0.0,
                                       std::make_shared<AlwaysChangedPolicy>());
        long midEvals = 0, downEvals = 0;

        auto mid = ComputeNode<double, double>::make(
            "mid", std::make_tuple(std::static_pointer_cast<INode>(src)),
            [&midEvals](const double&) -> double { ++midEvals; return 42.0; },
            std::move(midPolicy));

        auto down = ComputeNode<double, double>::make(
            "down", std::make_tuple(std::static_pointer_cast<INode>(mid)),
            [&downEvals](const double& v) -> double { ++downEvals; return v * 2.0; });

        for (int i = 0; i < 10; ++i) {
            src->set(static_cast<double>(i));
            EvalContext ctx;
            down->eval(ctx);
        }
        return std::make_pair(midEvals, downEvals);
    };

    const auto typed  = run(nullptr);   // TypedEqualityPolicy<double>
    const auto always = run(std::make_shared<AlwaysChangedPolicy>());

    EXPECT_EQ(typed.first,  10);
    EXPECT_EQ(typed.second, 10);   // NOT 1 — the constant suppresses nothing
    EXPECT_EQ(typed, always);      // and the policy makes no difference at all
}

// The same graph with both nodes Lazy: now the policy gates the consumer, which
// skips when its input's value did not change. Both tests are true at once,
// which is what the per-node mode is for.
TEST(DAGTests, EqualityPolicyOnALazyIntermediateNodeDoesSuppressDownstreamEval) {
    auto src = Input<double>::make("src", 0.0,
                                   std::make_shared<AlwaysChangedPolicy>());
    long midEvals = 0, downEvals = 0;

    auto mid = ComputeNode<double, double>::make(
        "mid", std::make_tuple(std::static_pointer_cast<INode>(src)),
        [&midEvals](const double&) -> double { ++midEvals; return 42.0; },
        InvalidationMode::Lazy);

    auto down = ComputeNode<double, double>::make(
        "down", std::make_tuple(std::static_pointer_cast<INode>(mid)),
        [&downEvals](const double& v) -> double { ++downEvals; return v * 2.0; },
        InvalidationMode::Lazy);

    for (int i = 0; i < 10; ++i) {
        src->set(static_cast<double>(i));
        EvalContext ctx;
        down->eval(ctx);
    }

    EXPECT_EQ(midEvals,  10) << "mid is below a source that moved every cycle";
    EXPECT_EQ(downEvals, 1)  << "but mid's value never changed, so down had "
                                "nothing to recompute from";
}

// ─────────────────────────────────────────────────────────────────────────────
// InPlaceComputeNode<Out, Ins...>
// ─────────────────────────────────────────────────────────────────────────────

TEST(InPlaceComputeNode, ComputesTheSameValuesAsComputeNode) {
    auto n = Input<double>::make("n", 3.0);
    auto node = InPlaceComputeNode<std::vector<double>, double>::make(
        "ramp", std::make_tuple(std::static_pointer_cast<INode>(n)),
        [](std::vector<double>& out, const double& count) {
            out.clear();
            for (int i = 0; i < static_cast<int>(count); ++i) out.push_back(i * 2.0);
        });
    EvalContext ctx;
    EXPECT_EQ(get_value<std::vector<double>>(node->eval(ctx)),
              (std::vector<double>{0.0, 2.0, 4.0}));

    n->set(5.0);
    EXPECT_EQ(get_value<std::vector<double>>(node->eval(ctx)),
              (std::vector<double>{0.0, 2.0, 4.0, 6.0, 8.0}));
}

TEST(InPlaceComputeNode, ScratchArrivesHoldingThePreviousValue) {
    // The contract: `out` is a retained buffer holding the previous value, not a
    // fresh Out — that is what lets its capacity survive. A functor that only
    // appends must be seen to accumulate, so the class cannot be "fixed" by
    // resetting the scratch, which would take the recycling away.
    auto n = Input<double>::make("n", 1.0, std::make_shared<AlwaysChangedPolicy>());
    auto node = InPlaceComputeNode<std::vector<double>, double>::make(
        "appender", std::make_tuple(std::static_pointer_cast<INode>(n)),
        [](std::vector<double>& out, const double& v) { out.push_back(v); },
        std::make_shared<AlwaysChangedPolicy>());

    EvalContext ctx;
    EXPECT_EQ(get_value<std::vector<double>>(node->eval(ctx)).size(), 1u);
    n->set(2.0);
    EXPECT_EQ(get_value<std::vector<double>>(node->eval(ctx)),
              (std::vector<double>{1.0, 2.0}));   // NOT {2.0}
}

TEST(InPlaceComputeNode, ShrinkingLeavesNoStaleTail) {
    // The other half of the contract: clear() + push_back must not let the
    // previous evaluation's longer tail show through the recycled storage,
    // either in scratch_ or in the ValueSlot buffer being copy-assigned into.
    auto n = Input<double>::make("n", 4.0);
    auto node = InPlaceComputeNode<std::vector<double>, double>::make(
        "ramp", std::make_tuple(std::static_pointer_cast<INode>(n)),
        [](std::vector<double>& out, const double& count) {
            out.clear();
            for (int i = 0; i < static_cast<int>(count); ++i) out.push_back(1.0);
        });
    EvalContext ctx;
    ASSERT_EQ(get_value<std::vector<double>>(node->eval(ctx)).size(), 4u);
    n->set(1.0);
    EXPECT_EQ(get_value<std::vector<double>>(node->eval(ctx)),
              (std::vector<double>{1.0}));
    n->set(0.0);
    EXPECT_TRUE(get_value<std::vector<double>>(node->eval(ctx)).empty());
}

TEST(InPlaceComputeNode, HonoursItsEqualityPolicyAndCachesLikeComputeNode) {
    auto n = Input<double>::make("n", 2.0, std::make_shared<AlwaysChangedPolicy>());
    long evals = 0;
    auto node = InPlaceComputeNode<std::vector<double>, double>::make(
        "const", std::make_tuple(std::static_pointer_cast<INode>(n)),
        [&evals](std::vector<double>& out, const double&) {
            ++evals;
            out.assign(1, 7.0);          // same value every time
        });

    EvalContext ctx;
    const ValuePtr first = node->eval(ctx);
    EXPECT_FALSE(node->dirty());
    EXPECT_EQ(node->eval(ctx), first);   // not dirty: cached, functor not re-run
    EXPECT_EQ(evals, 1);

    n->set(3.0);                          // AlwaysChangedPolicy on the source
    EXPECT_TRUE(node->dirty());
    const ValuePtr second = node->eval(ctx);
    EXPECT_EQ(evals, 2);
    // Default TypedEqualityPolicy<vector<double>> saw no change, so cached_ is
    // kept — the same pointer identity contract ComputeNode has.
    EXPECT_EQ(second, first);
}

TEST(InPlaceComputeNode, InputsRoundTripAndScalarInputsTakeTheCopyPath) {
    // Two trivially-copyable inputs, so applyInputs takes its `if constexpr`
    // branch rather than the held-ValuePtr one — the branch a container-valued
    // input never exercises.
    auto a = Input<double>::make("a", 1.5);
    auto b = Input<double>::make("b", 2.5);
    auto node = InPlaceComputeNode<std::vector<double>, double, double>::make(
        "pair", std::make_tuple(std::static_pointer_cast<INode>(a),
                                std::static_pointer_cast<INode>(b)),
        [](std::vector<double>& out, const double& x, const double& y) {
            out.clear();
            out.push_back(x);
            out.push_back(y);
        });
    EvalContext ctx;
    EXPECT_EQ(get_value<std::vector<double>>(node->eval(ctx)),
              (std::vector<double>{1.5, 2.5}));

    const auto ins = node->inputs();
    ASSERT_EQ(ins.size(), 2u);
    EXPECT_EQ(ins[0], std::static_pointer_cast<INode>(a));
    EXPECT_EQ(ins[1], std::static_pointer_cast<INode>(b));
    EXPECT_EQ(node->kind(), NodeKind::Compute);
}

TEST(Input, CustomEqualityPolicy) {
    auto eq = std::make_shared<EpsilonPolicy<double>>(0.1);
    auto inp = Input<double>::make("t", 1.0, eq);
    std::atomic<int> hookCount{0};
    inp->setWakeHook([&] { ++hookCount; });

    inp->set(1.05);  // within epsilon — no wake
    EXPECT_EQ(hookCount.load(), 0);

    inp->set(1.2);   // outside epsilon — wakes
    EXPECT_EQ(hookCount.load(), 1);
}

// ─────────────────────────────────────────────────────────────────────────────
// get_value<T> — type dispatch
//
// get_value() compares IValue::type() (a non-virtual load of a type_index fixed
// at construction) and static_casts. That is equivalent to a dynamic_cast ONLY
// because TypedValue<T> is final and is the sole IValue implementation. These
// tests pin the behaviour that equivalence claims: both throw paths, and that a
// hit returns a reference into the stored value rather than a copy.
// ─────────────────────────────────────────────────────────────────────────────

TEST(GetValue, NullValueThrowsRuntimeError) {
    const ValuePtr nothing;
    EXPECT_THROW(get_value<double>(nothing), std::runtime_error);
}

TEST(GetValue, WrongTypeThrowsBadCast) {
    const ValuePtr v = make_value<double>(3.5);

    EXPECT_THROW(get_value<int>(v),         std::bad_cast);
    EXPECT_THROW(get_value<float>(v),       std::bad_cast);
    EXPECT_THROW(get_value<std::string>(v), std::bad_cast);
    EXPECT_THROW(get_value<bool>(v),        std::bad_cast);
}

// Near-miss types must not alias: a signed/unsigned or same-width pair sharing a
// representation is exactly where an unchecked static_cast would silently
// succeed and hand back garbage.
TEST(GetValue, NearMissTypesDoNotAlias) {
    const ValuePtr signedVal = make_value<std::int64_t>(-1);
    EXPECT_THROW(get_value<std::uint64_t>(signedVal), std::bad_cast);
    EXPECT_EQ(get_value<std::int64_t>(signedVal), -1);

    const ValuePtr sizeVal = make_value<std::size_t>(7);
    EXPECT_THROW(get_value<double>(sizeVal), std::bad_cast);
    EXPECT_EQ(get_value<std::size_t>(sizeVal), 7u);
}

TEST(GetValue, CorrectTypeReturnsReferenceNotCopy) {
    // A type big enough that a copy would be obvious, and whose address we can
    // compare across two independent get_value() calls.
    const ValuePtr v = make_value<std::vector<double>>({1.0, 2.0, 3.0});

    const std::vector<double>& first  = get_value<std::vector<double>>(v);
    const std::vector<double>& second = get_value<std::vector<double>>(v);

    EXPECT_EQ(&first, &second) << "get_value must return a reference into the "
                                  "stored value, not a fresh copy";
    EXPECT_EQ(first.data(), second.data());
    ASSERT_EQ(first.size(), 3u);
    EXPECT_DOUBLE_EQ(first[2], 3.0);
}

TEST(GetValue, RoundTripsEveryTypeTheGraphUses) {
    EXPECT_DOUBLE_EQ(get_value<double>(make_value<double>(1.25)), 1.25);
    EXPECT_TRUE(get_value<bool>(make_value<bool>(true)));
    EXPECT_EQ(get_value<std::size_t>(make_value<std::size_t>(42)), 42u);

    const auto pair = get_value<std::pair<double, double>>(
        make_value<std::pair<double, double>>({1.0, 2.0}));
    EXPECT_DOUBLE_EQ(pair.first,  1.0);
    EXPECT_DOUBLE_EQ(pair.second, 2.0);

    const auto opt = get_value<std::optional<bool>>(
        make_value<std::optional<bool>>(std::nullopt));
    EXPECT_FALSE(opt.has_value());
}

// TypedEqualityPolicy swallows the bad_cast from a type mismatch and reports
// "not equal": it must not escape as an exception.
TEST(GetValue, EqualityPolicySurvivesTypeMismatch) {
    const TypedEqualityPolicy<double> policy;
    const ValuePtr asDouble = make_value<double>(1.0);
    const ValuePtr asInt    = make_value<int>(1);

    EXPECT_FALSE(policy.equal(asDouble, asInt));
    EXPECT_TRUE(policy.equal(asDouble, make_value<double>(1.0)));
    EXPECT_TRUE(policy.equal(ValuePtr{}, ValuePtr{}));
    EXPECT_FALSE(policy.equal(asDouble, ValuePtr{}));
}

// ─────────────────────────────────────────────────────────────────────────────
// ComputeNode::applyInputs — how inputs reach the functor
//
// Non-trivially-copyable inputs are bound by reference, with each input's
// ValuePtr held for the duration of the call so the referenced value stays alive
// AND stays out of ValueSlot's recycling pool while the functor runs. Copying
// them into a std::tuple<Ins...> would deep-copy every container-valued input on
// every cycle, once per consuming node.
// ─────────────────────────────────────────────────────────────────────────────

// Address equality distinguishes a reference from a copy. An allocation count
// does not: a compiler may elide a tuple's container copy in a graph this
// simple, so the count passes either way.
TEST(ApplyInputs, ContainerInputIsPassedByReferenceNotCopied) {
    auto src = Input<std::vector<int>>::make("src", std::vector<int>{1, 2, 3});

    const int* seenData = nullptr;
    auto consumer = ComputeNode<int, std::vector<int>>::make(
        "consumer", std::make_tuple(std::static_pointer_cast<INode>(src)),
        [&](const std::vector<int>& v) {
            seenData = v.data();          // where the functor's argument lives
            return static_cast<int>(v.size());
        });

    EvalContext ctx;
    ASSERT_EQ(get_value<int>(consumer->eval(ctx)), 3);

    // The address the producer stores its vector's buffer at.
    const ValuePtr produced = src->eval(ctx);
    const std::vector<int>& stored = get_value<std::vector<int>>(produced);
    EXPECT_EQ(seenData, stored.data())
        << "the functor received a copy of the input container, not a reference "
           "to the one the producing node holds";
}

TEST(ApplyInputs, ScalarInputsStillComputeCorrectly) {
    auto a = Input<double>::make("a", 3.0);
    auto b = Input<double>::make("b", 4.0);
    auto hyp = ComputeNode<double, double, double>::make(
        "hyp", std::make_tuple(std::static_pointer_cast<INode>(a),
                               std::static_pointer_cast<INode>(b)),
        [](const double& x, const double& y) { return std::sqrt(x * x + y * y); });

    EvalContext ctx;
    EXPECT_DOUBLE_EQ(get_value<double>(hyp->eval(ctx)), 5.0);

    a->set(5.0);
    b->set(12.0);
    EXPECT_DOUBLE_EQ(get_value<double>(hyp->eval(ctx)), 13.0);
}

TEST(ApplyInputs, MixedTrivialAndContainerInputsBindCorrectly) {
    auto scale = Input<double>::make("scale", 2.0);
    auto batch = Input<std::vector<int>>::make("batch", std::vector<int>{1, 2, 3, 4});

    auto weighted = ComputeNode<double, double, std::vector<int>>::make(
        "weighted", std::make_tuple(std::static_pointer_cast<INode>(scale),
                                    std::static_pointer_cast<INode>(batch)),
        [](const double& k, const std::vector<int>& v) {
            double total = 0.0;
            for (int x : v) total += k * x;
            return total;
        });

    EvalContext ctx;
    EXPECT_DOUBLE_EQ(get_value<double>(weighted->eval(ctx)), 20.0);   // 2*(1+2+3+4)

    scale->set(3.0);
    EXPECT_DOUBLE_EQ(get_value<double>(weighted->eval(ctx)), 30.0);
}

// Two inputs sharing one upstream producer, evaluated under forceRecompute so
// the second input re-runs the first input's producer mid-call. Values must stay
// correct and stable rather than drifting as ValueSlot buffers alternate.
//
// This is a correctness test for the shared-upstream + forceRecompute
// combination. It does not by itself discriminate the `held` array in
// applyInputs: a single-expression call keeps the pulled values alive too, since
// C++ temporaries live to the end of the full-expression.
TEST(ApplyInputs, SharedUpstreamUnderForceRecomputeStaysCorrect) {
    auto seed = Input<int>::make("seed", 2);

    // One producer of a container, consumed twice by the same node.
    auto producer = ComputeNode<std::vector<int>, int>::make(
        "producer", std::make_tuple(std::static_pointer_cast<INode>(seed)),
        [](const int& n) {
            std::vector<int> out;
            for (int i = 0; i < 4; ++i) out.push_back(n * (i + 1));
            return out;
        });

    auto consumer = ComputeNode<int, std::vector<int>, std::vector<int>>::make(
        "consumer", std::make_tuple(std::static_pointer_cast<INode>(producer),
                                    std::static_pointer_cast<INode>(producer)),
        [](const std::vector<int>& lhs, const std::vector<int>& rhs) {
            int total = 0;
            for (std::size_t i = 0; i < lhs.size(); ++i)
                total += lhs[i] * rhs[i];
            return total;
        });

    EvalContext forced;
    forced.forceRecompute = true;

    // seed=2 -> {2,4,6,8}; dot with itself = 4+16+36+64 = 120
    EXPECT_EQ(get_value<int>(consumer->eval(forced)), 120);

    seed->set(3);   // {3,6,9,12}; dot = 9+36+81+144 = 270
    EXPECT_EQ(get_value<int>(consumer->eval(forced)), 270);

    // Repeated forced evaluation must stay stable, not drift as buffers recycle.
    for (int i = 0; i < 20; ++i)
        ASSERT_EQ(get_value<int>(consumer->eval(forced)), 270) << "iteration " << i;
}

TEST(ApplyInputs, ZeroInputComputeNodeStillWorks) {
    auto constant = ComputeNode<double>::make(
        "constant", std::make_tuple(), [] { return 42.0; });

    EvalContext ctx;
    EXPECT_DOUBLE_EQ(get_value<double>(constant->eval(ctx)), 42.0);
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
