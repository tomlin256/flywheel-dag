// =============================================================================
// flywheel-dag — A header-only C++17 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

#pragma once
// dag_async.hpp — async input nodes, feed plumbing, and engine-ready wake hooks
//
// Thread model
// ────────────
// Feed threads call post() — lock-protected, O(1), returns immediately.
// Eval thread  calls flush() — applies staged value, does DAG invalidation.
// Engine       sleeps on a condition variable, woken by a hook fired in post().
//
// The wake hook is the only coupling between this layer and the Engine:
//   input->setWakeHook([&engine]{ engine.poke(); });
//
// Contents
// ────────
//  IFlushable       — interface for sources the eval thread must drain
//  AsyncInput<T>    — "latest wins" thread-safe input node
//  AsyncQueue<T>    — FIFO; eval() yields std::vector<T> (full per-cycle batch)
//  FeedRegistry     — groups sources; propagates wake hook to all members
//  TickLoop         — simple fixed-rate loop (retained for standalone use)

#include "dag.hpp"
#include <mutex>
#include <utility>
#include <deque>
#include <atomic>
#include <thread>
#include <chrono>
#include <functional>
#include <condition_variable>

namespace dag::async {

// ─────────────────────────────────────────────────────────────────────────────
// IFlushable — anything the eval thread must drain before pulling the graph.
//
// setWakeHook() is called once at registration time (before any threads post).
// The hook is invoked from post() to signal the engine that work is pending.
// ─────────────────────────────────────────────────────────────────────────────
class IFlushable {
public:
    virtual ~IFlushable() = default;

    /// Drain staged data on the eval thread. Returns values applied (0 = nothing new).
    virtual std::size_t flush() = 0;

    /// Values waiting to be flushed right now.
    virtual std::size_t pendingCount() const = 0;

    virtual std::string name() const = 0;

    /// Register the engine's wake hook. Called once before any threads start posting.
    /// The hook must be cheap and non-blocking (it signals a condition variable).
    virtual void setWakeHook(std::function<void()> hook) = 0;
};

// ─────────────────────────────────────────────────────────────────────────────
// AsyncInput<T>  —  "latest wins" thread-safe input node.
//
// post()  — feed thread: stage a value. If one already exists it is overwritten.
// flush() — eval thread: apply staged value, propagate DAG invalidation.
//
// The wake hook is called at the end of post(), outside the staging lock,
// so the engine can react to the new data without lock contention.
//
// Staging storage is recycled, not reallocated
// ─────────────────────────────────────────────
// A feed's highest-rate stream can post on nearly every engine cycle, and a feed
// that merges deltas posts an lvalue it has to keep for the next merge. So
// neither side of the handover may throw storage away:
//
//   post() takes const T& and copy-ASSIGNS into pending_, which is a live T
//   rather than a disengaged optional. A vector's copy-assign reuses the
//   destination buffer whenever its capacity suffices, so for a value whose
//   vectors keep their size from one update to the next — the common case for
//   a fixed-depth snapshot — that is no allocation at all.
//
//   flush() SWAPS pending_ with staged_ rather than moving it out. staged_ hands
//   its buffers back to pending_ for the next post() to copy into, and hands its
//   contents to ValueSlot::emit(const T&), which copy-assigns them into the
//   recycled TypedValue for the same reason.
//
// Capacity therefore cycles between pending_, staged_ and the two slot buffers,
// and after two updates the whole path allocates nothing. The copy now happens
// under the staging lock, where a move-out did not: for a two-vector value that
// is a small memcpy against the 2 malloc + 2 free it replaces.
//
// T must be default-constructible and copy-assignable — pending_ and staged_ are
// plain members, not optionals.
// ─────────────────────────────────────────────────────────────────────────────
template<typename T>
class AsyncInput
    : public NodeBase
    , public IFlushable
    , public std::enable_shared_from_this<AsyncInput<T>>
{
public:
    static std::shared_ptr<AsyncInput<T>> make(
        std::string name, T initial = T{},
        EqualityPolicyPtr eq = nullptr);

    // ── Feed-thread API ──────────────────────────────────────────────────────
    /// Stage a value for the next flush. Copy-assigns into storage this node
    /// retains, so a T with heap members costs no allocation once warm.
    void post(const T& val);

    // ── Eval-thread API ──────────────────────────────────────────────────────
    std::size_t flush() override;
    std::size_t pendingCount() const override;
    std::size_t skippedCount() const;
    void setWakeHook(std::function<void()> hook) override;
    const T& current() const;

    // ── INode ────────────────────────────────────────────────────────────────
    ValuePtr eval(EvalContext&) override;
    std::string name() const override;
    std::vector<NodePtr> inputs() const override;
    NodeKind kind() const override { return NodeKind::AsyncInput; }

private:
    AsyncInput(std::string n, T initial, EqualityPolicyPtr eq);

    std::string name_;
    ValuePtr value_;
    EqualityPolicyPtr eq_;

    mutable std::mutex mutex_;
    T pending_{};                      // storage retained across posts; capacity recycled
    bool hasPending_ = false;          // pending_ holds a value flush() has not taken yet
    std::size_t skipped_ = 0;
    std::function<void()> wakeHook_;   // set once; safe to read without lock thereafter
    T staged_{};                       // flush() only — swapped with pending_ under the lock
    ValueSlot<T> slot_;                // flush() only — eval thread, never post()
};

template<typename T>
using InputPtr = std::shared_ptr<AsyncInput<T>>;

// ─────────────────────────────────────────────────────────────────────────────
// AsyncQueue<T>  —  FIFO; flush delivers every value in arrival order.
//
// eval() returns std::vector<T> — the full batch of values posted since the
// last flush. Downstream ComputeNodes declare their input type as
// std::vector<T> and receive the entire batch each cycle, enabling per-cycle
// sums, counts and averages without any handler side-channel.
// ─────────────────────────────────────────────────────────────────────────────
template<typename T>
class AsyncQueue
    : public NodeBase
    , public IFlushable
    , public std::enable_shared_from_this<AsyncQueue<T>>
{
public:
    static std::shared_ptr<AsyncQueue<T>> make(
        std::string name, std::size_t maxQueueSize = 4096);

    void post(T val);

    std::size_t flush() override;
    std::size_t pendingCount() const override;
    std::size_t droppedCount() const;
    void setWakeHook(std::function<void()> hook) override;

    ValuePtr eval(EvalContext&) override;
    std::string name() const override;
    std::vector<NodePtr> inputs() const override;
    NodeKind kind() const override { return NodeKind::AsyncQueue; }

private:
    explicit AsyncQueue(std::string n, std::size_t maxSz);

    std::string name_;
    ValuePtr value_;                                // holds std::vector<T>
    ValuePtr emptyValue_;                           // the one [] value; flush() rebinds value_ to it
    mutable std::mutex mutex_;
    std::deque<T> queue_;
    std::deque<T> drain_;                           // flush() only — eval thread, never post();
                                                    // swapped with queue_, so its blocks are recycled
    std::size_t maxSize_;
    std::size_t dropped_ = 0;
    std::function<void()> wakeHook_;
};

template<typename T>
using QueuePtr = std::shared_ptr<AsyncQueue<T>>;

// ─────────────────────────────────────────────────────────────────────────────
// FeedRegistry — groups IFlushable sources.
//
// Propagates the wake hook to all current and future members so the engine
// only needs to call registry.setWakeHook() once.
// ─────────────────────────────────────────────────────────────────────────────
class FeedRegistry {
public:
    void add(std::shared_ptr<IFlushable> input);
    std::size_t flush();
    bool hasPending() const;
    /// Propagate hook to all existing and future members.
    void setWakeHook(std::function<void()> hook);
    const std::vector<std::shared_ptr<IFlushable>>& all() const;

private:
    std::vector<std::shared_ptr<IFlushable>> inputs_;
    std::function<void()> wakeHook_;
};

// ─────────────────────────────────────────────────────────────────────────────
// TickLoop — fixed-rate eval loop. Retained for simple standalone use cases
// where event-driven wake-up is not required. For production use, prefer Engine.
// ─────────────────────────────────────────────────────────────────────────────
class TickLoop {
public:
    using Callback = std::function<void()>;
    using Duration = std::chrono::milliseconds;

    explicit TickLoop(Duration interval, Callback cb);
    ~TickLoop();

    void start();
    void stop();
    bool running() const;

private:
    Duration interval_;
    Callback cb_;
    std::atomic<bool> running_{false};
    std::thread thread_;
};

} // namespace dag::async

#include "dag_async.inl"
