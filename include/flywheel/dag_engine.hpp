// =============================================================================
// flywheel-dag — A header-only C++23 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

#pragma once
// dag_engine.hpp — event-driven evaluation engine for dag DAGs
//
// Overview
// ────────
// The Engine ties together a set of input sources (async feeds and manually-set
// parameters) with a set of output nodes, running a tight event-driven loop that
// only wakes up when data actually arrives and only evaluates outputs that were
// dirtied by that data.
//
//   Engine e;
//
//   auto threshold = e.makeInput<double>("z_threshold", 2.0);  // Input<T>
//   e.addSource(registry);                      // a FeedRegistry of async feeds
//
//   e.addOutput<double>(ratioNode,  [](double v){ ... });
//   e.addOutput<bool>  (alertNode,  [](bool   v){ ... });
//
//   e.run();          // blocks; call e.stop() from another thread or callback
//
// Design
// ──────
// Wake mechanism
//   Every registered source installs the engine's wake hook.  The hook does one
//   thing: sets a boolean flag and signals a condition variable.  That flag, its
//   mutex and the condition variable are all a feed thread touches, and they are
//   not the Engine's own: the Engine and every hook it hands out share them through
//   a shared_ptr, so a source can still be posted to after its Engine is gone
//   (flywheel-dag#42).  stop() and poke() set the same state, and every write
//   another thread makes to what run() waits on is made under its mutex, so a
//   notification cannot fall between run()'s test and its block
//   (flywheel-dag#43).
//
// Cycle
//   On each wake: flush all sources (thread boundary), then for each output node
//   check dirty() cheaply before committing to a full eval().  A callback fires
//   only when eval() returns a ValuePtr that differs from the last one delivered
//   — pointer identity is sufficient because nodes only update their cached_
//   pointer when the equality policy says the value actually changed.  An
//   output stays due until a cycle reaches it, so a node or a callback that
//   throws out of a cycle leaves the outputs the cycle did not reach to the
//   next cycle.  A new output starts due, so the first cycle after it is
//   registered delivers the value its node holds then.  A registration, step()
//   or run() made inside a cycle, from a callback, a node's eval() or a source's
//   flush(), throws (addOutput(), step()).
//
// Flush list
//   A cycle flushes from a list the engine builds out of the sources and groups
//   it holds, in registration order, with a group replaced by its members() and
//   each source listed once, at the first place the walk reaches it.  A source
//   that two groups hold, without either seeing the other, is then flushed once
//   per cycle.  Flushed twice, it would find nothing staged the second time, and
//   an AsyncQueue would drop its batch (flywheel-dag#37).  addSource() marks the
//   list stale and the next cycle rebuilds it.  A cycle rebuilds it too when a
//   group it walked through has grown, which a size compare per group finds, with
//   no work per source.
//
// Input<T> (with wake hook)
//   Use makeInput<T>() for config / parameter values you set from application
//   code.  set() propagates dirty flags through the graph AND wakes the engine,
//   so the effect is visible on the very next cycle.
//
// Thread safety
//   run() must be called from exactly one thread.  stop() and poke() are safe
//   to call from any thread, including from within output callbacks, but not
//   from a signal handler: each takes the mutex run() waits under.  A stop()
//   that arrives as run() is about to wait is not lost (flywheel-dag#43).  A
//   source may be posted to after its Engine is destroyed, and nothing else on
//   a destroyed Engine may be used.

#include "dag_async.hpp"
#include "dag_compute_module.hpp"
#include "dag_ring_buffer.hpp"
#include "dag_state_store.hpp"
#include "dag_traversal.hpp"
#include <vector>
#include <functional>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_set>

namespace dag::async {

inline constexpr std::size_t kDefaultRollingCycleWindow = 1000;

// RollingCycleWindow — incremental mean over the last `window` cycle durations
// (ns), with the evict-before-push RingBuffer idiom of dag::ts::RollingSumNode
// (dag_timeseries.hpp). Engine bookkeeping, not a DAG node, so it lives in
// dag::async. ns is integral, so unlike RollingSumNode's double sum the running
// total needs no periodic re-sum to bound drift.
class RollingCycleWindow {
public:
    explicit RollingCycleWindow(std::size_t window);

    void   record(uint64_t ns);
    double meanUs() const;

private:
    // buf_/sum_ are a RingBuffer<uint64_t>-backed structure, not a scalar, so they
    // can't be made atomic piecewise: record() (engine thread) and meanUs() (any
    // other thread) both take this lock around the whole operation. RingBuffer
    // itself is eval-thread only (dag_ring_buffer.hpp).
    mutable std::mutex        mu_;
    dag::RingBuffer<uint64_t> buf_;
    std::size_t               window_;
    uint64_t                  sum_ = 0;
};

inline constexpr std::size_t kDefaultCycleSeqReadAttempts = 64;

// Seqlock over Engine::cycle(): lets another thread read cycle-written atomics as one set.
//
// A sanitizer cannot check it. ThreadSanitizer never reports on an atomic access, and it does not model the fences
// this lock orders with (dag_thread_fence.hpp), so a clean ThreadSanitizer run says nothing about readConsistent().
// The retry, wait-out and give-up tests in test_dag_async.cpp hold the protocol (flywheel-dag#49).
class CycleSeqLock {
public:
    // Holds one cycle open. RAII so a throwing callback can't leave it open and stall every reader.
    // Scopes on one lock must not nest: an inner scope turns the sequence word even while the outer
    // cycle is still open, and its end clears the record of the cycle's thread. Engine::cycle()
    // takes none while a cycle is in flight (flywheel-dag#39).
    class WriteScope {
    public:
        explicit WriteScope(CycleSeqLock& lock);
        ~WriteScope();
        WriteScope(const WriteScope&)            = delete;
        WriteScope& operator=(const WriteScope&) = delete;

    private:
        CycleSeqLock& lock_;
    };

    /// Returns fn() from a call that overlapped no cycle. Waiting out an open cycle is free; only
    /// a call another cycle interrupts spends an attempt. After maxAttempts it logs a warning and
    /// returns the last, possibly inconsistent, result. what names the read in that warning.
    template<typename F>
    auto readConsistent(F&& fn, std::string_view what = "snapshot",
                        std::size_t maxAttempts = kDefaultCycleSeqReadAttempts) const
        -> std::decay_t<std::invoke_result_t<F&>>;

private:
    std::atomic<uint64_t>        seq_{0};                     // odd while a cycle is open
    std::atomic<std::thread::id> writer_{std::thread::id{}};  // the open cycle's thread
};

using CycleSeqLockPtr = std::shared_ptr<const CycleSeqLock>;

namespace detail {

// BasicWakeState — what Engine::run() waits on and everything that wakes it: the flag that says there
// is work, the flag that says a run is going, and the mutex and condition variable that carry them.
// A source's wake hook and poke() call signalWork(), stop() calls requestStop(), and run() calls
// beginRun(), waitForWork() and endRun(). The Engine and every hook it hands out share one through a
// shared_ptr, so a source can still be posted to after its Engine is gone (flywheel-dag#42).
//
// What the waiter's predicate reads changes only under mu_. A condition variable drops a notification
// that finds nobody waiting, and a waiter tests its predicate and then blocks, two steps with mu_ held
// across both. A writer that stored without mu_ could land between them:
//
//     waiter:  lock mu_, test (no work, running) ........................ block
//     writer:                     running_ = false; notify_all() ......... (nobody waiting yet: dropped)
//
// and the waiter would block on a predicate that is already true, with no one left to say so: run()
// would sleep until a post or a poke() reached it, and stop() was the call meant to end it
// (flywheel-dag#43). With the store made under mu_, the writer comes either before the waiter's test,
// which then sees it, or after the waiter's block, which the notification then reaches.
//
// signalWork() and requestStop() are the only writers of hasWork_ and running_ that a waiter's
// predicate can see while it waits, and both take mu_. The members are out of the Engine's reach, so
// no other code can write them without it. beginRun() and endRun() store running_ without the lock:
// they run on the run thread, outside any wait, and endRun() runs in a destructor, where a lock that
// throws would terminate.
//
// Mutex and CondVar are std::mutex and std::condition_variable in the Engine. A test gives the class a
// mutex that reports contention and a condition variable that pauses between its predicate and its
// block: that gap is a few instructions wide, and this is the only way to hold a waiter in it
// (tests/test_wake_state.cpp).
template<typename Mutex, typename CondVar>
class BasicWakeState {
public:
    /// run() claims the run flag. False, with the flag untouched, when a run already holds it.
    bool beginRun();
    /// run()'s exit, a throw included. It takes no lock, so it cannot throw from a destructor.
    void endRun();
    bool running() const;

    /// A source's wake hook and poke(): there is work for run().
    void signalWork();
    /// stop(): ends the run in progress once its current cycle is over.
    void requestStop();
    /// run(): blocks until there is work or a stop. True: work, and the flag is consumed. False: stopped.
    bool waitForWork();

protected:
    // Protected, not private, so that a test's subclass can reach the mutex and condition variable it
    // gave the class. Nothing derives from it in the engine, and the Engine reaches none of these.
    Mutex             mu_;
    CondVar           cv_;
    bool              hasWork_ = false;   // guarded by mu_
    std::atomic<bool> running_{false};    // stored under mu_ by requestStop(); the others use it without
};

} // namespace detail

class Engine {
public:
    // ── Construction ──────────────────────────────────────────────────────────

    /// rollingWindow sizes the sample count backing rollingMeanCycleUs() (below).
    /// A bare `Engine engine;` takes the default.
    explicit Engine(std::size_t rollingWindow = kDefaultRollingCycleWindow);

    // ── Module installation ───────────────────────────────────────────────────

    /// Install a compute module: calls module->wire(*this) exactly once and
    /// keeps the module alive for the lifetime of the engine.
    /// Call it between cycles, as addOutput(): an output that wire() registers
    /// fires on the next cycle, with the value its node holds then. Inside a cycle it throws
    /// std::runtime_error, before the module is kept or wired.
    void install(const std::shared_ptr<IComputeModule>& module);

    // ── Source registration ───────────────────────────────────────────────────

    /// Register an async source (AsyncInput, AsyncQueue, …) or a FeedRegistry of them.
    /// The engine installs its wake hook and flushes the source on every cycle. It flushes a
    /// registry member by member, in the registry's place in the order, so a member added to the
    /// registry after this call is flushed too, from the next cycle.
    /// A source the engine already flushes, as one of its own or inside a registry it holds, is
    /// ignored: it keeps its first place in the order, and its wake hook is not installed again.
    /// The check sees what the engine holds when this is called. A source that reaches the engine
    /// by another route, through two registries or into a registry after it is registered, is
    /// flushed once all the same, at the first place the engine reaches it (flywheel-dag#37), and
    /// is given the wake hook once for each registry that holds it.
    /// Register a source between cycles, on the thread that runs them.
    void addSource(std::shared_ptr<IFlushable> src);

    /// Create a synchronous Input<T> that wakes the engine on set().
    /// This is the right type for thresholds, configuration, user-driven params.
    template<typename T>
    dag::InputPtr<T> makeInput(
        std::string name, T initial = T{},
        EqualityPolicyPtr eq = nullptr);

    // ── Output registration ───────────────────────────────────────────────────

    /// Register an output with a typed callback.
    /// The callback fires on the first cycle after the output is registered, with the value its
    /// node holds then, even when an earlier cycle or a caller has already evaluated the node.
    /// After that it fires only when the output's value actually changes.
    /// Register an output between cycles: before the first, between step()s, or once run() has
    /// returned. Inside a cycle, from an output callback or a node's eval(), it throws
    /// std::runtime_error and registers nothing. To add an output in response to a value, note the
    /// request in the callback and register once the cycle has ended. From another thread while
    /// run() is going it races the cycle: never do it.
    /// A cycle that a node or a callback throws out of leaves the outputs it did not reach to
    /// the next cycle, which delivers the value each holds then. A callback that throws has had
    /// its value: it is not offered that value again, only the next one.
    template<typename T>
    void addOutput(NodePtr node, std::function<void(const T&)> cb);

    /// Register an output with an untyped callback (receives the raw ValuePtr).
    void addOutput(NodePtr node, std::function<void(const ValuePtr&)> cb);

    /// Wire a node's output directly to an Input: on each cycle when the node
    /// fires a new value, the Input is updated, which dirties downstream nodes
    /// for the following cycle.  One-cycle lag is inherent and expected.
    /// It registers an output, so it follows addOutput()'s rule and throws inside a cycle.
    template<typename T>
    void addFeedback(NodePtr node, std::shared_ptr<dag::Input<T>> input);

    // ── State store ───────────────────────────────────────────────────────────

    /// Inject the state store.  Call before engine.run().
    /// If not called (or store is nullptr), saveState() and restoreState() are no-ops.
    void setStateStore(std::shared_ptr<IStateStore> store);

    /// Save the stateful nodes that discoverStatefulNodes() finds.
    /// No-op if no store has been set.
    void saveState() const;

    /// Restore the stateful nodes that discoverStatefulNodes() finds.
    /// Returns the result of IStateStore::restore() (false = cold start / no
    /// saved state), and false without a store.
    bool restoreState();

    /// Walk the live DAG from every registered output (via INode::inputs(),
    /// deduped, BFS order) and return every reachable IStatefulNode whose
    /// persistState() is true. Source of truth for save/restore — no manual
    /// per-module list is consulted. A stateful node that no registered output
    /// reaches is not persisted.
    /// The walk runs on the first call after an output is registered, and its result is kept
    /// until the next registration: a graph is not rewired once its nodes are built
    /// (INode::inputs()), and addOutput() is the one call that adds a root. A later call copies
    /// that list and asks persistState() of each node again, since a node may answer differently
    /// over time. Two threads may call it at once, because the result is filled under a mutex.
    /// A registration from another thread while it runs is not supported, as addOutput() says.
    std::vector<dag::StatefulNodePtr> discoverStatefulNodes() const;

    // ── Run control ───────────────────────────────────────────────────────────

    /// Block and run until stop() is called.
    /// Must be called from exactly one thread.
    /// An exception from a node's eval() or an output callback ends the run and leaves through
    /// run(), which can then be called again. The next cycle delivers the outputs the aborted
    /// one did not reach.
    /// Call it between cycles. Inside a cycle of an engine that step() drives, from an output
    /// callback, a node's eval() or a source's flush(), it throws std::runtime_error and starts no
    /// loop. Inside a cycle of an engine that run() drives, it throws as for any second run().
    void run();

    /// Signal the engine to stop after the current cycle finishes.
    /// Safe to call from any thread, including from within output callbacks, and a stop() that
    /// arrives as run() is about to wait is not lost (flywheel-dag#43). It takes the mutex run()
    /// waits under, so call it from a thread, not from a signal handler.
    void stop();

    /// Manually trigger a cycle — useful for testing or for external events
    /// that don't go through a registered input.
    void poke();

    /// Execute one cycle synchronously (flush → eval → callbacks).
    /// For deterministic testing only — do not call while run() is active.
    /// Call it between cycles. Inside a cycle, from an output callback, a node's eval() or a
    /// source's flush(), it throws std::runtime_error and begins no cycle: a cycle inside a cycle
    /// would flush every source a second time, which drops an AsyncQueue's batch, and leave
    /// CycleSeqLock out of step. To get another cycle, call step() once this one has returned, or
    /// poke() an engine that run() drives (flywheel-dag#39).
    void step();

    // ── Graph inspection ─────────────────────────────────────────────────────

    /// Returns the registered output nodes, one per output, in registration order.
    /// Used to seed graph traversal. Returns const pointers so callers can inspect
    /// but not mutate the nodes.
    std::vector<std::shared_ptr<const INode>> outputNodes() const;

    // ── Stats ─────────────────────────────────────────────────────────────────

    // A cycle is timed when it ends, whether it returns or throws, so every duration statistic
    // covers every cycle that has ended, and a read during a cycle leaves that cycle out. One that
    // throws is timed up to the throw: shorter than a full cycle by the work it skipped, longer by
    // the cost of the throw. cycleCount() alone counts a cycle as it begins, so a callback that
    // reads it sees its own cycle's number.

    uint64_t cycleCount()     const;
    uint64_t callbacksFired() const;

    double lastCycleUs() const;   ///< duration of the most recent cycle (µs)
    double meanCycleUs() const;   ///< cumulative mean cycle duration (µs)
    double minCycleUs()  const;   ///< minimum cycle duration seen (µs)
    double maxCycleUs()  const;   ///< maximum cycle duration seen (µs)
    double rollingMeanCycleUs() const;   ///< mean over the last `rollingWindow` cycles (µs)

    // ── Cross-thread reads ────────────────────────────────────────────────────

    /// The lock every cycle() holds open. A module grabs it in wire() to read cycle-written
    /// atomics from another thread.
    CycleSeqLockPtr cycleSeqLock() const;

private:
    void cycle();
    std::function<void()> makeWakeHook();

    // True from the count at the top of cycle() to the count in its guard, a throw included
    // (cycles_ and timedCycles_). Ask it on the thread that runs cycles: timedCycles_ is a plain
    // field that thread writes. cycle() asks it of a step() or run(), refuseInCycle() of a
    // registration.
    bool cycleInFlight() const;

    // Throws if a cycle is in flight. install(), addOutput() and addFeedback() call it before they
    // change anything.
    void refuseInCycle(std::string_view call) const;

    // What cycle() throws when one is in flight. step() and run() are the only callers of cycle(),
    // so the message names both rather than the one that called.
    [[noreturn]] static void throwNestedCycle();

    // The flush list (flywheel-dag#37). expandSource() walks one source into it: a source with no
    // members() is listed, once, and a group is entered, once, and its members walked in its place.
    void rebuildFlushList();
    void expandSource(const std::shared_ptr<IFlushable>& src,
                      std::unordered_set<const IFlushable*>& seen);
    bool flushListStale() const;

    // The walk behind discoverStatefulNodes(): every IStatefulNode that BFS from the outputs reaches,
    // in that order, with persistState() not yet asked. discovered_ keeps its result.
    using StatefulList    = std::vector<dag::StatefulNodePtr>;
    using StatefulListPtr = std::shared_ptr<const StatefulList>;
    StatefulList walkStatefulNodes() const;

    // ── State ─────────────────────────────────────────────────────────────────

    struct OutputEntry {
        NodePtr                              node;
        ValuePtr                             lastSeen;
        std::function<void(const ValuePtr&)> callback;
    };

    // A group the flush list was walked through, and the size its member list had then. A group's
    // list only grows (IFlushable::members()), so a different size is a member added since.
    struct WatchedGroup {
        std::shared_ptr<const IFlushable::Members> list;   // kept alive by the group it belongs to
        std::size_t                                size;
    };

    std::shared_ptr<IStateStore>                 store_;
    std::vector<std::shared_ptr<IComputeModule>> modules_;
    std::vector<std::shared_ptr<IFlushable>>     sources_;      // as registered
    std::vector<std::shared_ptr<IFlushable>>     flushList_;    // what cycle() flushes: sources_ with
                                                                // each group replaced by its members
    std::vector<WatchedGroup>                    watched_;
    bool                                         flushListStale_ = false;   // addSource() sets it
    std::vector<OutputEntry>                     outputs_;
    // One per output: owed a visit by cycle(). addOutput() sets it, so a new output is due; cycle()
    // marks an output whose node is dirty, and clears it once it reaches the output.
    std::vector<bool>                        due_;
    EvalContext                              ctx_;

    // What the wake hook touches, and what run() waits on and stop() and poke() set: the flags, with
    // their mutex and condition variable (detail::BasicWakeState). The Engine and every hook it has
    // handed out each hold a reference, so the state lasts as long as a source that can still call
    // its hook. A hook that held the Engine would lock a destroyed mutex when a post() outlives the
    // Engine (a feed thread between its push and its call to the hook, or any other holder of the
    // source): this one sets a flag nobody reads (flywheel-dag#42).
    using WakeState = detail::BasicWakeState<std::mutex, std::condition_variable>;
    std::shared_ptr<WakeState> wake_ = std::make_shared<WakeState>();

    std::atomic<uint64_t> cycles_{0};
    std::atomic<uint64_t> callbacks_{0};

    std::atomic<uint64_t> lastCycleNs_{0};
    // The mean of the cycles that have ended, which meanCycleUs() reads. The cycle's thread alone
    // touches the total and the count, and publishes their quotient in one store at the end of its
    // guard, so a reader on another thread sees the mean from before a cycle or from after it, never
    // a total from one over a count from the other. It is not totalCycleNs_ / cycles_: cycles_ also
    // counts a cycle in flight (flywheel-dag#34), and that difference is how cycleInFlight() tells
    // one is, so cycle() and refuseInCycle() read the count too, on the thread that runs cycles.
    uint64_t              totalCycleNs_ = 0;
    uint64_t              timedCycles_  = 0;
    std::atomic<double>   meanCycleNs_{0.0};
    std::atomic<uint64_t> minCycleNs_{std::numeric_limits<uint64_t>::max()};
    std::atomic<uint64_t> maxCycleNs_{0};

    RollingCycleWindow rollingCycles_;

    std::shared_ptr<CycleSeqLock> cycleSeqLock_ = std::make_shared<CycleSeqLock>();

    // The walk discoverStatefulNodes() makes, kept until the next addOutput() resets it: outputs_
    // changes nowhere else, and graphs are not rewired, so the walk gives the same list on every call
    // in between. It is the list before the persistState() test, which is asked on every call. Null
    // until a call walks.
    //
    // mutable because saveState() and discoverStatefulNodes() are const and public and fill it, and
    // guarded by a mutex because a host may call them from a thread of its own: a walk only reads, but
    // a fill writes, and two threads on a cold cache would race on it. The lock is held across the
    // walk, which calls inputs() on every node, so an inputs() must not call back into the engine;
    // persistState() is asked outside it. Declared after every other member, so that none cycle()
    // reads changes offset.
    mutable std::mutex      discoveryMu_;
    mutable StatefulListPtr discovered_;
};

} // namespace dag::async

#include "dag_engine.inl"
