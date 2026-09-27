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
    // TODO(perf): memoize. Outputs and DAG topology are invariant after wiring
    // (install/addOutput are documented as pre-run), so the BFS + dynamic_casts
    // here yield the same vector on every call. A host that calls saveState()
    // periodically pays for the full walk each time. If
    // this shows up in a profile, cache the result in a `mutable
    // std::optional<std::vector<StatefulNodePtr>>` and invalidate it from
    // install() and addOutput(). Not done yet — no measured impact.
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
    src->setWakeHook(makeWakeHook());
    sources_.push_back(std::move(src));
}

inline void Engine::addSource(FeedRegistry& reg) {
    reg.setWakeHook(makeWakeHook());
    for (auto& s : reg.all()) sources_.push_back(s);
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
    dirtySnapshot_.push_back(false);
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
    const auto t0 = std::chrono::steady_clock::now();

    // 1. Flush all sources — the only point where feed-thread data enters
    //    the DAG.  Invalidations propagate forward synchronously from here.
    for (auto& s : sources_) s->flush();

    // 2. Snapshot dirty flags before any eval() call clears them.
    //    ComputeNode::eval() recursively calls eval() on its inputs, which
    //    clears those inputs' dirty_ flags as a side effect.  Without the
    //    snapshot, a leaf node (AsyncInput, Input) registered after a
    //    downstream ComputeNode would see dirty_=false by the time the
    //    engine reaches it and its callback would never fire.
    for (std::size_t i = 0; i < outputs_.size(); ++i)
        dirtySnapshot_[i] = outputs_[i].node->dirty();

    // 3. Evaluate and fire callbacks using the pre-eval snapshot.
    for (std::size_t i = 0; i < outputs_.size(); ++i) {
        if (!dirtySnapshot_[i]) continue;         // fast path: nothing to do

        ValuePtr val = outputs_[i].node->eval(ctx_);

        // Pointer identity: if cached_ didn't change (equality policy
        // said equal), the node returns the same pointer it held before.
        // The first time lastSeen is null, so every output fires once.
        if (val != outputs_[i].lastSeen) {
            outputs_[i].lastSeen = val;
            callbacks_.fetch_add(1, std::memory_order_relaxed);
            outputs_[i].callback(val);
        }
    }

    const auto ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - t0).count());
    lastCycleNs_.store(ns, std::memory_order_relaxed);
    totalCycleNs_.fetch_add(ns, std::memory_order_relaxed);
    if (ns < minCycleNs_.load(std::memory_order_relaxed))
        minCycleNs_.store(ns, std::memory_order_relaxed);
    if (ns > maxCycleNs_.load(std::memory_order_relaxed))
        maxCycleNs_.store(ns, std::memory_order_relaxed);
    rollingCycles_.record(ns);
}

// ─────────────────────────────────────────────────────────────────────────────
// Wake hook factory
//
// Returns a cheap callable that sets the work flag and signals the cv.
// Calling it from a feed thread is safe and non-blocking.
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
