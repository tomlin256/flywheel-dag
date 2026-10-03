// =============================================================================
// flywheel-dag — A header-only C++17 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

#pragma once
// dag_window_status.hpp — WindowStatus value type and IWindowed interface
//
// WindowStatus carries the capacity and filled count of any windowed node as a
// plain struct so a downstream node can make warmup-aware decisions without
// inspecting its parent.
//
// IWindowed is implemented by the five windowed time-series nodes:
//   RollingStats, RollingSumNode, DelayNode<T>, WindowNode<T>, RollingMinMaxNode
//
// Each creates a companion DAG node on the first call to windowStatusNode().

#include "flywheel/dag.hpp"
#include <cstddef>

namespace dag::ts {

struct WindowStatus {
    std::size_t capacity = 0;
    std::size_t filled   = 0;

    bool   full()     const noexcept { return capacity > 0 && filled >= capacity; }
    double fraction() const noexcept { return capacity ? double(filled) / double(capacity) : 0.0; }

    friend bool operator==(const WindowStatus& a, const WindowStatus& b) noexcept {
        return a.capacity == b.capacity && a.filled == b.filled;
    }
    friend bool operator!=(const WindowStatus& a, const WindowStatus& b) noexcept {
        return !(a == b);
    }
};

class IWindowed {
public:
    virtual ~IWindowed() = default;
    virtual std::size_t capacity() const noexcept = 0;
    virtual std::size_t filled()   const noexcept = 0;
    /// DAG node whose value is the WindowStatus, recomputed whenever the
    /// windowed node fires. Created on the first call; while at least one strong
    /// consumer holds it, repeat calls return the same node (one DAG edge per
    /// consumer). When no consumer is holding it, repeat calls return a fresh
    /// node — the expected usage is "call once, wire into the consumer."
    /// **Precondition:** the implementing node must already be owned by a
    /// shared_ptr (i.e., make() has returned). Never call from a constructor.
    virtual dag::NodePtr windowStatusNode() const = 0;
};

} // namespace dag::ts
