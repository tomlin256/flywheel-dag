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
//  FeedRegistry    — flush-all, hasPending, pendingCount, order, wake hook propagation,
//                    nesting, includes(), and the engine flushing a member added after
//                    addSource()
//  Engine sources  — a source the engine already flushes is not added again
//  TickLoop        — start/stop lifecycle, callback delivery
//  CycleSeqLock    — consistent cross-thread reads over Engine::cycle()
//  Engine::run()   — a cycle that throws ends the run, and run() can start again
//  Engine::cycle() — a cycle that throws leaves the outputs it did not reach due,
//                    and a new output gets the value its node holds at its first cycle

#include <gtest/gtest.h>
#include "flywheel/dag.hpp"
#include "flywheel/dag_async.hpp"
#include "flywheel/dag_engine.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <limits>
#include <memory>
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
    EXPECT_EQ(inp->current(), third) << "latest wins";
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
    // A ComputeNode that folds the batch is the canonical use case.
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
// consumed. The refresh rebinds a shared constant: it allocates nothing.

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

namespace {

// A source that notes each flush in a shared log, so a test can read the order sources flush in.
class LoggedSource final : public IFlushable {
public:
    LoggedSource(std::string name, std::vector<std::string>& log)
        : name_(std::move(name)), log_(log) {}

    std::size_t flush() override { log_.push_back(name_); return 0; }
    std::size_t pendingCount() const override { return 0; }
    std::string name() const override { return name_; }
    void setWakeHook(std::function<void()>) override {}

private:
    std::string               name_;
    std::vector<std::string>& log_;
};

// A source that counts the flushes it gets and the wake hooks it is given.
class CountedSource final : public IFlushable {
public:
    std::size_t flush() override { ++flushes_; return 0; }
    std::size_t pendingCount() const override { return 0; }
    std::string name() const override { return "counted"; }
    void setWakeHook(std::function<void()>) override { ++hooks_; }

    int flushes() const { return flushes_; }
    int hooks()   const { return hooks_; }

private:
    int flushes_ = 0;
    int hooks_   = 0;
};

}  // namespace

TEST(FeedRegistry, PendingCountSumsTheMembers) {
    FeedRegistry reg;
    auto a = AsyncInput<double>::make("a", 0.0);
    auto q = AsyncQueue<int>::make("q");
    reg.add(a);
    reg.add(q);

    EXPECT_EQ(reg.pendingCount(), 0u);
    a->post(1.0);
    q->post(1);
    q->post(2);
    EXPECT_EQ(reg.pendingCount(), 3u);   // a's one latest-wins value and q's two items
    reg.flush();
    EXPECT_EQ(reg.pendingCount(), 0u);
}

TEST(FeedRegistry, NameIsFeedRegistry) {
    EXPECT_EQ(FeedRegistry().name(), "feed_registry");
}

TEST(FeedRegistry, FlushesMembersInTheOrderTheyWereAdded) {
    std::vector<std::string> log;
    FeedRegistry reg;
    reg.add(std::make_shared<LoggedSource>("first", log));
    reg.add(std::make_shared<LoggedSource>("second", log));
    reg.flush();
    reg.add(std::make_shared<LoggedSource>("third", log));   // added after a flush
    reg.flush();

    EXPECT_EQ(log, (std::vector<std::string>{"first", "second", "first", "second", "third"}));
}

TEST(FeedRegistry, ARegistryIsAMemberOfAnother) {
    auto inner = std::make_shared<FeedRegistry>();
    auto a = AsyncInput<double>::make("a", 0.0);
    inner->add(a);

    FeedRegistry outer;
    std::atomic<int> hookCount{0};
    outer.setWakeHook([&] { ++hookCount; });
    outer.add(inner);   // after the hook is set: it must reach a through inner

    a->post(1.0);
    EXPECT_EQ(hookCount.load(), 1);
    EXPECT_EQ(outer.pendingCount(), 1u);
    EXPECT_EQ(outer.flush(), 1u);
    EXPECT_EQ(a->current(), 1.0);
    EXPECT_EQ(outer.pendingCount(), 0u);
}

// ─────────────────────────────────────────────────────────────────────────────
// A source held twice (flywheel-dag#36)
//
// flush() drains, so a second flush in a cycle finds nothing, and an AsyncQueue's
// rebinds its value to [] before any node has read the batch the first made.
// The engine and a registry hold a source once for that reason.
// ─────────────────────────────────────────────────────────────────────────────

TEST(IFlushable, IncludesItselfAndNoOtherSource) {
    const CountedSource a;
    const CountedSource b;
    EXPECT_TRUE(a.includes(a));
    EXPECT_FALSE(a.includes(b));
}

TEST(FeedRegistry, IncludesItselfAndItsMembersAtAnyDepth) {
    auto deep = std::make_shared<CountedSource>();
    auto inner = std::make_shared<FeedRegistry>();
    inner->add(deep);
    auto member = std::make_shared<CountedSource>();
    FeedRegistry outer;
    outer.add(member);
    outer.add(inner);
    const CountedSource stranger;

    EXPECT_TRUE(outer.includes(outer));
    EXPECT_TRUE(outer.includes(*member));
    EXPECT_TRUE(outer.includes(*inner));
    EXPECT_TRUE(outer.includes(*deep));
    EXPECT_FALSE(outer.includes(stranger));
    EXPECT_FALSE(inner->includes(*member)) << "a registry does not include its siblings";
}

TEST(FeedRegistry, AMemberAddedTwiceIsHeldOnce) {
    FeedRegistry reg;
    reg.setWakeHook([] {});
    auto a = std::make_shared<CountedSource>();
    reg.add(a);
    reg.add(a);

    EXPECT_EQ(reg.all().size(), 1u);
    reg.flush();
    EXPECT_EQ(a->flushes(), 1);
    EXPECT_EQ(a->hooks(), 1) << "the repeat must not install the hook again";
}

TEST(FeedRegistry, AMemberOfAMemberRegistryIsNotAddedAgain) {
    auto inner = std::make_shared<FeedRegistry>();
    auto a = std::make_shared<CountedSource>();
    inner->add(a);
    FeedRegistry outer;
    outer.add(inner);
    outer.add(a);

    EXPECT_EQ(outer.all().size(), 1u);
    outer.flush();
    EXPECT_EQ(a->flushes(), 1);
}

// A registry includes itself, so adding it to itself is ignored. Held, it would send flush() and
// includes() round for ever; the check comes before anything is added or flushed for that reason.
TEST(FeedRegistry, ARegistryAddedToItselfIsIgnored) {
    auto reg = std::make_shared<FeedRegistry>();
    reg->add(reg);
    ASSERT_TRUE(reg->all().empty());

    auto a = std::make_shared<CountedSource>();
    reg->add(a);
    reg->flush();
    EXPECT_EQ(a->flushes(), 1);
}

// ─────────────────────────────────────────────────────────────────────────────
// Engine and FeedRegistry
// ─────────────────────────────────────────────────────────────────────────────

// A registry is a source like any other, so the engine flushes it whole: a member added after
// addSource() is flushed too (flywheel-dag#27).
TEST(EngineFeedRegistry, AMemberAddedAfterAddSourceIsFlushed) {
    Engine engine;
    auto reg = std::make_shared<FeedRegistry>();
    auto a = AsyncInput<int>::make("a", 0);
    reg->add(a);
    engine.addSource(reg);
    auto b = AsyncInput<int>::make("b", 0);
    reg->add(b);   // after addSource()

    int seenA = -1, seenB = -1;
    engine.addOutput<int>(a, [&](const int& v) { seenA = v; });
    engine.addOutput<int>(b, [&](const int& v) { seenB = v; });
    engine.step();
    a->post(1);
    b->post(2);
    engine.step();

    EXPECT_EQ(seenA, 1);
    EXPECT_EQ(seenB, 2);
    EXPECT_EQ(b->pendingCount(), 0u);
}

// A member added after addSource() posts from inside a cycle, as a feed thread would. The post
// must wake run() through the hook the engine gave the registry, and the next cycle must flush it.
// If either fails, run() waits on its condition variable for ever, which ctest reports as a
// timeout, not a wrong value.
TEST(EngineFeedRegistry, ALateMemberWakesRunAndIsFlushed) {
    Engine engine;
    auto reg = std::make_shared<FeedRegistry>();
    engine.addSource(reg);
    auto late = AsyncInput<int>::make("late", 0);
    reg->add(late);

    std::vector<int> seen;
    engine.addOutput<int>(late, [&](const int& v) {
        seen.push_back(v);
        if (v == 0) late->post(2);   // cycle 1 delivers the initial value
        else        engine.stop();
    });
    engine.run();

    EXPECT_EQ(seen, (std::vector<int>{0, 2}));
    EXPECT_EQ(engine.cycleCount(), 2u);
}

// The registry keeps its place in the engine's order, and a member added late joins it there.
TEST(EngineFeedRegistry, ALateMemberFlushesInTheRegistrysPlace) {
    std::vector<std::string> log;
    Engine engine;
    auto reg = std::make_shared<FeedRegistry>();
    engine.addSource(std::make_shared<LoggedSource>("before", log));
    engine.addSource(reg);
    engine.addSource(std::make_shared<LoggedSource>("after", log));
    reg->add(std::make_shared<LoggedSource>("member", log));   // added once "after" is registered
    engine.step();

    EXPECT_EQ(log, (std::vector<std::string>{"before", "member", "after"}));
}

// ─────────────────────────────────────────────────────────────────────────────
// Engine and a source added twice (flywheel-dag#36)
// ─────────────────────────────────────────────────────────────────────────────

TEST(EngineSources, ASourceAddedTwiceIsFlushedOncePerCycle) {
    Engine engine;
    auto s = std::make_shared<CountedSource>();
    engine.addSource(s);
    engine.addSource(s);
    engine.step();
    engine.step();

    EXPECT_EQ(s->flushes(), 2) << "one flush per cycle, over two cycles";
    EXPECT_EQ(s->hooks(), 1) << "the repeat must not install the hook again";
}

// The issue's sequence: the queue was flushed twice in the second cycle, and the second flush
// replaced its batch with [] before the output read it.
TEST(EngineSources, AnAsyncQueueAddedTwiceDeliversItsBatch) {
    Engine engine;
    auto q = AsyncQueue<int>::make("q");
    engine.addSource(q);
    engine.addSource(q);
    int calls = 0;
    std::vector<int> last;
    engine.addOutput<std::vector<int>>(q, [&](const std::vector<int>& batch) {
        ++calls;
        last = batch;
    });
    engine.step();   // delivers the empty batch every new output gets
    q->post(1);
    q->post(2);
    engine.step();

    EXPECT_EQ(calls, 2);
    EXPECT_EQ(last, (std::vector<int>{1, 2}));
}

TEST(EngineSources, ARepeatKeepsTheSourcesFirstPlaceInTheOrder) {
    std::vector<std::string> log;
    Engine engine;
    auto a = std::make_shared<LoggedSource>("a", log);
    engine.addSource(a);
    engine.addSource(std::make_shared<LoggedSource>("b", log));
    engine.addSource(a);   // after b, and ignored, so a still flushes first
    engine.step();

    EXPECT_EQ(log, (std::vector<std::string>{"a", "b"}));
}

TEST(EngineSources, ASourceInsideARegistryIsNotAddedAgain) {
    Engine engine;
    auto reg = std::make_shared<FeedRegistry>();
    auto s = std::make_shared<CountedSource>();
    reg->add(s);
    engine.addSource(reg);
    engine.addSource(s);
    engine.step();

    EXPECT_EQ(s->flushes(), 1);
}

TEST(EngineSources, ASourceAddedToARegistryAfterItsAddSourceIsNotAddedAgain) {
    Engine engine;
    auto reg = std::make_shared<FeedRegistry>();
    engine.addSource(reg);
    auto s = std::make_shared<CountedSource>();
    reg->add(s);   // after addSource()
    engine.addSource(s);
    engine.step();

    EXPECT_EQ(s->flushes(), 1);
}

TEST(EngineSources, ASourceInsideANestedRegistryIsNotAddedAgain) {
    Engine engine;
    auto inner = std::make_shared<FeedRegistry>();
    auto outer = std::make_shared<FeedRegistry>();
    auto s = std::make_shared<CountedSource>();
    inner->add(s);
    outer->add(inner);
    engine.addSource(outer);
    engine.addSource(s);
    engine.step();

    EXPECT_EQ(s->flushes(), 1);
}

TEST(EngineSources, ARegistryAddedTwiceIsFlushedOnce) {
    Engine engine;
    auto reg = std::make_shared<FeedRegistry>();
    auto s = std::make_shared<CountedSource>();
    reg->add(s);
    engine.addSource(reg);
    engine.addSource(reg);
    engine.step();

    EXPECT_EQ(s->flushes(), 1);
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
// Engine dirty-snapshot ordering
// ─────────────────────────────────────────────────────────────────────────────

// A ComputeNode that depends on an AsyncInput is registered as an output BEFORE
// the AsyncInput itself.  The compute node's eval() evaluates the AsyncInput,
// clearing its dirty state before the engine loop reaches the AsyncInput's
// entry.  cycle() marks every dirty output due before any eval() runs, so both
// callbacks must fire regardless of registration order.
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
    EXPECT_DOUBLE_EQ(lastSrc,      5.0);  // leaf also fired
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

// A cycle that throws is counted by cycleCount(), so it is timed too: every statistic covers the
// cycles cycleCount() counts. A throw takes microseconds, far above the clock's tick, so such a
// cycle always measures more than zero.

namespace {

class ThrowingSource final : public IFlushable {
public:
    std::size_t flush() override { throw std::runtime_error("from a source"); }
    std::size_t pendingCount() const override { return 0; }
    std::string name() const override { return "throwing"; }
    void setWakeHook(std::function<void()>) override {}
};

// The engine has run one cycle, and every statistic reads its duration.
void expectOnlyCycleTimed(const Engine& engine) {
    EXPECT_EQ(engine.cycleCount(), 1u);
    EXPECT_GT(engine.lastCycleUs(), 0.0);
    EXPECT_DOUBLE_EQ(engine.minCycleUs(),         engine.lastCycleUs());
    EXPECT_DOUBLE_EQ(engine.maxCycleUs(),         engine.lastCycleUs());
    EXPECT_DOUBLE_EQ(engine.meanCycleUs(),        engine.lastCycleUs());
    EXPECT_DOUBLE_EQ(engine.rollingMeanCycleUs(), engine.lastCycleUs());
}

}  // namespace

TEST(EngineCycleStats, ACycleThatThrowsFromACallbackIsTimed) {
    Engine engine;
    auto x = engine.makeInput<int>("x", 0);
    engine.addOutput<int>(x, [](const int&) { throw std::runtime_error("from a callback"); });

    EXPECT_THROW(engine.step(), std::runtime_error);

    expectOnlyCycleTimed(engine);
}

TEST(EngineCycleStats, ACycleThatThrowsFromANodeIsTimed) {
    Engine engine;
    auto x = engine.makeInput<int>("x", 0);
    auto node = ComputeNode<int, int>::make(
        "boom",
        std::make_tuple(std::static_pointer_cast<INode>(x)),
        [](const int&) -> int { throw std::runtime_error("from a node"); });
    engine.addOutput<int>(node, [](const int&) {});

    EXPECT_THROW(engine.step(), std::runtime_error);

    expectOnlyCycleTimed(engine);
}

TEST(EngineCycleStats, ACycleThatThrowsFromASourceIsTimed) {
    Engine engine;
    engine.addSource(std::make_shared<ThrowingSource>());

    EXPECT_THROW(engine.step(), std::runtime_error);

    expectOnlyCycleTimed(engine);
}

// A cycle that throws, then one that does not: each statistic covers both.
TEST(EngineCycleStats, StatsCoverACycleThatThrewAndOneThatDidNot) {
    Engine engine;
    auto x = engine.makeInput<int>("x", 0);
    bool fail = true;
    engine.addOutput<int>(x, [&fail](const int&) {
        if (fail) {
            fail = false;
            throw std::runtime_error("from a callback");
        }
    });

    EXPECT_THROW(engine.step(), std::runtime_error);
    const double thrown = engine.lastCycleUs();
    x->set(1);
    engine.step();
    const double completed = engine.lastCycleUs();

    EXPECT_EQ(engine.cycleCount(), 2u);
    EXPECT_DOUBLE_EQ(engine.minCycleUs(), std::min(thrown, completed));
    EXPECT_DOUBLE_EQ(engine.maxCycleUs(), std::max(thrown, completed));
    EXPECT_NEAR(engine.meanCycleUs(),        (thrown + completed) / 2.0, 1e-9);
    EXPECT_NEAR(engine.rollingMeanCycleUs(), (thrown + completed) / 2.0, 1e-9);
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

// A throw out of a cycle ends the run. The next run() must start, not throw "already running".
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

// ─────────────────────────────────────────────────────────────────────────────
// A cycle that throws
// ─────────────────────────────────────────────────────────────────────────────

// A cycle that throws must leave the outputs it did not reach due. a's output is registered before
// b's, and a pulls b clean before its callback throws, so b reads clean when the next cycle starts.
TEST(EngineAbortedCycle, ALaterOutputGetsItsValueAfterACallbackThrows) {
    Engine engine;
    auto x = engine.makeInput<double>("x", 1.0);
    auto b = ComputeNode<double, double>::make(
        "b",
        std::make_tuple(std::static_pointer_cast<INode>(x)),
        [](const double& v) { return v * 10.0; });
    auto a = ComputeNode<double, double>::make(
        "a",
        std::make_tuple(std::static_pointer_cast<INode>(b)),
        [](const double& v) { return v + 1.0; });
    bool fail = false;
    engine.addOutput<double>(a, [&fail](const double&) {
        if (fail) {
            fail = false;
            throw std::runtime_error("from a callback");
        }
    });
    std::vector<double> seenB;
    engine.addOutput<double>(b, [&seenB](const double& v) { seenB.push_back(v); });

    engine.step();
    x->set(2.0);
    fail = true;
    EXPECT_THROW(engine.step(), std::runtime_error);   // a pulls b to 20, then a's callback throws
    engine.step();
    EXPECT_EQ(seenB, (std::vector<double>{10.0, 20.0}));
}

// The same when a's functor throws after it has pulled b. a stays dirty, so each cycle retries it,
// and b's output waits for the one in which a succeeds.
TEST(EngineAbortedCycle, ALaterOutputGetsItsValueAfterANodeThrows) {
    Engine engine;
    auto x = engine.makeInput<double>("x", 1.0);
    auto b = ComputeNode<double, double>::make(
        "b",
        std::make_tuple(std::static_pointer_cast<INode>(x)),
        [](const double& v) { return v * 10.0; });
    bool fail = false;
    auto a = ComputeNode<double, double>::make(
        "a",
        std::make_tuple(std::static_pointer_cast<INode>(b)),
        [&fail](const double& v) {
            if (fail) throw std::domain_error("from a node");
            return v + 1.0;
        });
    std::vector<double> seenA;
    std::vector<double> seenB;
    engine.addOutput<double>(a, [&seenA](const double& v) { seenA.push_back(v); });
    engine.addOutput<double>(b, [&seenB](const double& v) { seenB.push_back(v); });

    engine.step();
    x->set(2.0);
    fail = true;
    EXPECT_THROW(engine.step(), std::domain_error);   // a pulls b to 20, then a's functor throws
    EXPECT_THROW(engine.step(), std::domain_error);   // a is still dirty, so this cycle retries it
    fail = false;
    engine.step();
    EXPECT_EQ(seenA, (std::vector<double>{11.0, 21.0}));
    EXPECT_EQ(seenB, (std::vector<double>{10.0, 20.0}));
}

// Through run(), which an application can start again after a throw. The output registered last is
// on halt, which the test sets before the second run(). Its callback stops the engine, so that run
// ends after its first cycle, which reaches the other outputs first, and neither run waits on a
// feed.
TEST(EngineAbortedCycle, ARestartedRunDeliversWhatTheAbortedCycleDidNot) {
    Engine engine;
    auto x = engine.makeInput<double>("x", 1.0);
    auto b = ComputeNode<double, double>::make(
        "b",
        std::make_tuple(std::static_pointer_cast<INode>(x)),
        [](const double& v) { return v * 10.0; });
    auto a = ComputeNode<double, double>::make(
        "a",
        std::make_tuple(std::static_pointer_cast<INode>(b)),
        [](const double& v) { return v + 1.0; });
    bool fail = false;
    engine.addOutput<double>(a, [&fail](const double&) {
        if (fail) {
            fail = false;
            throw std::runtime_error("from a callback");
        }
    });
    std::vector<double> seenB;
    engine.addOutput<double>(b, [&](const double& v) {
        seenB.push_back(v);
        if (seenB.size() == 1) {
            fail = true;
            x->set(2.0);   // wakes the run for a cycle in which a's callback throws
        }
    });
    auto halt = engine.makeInput<bool>("halt", false);
    engine.addOutput<bool>(halt, [&engine](const bool& h) {
        if (h) engine.stop();
    });

    EXPECT_EQ(runAndCatch(engine), "from a callback");
    halt->set(true);
    EXPECT_EQ(runAndCatch(engine), "");
    EXPECT_EQ(seenB, (std::vector<double>{10.0, 20.0}));
}

// A callback that throws has had its value. The engine records the value as delivered before it
// calls the callback, so it does not offer that value again, only the next one.
TEST(EngineAbortedCycle, ACallbackThatThrowsHasHadItsValue) {
    Engine engine;
    auto x = engine.makeInput<double>("x", 1.0);
    bool fail = false;
    std::vector<double> seen;
    engine.addOutput<double>(x, [&](const double& v) {
        seen.push_back(v);
        if (fail) {
            fail = false;
            throw std::runtime_error("from a callback");
        }
    });

    engine.step();
    x->set(2.0);
    fail = true;
    EXPECT_THROW(engine.step(), std::runtime_error);
    engine.step();
    EXPECT_EQ(seen, (std::vector<double>{1.0, 2.0}));
    x->set(3.0);
    engine.step();
    EXPECT_EQ(seen, (std::vector<double>{1.0, 2.0, 3.0}));
}

// ─────────────────────────────────────────────────────────────────────────────
// An output registered on a clean node
// ─────────────────────────────────────────────────────────────────────────────

namespace {

// b = 10·x and a = b + 1.
struct NewOutputGraph {
    Engine                engine;
    dag::InputPtr<double> x = engine.makeInput<double>("x", 1.0);
    NodePtr               b = ComputeNode<double, double>::make(
        "b",
        std::make_tuple(std::static_pointer_cast<INode>(x)),
        [](const double& v) { return v * 10.0; });
    NodePtr               a = ComputeNode<double, double>::make(
        "a",
        std::make_tuple(b),
        [](const double& v) { return v + 1.0; });
};

// A module downstream of another: it is given a node the other built, and registers an output on
// it in wire().
struct OutputOnNodeModule : IComputeModule {
    explicit OutputOnNodeModule(NodePtr n) : node(std::move(n)) {}

    std::string name() const override { return "output-on-node"; }

    void wire(Engine& engine) override {
        engine.addOutput<double>(node, [this](const double& v) { seen.push_back(v); });
    }

    NodePtr             node;
    std::vector<double> seen;
};

}  // namespace

// A new output must get the value its node holds, though another output's cycle has pulled the
// node clean. Registered before the first step(), b's output gets 10 on that step, so what a
// callback sees must not depend on when it was registered.
TEST(EngineNewOutput, GetsTheValueItsNodeHoldsAfterAnotherOutputPulledIt) {
    NewOutputGraph g;
    g.engine.addOutput<double>(g.a, [](const double&) {});
    g.engine.step();
    ASSERT_FALSE(g.b->dirty());   // a pulled b clean
    std::vector<double> seenB;
    g.engine.addOutput<double>(g.b, [&seenB](const double& v) { seenB.push_back(v); });

    g.engine.step();
    g.engine.step();
    EXPECT_EQ(seenB, std::vector<double>{10.0});   // once, over two cycles
    g.x->set(2.0);
    g.engine.step();
    EXPECT_EQ(seenB, (std::vector<double>{10.0, 20.0}));
}

// The same when a caller has evaluated the node.
TEST(EngineNewOutput, GetsTheValueItsNodeHoldsAfterACallerEvaluatedIt) {
    NewOutputGraph g;
    EvalContext ctx;
    const ValuePtr held = g.b->eval(ctx);
    ASSERT_EQ(get_value<double>(held), 10.0);
    ASSERT_FALSE(g.b->dirty());
    std::vector<double> seenB;
    g.engine.addOutput<double>(g.b, [&seenB](const double& v) { seenB.push_back(v); });

    g.engine.step();
    EXPECT_EQ(seenB, std::vector<double>{10.0});
}

// Through run(), which an application can call again once it has returned. halt's callback stops
// the engine. halt starts dirty, and the test sets it again before the second run(), so each run
// ends after its initial cycle, which reaches every output, b's included, and neither waits on a
// feed.
TEST(EngineNewOutput, ARunStartedAgainDeliversAnOutputRegisteredBetweenRuns) {
    NewOutputGraph g;
    g.engine.addOutput<double>(g.a, [](const double&) {});
    auto halt = g.engine.makeInput<int>("halt", 1);
    g.engine.addOutput<int>(halt, [&g](const int&) { g.engine.stop(); });

    EXPECT_EQ(runAndCatch(g.engine), "");
    ASSERT_FALSE(g.b->dirty());   // a pulled b clean
    std::vector<double> seenB;
    g.engine.addOutput<double>(g.b, [&seenB](const double& v) { seenB.push_back(v); });
    halt->set(2);
    EXPECT_EQ(runAndCatch(g.engine), "");
    EXPECT_EQ(seenB, std::vector<double>{10.0});
}

// addFeedback() registers through addOutput(), so a feedback registered after a cycle sets its
// input to the value its node holds.
TEST(EngineNewOutput, AFeedbackRegisteredAfterACycleSetsItsInput) {
    NewOutputGraph g;
    g.engine.addOutput<double>(g.a, [](const double&) {});
    g.engine.step();
    ASSERT_FALSE(g.b->dirty());   // a pulled b clean
    auto y = g.engine.makeInput<double>("y", 0.0);
    g.engine.addFeedback<double>(g.b, y);

    g.engine.step();
    EXPECT_EQ(y->get(), 10.0);
}

// install() registers through the module's wire(), so a module installed after a cycle gets the
// value its output's node holds.
TEST(EngineNewOutput, AModuleInstalledAfterACycleGetsItsOutputsValue) {
    NewOutputGraph g;
    g.engine.addOutput<double>(g.a, [](const double&) {});
    g.engine.step();
    ASSERT_FALSE(g.b->dirty());   // a pulled b clean
    auto downstream = std::make_shared<OutputOnNodeModule>(g.b);
    g.engine.install(downstream);

    g.engine.step();
    EXPECT_EQ(downstream->seen, std::vector<double>{10.0});
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
