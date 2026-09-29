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
// A tape is swept in either direction:
//
//   • Reverse (adjoints): one sweep down from a root gives its derivative with
//     respect to every node it depends on. One sweep per output.
//   • Forward (tangents): one sweep up from the seeds gives the derivative of
//     every root in the direction they give. One sweep per direction.
//
//   EvalContext ctx;
//   price->eval(ctx);    // a tape reads a clean root
//   hedge->eval(ctx);
//
//   // ∂price/∂spot, ∂price/∂vol and ∂price/∂rate, in one sweep:
//   auto g = aad::adjoints(price, {spot, vol, rate});
//
//   // ∂price/∂vol and ∂hedge/∂vol, in one sweep:
//   auto dv = aad::tangents({price, hedge}, {{vol, 1.0}});
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
//   • Any node can be a wrt or seed node, not only a leaf. Its adjoint is the
//     derivative with respect to a change in its own value. A seed on an
//     intermediate node adds to the tangent that reaches it. That keeps the two
//     sweeps exact duals: a root's tangent is Σ seed · adjoint.
//   • A node the root does not depend on at this point has derivative 0. That
//     includes a node on the branch not taken.
//   • A barrier with a wrt or seed node upstream throws std::domain_error,
//     naming both. The derivative through a barrier is unknown, and 0 would be
//     a silently wrong answer. A barrier with none upstream is a constant, and
//     a barrier can itself be a wrt or seed node. A reverse sweep counts only
//     the barriers its root reaches. A forward sweep serves every root, so it
//     counts every barrier on the tape.
//   • A partial times an adjoint or a tangent counts as 0 when either one is
//     0. So a constant's infinite or NaN partial never reaches a result, and
//     neither does an infinite rate through a partial of 0: z·√x at z = 0 and
//     x = 0 has ∂/∂x = 0 in both sweeps, because it is 0 for every x. A NaN on
//     a path the result depends on, not multiplied by 0, still gives NaN,
//     which is the true answer.
//   • A wrt or seed node on the tape must hold a double, or the sweep throws
//     std::invalid_argument. A node off the tape has derivative 0, whatever it
//     holds.
//   • A tape holds partials, not values, so it describes the point it was
//     recorded at. Once an input moves, record a new one. A sweep never reads
//     a value from the graph.
//   • Eval thread only, like eval().
//
// Never include dag_aad.inl directly — always include this file.

#include "dag.hpp"
#include "dag_ops.hpp"

#include <array>
#include <cmath>
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
// Seed — one part of a forward sweep's direction: a node, and the rate at which
// it moves.
// ─────────────────────────────────────────────────────────────────────────────
struct Seed {
    NodePtr node;
    double  tangent;
};

// ─────────────────────────────────────────────────────────────────────────────
// Tape — the nodes some roots depend on at the values they hold now, each with
// its local partials, in topological order: every node after the inputs it
// names.
//
// Record once and sweep many times. A full Jacobian is one reverse sweep per
// root, or one forward sweep per input, over the same tape.
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

    /// Forward sweep: each root's derivative in the direction the seeds give,
    /// Σ ∂root/∂s.node · s.tangent, in the roots' order, in one sweep however
    /// many roots there are.
    std::vector<double> tangents(const std::vector<Seed>& seeds) const;

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
    /// `target` names what the barriers were reached from, for the message.
    void checkBarriers(const std::string& target, const std::vector<bool>& reached,
                       const std::vector<NodePtr>& wrt) const;

    std::vector<NodePtr>                          roots_;
    std::vector<Entry>                            entries_;
    std::vector<Edge>                             edges_;
    std::unordered_map<const INode*, std::size_t> position_;
};

/// Records a tape for one root and sweeps it once in reverse: ∂root/∂w for
/// each w in wrt.
std::vector<double> adjoints(const NodePtr& root, const std::vector<NodePtr>& wrt);

/// Records a tape for the roots and sweeps it once forward: each root's
/// derivative in the direction the seeds give.
std::vector<double> tangents(const std::vector<NodePtr>& roots, const std::vector<Seed>& seeds);

// ─────────────────────────────────────────────────────────────────────────────
// Dual<N> — a value, and its partial derivatives with respect to N arguments.
//
// It takes a functor's partials in one call. A functor written once, as a
// generic callable, runs at double to evaluate and at Dual<N> to differentiate:
// argument i arrives as its value with d = eᵢ, and the result carries
// ∂result/∂(argument i) in d[i]. Everything is on the stack.
//
// The functions below are found by argument-dependent lookup. A functor calls
// them unqualified, after `using std::exp;` and the like, so that one body
// compiles at double and at Dual<N>. A functor that writes std::exp(x) does not
// compile at Dual<N>: a compile error, not a wrong answer. chain() lifts any
// other function whose derivative is known.
//
// Comparisons read the value alone, so a functor can branch. Its derivative is
// then the derivative of the branch it took, as a ConditionNode's is.
//
// The rule a tape keeps holds here too: a partial times a derivative counts as
// 0 when either one is 0. So a constant in a functor, a Dual whose d is 0,
// never turns an infinite or NaN partial into a NaN: pow(x, 3.0) at x = −2 has
// derivative 12, although a^b·ln a is NaN there.
//
// At a kink: abs′(0) is 0. min and max follow the argument they return, and
// return the first one on a tie, as std::min and std::max do.
// ─────────────────────────────────────────────────────────────────────────────
template<std::size_t N>
struct Dual {
    double                value = 0.0;
    std::array<double, N> d{};   ///< d[i] = ∂value/∂(argument i)

    Dual() = default;
    /// A constant, whose partials are all 0. Implicit, so that a double converts
    /// wherever a Dual is wanted: `Dual<2> y = 1.0`, or a conditional with a
    /// Dual on one side and a double on the other.
    Dual(double v);   // NOLINT(google-explicit-constructor)

    Dual& operator+=(const Dual& b);
    Dual& operator-=(const Dual& b);
    Dual& operator*=(const Dual& b);
    Dual& operator/=(const Dual& b);
};

/// f(x), given f(x.value) and f′(x.value): the chain rule, for a function this
/// header does not provide.
template<std::size_t N> Dual<N> chain(const Dual<N>& x, double fx, double dfx);

template<std::size_t N> Dual<N> operator+(const Dual<N>& a);
template<std::size_t N> Dual<N> operator-(const Dual<N>& a);

template<std::size_t N> Dual<N> operator+(const Dual<N>& a, const Dual<N>& b);
template<std::size_t N> Dual<N> operator+(const Dual<N>& a, double b);
template<std::size_t N> Dual<N> operator+(double a, const Dual<N>& b);
template<std::size_t N> Dual<N> operator-(const Dual<N>& a, const Dual<N>& b);
template<std::size_t N> Dual<N> operator-(const Dual<N>& a, double b);
template<std::size_t N> Dual<N> operator-(double a, const Dual<N>& b);
template<std::size_t N> Dual<N> operator*(const Dual<N>& a, const Dual<N>& b);
template<std::size_t N> Dual<N> operator*(const Dual<N>& a, double b);
template<std::size_t N> Dual<N> operator*(double a, const Dual<N>& b);
template<std::size_t N> Dual<N> operator/(const Dual<N>& a, const Dual<N>& b);
template<std::size_t N> Dual<N> operator/(const Dual<N>& a, double b);
template<std::size_t N> Dual<N> operator/(double a, const Dual<N>& b);

template<std::size_t N> bool operator==(const Dual<N>& a, const Dual<N>& b);
template<std::size_t N> bool operator==(const Dual<N>& a, double b);
template<std::size_t N> bool operator==(double a, const Dual<N>& b);
template<std::size_t N> bool operator!=(const Dual<N>& a, const Dual<N>& b);
template<std::size_t N> bool operator!=(const Dual<N>& a, double b);
template<std::size_t N> bool operator!=(double a, const Dual<N>& b);
template<std::size_t N> bool operator<(const Dual<N>& a, const Dual<N>& b);
template<std::size_t N> bool operator<(const Dual<N>& a, double b);
template<std::size_t N> bool operator<(double a, const Dual<N>& b);
template<std::size_t N> bool operator<=(const Dual<N>& a, const Dual<N>& b);
template<std::size_t N> bool operator<=(const Dual<N>& a, double b);
template<std::size_t N> bool operator<=(double a, const Dual<N>& b);
template<std::size_t N> bool operator>(const Dual<N>& a, const Dual<N>& b);
template<std::size_t N> bool operator>(const Dual<N>& a, double b);
template<std::size_t N> bool operator>(double a, const Dual<N>& b);
template<std::size_t N> bool operator>=(const Dual<N>& a, const Dual<N>& b);
template<std::size_t N> bool operator>=(const Dual<N>& a, double b);
template<std::size_t N> bool operator>=(double a, const Dual<N>& b);

template<std::size_t N> Dual<N> exp(const Dual<N>& x);
template<std::size_t N> Dual<N> log(const Dual<N>& x);
template<std::size_t N> Dual<N> sqrt(const Dual<N>& x);
template<std::size_t N> Dual<N> sin(const Dual<N>& x);
template<std::size_t N> Dual<N> cos(const Dual<N>& x);
template<std::size_t N> Dual<N> tanh(const Dual<N>& x);
template<std::size_t N> Dual<N> erf(const Dual<N>& x);
template<std::size_t N> Dual<N> erfc(const Dual<N>& x);
template<std::size_t N> Dual<N> abs(const Dual<N>& x);

/// With ops::PowerNode's partials, including where a^b is flat.
template<std::size_t N> Dual<N> pow(const Dual<N>& a, const Dual<N>& b);
template<std::size_t N> Dual<N> pow(const Dual<N>& a, double b);
template<std::size_t N> Dual<N> pow(double a, const Dual<N>& b);

template<std::size_t N> Dual<N> min(const Dual<N>& a, const Dual<N>& b);
template<std::size_t N> Dual<N> min(const Dual<N>& a, double b);
template<std::size_t N> Dual<N> min(double a, const Dual<N>& b);
template<std::size_t N> Dual<N> max(const Dual<N>& a, const Dual<N>& b);
template<std::size_t N> Dual<N> max(const Dual<N>& a, double b);
template<std::size_t N> Dual<N> max(double a, const Dual<N>& b);

} // namespace dag::aad

#include "dag_aad.inl"
