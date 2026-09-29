// =============================================================================
// flywheel-dag — A header-only C++17 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

#pragma once
// dag_aad.hpp — algorithmic differentiation over the graph
//
// The graph is the tape
// ─────────────────────
// An AAD library records every arithmetic operation on a tape as the program
// runs, through an active number type, and then sweeps the tape. Here the
// program is already a graph, and inputs() already names every edge. So a Tape
// records one entry per node instead of one per operation: the node's local
// partial derivatives at the values the graph holds now, which it reports
// through aad::IDifferentiable (dag.hpp). Nothing is recorded while the graph
// evaluates, so a graph that never asks for a derivative pays nothing.
//
//   EvalContext ctx;
//   price->eval(ctx);                                  // a tape reads a clean root
//   auto g = aad::adjoints(price, {spot, vol, rate});  // ∂price/∂spot, ∂price/∂vol, ∂price/∂rate
//
// What a tape records
// ───────────────────
// It walks up from the roots once, and follows only the inputs each node's
// partials name:
//
//   • A node with no inputs, such as Input, AsyncInput or ReplayInput, is a
//     LEAF.
//   • A node with inputs but no partials is a BARRIER, and its inputs are not
//     followed. Every dag::ts node is one. So is a ComputeNode,
//     InPlaceComputeNode or MemoizedComputeNode, because its functor is opaque.
//   • The branch a ConditionNode did not take, and the inputs of a tweaked
//     node, are never named, so the walk never reaches them.
//
// The rules
// ─────────
//   • Every root must be clean and hold a double, or the Tape throws
//     std::invalid_argument. A tape reads what the graph holds and evaluates
//     nothing. Evaluating a dirty root for the caller would break an engine:
//     Engine::cycle() snapshots its outputs' dirty flags before it evaluates
//     them, so a registered output that a pass evaluated between cycles reads
//     as clean, and its callback misses the change. Run a pass in the root's
//     output callback, where the engine has just evaluated it, or evaluate the
//     root first.
//   • Any node can be a wrt node, not only a leaf. Its adjoint is the
//     derivative with respect to a change in its own value.
//   • A node the root does not depend on at this point has derivative 0. That
//     includes a node on the branch not taken.
//   • A barrier with a wrt node upstream throws std::domain_error, naming both.
//     The derivative through a barrier is unknown, and 0 would be a silently
//     wrong answer. A barrier with no wrt node upstream is a constant, and a
//     barrier can itself be a wrt node. Only the barriers the swept root
//     reaches count.
//   • A zero adjoint propagates nothing, so a constant's infinite or NaN
//     partial never reaches a result. A NaN partial on a path the result
//     depends on still gives NaN, which is the true answer.
//   • A wrt node on the tape must hold a double, or the sweep throws
//     std::invalid_argument. A node off the tape has derivative 0, whatever it
//     holds.
//   • A tape holds partials, not values, so it describes the point it was
//     recorded at. Once an input moves, record a new one. A sweep never reads
//     a value from the graph.
//   • Eval thread only, like eval().
//
// Never include dag_aad.inl directly — always include this file.

#include "dag.hpp"

#include <cstddef>
#include <stdexcept>
#include <string>
#include <typeindex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace dag::aad {

// ─────────────────────────────────────────────────────────────────────────────
// Tape — the nodes some roots depend on at the values they hold now, each with
// its local partials, in topological order: every node after the inputs it
// names.
//
// Record once and sweep many times. A full Jacobian is one reverse sweep per
// root over the same tape.
// ─────────────────────────────────────────────────────────────────────────────
class Tape {
public:
    /// Records the nodes the roots depend on at the values they hold now.
    /// Throws std::invalid_argument unless every root is non-null, clean and
    /// holds a double.
    explicit Tape(std::vector<NodePtr> roots);

    /// Reverse sweep: ∂root/∂w for each w in wrt, in wrt's order, in one sweep
    /// however many there are. root must be one of the tape's roots.
    std::vector<double> adjoints(const NodePtr& root, const std::vector<NodePtr>& wrt) const;

    /// The number of nodes recorded.
    std::size_t size() const noexcept;

private:
    struct Edge {
        std::size_t input;   ///< the named input's position on the tape
        double      d;       ///< ∂node/∂input
    };

    struct Entry {
        NodePtr     node;
        std::size_t firstEdge;     ///< the node's edges are [firstEdge, lastEdge)
        std::size_t lastEdge;
        bool        barrier;       ///< it has inputs but no partials
        bool        holdsDouble;
    };

    void record();
    std::size_t rootPosition(const NodePtr& root) const;
    void checkWrt(const std::vector<NodePtr>& wrt) const;
    void checkBarriers(const NodePtr& root, const std::vector<bool>& reached,
                       const std::vector<NodePtr>& wrt) const;

    std::vector<NodePtr>                          roots_;
    std::vector<Entry>                            entries_;
    std::vector<Edge>                             edges_;
    std::unordered_map<const INode*, std::size_t> position_;
};

/// Records a tape for one root and sweeps it once in reverse: ∂root/∂w for
/// each w in wrt.
std::vector<double> adjoints(const NodePtr& root, const std::vector<NodePtr>& wrt);

} // namespace dag::aad

#include "dag_aad.inl"
