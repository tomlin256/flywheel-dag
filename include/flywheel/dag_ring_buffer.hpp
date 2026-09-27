// =============================================================================
// flywheel-dag — A header-only C++17 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

#pragma once
// dag_ring_buffer.hpp — fixed-capacity contiguous ring buffer.
//
// Why this exists
// ───────────────
// Every windowed time-series node used std::deque as its backing store. Two
// costs came with that:
//
//   • Allocation. libc++ deque holds 4096-byte blocks and turns one over each
//     time the sliding window walks off the end of the current block —
//     measured at 2 allocations (a 4096-byte block plus a 16-byte map slot)
//     per ~512 pushes, forever.
//
//   • Segmented storage. A deque's elements are not contiguous, so a loop over
//     one cannot vectorise and strides unpredictably through memory, which
//     alone keeps a per-cycle fold over a window scalar.
//
// RingBuffer allocates its storage exactly once, in setCapacity(), and never
// again. Elements live in one flat std::vector, so a scan is at worst two
// contiguous runs — see for_each_contiguous().
//
// Semantics
// ─────────
// Deliberately NOT "overwrite the oldest when full": push_back() on a full
// buffer throws. Every caller already pops explicitly (RollingStats evicts
// before pushing, DelayNode pops once it exceeds the delay), and silently
// dropping the oldest element on overflow is exactly the kind of quiet data
// loss this project rejects. Size the buffer for the transient peak — a node
// that pushes before popping needs capacity N+1, not N.
//
// Not thread-safe: eval-thread only, like the nodes that hold it.

#include <algorithm>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace dag {

template <typename T>
class RingBuffer {
public:
    RingBuffer() = default;
    explicit RingBuffer(std::size_t capacity);

    /// Allocate storage for `capacity` elements and drop any current contents.
    /// The only function here that allocates. Calling it with the capacity it
    /// already has is a no-op and preserves the contents.
    void setCapacity(std::size_t capacity);

    void push_back(const T& v);
    void push_back(T&& v);
    void pop_front();
    void pop_back();
    void clear() noexcept;

    const T& front() const;
    const T& back() const;
    /// Index 0 is the oldest element.
    const T& operator[](std::size_t i) const;

    std::size_t size()     const noexcept { return size_; }
    std::size_t capacity() const noexcept { return buf_.size(); }
    bool        empty()    const noexcept { return size_ == 0; }
    bool        full()     const noexcept { return size_ == buf_.size(); }

    /// Visit the contents in order as at most two contiguous runs:
    ///   fn(const T* data, std::size_t count)
    /// Callers get flat pointers, so the loop inside fn is vectorisable in a
    /// way a deque walk never was. Nothing is copied.
    template <typename Fn>
    void for_each_contiguous(Fn&& fn) const;

private:
    [[noreturn]] static void throwEmpty(const char* op);

    std::vector<T> buf_;
    std::size_t    head_ = 0;   // index of the oldest element
    std::size_t    size_ = 0;
};

}  // namespace dag

#include "dag_ring_buffer.inl"
