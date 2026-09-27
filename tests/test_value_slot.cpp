// =============================================================================
// flywheel-dag — A header-only C++17 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

// test_value_slot.cpp — pins the ValueSlot<T> recycling contract.
//
// ValueSlot rewrites a TypedValue buffer in place instead of allocating a fresh
// one, which is only safe because it does so exclusively when it holds the last
// reference (use_count() == 1). Everything below tests one of the two things
// that guard buys:
//
//   1. Nobody observes a value changing under them.
//   2. The emitted pointer is never the one cached_ already holds — which the
//      equality check and the engine's pointer-identity change detection both
//      rely on.
//
// The allocation counter must be defined before the flywheel headers.

#include <atomic>
#include <cstdlib>
#include <deque>
#include <new>
#include <string>
#include <vector>

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
void operator delete(void* p) noexcept              { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

#include <gtest/gtest.h>

#include "flywheel/dag.hpp"
#include "flywheel/dag_async.hpp"
#include "flywheel/dag_engine.hpp"
#include "flywheel/dag_ops.hpp"
#include "flywheel/dag_timeseries.hpp"

using namespace dag;

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

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// 1. Steady state allocates nothing.
// ─────────────────────────────────────────────────────────────────────────────

TEST(ValueSlot, SteadyStateEvalAllocatesNothing) {
    // Deliberately excludes the windowed nodes: their backing store is a
    // separate concern from ValueSlot's. See
    // WindowedNodesAllocateNothingInSteadyState below.
    auto src   = async::AsyncInput<double>::make("src", 1.0);
    auto ewma  = ts::EWMANode::make("ewma", src, 0.3);
    auto delta = ts::DeltaNode<double>::make("delta", ewma);
    auto thr   = ts::ThresholdNode<double>::make("thr", delta, 0.0);
    auto scaled = ops::ProductNode<double>::make(
        "scaled", {std::static_pointer_cast<INode>(ewma),
                   std::static_pointer_cast<INode>(delta)});
    auto ratio = ops::DivideNode<double>::make("ratio", scaled, ewma);

    async::Engine engine;
    engine.addSource(src);
    double sink = 0.0;
    int    hits = 0;
    engine.addOutput<double>(ratio, [&](const double& v) { sink += v; });
    engine.addOutput<bool>(thr,     [&](const bool& v)   { hits += v; });

    for (int i = 0; i < 200; ++i) {   // warm every node and both slot buffers
        src->post(1.0 + (i % 17) * 0.5);
        engine.step();
    }

    const long allocs = allocationsDuring([&] {
        for (int i = 0; i < 500; ++i) {
            src->post(1.0 + (i % 17) * 0.5);
            engine.step();
        }
    });

    EXPECT_EQ(allocs, 0)
        << "steady-state eval allocated " << allocs << " times over 500 cycles; "
           "every recomputing node should be recycling its ValueSlot buffers";
    EXPECT_NE(sink, 0.0);   // and it actually did the work
}

// RollingStats used to allocate here — std::deque turning over a 4096-byte
// block (plus a 16-byte map slot) every ~512 pushes as the sliding window
// walked off the end of the current one. Its backing store is a contiguous
// RingBuffer now, allocated once at construction, so this is exactly 0.
//
// WindowNode is deliberately absent: its output type IS std::deque<T>, so its
// deque cannot go until the output type changes.
TEST(ValueSlot, WindowedNodesAllocateNothingInSteadyState) {
    auto src   = async::AsyncInput<double>::make("src", 1.0);
    auto stats = ts::RollingStats::make("stats", src, 16);
    auto delay = ts::DelayNode<double>::make("delay", src, 8);
    auto mm    = ts::RollingMinMaxNode::make("minmax", src, 16);

    async::Engine engine;
    engine.addSource(src);
    double sink = 0.0;
    engine.addOutput<double>(stats, [&](const double& v) { sink += v; });
    engine.addOutput<double>(delay, [&](const double& v) { sink += v; });
    engine.addOutput<std::pair<double, double>>(
        mm, [&](const std::pair<double, double>& v) { sink += v.first; });

    for (int i = 0; i < 200; ++i) {
        src->post(1.0 + (i % 17) * 0.5);
        engine.step();
    }

    const long allocs = allocationsDuring([&] {
        for (int i = 0; i < 2000; ++i) {   // long enough to cross old deque blocks
            src->post(1.0 + (i % 17) * 0.5);
            engine.step();
        }
    });

    EXPECT_EQ(allocs, 0)
        << "windowed nodes allocated " << allocs << " times over 2000 cycles; "
           "RingBuffer allocates once at construction and never again";
    EXPECT_NE(sink, 0.0);
}

// dag::Input<T>::set() is deliberately NOT slotted — it is the one entry point
// documented as callable off the eval thread, where use_count() is not a sound
// ownership test. This pins that exclusion so it cannot be "optimised" away
// without someone deciding to.
TEST(ValueSlot, InputSetStillAllocatesByDesign) {
    auto in     = Input<double>::make("in", 0.0);
    auto square = ComputeNode<double, double>::make(
        "square", std::make_tuple(std::static_pointer_cast<INode>(in)),
        [](const double& x) { return x * x; });

    async::Engine engine;
    double sink = 0.0;
    engine.addOutput<double>(square, [&](const double& v) { sink += v; });
    for (int i = 0; i < 20; ++i) { in->set(i); engine.step(); }

    const long allocs = allocationsDuring([&] {
        for (int i = 0; i < 100; ++i) { in->set(100 + i); engine.step(); }
    });

    EXPECT_EQ(allocs, 100) << "expected exactly one allocation per Input::set()";
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. A held ValuePtr is never rewritten underneath its holder.
// ─────────────────────────────────────────────────────────────────────────────

TEST(ValueSlot, HeldValueIsNotMutatedByLaterEvals) {
    auto in    = Input<double>::make("in", 2.0);
    auto twice = ops::SumNode<double>::make(
        "twice", {std::static_pointer_cast<INode>(in),
                  std::static_pointer_cast<INode>(in)});

    EvalContext ctx;
    const ValuePtr held = twice->eval(ctx);
    ASSERT_DOUBLE_EQ(get_value<double>(held), 4.0);

    // Drive many more evals than there are buffers, so the slot cycles through
    // every reuse opportunity it has while `held` is outstanding.
    for (int i = 3; i < 40; ++i) {
        in->set(static_cast<double>(i));
        twice->eval(ctx);
    }

    EXPECT_DOUBLE_EQ(get_value<double>(held), 4.0)
        << "a ValuePtr handed out by eval() was rewritten while still held";
}

// The fallback path: hold BOTH buffers, so the next emit cannot recycle and
// must allocate. Correctness must not depend on a buffer being available.
TEST(ValueSlot, FallsBackToAllocationWhenBothBuffersAreHeld) {
    auto in    = Input<double>::make("in", 1.0);
    auto twice = ops::SumNode<double>::make(
        "twice", {std::static_pointer_cast<INode>(in),
                  std::static_pointer_cast<INode>(in)});

    EvalContext ctx;
    std::vector<ValuePtr> held;
    std::vector<double>   expected;
    for (int i = 1; i <= 6; ++i) {
        in->set(static_cast<double>(i));
        held.push_back(twice->eval(ctx));
        expected.push_back(2.0 * i);
    }

    // Every value handed out is still exactly what it was when handed out,
    // even though only two buffers exist.
    for (std::size_t i = 0; i < held.size(); ++i)
        EXPECT_DOUBLE_EQ(get_value<double>(held[i]), expected[i])
            << "value " << i << " changed after being handed out";

    // ...and they are all distinct objects.
    for (std::size_t i = 1; i < held.size(); ++i)
        EXPECT_NE(held[i].get(), held[i - 1].get());
}

// ─────────────────────────────────────────────────────────────────────────────
// 3. A changed value never reuses the pointer cached_ is holding.
// ─────────────────────────────────────────────────────────────────────────────

TEST(ValueSlot, ChangedValueAlwaysYieldsADifferentPointer) {
    auto in    = Input<double>::make("in", 0.0);
    auto twice = ops::SumNode<double>::make(
        "twice", {std::static_pointer_cast<INode>(in),
                  std::static_pointer_cast<INode>(in)});

    EvalContext ctx;
    const IValue* previous = nullptr;
    for (int i = 1; i <= 50; ++i) {
        in->set(static_cast<double>(i));
        const ValuePtr v = twice->eval(ctx);
        EXPECT_NE(v.get(), previous)
            << "cycle " << i << ": a changed value reused the previous pointer, "
               "which would make the engine miss the change";
        EXPECT_DOUBLE_EQ(get_value<double>(v), 2.0 * i);
        previous = v.get();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 4. An unchanged value keeps the cached pointer and fires no callback.
// ─────────────────────────────────────────────────────────────────────────────

TEST(ValueSlot, UnchangedValueKeepsCachedPointerAndFiresNoCallback) {
    auto in     = Input<double>::make("in", 5.0);
    // Output depends only on the sign, so distinct inputs give an equal result.
    auto isPositive = ComputeNode<bool, double>::make(
        "is_positive", std::make_tuple(std::static_pointer_cast<INode>(in)),
        [](const double& x) { return x > 0.0; });

    async::Engine engine;
    int fired = 0;
    engine.addOutput<bool>(isPositive, [&](const bool&) { ++fired; });

    engine.step();
    const int firedAfterFirst = fired;
    EXPECT_EQ(firedAfterFirst, 1) << "first cycle should deliver the initial value";

    EvalContext ctx;
    const ValuePtr cachedAfterFirst = isPositive->eval(ctx);

    for (int i = 1; i <= 20; ++i) {   // all still positive → result unchanged
        in->set(5.0 + i);
        engine.step();
    }

    EXPECT_EQ(fired, firedAfterFirst)
        << "equality policy said unchanged, so no callback should have fired";
    EXPECT_EQ(isPositive->eval(ctx).get(), cachedAfterFirst.get())
        << "an unchanged node must keep returning the same cached pointer";

    in->set(-1.0);          // now it genuinely changes
    engine.step();
    EXPECT_EQ(fired, firedAfterFirst + 1);
}

// ─────────────────────────────────────────────────────────────────────────────
// 5. RateLimiterNode absorbs invalidation — unaffected by slot recycling.
// ─────────────────────────────────────────────────────────────────────────────

// The DAG-level contract, with the consumer Lazy.
//
// "Suppresses" is about WORK, not about a flag. This used to assert
// !consumer->dirty(), which the tri-state protocol makes the wrong question —
// the limiter now forwards Maybe, so a consumer legitimately IS dirty and still
// has nothing to do. The functor either ran or it did not, and that is not
// satisfiable by an accident of flag bookkeeping.
TEST(ValueSlot, RateLimiterStillSuppressesSubThresholdChangesDownstream) {
    auto in      = Input<double>::make("in", 0.0);
    auto limited = ts::RateLimiterNode<double>::make("limited", in, 10.0);

    int computes = 0;
    auto consumer = ComputeNode<double, double>::make(
        "consumer", std::make_tuple(std::static_pointer_cast<INode>(limited)),
        [&](const double& v) { ++computes; return v * 2.0; },
        InvalidationMode::Lazy);

    EvalContext ctx;
    consumer->eval(ctx);                 // prime
    const int afterFirst = computes;
    ASSERT_GT(afterFirst, 0);

    for (int i = 1; i <= 9; ++i) {       // all sub-threshold
        in->set(static_cast<double>(i));
        consumer->eval(ctx);
        ASSERT_EQ(computes, afterFirst)
            << "cycle " << i << ": a sub-minDelta change reached downstream";
    }

    in->set(50.0);                       // clears minDelta
    EXPECT_DOUBLE_EQ(get_value<double>(consumer->eval(ctx)), 100.0);
    EXPECT_EQ(computes, afterFirst + 1)
        << "a change of at least minDelta must reach downstream, exactly once";
}

// An Eager consumer keeps recomputing — and that is the correct reading of the
// mode, not a regression. What it GAINS is the release, which it never used to
// get at all: the limiter absorbed invalidation without forwarding,
// so in a pull-based graph a consumer that was never dirtied never pulled the
// limiter, and the limiter never reached the eval() that would have told it.
// Suppression for such a consumer is now opt-in, by saying Lazy — which is what
// the flag is for.
TEST(ValueSlot, RateLimiterReleaseReachesAnEagerConsumerToo) {
    auto in      = Input<double>::make("in", 0.0);
    auto limited = ts::RateLimiterNode<double>::make("limited", in, 10.0);

    int computes = 0;
    auto consumer = ComputeNode<double, double>::make(
        "consumer", std::make_tuple(std::static_pointer_cast<INode>(limited)),
        [&](const double& v) { ++computes; return v * 2.0; });   // Eager

    EvalContext ctx;
    consumer->eval(ctx);
    for (int i = 1; i <= 9; ++i) { in->set(static_cast<double>(i)); consumer->eval(ctx); }
    EXPECT_DOUBLE_EQ(get_value<double>(consumer->eval(ctx)), 0.0)
        << "still suppressed by VALUE — the limiter did not emit";

    in->set(50.0);
    EXPECT_DOUBLE_EQ(get_value<double>(consumer->eval(ctx)), 100.0)
        << "and the release arrives";
}

// ─────────────────────────────────────────────────────────────────────────────
// A limiter wired MID-GRAPH, driven through the Engine.
//
// The original reproduction: a limiter with a registered output below it
// rather than on it:
//
//     engine-driven, limiter NOT a registered output:
//       base=1  after 9 sub-threshold=1  after 50.0=1  sink=0.0
//       -> release NEVER PROPAGATES
//
// The cause was the absorbing invalidate() override: the consumer was never
// dirtied, so the engine never pulled it, so it never pulled the limiter, so the
// limiter never reached the eval() that would have released. The remedy was
// "forward invalidation with a suppression flag" — the Maybe state IS that
// flag, so deleting the override is the whole fix.
// ─────────────────────────────────────────────────────────────────────────────
TEST(ValueSlot, RateLimiterWiredMidGraphPropagatesItsReleaseThroughTheEngine) {
    async::Engine engine;
    auto in      = engine.makeInput<double>("in", 0.0);
    auto limited = ts::RateLimiterNode<double>::make("limited", in, 10.0);

    int computes = 0;
    auto consumer = ComputeNode<double, double>::make(
        "consumer", std::make_tuple(std::static_pointer_cast<INode>(limited)),
        [&](const double& v) { ++computes; return v * 2.0; },
        InvalidationMode::Lazy);

    double sink = 0.0;
    long   callbacks = 0;
    engine.addOutput<double>(consumer, [&](const double& v) { sink = v; ++callbacks; });

    engine.step();
    const int baseComputes = computes;

    for (int i = 1; i <= 9; ++i) { in->set(static_cast<double>(i)); engine.step(); }
    EXPECT_EQ(computes, baseComputes)
        << "sub-threshold changes must not reach the consumer's functor";

    in->set(50.0);
    engine.step();
    EXPECT_EQ(computes, baseComputes + 1) << "the release must reach it";
    EXPECT_DOUBLE_EQ(sink, 100.0)
        << "this was 0.0 — the release never propagated at all";
}

// Characterisation test for a PRE-EXISTING bug, not a spec.
//
// StatefulNodeBase::eval assigns cached_ unconditionally after
// notifyDownstream() has already decided whether to, so cached_ changes
// identity every eval and the engine's pointer-identity change detection fires
// on every dirty cycle. Verified identical before and after ValueSlot, so slot
// recycling neither causes nor hides it.
//
// This asserts the CURRENT WRONG behaviour so the fix cannot land silently.
// When the bug is fixed this test will fail — replace it with the correct
// expectation (extraCallbacks == 0) rather than adjusting the number.
TEST(ValueSlot, KnownBug_StatefulNodesFireCallbacksOnEveryDirtyCycle) {
    auto in  = Input<double>::make("in", 0.0);
    auto thr = ts::ThresholdNode<double>::make("thr", in, 100.0);   // never crossed

    async::Engine engine;
    int fired = 0;
    engine.addOutput<bool>(thr, [&](const bool&) { ++fired; });

    engine.step();
    const int afterFirst = fired;

    for (int i = 1; i <= 20; ++i) {   // output stays false throughout
        in->set(static_cast<double>(i));
        engine.step();
    }

    EXPECT_EQ(fired - afterFirst, 20)
        << "the known bug appears to be FIXED — a constant-output stateful node no "
           "longer fires spuriously. Replace this characterisation test with "
           "EXPECT_EQ(fired - afterFirst, 0).";
}

// ─────────────────────────────────────────────────────────────────────────────
// 6. AsyncInput::flush() is slotted; its equality suppression still holds.
// ─────────────────────────────────────────────────────────────────────────────

TEST(ValueSlot, AsyncInputFlushRecyclesAndStillSuppressesEqualPosts) {
    auto src = async::AsyncInput<double>::make("src", 0.0);

    async::Engine engine;
    engine.addSource(src);
    int fired = 0;
    engine.addOutput<double>(src, [&](const double&) { ++fired; });

    src->post(1.0);
    engine.step();
    const int afterFirst = fired;

    for (int i = 0; i < 10; ++i) {   // same value every time
        src->post(1.0);
        engine.step();
    }
    EXPECT_EQ(fired, afterFirst) << "posting an equal value must not fire";

    src->post(2.0);
    engine.step();
    EXPECT_EQ(fired, afterFirst + 1);
}

// ─────────────────────────────────────────────────────────────────────────────
// 7. A T that owns heap keeps the recycled buffer's capacity.
//
// Recycling the TypedValue is only half the job when T owns heap of its own: a
// move-assign into the buffer frees the very vectors being recycled. These pin
// the emit(const T&) overload that copy-assigns instead — and that the rvalue
// overload still moves, since every compute/op/time-series node relies on it.
// ─────────────────────────────────────────────────────────────────────────────

namespace {

/// A heap-owning value like a fixed-depth snapshot. Its payload size rarely
/// changes from one update to the next, which is exactly the condition that
/// makes capacity reuse pay.
struct HeapVal {
    std::string         tag;
    std::vector<double> payload;
    bool operator==(const HeapVal& o) const { return tag == o.tag && payload == o.payload; }
};

HeapVal makeHeapVal(std::size_t n, double base) {
    HeapVal v;
    v.tag = "sym";                       // short: SSO, never its own allocation
    v.payload.reserve(n);
    for (std::size_t i = 0; i < n; ++i) v.payload.push_back(base + static_cast<double>(i));
    return v;
}

}  // namespace

TEST(ValueSlot, EmitFromLvalueReusesBufferCapacity) {
    ValueSlot<HeapVal> slot;
    const HeapVal v = makeHeapVal(10, 1.0);

    // Warm to the steady state a node runs in: `held` pins one buffer (as
    // cached_ does), leaving exactly one free for the slot to rewrite.
    ValuePtr held = slot.emit(v);
    held = slot.emit(v);
    held = slot.emit(v);

    const long allocs = allocationsDuring([&] {
        for (int i = 0; i < 100; ++i) held = slot.emit(v);
    });

    EXPECT_EQ(allocs, 0)
        << "emitting a same-size lvalue allocated " << allocs << " times over 100 emits; "
           "copy-assigning into the recycled buffer should reuse its capacity";
    EXPECT_EQ(get_value<HeapVal>(held), v);
}

TEST(ValueSlot, EmitFromRvalueStillMoves) {
    ValueSlot<HeapVal> slot;
    ValuePtr held = slot.emit(makeHeapVal(10, 1.0));
    held = slot.emit(makeHeapVal(10, 2.0));
    held = slot.emit(makeHeapVal(10, 3.0));   // warm: now rewriting a buffer

    HeapVal src = makeHeapVal(10, 7.0);
    const double* const buffer = src.payload.data();

    held = slot.emit(std::move(src));

    EXPECT_EQ(get_value<HeapVal>(held).payload.data(), buffer)
        << "the rvalue overload must still steal the source's buffer, not copy it — "
           "every compute/op/time-series node emits through it";
    EXPECT_EQ(get_value<HeapVal>(held).payload.size(), 10u);
    EXPECT_DOUBLE_EQ(get_value<HeapVal>(held).payload.front(), 7.0);
}

TEST(ValueSlot, EmitFromLvalueRespectsSoleOwnershipGuard) {
    ValueSlot<HeapVal> slot;
    const HeapVal first  = makeHeapVal(10, 1.0);
    const HeapVal second = makeHeapVal(10, 100.0);

    ValuePtr a = slot.emit(first);
    ValuePtr b = slot.emit(second);   // `a` is still held: its buffer is off limits

    EXPECT_NE(a.get(), b.get());
    EXPECT_EQ(get_value<HeapVal>(a), first)
        << "a held ValuePtr must not be rewritten underneath its holder";

    // Both buffers held now — the copy path must fall back to allocating rather
    // than rewrite either one.
    ValuePtr c = slot.emit(first);
    EXPECT_NE(c.get(), a.get());
    EXPECT_NE(c.get(), b.get());
    EXPECT_EQ(get_value<HeapVal>(a), first);
    EXPECT_EQ(get_value<HeapVal>(b), second);
}

TEST(ValueSlot, EmitFromLvalueGrowsAndShrinksCorrectly) {
    ValueSlot<HeapVal> slot;
    const HeapVal big   = makeHeapVal(10, 1.0);
    const HeapVal small = makeHeapVal(3, 50.0);

    ValuePtr held = slot.emit(big);
    held = slot.emit(big);
    held = slot.emit(big);
    EXPECT_EQ(get_value<HeapVal>(held), big);

    held = slot.emit(small);   // into a buffer whose capacity is sized for 10
    EXPECT_EQ(get_value<HeapVal>(held), small)
        << "capacity reuse must not leave stale tail elements visible";
    EXPECT_EQ(get_value<HeapVal>(held).payload.size(), 3u);

    held = slot.emit(big);     // and back up, within the retained capacity
    EXPECT_EQ(get_value<HeapVal>(held), big);
}

TEST(ValueSlot, AsyncInputPostAndFlushAllocateNothingInSteadyState) {
    // The snapshot ingest path end to end: post() copy-assigns into pending_,
    // flush() swaps it into staged_ and emits it into a recycled slot buffer.
    // Two alternating values so value_ actually rotates between both buffers,
    // as it does under a live feed — an unchanging value would leave one of them
    // permanently free and hide a fallback allocation.
    auto inp = async::AsyncInput<HeapVal>::make("snapshot", HeapVal{});
    const HeapVal a = makeHeapVal(10, 1.0);
    const HeapVal b = makeHeapVal(10, 2.0);

    for (int i = 0; i < 8; ++i) {   // warm pending_, staged_ and both buffers
        inp->post((i % 2) ? a : b);
        inp->flush();
    }

    const long allocs = allocationsDuring([&] {
        for (int i = 0; i < 100; ++i) {
            inp->post((i % 2) ? a : b);
            inp->flush();
        }
    });

    EXPECT_EQ(allocs, 0)
        << "post + flush allocated " << allocs << " times over 100 snapshot updates; "
           "a stream that posts every engine cycle needs this path to recycle its storage";
    EXPECT_EQ(inp->current(), a) << "the last of the 100 updates (i=99) posted a";
}

// ─────────────────────────────────────────────────────────────────────────────
// 8. An idle AsyncQueue allocates nothing.
//
// flush() refreshes its cached batch on every cycle so a downstream node dirtied
// by another input reads [] rather than a batch it already consumed. That used to
// mean a fresh make_value per queue per cycle, paid whether or not anything
// arrived — and a quiet queue receives nothing on almost every cycle.
//
// This one discriminates only where a default-constructed std::deque allocates
// (libstdc++ does; libc++ does not), which is how flush()'s local drain deque
// stayed hidden on macOS while this test was red in CI. Test 9 below covers the
// same fix on both standard libraries.
// ─────────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
// InPlaceComputeNode — the eval-thread half of the recycling story
// ─────────────────────────────────────────────────────────────────────────────

TEST(ValueSlot, InPlaceComputeNodeSteadyStateAllocatesNothing) {
    // ComputeNode's Fn returns Out by value, so a heap-owning Out allocates on
    // every eval and its emit(std::move(...)) then frees the buffer ValueSlot
    // was holding to reuse. InPlaceComputeNode writes into a retained scratch
    // and emits it as a const lvalue, so capacity cycles between scratch_ and
    // the two slot buffers and the steady state costs nothing.
    //
    // The value alternates between two lengths on purpose: an unchanging output
    // would leave one slot buffer permanently free and hide a fallback
    // allocation, the same trap AsyncInputPostAndFlushAllocateNothingInSteadyState
    // avoids by alternating payloads.
    auto n = async::AsyncInput<double>::make("n", 4.0,
                                             std::make_shared<AlwaysChangedPolicy>());
    auto node = InPlaceComputeNode<std::vector<double>, double>::make(
        "ramp", std::make_tuple(std::static_pointer_cast<INode>(n)),
        [](std::vector<double>& out, const double& count) {
            out.clear();
            for (int i = 0; i < static_cast<int>(count); ++i)
                out.push_back(static_cast<double>(i));
        },
        std::make_shared<AlwaysChangedPolicy>());

    async::Engine engine;
    engine.addSource(n);
    double sink = 0.0;
    engine.addOutput<std::vector<double>>(
        node, [&sink](const std::vector<double>& v) { sink += v.size(); });

    const auto drive = [&](int i) {
        n->post((i % 2 == 0) ? 8.0 : 6.0);
        engine.step();
    };
    for (int i = 0; i < 20; ++i) drive(i);          // warm scratch_ and both buffers

    const long allocs = allocationsDuring([&] {
        for (int i = 0; i < 100; ++i) drive(i);
    });
    EXPECT_EQ(allocs, 0);
    EXPECT_GT(sink, 0.0);                            // the graph really ran
}

TEST(ValueSlot, ComputeNodeAllocatesOncePerHeapMemberPerEval) {
    // The control for the test above, and the reason InPlaceComputeNode exists.
    // Same graph, same drive, ComputeNode instead: the functor builds a fresh
    // vector (one allocation) and emit(std::move(...)) then FREES the storage
    // the slot buffer was holding to reuse and installs the temporary's
    // instead. One allocation and one free per heap member per eval — which is
    // 2 and 2 for a value holding a pair of vectors.
    //
    // This is a characterisation of ComputeNode, not a defect report: returning
    // by value is the right interface for the many nodes whose Out is a double.
    auto n = async::AsyncInput<double>::make("n", 4.0,
                                             std::make_shared<AlwaysChangedPolicy>());
    auto node = ComputeNode<std::vector<double>, double>::make(
        "ramp", std::make_tuple(std::static_pointer_cast<INode>(n)),
        [](const double& count) -> std::vector<double> {
            std::vector<double> out;
            out.reserve(static_cast<std::size_t>(count));
            for (int i = 0; i < static_cast<int>(count); ++i)
                out.push_back(static_cast<double>(i));
            return out;
        },
        std::make_shared<AlwaysChangedPolicy>());

    async::Engine engine;
    engine.addSource(n);
    double sink = 0.0;
    engine.addOutput<std::vector<double>>(
        node, [&sink](const std::vector<double>& v) { sink += v.size(); });

    const auto drive = [&](int i) {
        n->post((i % 2 == 0) ? 8.0 : 6.0);
        engine.step();
    };
    for (int i = 0; i < 20; ++i) drive(i);

    const long allocs = allocationsDuring([&] {
        for (int i = 0; i < 100; ++i) drive(i);
    });
    EXPECT_EQ(allocs, 100);   // 100 evals, one vector each
    EXPECT_GT(sink, 0.0);
}

TEST(ValueSlot, IdleAsyncQueuesAllocateNothing) {
    // Two queues, both registered as engine sources, stepped with nothing
    // posted.
    auto first  = async::AsyncQueue<double>::make("src.a");
    auto second = async::AsyncQueue<double>::make("src.b");

    async::Engine engine;
    engine.addSource(first);
    engine.addSource(second);

    for (int i = 0; i < 8; ++i) engine.step();   // warm

    const long allocs = allocationsDuring([&] {
        for (int i = 0; i < 100; ++i) engine.step();
    });

    EXPECT_EQ(allocs, 0)
        << "two idle queues allocated " << allocs << " times over 100 cycles; "
           "the empty batch is a constant and must not be rebuilt per cycle";
}

// ─────────────────────────────────────────────────────────────────────────────
// 9. A queue that receives allocates its batch and nothing else.
//
// flush() drains through a retained member deque rather than a local one, so the
// deque's map and block are recycled across cycles instead of being freed with
// the local and reallocated by the next post(). What is left is the batch vector
// and the TypedValue that carries it — two allocations, unavoidable, per cycle
// that actually delivers something.
// ─────────────────────────────────────────────────────────────────────────────

TEST(ValueSlot, QueueArrivalAllocatesOnlyItsBatch) {
    auto q = async::AsyncQueue<double>::make("src.q");

    for (int i = 0; i < 8; ++i) { q->post(1.0); q->flush(); }   // warm both deques

    const long allocs = allocationsDuring([&] {
        for (int i = 0; i < 100; ++i) { q->post(1.0); q->flush(); }
    });

    EXPECT_EQ(allocs, 200)
        << "100 single-item arrivals allocated " << allocs << " times; the batch "
           "vector and its TypedValue are 2 per cycle, and the drain deque must "
           "not add a map and a block on top of them";
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
