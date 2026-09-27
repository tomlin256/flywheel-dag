// =============================================================================
// flywheel-dag — A header-only C++17 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

// dag_traversal.inl — implementation of dag_traversal.hpp declarations.
// Included at the bottom of dag_traversal.hpp; never include this file directly.

#pragma once

namespace dag::traversal {

// ─────────────────────────────────────────────────────────────────────────────
// BfsRange
// ─────────────────────────────────────────────────────────────────────────────

inline BfsRange::BfsRange(std::vector<NodePtr> roots, NeighborsFn neighbors)
    : roots_(std::move(roots))
    , neighbors_(std::move(neighbors)) {}

inline BfsRange::iterator BfsRange::begin() const {
    return iterator(this);
}

inline BfsRange::iterator BfsRange::end() const {
    return iterator();
}

// ─────────────────────────────────────────────────────────────────────────────
// BfsRange::iterator
// ─────────────────────────────────────────────────────────────────────────────

inline BfsRange::iterator::iterator(const BfsRange* owner)
    : owner_(owner) {
    for (const auto& r : owner_->roots_) {
        if (!r) continue;
        if (visited_.insert(r.get()).second)
            pending_.push(r);
    }
    advance();
}

inline void BfsRange::iterator::advance() {
    if (pending_.empty()) {
        current_.reset();
        owner_  = nullptr;          // become end sentinel
        return;
    }
    current_ = std::move(pending_.front());
    pending_.pop();

    if (owner_ && owner_->neighbors_) {
        for (const auto& nb : owner_->neighbors_(*current_)) {
            if (!nb) continue;
            if (visited_.insert(nb.get()).second)
                pending_.push(nb);
        }
    }
}

inline BfsRange::iterator::reference BfsRange::iterator::operator*() const {
    return current_;
}

inline BfsRange::iterator::pointer BfsRange::iterator::operator->() const {
    return &current_;
}

inline BfsRange::iterator& BfsRange::iterator::operator++() {
    advance();
    return *this;
}

inline BfsRange::iterator BfsRange::iterator::operator++(int) {
    // Input-iterator post-increment: return a copy that holds the current
    // value, then advance *this. Iteration is single-pass — the returned
    // copy is only valid for one dereference.
    iterator snap;
    snap.current_ = current_;
    advance();
    return snap;
}

inline bool BfsRange::iterator::operator==(const iterator& other) const {
    // Two iterators are equal iff both are end sentinels.
    // Begin iterators are never equal to other begin iterators; this is
    // sufficient for single-pass input-iterator semantics (begin != end
    // until the range is exhausted).
    const bool a_end = (owner_ == nullptr);
    const bool b_end = (other.owner_ == nullptr);
    return a_end && b_end;
}

inline bool BfsRange::iterator::operator!=(const iterator& other) const {
    return !(*this == other);
}

} // namespace dag::traversal
