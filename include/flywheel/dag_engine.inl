// =============================================================================
// flywheel-dag — A header-only C++17 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

// dag_engine.inl — implementation of all dag_engine.hpp declarations
// Included at the bottom of dag_engine.hpp; never include this file directly.

#pragma once

#include <spdlog/spdlog.h>

namespace dag::async {

// ─────────────────────────────────────────────────────────────────────────────
// RollingCycleWindow
// ─────────────────────────────────────────────────────────────────────────────

inline RollingCycleWindow::RollingCycleWindow(std::size_t window)
    : buf_(window + 1), window_(window)
{
    if (window == 0)
        throw std::invalid_argument("RollingCycleWindow: window must be >= 1");
}

inline void RollingCycleWindow::record(uint64_t ns) {
    std::lock_guard<std::mutex> lk(mu_);
    if (buf_.size() == window_) {
        sum_ -= buf_.front();
        buf_.pop_front();
    }
    buf_.push_back(ns);
    sum_ += ns;
}

inline double RollingCycleWindow::meanUs() const {
    std::lock_guard<std::mutex> lk(mu_);
    return buf_.empty() ? 0.0
        : static_cast<double>(sum_) / static_cast<double>(buf_.size()) / 1'000.0;
}

// ─────────────────────────────────────────────────────────────────────────────
// CycleSeqLock
// ─────────────────────────────────────────────────────────────────────────────

inline CycleSeqLock::WriteScope::WriteScope(CycleSeqLock& lock) : lock_(lock) {
    lock_.writer_.store(std::this_thread::get_id(), std::memory_order_relaxed);
    lock_.seq_.fetch_add(1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
}

inline CycleSeqLock::WriteScope::~WriteScope() {
    std::atomic_thread_fence(std::memory_order_release);
    lock_.seq_.fetch_add(1, std::memory_order_relaxed);
    lock_.writer_.store(std::thread::id{}, std::memory_order_relaxed);
}

template<typename F>
auto CycleSeqLock::readConsistent(F&& fn, const char* what, std::size_t maxAttempts) const
    -> std::decay_t<std::invoke_result_t<F&>>
{
    if (maxAttempts == 0)
        throw std::invalid_argument("CycleSeqLock::readConsistent: maxAttempts must be >= 1");

    // Inside our own open cycle (e.g. replay's exhaustion callback): nothing can race us, and
    // the cycle cannot close until we return.
    if (writer_.load(std::memory_order_relaxed) == std::this_thread::get_id())
        return fn();

    for (std::size_t attempt = 1;; ++attempt) {
        uint64_t before = seq_.load(std::memory_order_relaxed);
        while (before & 1u) {
            std::this_thread::yield();
            before = seq_.load(std::memory_order_relaxed);
        }
        std::atomic_thread_fence(std::memory_order_acquire);   // pairs with WriteScope's fences
        auto result = fn();
        std::atomic_thread_fence(std::memory_order_acquire);
        if (seq_.load(std::memory_order_relaxed) == before)
            return result;
        if (attempt == maxAttempts) {
            spdlog::warn("CycleSeqLock: {} interrupted by a cycle {} times in a row — returning a "
                         "possibly inconsistent read", what, maxAttempts);
            return result;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Construction
// ─────────────────────────────────────────────────────────────────────────────

inline Engine::Engine(std::size_t rollingWindow)
    : rollingCycles_(rollingWindow)
{}

// ─────────────────────────────────────────────────────────────────────────────
// State store
// ─────────────────────────────────────────────────────────────────────────────

inline void Engine::setStateStore(std::shared_ptr<IStateStore> store) {
    store_ = std::move(store);
}

inline void Engine::saveState() const {
    if (!store_) return;
    store_->save(discoverStatefulNodes());
}

inline bool Engine::restoreState() {
    if (!store_) return false;
    return store_->restore(discoverStatefulNodes());
}

inline std::vector<dag::StatefulNodePtr>
Engine::discoverStatefulNodes() const {
    std::vector<NodePtr> roots;
    roots.reserve(outputs_.size());
    for (const auto& e : outputs_)
        if (e.node) roots.push_back(e.node);

    std::vector<dag::StatefulNodePtr> result;
    for (const auto& n : dag::traversal::bfs_upstream(std::move(roots))) {
        if (auto sn = std::dynamic_pointer_cast<IStatefulNode>(n))
            if (sn->persistState())
                result.push_back(std::move(sn));
    }
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// Module installation
// ─────────────────────────────────────────────────────────────────────────────

inline void Engine::install(std::shared_ptr<IComputeModule> m) {
    modules_.push_back(m);
    m->wire(*this);
}

// ─────────────────────────────────────────────────────────────────────────────
// Source registration
// ─────────────────────────────────────────────────────────────────────────────

inline void Engine::addSource(std::shared_ptr<IFlushable> src) {
    for (const auto& s : sources_)
        if (s->includes(*src)) return;
    src->setWakeHook(makeWakeHook());
    sources_.push_back(std::move(src));
    flushListStale_ = true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Flush list
//
// A source two groups hold, where neither group sees the other, is held twice and cannot be
// refused at registration (flywheel-dag#37). So cycle() flushes from a list that holds each source
// once, built by walking sources_ in order.
// ─────────────────────────────────────────────────────────────────────────────

// A source or a group is entered once, at the first place the walk reaches it, which is the place a
// source flushes in. That also ends a walk through a ring of registries.
inline void Engine::expandSource(const std::shared_ptr<IFlushable>& src,
                                 std::unordered_set<const IFlushable*>& seen)
{
    if (!seen.insert(src.get()).second) return;
    const IFlushable::Members* members = src->members();
    if (!members) { flushList_.push_back(src); return; }
    // The group owns its list, so the pointer shares the group's ownership: it cannot outlive it.
    watched_.push_back({std::shared_ptr<const IFlushable::Members>(src, members), members->size()});
    for (const auto& m : *members) expandSource(m, seen);
}

inline void Engine::rebuildFlushList() {
    flushList_.clear();
    watched_.clear();
    std::unordered_set<const IFlushable*> seen;
    for (const auto& s : sources_) expandSource(s, seen);
    flushListStale_ = false;
}

// A size read and a compare for each group, none for each source. With no group it is an empty
// loop, so an engine that registers only plain sources pays nothing for the list.
inline bool Engine::flushListStale() const {
    if (flushListStale_) return true;
    for (const auto& w : watched_)
        if (w.list->size() != w.size) return true;
    return false;
}

template<typename T>
dag::InputPtr<T> Engine::makeInput(
    std::string name, T initial,
    EqualityPolicyPtr eq)
{
    auto inp = dag::Input<T>::make(std::move(name), std::move(initial), std::move(eq));
    inp->setWakeHook(makeWakeHook());
    return inp;
}

// ─────────────────────────────────────────────────────────────────────────────
// Output registration
// ─────────────────────────────────────────────────────────────────────────────

template<typename T>
void Engine::addOutput(NodePtr node, std::function<void(const T&)> cb) {
    addOutput(std::move(node), [cb = std::move(cb)](const ValuePtr& v) {
        cb(get_value<T>(v));
    });
}

inline void Engine::addOutput(
    NodePtr node, std::function<void(const ValuePtr&)> cb)
{
    outputs_.push_back({ std::move(node), nullptr, std::move(cb) });
    // Due from the start, so the next cycle reaches the output even when its node is already
    // clean, because an earlier cycle pulled it as another output's input or a caller evaluated
    // it.
    due_.push_back(true);
}

template<typename T>
void Engine::addFeedback(NodePtr node, dag::InputPtr<T> input) {
    addOutput<T>(std::move(node), [inp = std::move(input)](const T& v) {
        inp->set(v);
    });
}

// ─────────────────────────────────────────────────────────────────────────────
// Run control
// ─────────────────────────────────────────────────────────────────────────────

inline void Engine::run() {
    if (running_.exchange(true))
        throw std::runtime_error("Engine::run() called while already running");

    // A node or callback that throws ends the run, and its exception leaves through here. Clear
    // running_ on every exit, or no later run() could start. This comes after the check: a run()
    // refused there must leave the flag of the run already going alone.
    struct ClearOnExit {
        std::atomic<bool>& flag;
        ~ClearOnExit() { flag = false; }
    };
    const ClearOnExit clearRunning{running_};

    // Initial cycle: flush any already-pending data, deliver starting values.
    cycle();

    while (running_) {
        std::unique_lock lk(mu_);
        cv_.wait(lk, [this] { return hasWork_ || !running_; });
        if (!running_) break;
        hasWork_ = false;
        lk.unlock();

        cycle();
    }
}

inline void Engine::stop() {
    running_ = false;
    cv_.notify_all();
}

inline void Engine::poke() {
    {
        std::lock_guard lk(mu_);
        hasWork_ = true;
    }
    cv_.notify_one();
}

inline void Engine::step() { cycle(); }

// ─────────────────────────────────────────────────────────────────────────────
// Graph inspection
// ─────────────────────────────────────────────────────────────────────────────

inline std::vector<std::shared_ptr<const INode>> Engine::outputNodes() const {
    std::vector<std::shared_ptr<const INode>> result;
    result.reserve(outputs_.size());
    for (const auto& e : outputs_) result.push_back(e.node);
    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// Stats
// ─────────────────────────────────────────────────────────────────────────────

inline uint64_t Engine::cycleCount()     const { return cycles_.load(std::memory_order_relaxed); }
inline uint64_t Engine::callbacksFired() const { return callbacks_.load(std::memory_order_relaxed); }

inline double Engine::lastCycleUs() const {
    return static_cast<double>(lastCycleNs_.load(std::memory_order_relaxed)) / 1'000.0;
}
inline double Engine::meanCycleUs() const {
    const uint64_t cycles = cycles_.load(std::memory_order_relaxed);
    return (cycles == 0) ? 0.0
        : static_cast<double>(totalCycleNs_.load(std::memory_order_relaxed))
            / static_cast<double>(cycles) / 1'000.0;
}
inline double Engine::minCycleUs() const {
    const uint64_t ns = minCycleNs_.load(std::memory_order_relaxed);
    return (ns == std::numeric_limits<uint64_t>::max())
        ? 0.0 : static_cast<double>(ns) / 1'000.0;
}
inline double Engine::maxCycleUs() const {
    return static_cast<double>(maxCycleNs_.load(std::memory_order_relaxed)) / 1'000.0;
}
inline double Engine::rollingMeanCycleUs() const {
    return rollingCycles_.meanUs();
}

// ─────────────────────────────────────────────────────────────────────────────
// Cross-thread reads
// ─────────────────────────────────────────────────────────────────────────────

inline CycleSeqLockPtr Engine::cycleSeqLock() const { return cycleSeqLock_; }

// ─────────────────────────────────────────────────────────────────────────────
// Internal cycle
// ─────────────────────────────────────────────────────────────────────────────

inline void Engine::cycle() {
    const CycleSeqLock::WriteScope writeScope(*cycleSeqLock_);
    cycles_.fetch_add(1, std::memory_order_relaxed);

    // Times the cycle on every exit, a throw included, so every cycle that ends is counted and
    // timed alike. Declared after writeScope, so it is destroyed first and records while a
    // CycleSeqLock reader still sees the cycle open. It records in its own destructor: Apple Clang
    // does not inline a member function called from there, which slows bench_hot_path's
    // idle-queues row by 3%.
    struct TimeOnExit {
        Engine&                                     engine;
        const std::chrono::steady_clock::time_point started;
        ~TimeOnExit() {
            const auto ns = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - started).count());
            engine.lastCycleNs_.store(ns, std::memory_order_relaxed);
            engine.totalCycleNs_.fetch_add(ns, std::memory_order_relaxed);
            if (ns < engine.minCycleNs_.load(std::memory_order_relaxed))
                engine.minCycleNs_.store(ns, std::memory_order_relaxed);
            if (ns > engine.maxCycleNs_.load(std::memory_order_relaxed))
                engine.maxCycleNs_.store(ns, std::memory_order_relaxed);
            engine.rollingCycles_.record(ns);
        }
    };
    const TimeOnExit timeCycle{*this, std::chrono::steady_clock::now()};

    // 1. Flush all sources — the only point where feed-thread data enters
    //    the DAG.  Invalidations propagate forward synchronously from here.
    //    Each source is flushed once, from the list of the sources and groups
    //    the engine holds (flywheel-dag#37), rebuilt first if a source was
    //    added since the last cycle or a group grew.
    if (flushListStale()) rebuildFlushList();
    for (auto& s : flushList_) s->flush();

    // 2. Mark each output whose node is dirty as due, before any eval() call
    //    clears the flags.  ComputeNode::eval() recursively calls eval() on
    //    its inputs, which clears those inputs' dirty flags as a side effect.
    //    Without this pass, a leaf node (AsyncInput, Input) registered after
    //    a downstream ComputeNode would read clean by the time the engine
    //    reaches it, and its callback would never fire.
    //
    //    Mark, never overwrite: an output stays due until step 3 reaches it.
    //    A node or a callback that throws out of step 3 leaves the outputs
    //    after it due.  One that an earlier output pulled clean reads clean
    //    here, and overwritten, its callback would miss the value its node
    //    holds until that value moved again.  A cycle that does not throw
    //    clears every entry, so the next starts with none due but the outputs
    //    registered since.
    for (std::size_t i = 0; i < outputs_.size(); ++i)
        if (outputs_[i].node->dirty()) due_[i] = true;

    // 3. Evaluate the due outputs and fire their callbacks.
    for (std::size_t i = 0; i < outputs_.size(); ++i) {
        if (!due_[i]) continue;                   // fast path: nothing to do

        ValuePtr val = outputs_[i].node->eval(ctx_);

        // Reached.  Cleared after eval(), so an output whose node throws
        // stays due, whatever state the throw left the node in.  Cleared
        // before the callback, which has had the value once it is called:
        // lastSeen holds it, and a callback that throws is not offered it
        // again, only the next one.
        due_[i] = false;

        // Pointer identity: if cached_ didn't change (equality policy
        // said equal), the node returns the same pointer it held before.
        // A new output is due and its lastSeen is null, so every output
        // fires once, on the first cycle after it is registered.
        if (val != outputs_[i].lastSeen) {
            outputs_[i].lastSeen = val;
            callbacks_.fetch_add(1, std::memory_order_relaxed);
            outputs_[i].callback(val);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Wake hook factory
//
// Returns a cheap callable that sets the work flag and signals the cv. A feed
// thread may call it: it holds mu_ only to set the flag.
// ─────────────────────────────────────────────────────────────────────────────

inline std::function<void()> Engine::makeWakeHook() {
    return [this] {
        {
            std::lock_guard lk(mu_);
            hasWork_ = true;
        }
        cv_.notify_one();
    };
}

} // namespace dag::async
