// =============================================================================
// flywheel-dag — A header-only C++17 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

#pragma once
// dag_replay.hpp — deterministic replay primitives for the reactive engine.
//
// A recorded session is played back through an unmodified DAG by three pieces:
//
//   ReplayClock       — maps recorded capture time onto a synthetic steady_clock
//                        timeline so wall-clock-dependent nodes run on recorded
//                        time. now() can stand in for any clock function an
//                        application injects.
//   ReplayCoordinator — an IFlushable that owns the master cursor. Each flush()
//                        advances one seq-group and re-arms the engine's wake
//                        hook, so a self-chaining wake drives Engine::run() to
//                        completion with no threads, timers, or network.
//   ReplayInput<T> /   — IFlushable/INode replay counterparts of AsyncInput<T> /
//   ReplayQueue<T>       AsyncQueue<T>; they gate delivery on the coordinator's
//                        currentSeq().
//
// Why the coordinator wakes the engine: the run loop is condition-variable
// driven (dag_engine.inl), so it only cycles when a source fires the wake hook.
// A live AsyncInput fires it from post() on a feed thread; in replay there is no
// feed thread, so the coordinator fires it from flush() on the eval thread —
// safe, because the hook only sets a flag the run loop re-checks after the cycle.

#include "dag_async.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace dag::async {

// ─────────────────────────────────────────────────────────────────────────────
// ReplayClock — recorded time on a synthetic steady_clock timeline.
//
//   now() = base + (currentGroupTs − firstGroupTs)
//
// base is captured at construction; the coordinator advances currentGroupTs as
// it delivers each group. Timestamps are carried, never slept on — replay runs
// at full engine speed. Monotonic given non-decreasing recorded timestamps.
// ─────────────────────────────────────────────────────────────────────────────
class ReplayClock {
public:
    using time_point = std::chrono::steady_clock::time_point;

    ReplayClock() : base_(std::chrono::steady_clock::now()) {}

    /// Anchor the timeline at the first group's ts (offset zero maps to base).
    void setFirstTs(std::int64_t tsUs) { firstTs_ = tsUs; currentTs_ = tsUs; }

    /// Advance the synthetic clock to a group's ts.
    void setCurrentTs(std::int64_t tsUs) { currentTs_ = tsUs; }

    time_point now() const {
        return base_ + std::chrono::microseconds(currentTs_ - firstTs_);
    }

private:
    time_point   base_;
    std::int64_t firstTs_   = 0;
    std::int64_t currentTs_ = 0;
};

using ReplayClockPtr = std::shared_ptr<ReplayClock>;

// ─────────────────────────────────────────────────────────────────────────────
// ReplayCoordinator — the master cursor over a recorded session.
//
// Holds a schedule: the ascending, distinct-seq list of (seq, ts) groups merged
// across every replayed stream (a group's ts is the latest event ts in it).
// Registered FIRST in the replay FeedRegistry so the cursor advances before any
// replay source flushes in the same cycle; sources then gate on currentSeq().
//
// flush() delivers one group per call and fires the wake hook, so Engine::run()
// self-chains to completion. After the last group it keeps waking for
// drainCycles further cycles (default 1) — letting endogenous events posted
// during the final delivery (e.g. an application's simulated responses) flush
// and propagate — then fires the exhausted callback exactly once. The app calls
// Engine::stop() from it.
//
// Schedule precondition: sorted ascending by seq with distinct seqs. An
// application's replay reader builds it that way; standalone tests construct it
// directly.
// ─────────────────────────────────────────────────────────────────────────────
class ReplayCoordinator : public IFlushable {
public:
    struct Group {
        std::uint64_t seq  = 0;
        std::int64_t  tsUs = 0;
    };

    static std::shared_ptr<ReplayCoordinator> make(
        std::vector<Group> schedule, std::size_t drainCycles = 1);

    // ── IFlushable ────────────────────────────────────────────────────────────
    std::size_t flush() override;
    std::size_t pendingCount() const override;
    std::string name() const override;
    void setWakeHook(std::function<void()> hook) override;

    // ── Cursor / lifecycle ────────────────────────────────────────────────────

    /// The seq of the group being delivered this cycle. 0 when no group is being
    /// delivered (before the first flush, and during drain / after exhaustion) —
    /// 0 never occurs as a real seq because Engine::cycleCount() ≥ 1 at capture.
    std::uint64_t currentSeq() const { return currentSeq_; }

    /// True once every group has been delivered (the recorded session is
    /// spent). Flips on the first drain cycle, not on the last delivery cycle, so
    /// the last group's outputs still count as "running". An application's
    /// is-running check is the negation of this.
    bool exhausted() const { return pastLast_; }

    /// Fired exactly once, drainCycles cycles after the last group. Safe to call
    /// Engine::stop() from it — the in-flight cycle completes first.
    void setExhaustedCallback(std::function<void()> cb) { onExhausted_ = std::move(cb); }

    ReplayClockPtr clock() const { return clock_; }

    /// Rewind to the start of the schedule (for re-running a replay). Sources
    /// hold their own cursors and must be reset alongside this.
    void reset();

    std::size_t scheduleSize() const { return schedule_.size(); }

private:
    ReplayCoordinator(std::vector<Group> schedule, std::size_t drainCycles);

    void fireWake();

    std::vector<Group>    schedule_;
    std::size_t           drainCycles_;
    std::size_t           nextIdx_       = 0;   // index of the next group to deliver
    std::size_t           drainRemaining_;
    std::uint64_t         currentSeq_    = 0;
    bool                  pastLast_      = false;
    bool                  exhaustedFired_ = false;
    std::function<void()> wakeHook_;
    std::function<void()> onExhausted_;
    ReplayClockPtr        clock_;
};

using ReplayCoordinatorPtr = std::shared_ptr<ReplayCoordinator>;

// ─────────────────────────────────────────────────────────────────────────────
// ReplaySample<T> — one recorded (seq, value) pair for a replay source.
//
// The replay nodes are domain-agnostic, so they carry their own minimal sample
// type rather than depending on an application's event type. The application's
// replay reader maps each recorded stream onto a vector of these. Precondition:
// ascending, distinct seqs.
// ─────────────────────────────────────────────────────────────────────────────
template<typename T>
struct ReplaySample {
    std::uint64_t seq = 0;
    T             value{};
};

// ─────────────────────────────────────────────────────────────────────────────
// ReplayInput<T> — replay counterpart of AsyncInput<T>.
//
// Delivers its next recorded value on the cycle whose currentSeq() matches, then
// applies the identical equality-policy + dirty-propagation logic as
// AsyncInput::flush(). Construction state mirrors AsyncInput exactly (value T{},
// dirty), so — since every node starts dirty — the first replay cycle reproduces
// the first recorded cycle with no initial-value special-casing. setWakeHook()
// stores but never fires the hook; the coordinator is what wakes the engine.
// ─────────────────────────────────────────────────────────────────────────────
template<typename T>
class ReplayInput
    : public NodeBase
    , public IFlushable
    , public std::enable_shared_from_this<ReplayInput<T>>
{
public:
    static std::shared_ptr<ReplayInput<T>> make(
        std::string name,
        std::vector<ReplaySample<T>> samples,
        ReplayCoordinatorPtr coordinator,
        EqualityPolicyPtr eq = nullptr);

    // ── IFlushable ────────────────────────────────────────────────────────────
    std::size_t flush() override;
    std::size_t pendingCount() const override;
    std::string name() const override;
    void setWakeHook(std::function<void()> hook) override;

    const T& current() const;

    // ── INode ─────────────────────────────────────────────────────────────────
    ValuePtr eval(EvalContext&) override;
    std::vector<NodePtr> inputs() const override;
    NodeKind kind() const override { return NodeKind::AsyncInput; }

private:
    ReplayInput(std::string n, std::vector<ReplaySample<T>> samples,
                ReplayCoordinatorPtr coord, EqualityPolicyPtr eq);

    std::string                       name_;
    ValuePtr                          value_;
    EqualityPolicyPtr                 eq_;
    std::vector<ReplaySample<T>>      samples_;
    std::size_t                       cursor_ = 0;
    ReplayCoordinatorPtr              coord_;
    std::function<void()>             wakeHook_;   // stored, never fired
};

template<typename T>
using ReplayInputPtr = std::shared_ptr<ReplayInput<T>>;

// ─────────────────────────────────────────────────────────────────────────────
// ReplayQueue<T> — replay counterpart of AsyncQueue<T>.
//
// Applies its whole recorded batch on the matching-seq cycle and mirrors
// AsyncQueue::flush() exactly: always refresh value_ (to [] on a non-matching
// cycle, so downstream never reads a stale batch), but dirty + invalidate only
// for a non-empty batch. Recorded batches are always non-empty (a recorder has
// no reason to write an empty cycle), so a matching cycle always dirties.
// ─────────────────────────────────────────────────────────────────────────────
template<typename T>
class ReplayQueue
    : public NodeBase
    , public IFlushable
    , public std::enable_shared_from_this<ReplayQueue<T>>
{
public:
    static std::shared_ptr<ReplayQueue<T>> make(
        std::string name,
        std::vector<ReplaySample<std::vector<T>>> batches,
        ReplayCoordinatorPtr coordinator);

    // ── IFlushable ────────────────────────────────────────────────────────────
    std::size_t flush() override;
    std::size_t pendingCount() const override;
    std::string name() const override;
    void setWakeHook(std::function<void()> hook) override;

    // ── INode ─────────────────────────────────────────────────────────────────
    ValuePtr eval(EvalContext&) override;
    std::vector<NodePtr> inputs() const override;
    NodeKind kind() const override { return NodeKind::AsyncQueue; }

private:
    ReplayQueue(std::string n, std::vector<ReplaySample<std::vector<T>>> batches,
                ReplayCoordinatorPtr coord);

    std::string                               name_;
    ValuePtr                                  value_;   // holds std::vector<T>
    std::vector<ReplaySample<std::vector<T>>> batches_;
    std::size_t                               cursor_ = 0;
    ReplayCoordinatorPtr                      coord_;
    std::function<void()>                     wakeHook_;   // stored, never fired
};

template<typename T>
using ReplayQueuePtr = std::shared_ptr<ReplayQueue<T>>;

} // namespace dag::async

#include "dag_replay.inl"
