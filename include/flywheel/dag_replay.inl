// =============================================================================
// flywheel-dag — A header-only C++17 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

// dag_replay.inl — implementation of all dag_replay.hpp declarations.
// Included at the bottom of dag_replay.hpp; never include this file directly.

#pragma once

namespace dag::async {

// ─────────────────────────────────────────────────────────────────────────────
// ReplayCoordinator
// ─────────────────────────────────────────────────────────────────────────────

inline ReplayCoordinator::ReplayCoordinator(std::vector<Group> schedule, std::size_t drainCycles)
    : schedule_(std::move(schedule))
    , drainCycles_(drainCycles)
    , drainRemaining_(drainCycles)
    , clock_(std::make_shared<ReplayClock>())
{
    if (!schedule_.empty())
        clock_->setFirstTs(schedule_.front().tsUs);
}

inline std::shared_ptr<ReplayCoordinator> ReplayCoordinator::make(
    std::vector<Group> schedule, std::size_t drainCycles)
{
    return std::shared_ptr<ReplayCoordinator>(
        new ReplayCoordinator(std::move(schedule), drainCycles));
}

inline void ReplayCoordinator::fireWake() {
    if (wakeHook_) wakeHook_();
}

inline std::size_t ReplayCoordinator::flush() {
    if (nextIdx_ < schedule_.size()) {
        // Deliver the next group this cycle; sources (flushed after us) match it.
        const Group& g = schedule_[nextIdx_];
        currentSeq_ = g.seq;
        clock_->setCurrentTs(g.tsUs);
        ++nextIdx_;
        fireWake();          // groups and/or drain cycles still remain
        return 0;
    }

    // Past the last group — currentSeq_ = 0 matches no source (real seq ≥ 1).
    currentSeq_ = 0;
    pastLast_   = true;

    if (drainRemaining_ > 0) {
        --drainRemaining_;
        fireWake();          // keep cycling so endogenous events flush + propagate
        return 0;
    }

    if (!exhaustedFired_) {
        exhaustedFired_ = true;
        if (onExhausted_) onExhausted_();   // app calls Engine::stop() here
    }
    return 0;
}

inline std::size_t ReplayCoordinator::pendingCount() const {
    return nextIdx_ < schedule_.size() ? schedule_.size() - nextIdx_ : 0;
}

inline std::string ReplayCoordinator::name() const { return "replay_coordinator"; }

inline void ReplayCoordinator::setWakeHook(std::function<void()> hook) {
    wakeHook_ = std::move(hook);
}

inline void ReplayCoordinator::reset() {
    nextIdx_        = 0;
    drainRemaining_ = drainCycles_;
    currentSeq_     = 0;
    pastLast_       = false;
    exhaustedFired_ = false;
    if (!schedule_.empty())
        clock_->setFirstTs(schedule_.front().tsUs);
}

// ─────────────────────────────────────────────────────────────────────────────
// ReplayInput<T>
// ─────────────────────────────────────────────────────────────────────────────

template<typename T>
ReplayInput<T>::ReplayInput(std::string n, std::vector<ReplaySample<T>> samples,
                            ReplayCoordinatorPtr coord, EqualityPolicyPtr eq)
    : name_(std::move(n))
    , value_(make_value(T{}))
    , eq_(std::move(eq))
    , samples_(std::move(samples))
    , coord_(std::move(coord)) {}

template<typename T>
std::shared_ptr<ReplayInput<T>> ReplayInput<T>::make(
    std::string name, std::vector<ReplaySample<T>> samples,
    ReplayCoordinatorPtr coordinator, EqualityPolicyPtr eq)
{
    if (!eq) eq = std::make_shared<TypedEqualityPolicy<T>>();
    return std::shared_ptr<ReplayInput<T>>(
        new ReplayInput<T>(std::move(name), std::move(samples),
                           std::move(coordinator), std::move(eq)));
}

template<typename T>
std::size_t ReplayInput<T>::flush() {
    if (cursor_ >= samples_.size()) return 0;
    if (samples_[cursor_].seq != coord_->currentSeq()) return 0;
    // At most one event per input per cycle (seqs are distinct per stream).
    auto newV = make_value(samples_[cursor_].value);
    ++cursor_;
    if (!eq_->equal(value_, newV)) {
        value_ = newV;
        markDirty();
        notifyDownstream();
        return 1;
    }
    return 0;
}

template<typename T>
std::size_t ReplayInput<T>::pendingCount() const { return samples_.size() - cursor_; }

template<typename T>
std::string ReplayInput<T>::name() const { return name_; }

template<typename T>
void ReplayInput<T>::setWakeHook(std::function<void()> hook) {
    wakeHook_ = std::move(hook);   // stored, never fired — the coordinator wakes
}

template<typename T>
const T& ReplayInput<T>::current() const { return get_value<T>(value_); }

template<typename T>
ValuePtr ReplayInput<T>::eval(EvalContext&) { markClean(); return value_; }

template<typename T>
std::vector<NodePtr> ReplayInput<T>::inputs() const { return {}; }

// ─────────────────────────────────────────────────────────────────────────────
// ReplayQueue<T>
// ─────────────────────────────────────────────────────────────────────────────

template<typename T>
ReplayQueue<T>::ReplayQueue(std::string n,
                            std::vector<ReplaySample<std::vector<T>>> batches,
                            ReplayCoordinatorPtr coord)
    : name_(std::move(n))
    , value_(make_value(std::vector<T>{}))
    , batches_(std::move(batches))
    , coord_(std::move(coord)) {}

template<typename T>
std::shared_ptr<ReplayQueue<T>> ReplayQueue<T>::make(
    std::string name, std::vector<ReplaySample<std::vector<T>>> batches,
    ReplayCoordinatorPtr coordinator)
{
    return std::shared_ptr<ReplayQueue<T>>(
        new ReplayQueue<T>(std::move(name), std::move(batches), std::move(coordinator)));
}

template<typename T>
std::size_t ReplayQueue<T>::flush() {
    std::vector<T> batch;   // empty on a non-matching cycle
    if (cursor_ < batches_.size() && batches_[cursor_].seq == coord_->currentSeq()) {
        batch = batches_[cursor_].value;
        ++cursor_;
    }
    const std::size_t n = batch.size();
    value_ = make_value(std::move(batch));   // always refresh (mirrors AsyncQueue)
    if (n == 0) return 0;
    markDirty();
    notifyDownstream();
    return n;
}

template<typename T>
std::size_t ReplayQueue<T>::pendingCount() const { return batches_.size() - cursor_; }

template<typename T>
std::string ReplayQueue<T>::name() const { return name_; }

template<typename T>
void ReplayQueue<T>::setWakeHook(std::function<void()> hook) {
    wakeHook_ = std::move(hook);   // stored, never fired
}

template<typename T>
ValuePtr ReplayQueue<T>::eval(EvalContext&) { markClean(); return value_; }

template<typename T>
std::vector<NodePtr> ReplayQueue<T>::inputs() const { return {}; }

} // namespace dag::async
