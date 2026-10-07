// =============================================================================
// flywheel-dag — A header-only C++17 reactive DAG computation engine
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
//   thing: sets a boolean flag and signals a condition variable.  This is the
//   only Engine state a feed thread touches.
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
//   registered delivers the value its node holds then.
//
// Input<T> (with wake hook)
//   Use makeInput<T>() for config / parameter values you set from application
//   code.  set() propagates dirty flags through the graph AND wakes the engine,
//   so the effect is visible on the very next cycle.
//
// Thread safety
//   run() must be called from exactly one thread.  stop() and poke() are safe
//   to call from any thread, including from within output callbacks.

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
#include <thread>
#include <type_traits>

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
class CycleSeqLock {
public:
    // Holds one cycle open. RAII so a throwing callback can't leave it open and stall every reader.
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
    /// returns the last, possibly inconsistent, result.
    template<typename F>
    auto readConsistent(F&& fn, const char* what = "snapshot",
                        std::size_t maxAttempts = kDefaultCycleSeqReadAttempts) const
        -> std::decay_t<std::invoke_result_t<F&>>;

private:
    std::atomic<uint64_t>        seq_{0};                     // odd while a cycle is open
    std::atomic<std::thread::id> writer_{std::thread::id{}};  // the open cycle's thread
};

using CycleSeqLockPtr = std::shared_ptr<const CycleSeqLock>;

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
    /// fires on the next cycle, with the value its node holds then.
    void install(std::shared_ptr<IComputeModule> module);

    // ── Source registration ───────────────────────────────────────────────────

    /// Register an async source (AsyncInput, AsyncQueue, …) or a FeedRegistry of them.
    /// The engine installs its wake hook and flushes the source on every cycle. It flushes a
    /// registry as a whole, so a member added to the registry after this call is flushed too.
    /// A source the engine already flushes, as one of its own or inside a registry it holds, is
    /// ignored: it keeps its first place in the order, and its wake hook is not installed again.
    /// The check sees what the engine holds, so a registry that holds a source the engine flushes
    /// outside it flushes that source twice (flywheel-dag#37): add a source to one place.
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
    /// returned. Never register one from inside a cycle, from an output callback or a node's
    /// eval() (flywheel-dag#24), or from another thread while run() is going.
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
    std::vector<dag::StatefulNodePtr> discoverStatefulNodes() const;

    // ── Run control ───────────────────────────────────────────────────────────

    /// Block and run until stop() is called.
    /// Must be called from exactly one thread.
    /// An exception from a node's eval() or an output callback ends the run and leaves through
    /// run(), which can then be called again. The next cycle delivers the outputs the aborted
    /// one did not reach.
    void run();

    /// Signal the engine to stop after the current cycle finishes.
    /// Safe to call from any thread, including from within output callbacks.
    void stop();

    /// Manually trigger a cycle — useful for testing or for external events
    /// that don't go through a registered input.
    void poke();

    /// Execute one cycle synchronously (flush → eval → callbacks).
    /// For deterministic testing only — do not call while run() is active.
    void step();

    // ── Graph inspection ─────────────────────────────────────────────────────

    /// Returns the registered output nodes, one per output, in registration order.
    /// Used to seed graph traversal. Returns const pointers so callers can inspect
    /// but not mutate the nodes.
    std::vector<std::shared_ptr<const INode>> outputNodes() const;

    // ── Stats ─────────────────────────────────────────────────────────────────

    // A cycle is timed when it ends, whether it returns or throws, so every statistic covers every
    // cycle that has ended. One that throws is timed up to the throw: shorter than a full cycle by
    // the work it skipped, longer by the cost of the throw. A meanCycleUs() read during a cycle
    // counts it before it is timed, and reads low (flywheel-dag#34).

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

    // ── State ─────────────────────────────────────────────────────────────────

    struct OutputEntry {
        NodePtr                              node;
        ValuePtr                             lastSeen;
        std::function<void(const ValuePtr&)> callback;
    };

    std::shared_ptr<IStateStore>                 store_;
    std::vector<std::shared_ptr<IComputeModule>> modules_;
    std::vector<std::shared_ptr<IFlushable>>     sources_;
    std::vector<OutputEntry>                     outputs_;
    // One per output: owed a visit by cycle(). addOutput() sets it, so a new output is due; cycle()
    // marks an output whose node is dirty, and clears it once it reaches the output.
    std::vector<bool>                        due_;
    EvalContext                              ctx_;

    std::mutex              mu_;
    std::condition_variable cv_;
    bool                    hasWork_ = false;
    std::atomic<bool>       running_{false};

    std::atomic<uint64_t> cycles_{0};
    std::atomic<uint64_t> callbacks_{0};

    std::atomic<uint64_t> lastCycleNs_{0};
    std::atomic<uint64_t> totalCycleNs_{0};
    std::atomic<uint64_t> minCycleNs_{std::numeric_limits<uint64_t>::max()};
    std::atomic<uint64_t> maxCycleNs_{0};

    RollingCycleWindow rollingCycles_;

    std::shared_ptr<CycleSeqLock> cycleSeqLock_ = std::make_shared<CycleSeqLock>();
};

} // namespace dag::async

#include "dag_engine.inl"
