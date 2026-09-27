// =============================================================================
// flywheel-dag — A header-only C++17 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

#pragma once
// dag_traversal.hpp — BFS range + iterator API over the DAG.
//
// Defines a lazy, dedup-by-pointer BFS over INode upstream edges, exposed as
// an iterable range whose iterator models std::input_iterator_tag. The
// neighbors function is a customisation point: pass any NeighborsFn to drive
// upstream walks, downstream walks, or filtered subgraphs.
//
// The convenience free function bfs_upstream(roots) seeds a BfsRange with
// INode::inputs() as the neighbors function — the only constructor production
// code currently needs.
//
// Usage:
//
//   for (const NodePtr& n : dag::traversal::bfs_upstream(roots)) { ... }
//
//   auto found = std::find_if(
//       bfs.begin(), bfs.end(),
//       [](const NodePtr& n) { return n->name() == "foo"; });
//
// Never include dag_traversal.inl directly — always include this file.

#include "dag.hpp"

#include <functional>
#include <iterator>
#include <queue>
#include <unordered_set>
#include <utility>
#include <vector>

namespace dag::traversal {

// ─────────────────────────────────────────────────────────────────────────────
// NeighborsFn — customisation point.
//
// Given a node, return the nodes that should be visited next. The default
// upstream walk uses INode::inputs(); downstream walks or filtered subgraphs
// supply their own function.
// ─────────────────────────────────────────────────────────────────────────────
using NeighborsFn = std::function<std::vector<NodePtr>(const INode&)>;

// ─────────────────────────────────────────────────────────────────────────────
// BfsRange
//
// Lazy, dedup-by-pointer BFS over the DAG. Visits each unique INode (by raw
// pointer identity) exactly once, in BFS order from the supplied roots.
//
// The iterator models std::input_iterator_tag — single-pass, dereferences to
// const NodePtr&, supports range-for and STL algorithms (find_if, count_if,
// for_each, copy_if, ...).
// ─────────────────────────────────────────────────────────────────────────────
class BfsRange {
public:
    BfsRange(std::vector<NodePtr> roots, NeighborsFn neighbors);

    class iterator {
    public:
        using iterator_category = std::input_iterator_tag;
        using value_type        = NodePtr;
        using reference         = const NodePtr&;
        using pointer           = const NodePtr*;
        using difference_type   = std::ptrdiff_t;

        reference operator*()  const;
        pointer   operator->() const;
        iterator& operator++();
        iterator  operator++(int);
        bool operator==(const iterator& other) const;
        bool operator!=(const iterator& other) const;

    private:
        friend class BfsRange;
        iterator() = default;                          // end sentinel
        explicit iterator(const BfsRange* owner);      // seeded begin
        void advance();

        const BfsRange*                  owner_{nullptr};
        std::queue<NodePtr>              pending_;
        std::unordered_set<const INode*> visited_;
        NodePtr                          current_;
    };

    iterator begin() const;
    iterator end()   const;

private:
    std::vector<NodePtr> roots_;
    NeighborsFn          neighbors_;
};

// ─────────────────────────────────────────────────────────────────────────────
// bfs_upstream — convenience constructor for the upstream walk.
//
// Walks INode::inputs(), deduped by pointer identity, in BFS order.
// ─────────────────────────────────────────────────────────────────────────────
inline BfsRange bfs_upstream(std::vector<NodePtr> roots) {
    return BfsRange(std::move(roots),
                    [](const INode& n) { return n.inputs(); });
}

} // namespace dag::traversal

#include "dag_traversal.inl"
