// =============================================================================
// flywheel-dag — A header-only C++17 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

// test_dag_async.cpp — unit tests for dag_async.hpp
//
// Coverage
// ────────
//  AsyncInput<T>   — post/flush semantics, latest-wins, skipped/pending counts,
//                    equality policy, downstream invalidation, wake hook, thread safety
//  AsyncQueue<T>   — FIFO delivery, max-size overflow, dropped count, wake hook
//  FeedRegistry    — flush-all, hasPending, wake hook propagation
//  TickLoop        — start/stop lifecycle, callback delivery
//  CycleSeqLock    — consistent cross-thread reads over Engine::cycle()
//  Engine::run()   — a cycle that throws ends the run, and run() can start again

#include <gtest/gtest.h>
#include "flywheel/dag.hpp"
#include "flywheel/dag_async.hpp"
#include "flywheel/dag_engine.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace dag;
using namespace dag::async;
using namespace std::chrono_literals;

// ─────────────────────────────────────────────────────────────────────────────
// AsyncInput<T>
// ─────────────────────────────────────────────────────────────────────────────

namespace {

/// A value type that owns heap, like the fixed-depth snapshot a feed might post.
/// AsyncInput recycles the storage of a T like this between pending_ and
/// staged_ rather than reallocating it; the cases at the end of this section pin
/// what that must not change.
struct Payload {
    std::string         tag;
    std::vector<double> levels;
    bool operator==(const Payload& o) const { return tag == o.tag && levels == o.levels; }
};

Payload makePayload(std::size_t n, double base) {
    Payload p;
    p.tag = "sym";
    p.levels.reserve(n);
    for (std::size_t i = 0; i < n; ++i) p.levels.push_back(base + static_cast<double>(i));
    return p;
}

}  // namespace

TEST(AsyncInput, InitialValueAccessible) {
    auto inp = AsyncInput<double>::make("x", 42.0);
    EXPECT_EQ(inp->current(), 42.0);
}

TEST(AsyncInput, ValueNotChangedBeforeFlush) {
    auto inp = AsyncInput<double>::make("x", 1.0);
    inp->post(99.0);
    EXPECT_EQ(inp->current(), 1.0);  // unchanged until flush
}

TEST(AsyncInput, FlushAppliesPostedValue) {
    auto inp = AsyncInput<double>::make("x", 1.0);
    inp->post(99.0);
    inp->flush();
    EXPECT_EQ(inp->current(), 99.0);
}

TEST(AsyncInput, LatestWins) {
    auto inp = AsyncInput<double>::make("x", 0.0);
    inp->post(10.0);
    inp->post(20.0);
    inp->post(30.0);
    inp->flush();
    EXPECT_EQ(inp->current(), 30.0);
}

TEST(AsyncInput, SkippedCountTracksOverwrites) {
    auto inp = AsyncInput<double>::make("x", 0.0);
    inp->post(1.0);
    inp->post(2.0);
    inp->post(3.0);
    EXPECT_EQ(inp->skippedCount(), 2u);
}

TEST(AsyncInput, SkippedCountResetAfterFlush) {
    auto inp = AsyncInput<double>::make("x", 0.0);
    inp->post(1.0);
    inp->post(2.0);
    inp->flush();
    EXPECT_EQ(inp->skippedCount(), 0u);
}

TEST(AsyncInput, PendingCountOneAfterPost) {
    auto inp = AsyncInput<double>::make("x", 0.0);
    EXPECT_EQ(inp->pendingCount(), 0u);
    inp->post(5.0);
    EXPECT_EQ(inp->pendingCount(), 1u);
}

TEST(AsyncInput, PendingCountZeroAfterFlush) {
    auto inp = AsyncInput<double>::make("x", 0.0);
    inp->post(5.0);
    inp->flush();
    EXPECT_EQ(inp->pendingCount(), 0u);
}

TEST(AsyncInput, FlushReturnsZeroWhenEmpty) {
    auto inp = AsyncInput<double>::make("x", 1.0);
    EXPECT_EQ(inp->flush(), 0u);
}

TEST(AsyncInput, FlushReturnsOneWhenNewValueApplied) {
    auto inp = AsyncInput<double>::make("x", 1.0);
    inp->post(2.0);
    EXPECT_EQ(inp->flush(), 1u);
}

TEST(AsyncInput, FlushReturnsZeroForSameValue) {
    auto inp = AsyncInput<double>::make("x", 5.0);
    inp->post(5.0);
    EXPECT_EQ(inp->flush(), 0u);  // equality policy suppresses update
}

TEST(AsyncInput, FlushInvalidatesDownstream) {
    auto inp = AsyncInput<double>::make("x", 1.0);
    int evals = 0;
    auto ds = ComputeNode<double, double>::make(
        "ds",
        std::make_tuple(std::static_pointer_cast<INode>(inp)),
        [&](const double& v) { ++evals; return v * 2.0; });

    EvalContext ctx;
    ds->eval(ctx);
    EXPECT_EQ(evals, 1);

    ds->eval(ctx);
    EXPECT_EQ(evals, 1);  // cached

    inp->post(10.0);
    inp->flush();
    ds->eval(ctx);
    EXPECT_EQ(evals, 2);  // recomputed after flush
}

TEST(AsyncInput, SameValueFlushDoesNotInvalidateDownstream) {
    auto inp = AsyncInput<double>::make("x", 5.0);
    int evals = 0;
    auto ds = ComputeNode<double, double>::make(
        "ds",
        std::make_tuple(std::static_pointer_cast<INode>(inp)),
        [&](const double& v) { ++evals; return v; });

    EvalContext ctx;
    ds->eval(ctx);
    int base = evals;

    inp->post(5.0);
    inp->flush();
    ds->eval(ctx);
    EXPECT_EQ(evals, base);  // no recompute — value unchanged
}

TEST(AsyncInput, WakeHookFiredOnPost) {
    auto inp = AsyncInput<double>::make("x", 0.0);
    std::atomic<int> hookCount{0};
    inp->setWakeHook([&] { ++hookCount; });

    inp->post(1.0);
    EXPECT_EQ(hookCount.load(), 1);

    inp->post(2.0);
    EXPECT_EQ(hookCount.load(), 2);
}

TEST(AsyncInput, EvalClearsDirtyFlag) {
    auto inp = AsyncInput<double>::make("x", 1.0);
    EXPECT_TRUE(inp->dirty());
    EvalContext ctx;
    inp->eval(ctx);
    EXPECT_FALSE(inp->dirty());
}

TEST(AsyncInput, NameReturned) {
    auto inp = AsyncInput<double>::make("my_input", 0.0);
    EXPECT_EQ(inp->name(), "my_input");
}

TEST(AsyncInput, ThreadSafety_ConcurrentPostAndFlush) {
    auto inp = AsyncInput<int>::make("x", 0);
    std::atomic<bool> go{false};

    std::thread poster([&] {
        while (!go.load()) {}
        for (int i = 1; i <= 1000; ++i)
            inp->post(i);
    });

    go = true;
    for (int i = 0; i < 200; ++i)
        inp->flush();

    poster.join();
    inp->flush();  // drain anything remaining

    EXPECT_GE(inp->current(), 0);
    EXPECT_LE(inp->current(), 1000);
}

// ── Staging storage is recycled, not reallocated ─────────────────────────────
//
// post() copy-assigns into a live pending_ and flush() swaps it with staged_,
// so the buffers cycle instead of being freed and reallocated. These four pin
// the parts of that which could go wrong silently.

TEST(AsyncInput, PostLeavesTheCallersValueIntact) {
    auto inp = AsyncInput<Payload>::make("snapshot");
    Payload cached = makePayload(10, 1.0);

    inp->post(cached);
    inp->flush();

    EXPECT_EQ(cached, makePayload(10, 1.0))
        << "a feed keeps its cached value to merge the next delta into — "
           "post() must not consume or disturb it";
    EXPECT_EQ(inp->current(), cached);
}

TEST(AsyncInput, RecycledStagingNeverLeaksStaleContents) {
    auto inp = AsyncInput<Payload>::make("snapshot");
    const Payload big   = makePayload(10, 1.0);
    const Payload small = makePayload(3, 50.0);

    for (int i = 0; i < 4; ++i) {   // warm: pending_/staged_/both slot buffers
        inp->post(big);
        inp->flush();
    }
    EXPECT_EQ(inp->current(), big);

    inp->post(small);               // into storage sized for ten levels
    inp->flush();
    EXPECT_EQ(inp->current(), small)
        << "reusing capacity must not leave the previous update's tail visible";
    EXPECT_EQ(inp->current().levels.size(), 3u);

    inp->post(big);                 // and back up, within the retained capacity
    inp->flush();
    EXPECT_EQ(inp->current(), big);
}

TEST(AsyncInput, SkippedCountSurvivesStagingRecycle) {
    auto inp = AsyncInput<Payload>::make("snapshot");
    const Payload first  = makePayload(10, 1.0);
    const Payload second = makePayload(10, 2.0);
    const Payload third  = makePayload(10, 3.0);

    inp->post(first);
    inp->flush();                   // staged_ now holds the buffers to recycle

    inp->post(second);
    inp->post(third);               // overwrites the pending copy in place
    EXPECT_EQ(inp->skippedCount(), 1u);
    EXPECT_EQ(inp->pendingCount(), 1u);

    EXPECT_EQ(inp->flush(), 1u);
    EXPECT_EQ(inp->skippedCount(), 0u);
    EXPECT_EQ(inp->pendingCount(), 0u);
    EXPECT_EQ(inp->current(), third) << "latest wins, as before";
}

TEST(AsyncInput, EmptyFlushAfterRecycleLeavesLastValue) {
    auto inp = AsyncInput<Payload>::make("snapshot");
    const Payload v = makePayload(10, 1.0);
    const Payload w = makePayload(10, 9.0);

    inp->post(v);
    EXPECT_EQ(inp->flush(), 1u);

    EXPECT_EQ(inp->flush(), 0u) << "nothing pending: no swap, no emit";
    EXPECT_EQ(inp->current(), v) << "an empty flush must not disturb the last value";

    inp->post(w);                   // the staging pair must still be usable
    EXPECT_EQ(inp->flush(), 1u);
    EXPECT_EQ(inp->current(), w);
}

// ─────────────────────────────────────────────────────────────────────────────
// AsyncQueue<T>
// ─────────────────────────────────────────────────────────────────────────────

TEST(AsyncQueue, AllValuesDeliveredInOrder) {
    auto q = AsyncQueue<int>::make("q");
    q->post(1);
    q->post(2);
    q->post(3);
    q->flush();

    EvalContext ctx;
    auto batch = get_value<std::vector<int>>(q->eval(ctx));
    ASSERT_EQ(batch.size(), 3u);
    EXPECT_EQ(batch[0], 1);
    EXPECT_EQ(batch[1], 2);
    EXPECT_EQ(batch[2], 3);
}

TEST(AsyncQueue, FlushReturnsDeliveredCount) {
    auto q = AsyncQueue<int>::make("q");
    q->post(1);
    q->post(2);
    EXPECT_EQ(q->flush(), 2u);
}

TEST(AsyncQueue, FlushReturnsZeroWhenEmpty) {
    auto q = AsyncQueue<int>::make("q");
    EXPECT_EQ(q->flush(), 0u);
}

TEST(AsyncQueue, PendingCountTracksQueueDepth) {
    auto q = AsyncQueue<int>::make("q");
    EXPECT_EQ(q->pendingCount(), 0u);

    q->post(1);
    q->post(2);
    EXPECT_EQ(q->pendingCount(), 2u);

    q->flush();
    EXPECT_EQ(q->pendingCount(), 0u);
}

TEST(AsyncQueue, MaxQueueSizeDropsOldestValues) {
    auto q = AsyncQueue<int>::make("q", /*maxQueueSize=*/3);
    q->post(1);
    q->post(2);
    q->post(3);
    q->post(4);  // drops 1
    q->post(5);  // drops 2
    q->flush();

    EvalContext ctx;
    auto batch = get_value<std::vector<int>>(q->eval(ctx));
    ASSERT_EQ(batch.size(), 3u);
    EXPECT_EQ(batch[0], 3);
    EXPECT_EQ(batch[1], 4);
    EXPECT_EQ(batch[2], 5);
}

TEST(AsyncQueue, DroppedCountTracksOverflow) {
    auto q = AsyncQueue<int>::make("q", /*maxQueueSize=*/2);
    q->post(1);
    q->post(2);
    q->post(3);  // drops 1
    q->post(4);  // drops 2
    EXPECT_EQ(q->droppedCount(), 2u);
}

TEST(AsyncQueue, WakeHookFiredOnPost) {
    std::atomic<int> hookCount{0};
    auto q = AsyncQueue<int>::make("q");
    q->setWakeHook([&] { ++hookCount; });

    q->post(1);
    q->post(2);
    EXPECT_EQ(hookCount.load(), 2);
}

TEST(AsyncQueue, FlushInvalidatesDownstream) {
    auto q = AsyncQueue<double>::make("q");
    int evals = 0;
    auto ds = ComputeNode<double, std::vector<double>>::make(
        "ds",
        std::make_tuple(std::static_pointer_cast<INode>(q)),
        [&](const std::vector<double>& batch) {
            ++evals;
            double sum = 0.0;
            for (auto v : batch) sum += v;
            return sum;
        });

    EvalContext ctx;
    ds->eval(ctx);
    int base = evals;

    q->post(1.0);
    q->post(2.0);
    q->flush();
    ds->eval(ctx);
    EXPECT_GT(evals, base);
}

TEST(AsyncQueue, NameReturned) {
    auto q = AsyncQueue<int>::make("myq");
    EXPECT_EQ(q->name(), "myq");
}

TEST(AsyncQueue, BatchSumViaDag) {
    // A ComputeNode summing tick volumes is the canonical use case.
    auto q = AsyncQueue<int>::make("q");
    auto sumNode = ComputeNode<int, std::vector<int>>::make(
        "sum",
        std::make_tuple(std::static_pointer_cast<INode>(q)),
        [](const std::vector<int>& batch) {
            int s = 0;
            for (auto v : batch) s += v;
            return s;
        });

    q->post(10);
    q->post(20);
    q->post(30);
    q->flush();

    EvalContext ctx;
    EXPECT_EQ(get_value<int>(sumNode->eval(ctx)), 60);
}

// ── The empty batch is a constant, not a fresh allocation ────────────────────
//
// flush() refreshes value_ on every cycle, empty or not, so a downstream node
// dirtied by a DIFFERENT input reads [] rather than a batch it has already
// consumed. That refresh must stay; what went is the allocation behind it.

TEST(AsyncQueue, EmptyFlushReturnsTheSameValuePointer) {
    auto q = AsyncQueue<int>::make("q");
    EvalContext ctx;

    q->flush();
    const ValuePtr first = q->eval(ctx);
    q->flush();
    const ValuePtr second = q->eval(ctx);
    EXPECT_EQ(first.get(), second.get())
        << "consecutive empty flushes must rebind to the one shared [] value";

    q->post(7);
    q->flush();
    const ValuePtr batch = q->eval(ctx);
    EXPECT_NE(batch.get(), first.get()) << "a real batch is its own value";

    q->flush();
    EXPECT_EQ(q->eval(ctx).get(), first.get()) << "and an empty flush comes back to it";
}

TEST(AsyncQueue, EmptyFlushRefreshesToEmptyNotStaleBatch) {
    // The consumer takes the batch AND a second input, so it can be dirtied on a
    // cycle where the queue itself received nothing — which is the only way the
    // stale batch would ever be observed.
    auto q     = AsyncQueue<int>::make("q");
    auto other = AsyncInput<double>::make("other", 0.0);

    std::vector<int> seen;
    auto consumer = ComputeNode<int, std::vector<int>, double>::make(
        "consumer",
        std::make_tuple(std::static_pointer_cast<INode>(q),
                        std::static_pointer_cast<INode>(other)),
        [&](const std::vector<int>& batch, const double&) {
            seen = batch;
            int total = 0;
            for (auto v : batch) total += v;
            return total;
        });

    Engine engine;
    engine.addSource(q);
    engine.addSource(other);
    engine.addOutput<int>(consumer, [](const int&) {});

    q->post(10);
    q->post(20);
    other->post(1.0);
    engine.step();
    EXPECT_EQ(seen, (std::vector<int>{10, 20}));

    // Nothing posted to the queue this cycle; the consumer is dirtied by `other`.
    other->post(2.0);
    engine.step();
    EXPECT_TRUE(seen.empty())
        << "the consumer re-read a batch it had already consumed — flush() must "
           "still refresh value_ to [] on an empty cycle, not just skip the refresh";
}

TEST(AsyncQueue, BatchValueSurvivesALaterEmptyFlush) {
    auto q = AsyncQueue<int>::make("q");
    EvalContext ctx;

    q->post(5);
    q->post(6);
    q->flush();
    const ValuePtr held = q->eval(ctx);
    EXPECT_EQ(get_value<std::vector<int>>(held), (std::vector<int>{5, 6}));

    q->flush();   // empty: value_ rebinds to the shared [] value

    EXPECT_EQ(get_value<std::vector<int>>(held), (std::vector<int>{5, 6}))
        << "a held batch is immutable — rebinding value_ must not touch it";
    EXPECT_TRUE(get_value<std::vector<int>>(q->eval(ctx)).empty());
}

// ─────────────────────────────────────────────────────────────────────────────
// FeedRegistry
// ─────────────────────────────────────────────────────────────────────────────

TEST(FeedRegistry, FlushAllReturnsTotal) {
    FeedRegistry reg;
    auto a = AsyncInput<double>::make("a", 0.0);
    auto b = AsyncInput<double>::make("b", 0.0);
    reg.add(a);
    reg.add(b);

    a->post(1.0);
    b->post(2.0);
    EXPECT_EQ(reg.flush(), 2u);
}

TEST(FeedRegistry, FlushReturnsZeroWhenNothingPending) {
    FeedRegistry reg;
    auto a = AsyncInput<double>::make("a", 0.0);
    reg.add(a);
    EXPECT_EQ(reg.flush(), 0u);
}

TEST(FeedRegistry, HasPendingTrueWhenAnyInputPending) {
    FeedRegistry reg;
    auto a = AsyncInput<double>::make("a", 0.0);
    auto b = AsyncInput<double>::make("b", 0.0);
    reg.add(a);
    reg.add(b);

    EXPECT_FALSE(reg.hasPending());
    b->post(1.0);
    EXPECT_TRUE(reg.hasPending());
}

TEST(FeedRegistry, HasPendingFalseAfterFlush) {
    FeedRegistry reg;
    auto a = AsyncInput<double>::make("a", 0.0);
    reg.add(a);

    a->post(1.0);
    EXPECT_TRUE(reg.hasPending());
    reg.flush();
    EXPECT_FALSE(reg.hasPending());
}

TEST(FeedRegistry, SetWakeHookPropagatesExistingMembers) {
    FeedRegistry reg;
    auto a = AsyncInput<double>::make("a", 0.0);
    auto b = AsyncInput<double>::make("b", 0.0);
    reg.add(a);
    reg.add(b);

    std::atomic<int> hookCount{0};
    reg.setWakeHook([&] { ++hookCount; });

    a->post(1.0);
    b->post(2.0);
    EXPECT_EQ(hookCount.load(), 2);
}

TEST(FeedRegistry, SetWakeHookPropagatesNewMembers) {
    FeedRegistry reg;
    std::atomic<int> hookCount{0};
    reg.setWakeHook([&] { ++hookCount; });

    // Add input AFTER hook is set — hook must still propagate
    auto a = AsyncInput<double>::make("a", 0.0);
    reg.add(a);

    a->post(1.0);
    EXPECT_EQ(hookCount.load(), 1);
}

TEST(FeedRegistry, AllReturnsAllInputs) {
    FeedRegistry reg;
    reg.add(AsyncInput<double>::make("a", 0.0));
    reg.add(AsyncInput<double>::make("b", 0.0));
    reg.add(AsyncInput<double>::make("c", 0.0));
    EXPECT_EQ(reg.all().size(), 3u);
}

// ─────────────────────────────────────────────────────────────────────────────
// TickLoop
// ─────────────────────────────────────────────────────────────────────────────

TEST(TickLoop, StartAndStop) {
    TickLoop loop(10ms, [] {});
    loop.start();
    EXPECT_TRUE(loop.running());
    loop.stop();
    EXPECT_FALSE(loop.running());
}

TEST(TickLoop, CallbackFiresAtLeastOnce) {
    std::atomic<int> count{0};
    std::mutex mu;
    std::condition_variable cv;
    TickLoop loop(5ms, [&] { ++count; cv.notify_one(); });
    loop.start();
    {
        std::unique_lock<std::mutex> lk(mu);
        cv.wait_for(lk, std::chrono::seconds(5), [&]{ return count.load() > 0; });
    }
    loop.stop();
    EXPECT_GT(count.load(), 0);
}

TEST(TickLoop, DestructorStopsLoop) {
    std::atomic<int> count{0};
    std::mutex mu;
    std::condition_variable cv;
    {
        TickLoop loop(5ms, [&] { ++count; cv.notify_one(); });
        loop.start();
        // Wait until at least one callback fires before letting the destructor run.
        std::unique_lock<std::mutex> lk(mu);
        cv.wait_for(lk, std::chrono::seconds(5), [&]{ return count.load() > 0; });
        // destructor calls stop() and joins the thread
    }
    int countAfterDestruct = count.load();
    // The thread is joined by the destructor — no more callbacks can ever fire.
    EXPECT_EQ(count.load(), countAfterDestruct);
}

// ─────────────────────────────────────────────────────────────────────────────
// Engine dirty-snapshot ordering fix
// ─────────────────────────────────────────────────────────────────────────────

// Regression test for the dirty-flag ordering hazard.
//
// A ComputeNode that depends on an AsyncInput is registered as an output
// BEFORE the AsyncInput itself.  Without the dirty-flag snapshot in cycle(),
// the compute node's eval() would recursively call eval() on the AsyncInput,
// clearing its dirty_ flag before the engine loop reaches the AsyncInput
// entry — so the AsyncInput's own callback would silently never fire.
//
// With the snapshot fix the dirty state is captured before any eval(), so
// both callbacks must fire regardless of registration order.
TEST(EngineDirtySnapshot, LeafCallbackFiresWhenComputeNodeRegisteredFirst) {
    Engine engine;

    auto src = AsyncInput<double>::make("price", 0.0);
    engine.addSource(src);

    auto doubled = ComputeNode<double, double>::make(
        "doubled",
        std::make_tuple(std::static_pointer_cast<INode>(src)),
        [](const double& v) { return v * 2.0; });

    double lastDoubled = -1.0;
    double lastSrc     = -1.0;

    // Hazard order: compute node registered BEFORE its leaf input.
    engine.addOutput<double>(doubled, [&](const double& v) { lastDoubled = v; });
    engine.addOutput<double>(src,     [&](const double& v) { lastSrc     = v; });

    src->post(5.0);
    engine.step();

    EXPECT_DOUBLE_EQ(lastDoubled, 10.0);  // compute fired
    EXPECT_DOUBLE_EQ(lastSrc,      5.0);  // leaf also fired — was 0.0 before fix
}

// ─────────────────────────────────────────────────────────────────────────────
// RollingCycleWindow
// ─────────────────────────────────────────────────────────────────────────────

TEST(RollingCycleWindow, MeanIsZeroBeforeAnyRecord) {
    RollingCycleWindow w(5);
    EXPECT_DOUBLE_EQ(w.meanUs(), 0.0);
}

TEST(RollingCycleWindow, MeanIsExactUnderCapacity) {
    RollingCycleWindow w(5);
    w.record(1000);
    w.record(2000);
    w.record(3000);
    EXPECT_DOUBLE_EQ(w.meanUs(), 2.0);   // (1000+2000+3000) ns / 3 -> 2.0 us
}

TEST(RollingCycleWindow, OldestSampleIsEvictedOnceWindowFills) {
    RollingCycleWindow w(3);
    w.record(1'000'000);   // a startup-outlier stand-in
    w.record(1000);
    w.record(1000);
    w.record(1000);
    EXPECT_DOUBLE_EQ(w.meanUs(), 1.0);   // the outlier has aged out of the window
}

TEST(RollingCycleWindow, ZeroWindowThrows) {
    EXPECT_THROW(RollingCycleWindow{0}, std::invalid_argument);
}

// ─────────────────────────────────────────────────────────────────────────────
// Engine cycle stats
// ─────────────────────────────────────────────────────────────────────────────

// Helper: build a minimal engine with one input → passthrough node → output.
// Returns the input so the caller can drive cycles via set().
static dag::InputPtr<int> makeMinimalEngine(Engine& engine) {
    auto inp = engine.makeInput<int>("x", 0);
    auto node = ComputeNode<int, int>::make(
        "pass",
        std::make_tuple(std::static_pointer_cast<INode>(inp)),
        [](const int& v) { return v; });
    engine.addOutput<int>(node, [](const int&) {});
    return inp;
}

// Drive N distinct values through the engine synchronously — one step per value.
static void driveNCycles(Engine& engine, const dag::InputPtr<int>& inp, int n) {
    for (int i = 1; i <= n; ++i) {
        inp->set(i);
        engine.step();
    }
}

TEST(EngineCycleStats, CycleCountAndCallbacksMonotonicallyIncrease) {
    Engine engine;

    auto inp  = engine.makeInput<int>("x", 0);
    auto node = ComputeNode<int, int>::make(
        "pass",
        std::make_tuple(std::static_pointer_cast<INode>(inp)),
        [](const int& v) { return v; });

    // Sample both counters inside the callback — always on the eval thread,
    // so no races with the engine updating them.
    std::vector<uint64_t> cycleSamples, callbackSamples;
    engine.addOutput<int>(node, [&](const int&) {
        cycleSamples.push_back(engine.cycleCount());
        callbackSamples.push_back(engine.callbacksFired());
    });

    driveNCycles(engine, inp, 5);

    ASSERT_GE(cycleSamples.size(), 5u);
    for (std::size_t i = 1; i < cycleSamples.size(); ++i) {
        EXPECT_GE(cycleSamples[i],    cycleSamples[i - 1]);
        EXPECT_GE(callbackSamples[i], callbackSamples[i - 1]);
    }
}

// Drive n cycles on a background thread while calling readEachIteration() in a tight loop on
// the calling thread until the writer finishes. The writer is the only thread that ever calls
// step() — matches production, where exactly one thread drives cycle(). Mirrors
// AsyncInput.ThreadSafety_ConcurrentPostAndFlush's shape: fixed writer count, unsynchronized
// reader-iteration count, join, then the caller asserts on final state only.
static void stressConcurrentReads(
    Engine& engine, dag::InputPtr<int> inp, int n,
    const std::function<void()>& readEachIteration)
{
    std::atomic<bool> go{false};
    std::atomic<bool> done{false};

    std::thread writer([&] {
        while (!go.load()) {}
        for (int i = 1; i <= n; ++i) {
            inp->set(i);
            engine.step();
        }
        done = true;
    });

    go = true;
    while (!done.load()) readEachIteration();
    writer.join();
}

TEST(EngineCycleStats, ConcurrentStepAndScalarStatsReadsAreRaceFree) {
    Engine engine;
    auto inp = makeMinimalEngine(engine);

    stressConcurrentReads(engine, inp, 2000, [&] {
        (void)engine.cycleCount();
        (void)engine.callbacksFired();
        (void)engine.lastCycleUs();
        (void)engine.meanCycleUs();
        (void)engine.minCycleUs();
        (void)engine.maxCycleUs();
    });

    EXPECT_GE(engine.cycleCount(), 2000u);
    EXPECT_LE(engine.minCycleUs(), engine.meanCycleUs());
    EXPECT_GE(engine.maxCycleUs(), engine.meanCycleUs());
}

TEST(EngineCycleStats, MinMaxBoundLastCycleAndMean) {
    Engine engine;
    auto inp = makeMinimalEngine(engine);

    driveNCycles(engine, inp, 5);

    EXPECT_GE(engine.cycleCount(), 5u);

    // min is the smallest cycle ever seen — it can't exceed any individual cycle.
    EXPECT_LE(engine.minCycleUs(), engine.lastCycleUs());
    EXPECT_LE(engine.minCycleUs(), engine.meanCycleUs());

    // max is the largest cycle ever seen — it can't be less than any individual cycle.
    EXPECT_GE(engine.maxCycleUs(), engine.lastCycleUs());
    EXPECT_GE(engine.maxCycleUs(), engine.meanCycleUs());
}

TEST(EngineCycleStats, RollingMeanBoundedByMinAndMax) {
    Engine engine;
    auto inp = makeMinimalEngine(engine);

    driveNCycles(engine, inp, 5);

    // The rolling mean is the mean of some subset of the same cycles the
    // lifetime min/max were drawn from, so it can't fall outside their range.
    // The exact eviction arithmetic is pinned in isolation by the
    // RollingCycleWindow suite above — this only checks Engine wiring.
    EXPECT_GE(engine.rollingMeanCycleUs(), engine.minCycleUs());
    EXPECT_LE(engine.rollingMeanCycleUs(), engine.maxCycleUs());
}

TEST(EngineCycleStats, ConcurrentStepAndRollingMeanReadIsRaceFree) {
    Engine engine;
    auto inp = makeMinimalEngine(engine);

    stressConcurrentReads(engine, inp, 2000, [&] {
        (void)engine.rollingMeanCycleUs();
    });

    EXPECT_GE(engine.rollingMeanCycleUs(), engine.minCycleUs());
    EXPECT_LE(engine.rollingMeanCycleUs(), engine.maxCycleUs());
}

TEST(EngineCycleStats, ExplicitWindowConstructorIsWired) {
    Engine engine(3);   // smaller than the default, to prove the ctor param reaches rollingCycles_
    auto inp = makeMinimalEngine(engine);

    driveNCycles(engine, inp, 5);   // more cycles than the window, so eviction has run at least once

    EXPECT_GE(engine.rollingMeanCycleUs(), engine.minCycleUs());
    EXPECT_LE(engine.rollingMeanCycleUs(), engine.maxCycleUs());
}

// ─────────────────────────────────────────────────────────────────────────────
// CycleSeqLock
// ─────────────────────────────────────────────────────────────────────────────

namespace {

// Two values a writer only ever changes together, inside one WriteScope. A read
// that sees them disagree is the torn read CycleSeqLock exists to reject.
struct PairedValues {
    std::atomic<int> a{0};
    std::atomic<int> b{0};

    std::pair<int, int> read() const {
        return {a.load(std::memory_order_relaxed), b.load(std::memory_order_relaxed)};
    }
};

void writeCycle(CycleSeqLock& lock, PairedValues& v, int value) {
    const CycleSeqLock::WriteScope scope(lock);
    v.a.store(value, std::memory_order_relaxed);
    v.b.store(value, std::memory_order_relaxed);
}

}  // namespace

TEST(CycleSeqLock, ReadIsImmediateWithNoConcurrentWriter) {
    CycleSeqLock lock;
    PairedValues v;
    writeCycle(lock, v, 7);

    int calls = 0;
    const auto got = lock.readConsistent([&] { ++calls; return v.read(); });

    EXPECT_EQ(calls, 1);
    EXPECT_EQ(got, std::make_pair(7, 7));
}

TEST(CycleSeqLock, RetriesWhenAnotherThreadsCycleInterruptsTheRead) {
    CycleSeqLock lock;
    PairedValues v;
    writeCycle(lock, v, 1);

    std::promise<void> goWrite, written;
    auto goWriteF = goWrite.get_future();
    auto writtenF = written.get_future();
    std::thread writer([&] {
        goWriteF.wait();
        writeCycle(lock, v, 2);
        written.set_value();
    });

    int calls = 0;
    const auto got = lock.readConsistent([&] {
        const int a = v.a.load(std::memory_order_relaxed);
        if (++calls == 1) {   // a whole cycle lands between reading a and b
            goWrite.set_value();
            writtenF.wait();
        }
        return std::make_pair(a, v.b.load(std::memory_order_relaxed));
    });
    writer.join();

    EXPECT_EQ(calls, 2);   // the torn (1, 2) read was rejected
    EXPECT_EQ(got, std::make_pair(2, 2));
}

TEST(CycleSeqLock, WaitsOutACycleAlreadyOpenOnAnotherThread) {
    CycleSeqLock lock;
    PairedValues v;
    writeCycle(lock, v, 1);

    std::promise<void> opened, readerStarted;
    auto openedF        = opened.get_future();
    auto readerStartedF = readerStarted.get_future();
    std::thread writer([&] {
        const CycleSeqLock::WriteScope scope(lock);
        v.a.store(2, std::memory_order_relaxed);   // half-written: b is still 1
        opened.set_value();
        readerStartedF.wait();
        v.b.store(2, std::memory_order_relaxed);
    });

    openedF.wait();
    readerStarted.set_value();
    int calls = 0;
    const auto got = lock.readConsistent([&] { ++calls; return v.read(); });
    writer.join();

    EXPECT_EQ(calls, 1);   // fn never ran while the cycle was open
    EXPECT_EQ(got, std::make_pair(2, 2));
}

TEST(CycleSeqLock, ReadInsideItsOwnOpenCycleReturnsAtOnce) {
    CycleSeqLock lock;
    PairedValues v;
    writeCycle(lock, v, 1);

    const CycleSeqLock::WriteScope scope(lock);
    v.a.store(2, std::memory_order_relaxed);
    int calls = 0;
    const auto got = lock.readConsistent([&] { ++calls; return v.read(); });   // waiting would hang

    EXPECT_EQ(calls, 1);
    EXPECT_EQ(got, std::make_pair(2, 1));   // the open cycle's own state, as it stands
}

TEST(CycleSeqLock, GivesUpAfterMaxAttemptsAndReturnsTheLastRead) {
    CycleSeqLock lock;
    PairedValues v;

    int calls = 0;
    const auto got = lock.readConsistent([&] {
        writeCycle(lock, v, ++calls);   // every read is interrupted by a whole cycle
        return v.read();
    }, "test", 3);

    EXPECT_EQ(calls, 3);
    EXPECT_EQ(got, std::make_pair(3, 3));
}

TEST(CycleSeqLock, ZeroMaxAttemptsThrows) {
    const CycleSeqLock lock;
    EXPECT_THROW(lock.readConsistent([] { return 0; }, "test", 0), std::invalid_argument);
}

TEST(EngineCycleSeqLock, EveryCycleInterruptsAReadInFlight) {
    Engine engine;
    makeMinimalEngine(engine);
    const auto lock = engine.cycleSeqLock();

    int calls = 0;
    lock->readConsistent([&] {
        if (++calls == 1) engine.step();   // cycle() must hold the engine's own lock open
        return calls;
    });

    EXPECT_EQ(calls, 2);
}

TEST(EngineCycleSeqLock, ReadFromInsideAnOutputCallbackReturnsAtOnce) {
    // Mirrors the replay exhaustion callback, which reads snapshots from inside cycle().
    Engine engine;
    auto inp = engine.makeInput<int>("x", 0);
    const auto lock = engine.cycleSeqLock();
    int readInside = -1;
    engine.addOutput<int>(inp, [&](const int& v) {
        readInside = lock->readConsistent([&] { return v; });
    });

    inp->set(5);
    engine.step();

    EXPECT_EQ(readInside, 5);
}

TEST(EngineCycleSeqLock, ThrowingCallbackLeavesNoCycleOpen) {
    Engine engine;
    auto inp = engine.makeInput<int>("x", 0);
    engine.addOutput<int>(inp, [](const int&) { throw std::runtime_error("boom"); });
    EXPECT_THROW(engine.step(), std::runtime_error);

    // A reader on another thread would wait forever on a cycle the throw left open.
    const auto lock = engine.cycleSeqLock();
    int got = 0;
    std::thread reader([&] { got = lock->readConsistent([] { return 1; }); });
    reader.join();

    EXPECT_EQ(got, 1);
}

TEST(EngineCycleSeqLock, ConcurrentStepAndConsistentReadNeverTears) {
    // Several outputs of one input fire one after another inside each cycle, so
    // mid-cycle they disagree; a read the lock accepts must never see that.
    constexpr std::size_t kOutputs = 8;
    Engine engine;
    auto inp = engine.makeInput<int>("x", 0);
    std::array<std::atomic<int>, kOutputs> vals{};
    for (std::size_t k = 0; k < kOutputs; ++k) {
        auto node = ComputeNode<int, int>::make(
            "pass" + std::to_string(k),
            std::make_tuple(std::static_pointer_cast<INode>(inp)),
            [](const int& v) { return v; });
        engine.addOutput<int>(node, [&vals, k](const int& v) {
            vals[k].store(v, std::memory_order_relaxed);
        });
    }
    const auto lock = engine.cycleSeqLock();

    int torn = 0;
    stressConcurrentReads(engine, inp, 2000, [&] {
        const auto got = lock->readConsistent([&] {
            std::array<int, kOutputs> r{};
            for (std::size_t k = 0; k < kOutputs; ++k) r[k] = vals[k].load(std::memory_order_relaxed);
            return r;
        }, "stress", std::numeric_limits<std::size_t>::max());   // never fall back to a torn read
        for (const int x : got) if (x != got[0]) { ++torn; break; }
    });

    EXPECT_EQ(torn, 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// Engine::run()
// ─────────────────────────────────────────────────────────────────────────────

namespace {

// What run() threw, or "" when it returned. Every run() these tests start ends inside its own
// cycles, by a throw or by a callback calling stop(), so none of them waits on a feed.
std::string runAndCatch(Engine& engine) {
    try {
        engine.run();
    } catch (const std::exception& e) {
        return e.what();
    }
    return "";
}

}  // namespace

// A throw out of a cycle ends the run. The next run() must start, not throw "already running"
// (flywheel-dag#16).
TEST(EngineRun, RunsAgainAfterANodeThrows) {
    Engine engine;
    auto x = engine.makeInput<double>("x", 1.0);
    bool fail = true;
    auto node = ComputeNode<double, double>::make(
        "node",
        std::make_tuple(std::static_pointer_cast<INode>(x)),
        [&fail](const double& v) {
            if (fail) throw std::domain_error("from a node");
            return v;
        });
    std::vector<double> seen;
    engine.addOutput<double>(node, [&](const double& v) {
        seen.push_back(v);
        engine.stop();
    });

    EXPECT_EQ(runAndCatch(engine), "from a node");
    EXPECT_EQ(runAndCatch(engine), "from a node");   // the node is still dirty, so this run retries it
    fail = false;
    EXPECT_EQ(runAndCatch(engine), "");
    EXPECT_EQ(seen, std::vector<double>{1.0});
}

TEST(EngineRun, RunsAgainAfterACallbackThrows) {
    Engine engine;
    auto x = engine.makeInput<double>("x", 1.0);
    bool fail = true;
    std::vector<double> seen;
    engine.addOutput<double>(x, [&](const double& v) {
        if (fail) throw std::runtime_error("from a callback");
        seen.push_back(v);
        engine.stop();
    });

    EXPECT_EQ(runAndCatch(engine), "from a callback");
    fail = false;
    x->set(2.0);   // x is clean after the first run's cycle, so give the second run a value to deliver
    EXPECT_EQ(runAndCatch(engine), "");
    EXPECT_EQ(seen, std::vector<double>{2.0});
}

// The flag a refused run() found set belongs to the run already going, so the refusal must leave
// it set.
TEST(EngineRun, ARunCalledWhileRunningThrowsAndLeavesTheRunGoing) {
    Engine engine;
    auto x = engine.makeInput<double>("x", 1.0);
    std::string nested;
    std::vector<double> seen;
    engine.addOutput<double>(x, [&](const double& v) {
        seen.push_back(v);
        if (seen.size() == 1) {
            nested = runAndCatch(engine);
            x->set(2.0);   // wakes the run for a second cycle
        } else {
            engine.stop();
        }
    });

    EXPECT_EQ(runAndCatch(engine), "");
    EXPECT_EQ(nested, "Engine::run() called while already running");
    EXPECT_EQ(seen, (std::vector<double>{1.0, 2.0}));
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
