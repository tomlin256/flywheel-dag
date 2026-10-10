// =============================================================================
// flywheel-dag — A header-only C++17 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

// dag_async.inl — implementation of all dag_async.hpp declarations
// Included at the bottom of dag_async.hpp; never include this file directly.

#pragma once

namespace dag::async {

// ─────────────────────────────────────────────────────────────────────────────
// AsyncInput<T>
// ─────────────────────────────────────────────────────────────────────────────

template<typename T>
std::shared_ptr<AsyncInput<T>> AsyncInput<T>::make(
    std::string name, T initial,
    EqualityPolicyPtr eq)
{
    if (!eq) eq = std::make_shared<TypedEqualityPolicy<T>>();
    return std::shared_ptr<AsyncInput<T>>(
        new AsyncInput<T>(std::move(name), std::move(initial), std::move(eq)));
}

template<typename T>
void AsyncInput<T>::post(const T& val) {
    {
        std::lock_guard lock(mutex_);
        if (hasPending_) ++skipped_;
        // Copy-assign, not move-assign: pending_ still owns the buffers the last
        // flush() swapped back into it, and assigning reuses them.
        pending_    = val;
        hasPending_ = true;
    }
    // The hook runs outside the lock: it only signals a condition variable.
    if (wakeHook_) wakeHook_();
}

template<typename T>
std::size_t AsyncInput<T>::flush() {
    {
        std::lock_guard lock(mutex_);
        const bool had = hasPending_;
        // Swap rather than move out: staged_ gives pending_ its buffers back, so
        // the next post() copy-assigns into storage that is already the right
        // size.
        if (had) std::swap(pending_, staged_);
        hasPending_ = false;
        skipped_    = 0;
        if (!had) return 0;
    }
    // Const lvalue: emit copy-assigns into the recycled buffer and leaves
    // staged_'s own storage intact for the next swap.
    auto newV = slot_.emit(std::as_const(staged_));
    if (!eq_->equal(value_, newV)) {
        value_ = newV;
        markDirty();
        notifyDownstream();
        return 1;
    }
    return 0;
}

template<typename T>
std::size_t AsyncInput<T>::pendingCount() const {
    std::lock_guard lock(mutex_);
    return hasPending_ ? 1 : 0;
}

template<typename T>
std::size_t AsyncInput<T>::skippedCount() const {
    std::lock_guard lock(mutex_);
    return skipped_;
}

template<typename T>
void AsyncInput<T>::setWakeHook(std::function<void()> hook) {
    // Set before any threads start posting — no lock needed.
    wakeHook_ = std::move(hook);
}

template<typename T>
const T& AsyncInput<T>::current() const { return get_value<T>(value_); }

template<typename T>
ValuePtr AsyncInput<T>::eval(EvalContext&) { markClean(); return value_; }

template<typename T>
std::string AsyncInput<T>::name() const { return name_; }

template<typename T>
std::vector<NodePtr> AsyncInput<T>::inputs() const { return {}; }

template<typename T>
AsyncInput<T>::AsyncInput(std::string n, T initial, EqualityPolicyPtr eq)
    : name_(std::move(n)), value_(make_value(std::move(initial))), eq_(std::move(eq)) {}

// ─────────────────────────────────────────────────────────────────────────────
// AsyncQueue<T>
// ─────────────────────────────────────────────────────────────────────────────

template<typename T>
std::shared_ptr<AsyncQueue<T>> AsyncQueue<T>::make(
    std::string name, std::size_t maxQueueSize)
{
    return std::shared_ptr<AsyncQueue<T>>(
        new AsyncQueue<T>(std::move(name), maxQueueSize));
}

template<typename T>
void AsyncQueue<T>::post(T val) {
    {
        std::lock_guard lock(mutex_);
        if (queue_.size() >= maxSize_) { queue_.pop_front(); ++dropped_; }
        queue_.push_back(std::move(val));
    }
    if (wakeHook_) wakeHook_();
}

template<typename T>
std::size_t AsyncQueue<T>::flush() {
    // The drain deque is a member, not a local: libstdc++ allocates a deque's
    // map and first block in the DEFAULT CONSTRUCTOR, so a local one costs two
    // allocations per queue per cycle — on the idle path too, where there is
    // nothing to drain. Swapping a retained deque instead recycles those blocks
    // the same way staged_ recycles AsyncInput's. drain_ is empty on entry: the
    // arrival path below clears it before returning.
    std::size_t n = 0;
    {
        std::lock_guard lock(mutex_);
        n = queue_.size();
        if (n != 0) std::swap(drain_, queue_);
    }
    if (n == 0) {
        // Refresh to the empty batch: a downstream node dirtied by a DIFFERENT
        // input still calls eval() on this queue, and must read [] rather than
        // the previous cycle's batch, which it has already consumed. The empty
        // batch is a constant built once in the constructor, so this allocates
        // nothing. Do not move this return above the refresh.
        value_ = emptyValue_;
        return 0;
    }
    std::vector<T> batch(std::make_move_iterator(drain_.begin()),
                         std::make_move_iterator(drain_.end()));
    drain_.clear();   // keeps one block; the next swap hands it back to queue_
    value_ = make_value(std::move(batch));
    // Only dirty and invalidate downstream when real items arrived.
    markDirty();
    notifyDownstream();
    return n;
}

template<typename T>
std::size_t AsyncQueue<T>::pendingCount() const {
    std::lock_guard lock(mutex_); return queue_.size();
}

template<typename T>
std::size_t AsyncQueue<T>::droppedCount() const {
    std::lock_guard lock(mutex_); return dropped_;
}

template<typename T>
void AsyncQueue<T>::setWakeHook(std::function<void()> hook) { wakeHook_ = std::move(hook); }

template<typename T>
ValuePtr AsyncQueue<T>::eval(EvalContext&) {
    markClean();
    return value_;   // TypedValue<std::vector<T>>
}

template<typename T>
std::string AsyncQueue<T>::name() const { return name_; }

template<typename T>
std::vector<NodePtr> AsyncQueue<T>::inputs() const { return {}; }

template<typename T>
AsyncQueue<T>::AsyncQueue(std::string n, std::size_t maxSz)
    : name_(std::move(n))
    , value_(make_value(std::vector<T>{}))
    , emptyValue_(value_)          // same object: one allocation, for the node's lifetime
    , maxSize_(maxSz) {}

// ─────────────────────────────────────────────────────────────────────────────
// FeedRegistry
// ─────────────────────────────────────────────────────────────────────────────

inline void FeedRegistry::add(std::shared_ptr<IFlushable> input) {
    if (includes(*input)) return;
    if (wakeHook_) input->setWakeHook(wakeHook_);
    inputs_.push_back(std::move(input));
}

inline std::size_t FeedRegistry::flush() {
    std::size_t total = 0;
    for (auto& inp : inputs_) total += inp->flush();
    return total;
}

inline std::size_t FeedRegistry::pendingCount() const {
    std::size_t total = 0;
    for (auto& inp : inputs_) total += inp->pendingCount();
    return total;
}

inline std::string FeedRegistry::name() const { return "feed_registry"; }

inline bool FeedRegistry::hasPending() const {
    for (auto& inp : inputs_) if (inp->pendingCount() > 0) return true;
    return false;
}

inline void FeedRegistry::setWakeHook(std::function<void()> hook) {
    wakeHook_ = hook;
    for (auto& inp : inputs_) inp->setWakeHook(hook);
}

inline bool FeedRegistry::includes(const IFlushable& src) const {
    if (this == &src) return true;
    for (const auto& inp : inputs_) if (inp->includes(src)) return true;
    return false;
}

inline const IFlushable::Members* FeedRegistry::members() const {
    return typeid(*this) == typeid(FeedRegistry) ? &inputs_ : nullptr;
}

inline const IFlushable::Members& FeedRegistry::all() const {
    return inputs_;
}

// ─────────────────────────────────────────────────────────────────────────────
// TickLoop
// ─────────────────────────────────────────────────────────────────────────────

inline TickLoop::TickLoop(Duration interval, Callback cb)
    : interval_(interval), cb_(std::move(cb)) {}

inline TickLoop::~TickLoop() { stop(); }

inline void TickLoop::start() {
    running_ = true;
    thread_  = std::thread([this] {
        while (running_) {
            auto next = std::chrono::steady_clock::now() + interval_;
            cb_();
            std::this_thread::sleep_until(next);
        }
    });
}

inline void TickLoop::stop() { running_ = false; if (thread_.joinable()) thread_.join(); }

inline bool TickLoop::running() const { return running_; }

} // namespace dag::async
