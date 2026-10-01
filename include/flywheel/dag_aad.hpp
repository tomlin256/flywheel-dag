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
//     A DifferentiableNode (below) is the compute node a tape can see into.
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
// Sensitivities as nodes
// ──────────────────────
// A pass in the root's output callback misses a gradient that moves while the
// root's value stands still: x·y is 6 at (2, 3) and at (3, 2), with gradients
// (3, 2) and (2, 3). A GradientNode (below) holds the gradient as its value, so
// an engine delivers it through addOutput like any other value:
//
//   auto grad = aad::GradientNode::make("dprice", price, {spot, vol, rate});
//   engine.addOutput<std::vector<double>>(grad, [](const std::vector<double>& g) {
//       // g[0] = ∂price/∂spot, g[1] = ∂price/∂vol, g[2] = ∂price/∂rate
//   });
//
// A TangentNode (below) is its forward-mode counterpart: each of several roots'
// derivative in one direction, from one tape and one sweep:
//
//   auto dspot = aad::TangentNode::make("dspot", {price, hedge}, {{spot, 1.0}});
//   engine.addOutput<std::vector<double>>(dspot, [](const std::vector<double>& t) {
//       // t[0] = ∂price/∂spot, t[1] = ∂hedge/∂spot
//   });
//
// Never include dag_aad.inl directly — always include this file.

#include "dag.hpp"
#include "dag_ops.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <functional>
#include <memory>
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
    // The sensitivity nodes record each root right after they pull it, through
    // record(), and TangentNode records its roots one at a time.
    friend class GradientNode;
    friend class TangentNode;

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

    /// An empty tape, for add().
    Tape() = default;
    /// Records root, and each node it depends on that the tape does not hold
    /// yet, at the values they hold now. Throws std::invalid_argument unless
    /// root is non-null, clean and holds a double. Private because between two
    /// calls nothing may move the graph but the pull of the next root: a caller
    /// that moved an input between them would get a tape that mixes two points.
    void add(const NodePtr& root);
    /// add() without the clean check, for a root the caller has just pulled.
    /// An always-dirty node that reaches the root by two paths leaves it dirty
    /// after its pull, and the walk then evaluates what went dirty again, as it
    /// evaluates any always-dirty node it meets. Throws std::invalid_argument
    /// unless root holds a double.
    void record(const NodePtr& root);
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
// The quotient, exp, log, sqrt, pow and the trigonometric functions take their
// partials from the ops' Derivative<Op>, so a Dual and an op node agree to the
// bit.
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
template<std::size_t N> Dual<N> tan(const Dual<N>& x);
template<std::size_t N> Dual<N> asin(const Dual<N>& x);
template<std::size_t N> Dual<N> acos(const Dual<N>& x);
template<std::size_t N> Dual<N> atan(const Dual<N>& x);
template<std::size_t N> Dual<N> tanh(const Dual<N>& x);
template<std::size_t N> Dual<N> erf(const Dual<N>& x);
template<std::size_t N> Dual<N> erfc(const Dual<N>& x);
template<std::size_t N> Dual<N> abs(const Dual<N>& x);

/// With ops::PowerNode's partials, including where a^b is flat.
template<std::size_t N> Dual<N> pow(const Dual<N>& a, const Dual<N>& b);
template<std::size_t N> Dual<N> pow(const Dual<N>& a, double b);
template<std::size_t N> Dual<N> pow(double a, const Dual<N>& b);

/// y first, as std::atan2. With ops::Atan2Node's partials, which neither
/// overflow nor underflow.
template<std::size_t N> Dual<N> atan2(const Dual<N>& a, const Dual<N>& b);
template<std::size_t N> Dual<N> atan2(const Dual<N>& a, double b);
template<std::size_t N> Dual<N> atan2(double a, const Dual<N>& b);

template<std::size_t N> Dual<N> min(const Dual<N>& a, const Dual<N>& b);
template<std::size_t N> Dual<N> min(const Dual<N>& a, double b);
template<std::size_t N> Dual<N> min(double a, const Dual<N>& b);
template<std::size_t N> Dual<N> max(const Dual<N>& a, const Dual<N>& b);
template<std::size_t N> Dual<N> max(const Dual<N>& a, double b);
template<std::size_t N> Dual<N> max(double a, const Dual<N>& b);

namespace detail {

/// std::function<T(const T&, …)>, with N parameters.
template<typename T, typename Seq> struct RepeatedFn;
template<typename T, std::size_t... Is>
struct RepeatedFn<T, std::index_sequence<Is...>> {
    template<std::size_t> using Param = const T&;
    using type = std::function<T(Param<Is>...)>;
};

} // namespace detail

// ─────────────────────────────────────────────────────────────────────────────
// DifferentiableNode<N> — a ComputeNode over N doubles whose functor a tape can
// differentiate.
//
// A ComputeNode's functor is a std::function<Out(const Ins&...)>, so it is
// opaque: a tape cannot see into it, and the node is a barrier. This node takes
// its functor once, as a generic callable, and instantiates it at two types: at
// double to evaluate, and at Dual<N> to take all N partials in one call when a
// tape asks for them.
//
//   auto d1 = aad::DifferentiableNode<3>::make(
//       "d1", {logMoneyness, vol, expiry},
//       [](const auto& m, const auto& v, const auto& t) {
//           using std::sqrt;
//           return (m + 0.5 * v * v * t) / (v * sqrt(t));
//       },
//       InvalidationMode::Lazy);
//
// Everything else is ComputeNode: the same three make() overloads for the
// equality policy and the invalidation mode, a ValueSlot, and an
// allocation-free steady state. Its inputs and its output are doubles.
//
// THE FUNCTOR MUST HAVE NO SIDE EFFECTS. It runs again, on duals, each time a
// tape records the node, so a functor that mutated captured state would mutate
// it twice. Reading captured state is allowed, and makes the node Eager, as it
// would a ComputeNode.
//
// Why a sibling and not another ComputeNode::make(): std::function erases a
// generic callable to one signature, so ComputeNode would need a second callable
// member. That is an extra member on the hottest class in the engine, for a
// handful of nodes, which is also why InPlaceComputeNode is a sibling.
// ─────────────────────────────────────────────────────────────────────────────
template<std::size_t N>
class DifferentiableNode
    : public NodeBase
    , public IDifferentiable
    , public std::enable_shared_from_this<DifferentiableNode<N>>
{
    static_assert(N > 0, "DifferentiableNode needs an input: a constant is an Input");

public:
    using Inputs = std::array<NodePtr, N>;
    using Fn     = typename detail::RepeatedFn<double, std::make_index_sequence<N>>::type;
    using DualFn = typename detail::RepeatedFn<Dual<N>, std::make_index_sequence<N>>::type;

    /// fn must be callable with N doubles and with N Dual<N>s: a generic lambda
    /// whose body calls its functions unqualified.
    template<typename F>
    static std::shared_ptr<DifferentiableNode> make(
        std::string name, Inputs inNodes, F fn, EqualityPolicyPtr eq = nullptr);
    template<typename F>
    static std::shared_ptr<DifferentiableNode> make(
        std::string name, Inputs inNodes, F fn, InvalidationMode mode);
    template<typename F>
    static std::shared_ptr<DifferentiableNode> make(
        std::string name, Inputs inNodes, F fn, EqualityPolicyPtr eq, InvalidationMode mode);

    ValuePtr eval(EvalContext& ctx) override;
    std::string name() const override;
    std::vector<NodePtr> inputs() const override;
    NodeKind kind() const override { return NodeKind::Compute; }

    /// All N partials, from one call of the functor on duals.
    bool partials(EvalContext& ctx, Partials& out) override;

private:
    DifferentiableNode(std::string name, Inputs ins, Fn fn, DualFn dualFn,
                       EqualityPolicyPtr eq, InvalidationMode mode);

    template<std::size_t... Is>
    ValuePtr applyInputs(EvalContext& ctx, std::index_sequence<Is...>);
    template<std::size_t... Is>
    void dualPartials(EvalContext& ctx, Partials& out, std::index_sequence<Is...>);

    std::string       name_;
    Inputs            inputs_;
    Fn                fn_;
    DualFn            dualFn_;
    EqualityPolicyPtr eq_;
    ValuePtr          cached_;
    ValueSlot<double> slot_;
};

template<std::size_t N>
using DifferentiableNodePtr = std::shared_ptr<DifferentiableNode<N>>;

// ─────────────────────────────────────────────────────────────────────────────
// GradientNode — a node whose value is a gradient: ∂root/∂w for each w in wrt,
// in wrt's order, as a std::vector<double>.
//
// Each time it recomputes, it pulls its root, records a Tape on it and sweeps it
// in reverse, as aad::adjoints() does. It publishes the result through its
// equality policy like any other node, so an engine output fires only when the
// gradient moves.
//
// Its one input is its root. A wrt node the root depends on sits upstream of the
// root, so the root carries its changes. One the root does not depend on has
// derivative 0. Pulling the wrt nodes as well would evaluate nodes the root does
// not read, such as one on the branch a ConditionNode did not take.
//
// It is EAGER, fixed here, and that is the point of it. Its value depends on the
// partials of every node the tape records, and it declares none of them as
// inputs. A change upstream of the root marks it at least Maybe, and an Eager
// node recomputes then. A Lazy node would skip whenever the root's value stood
// still, and x·y at (2, 3) and at (3, 2) is exactly that case.
//
// It evaluates what the root's eval() evaluates, and nothing more: the tape then
// reads clean nodes alone, an always-dirty node apart (below). Pulling the root
// is what any consumer does to its input, so it is not the evaluation the tape's
// rules forbid, and the engine's dirty snapshot still covers a root that is a
// registered output too.
//
// eval() throws what the tape throws, and the node stays dirty, so the next
// eval() tries again: std::domain_error for a barrier with a wrt node upstream,
// and std::invalid_argument for a root, or a wrt node on the tape, that does not
// hold a double. make() cannot check either, because both depend on values.
//
// It stays dirty when its root goes dirty again during its evaluation, as any
// node does when an input it read does (NodeBase::endEval()). The tape's pulls
// can evaluate a node that is always dirty, such as an application's
// clock-driven node, which then marks the root dirty again. Marked clean, this
// node would never see the next change, which stops at the root
// (flywheel-dag#19). It tells its consumers too, so a node over it sees that
// change as well (flywheel-dag#18).
//
// It records the root it has just pulled, clean or not (Tape::record()). An
// always-dirty node that reaches the root by two paths leaves the root dirty
// after its own pull, and requiring it clean would make this node throw on
// every evaluation. Recorded anyway, the tape's walk evaluates what went dirty
// again, which the caveat on always-dirty nodes already allows.
//
// COST. Every recompute records a new tape, which costs about 20 evaluations of
// the root, and an Eager node recomputes on every change upstream of its root,
// including a change the gradient does not depend on. A graph that wants its
// sensitivities only now and then should run a pass on demand instead.
//
// Under the default policy, a gradient holding a NaN is unequal to itself, so
// it is published on every recompute, as a double NaN is. It is not an
// IDifferentiable: its value is a vector, so no tape can take it as a root or a
// wrt node, and a node that reads one of its entries is a barrier.
// ─────────────────────────────────────────────────────────────────────────────
class GradientNode
    : public NodeBase
    , public std::enable_shared_from_this<GradientNode>
{
public:
    /// ∂root/∂w for each w in wrt, in wrt's order. Throws std::invalid_argument
    /// if root or a wrt node is null, or if wrt is empty. The default policy is
    /// TypedEqualityPolicy<std::vector<double>>.
    static std::shared_ptr<GradientNode> make(
        std::string name, NodePtr root, std::vector<NodePtr> wrt,
        EqualityPolicyPtr eq = nullptr);

    ValuePtr eval(EvalContext& ctx) override;
    std::string name() const override;
    /// The root alone.
    std::vector<NodePtr> inputs() const override;
    NodeKind kind() const override { return NodeKind::Compute; }

private:
    GradientNode(std::string name, NodePtr root, std::vector<NodePtr> wrt,
                 EqualityPolicyPtr eq);

    std::string                    name_;
    NodePtr                        root_;
    std::vector<NodePtr>           wrt_;
    EqualityPolicyPtr              eq_;
    ValuePtr                       cached_;
    ValueSlot<std::vector<double>> slot_;
};

using GradientNodePtr = std::shared_ptr<GradientNode>;

// ─────────────────────────────────────────────────────────────────────────────
// TangentNode — a node whose value is a forward sweep's tangents: each root's
// derivative in the direction the seeds give, Σ ∂root/∂s.node · s.tangent, in
// the roots' order, as a std::vector<double>.
//
// GradientNode's forward-mode counterpart, and what GradientNode says about
// itself holds here too. It is EAGER, fixed here. Its inputs are its roots, and
// a seed's node is not one. It evaluates what its roots' eval() evaluates, it
// throws what the tape throws, and it is not an IDifferentiable. The seeds are
// constants, given at make().
//
// ONE TAPE. Each recompute pulls the roots in turn, and records each root right
// after its pull, not after all of them. A later root's pull can leave an
// earlier root dirty again, when an always-dirty node reaches both. Recorded
// right after its pull, each root is recorded at the values its pull left, and
// a later pull evaluates only nodes the tape does not hold yet, an always-dirty
// node apart, so it moves no other value the tape recorded. One forward sweep of
// the one tape serves every root. (Before flywheel-dag#18's fix, a stale node on
// the branch a ConditionNode did not take did the same, and a tape recorded
// after every pull threw.)
//
// It stays dirty when a root goes dirty again during its evaluation, for
// GradientNode's reason, whether a later root's pull or an always-dirty node
// the tape pulled left it so, and it tells its consumers. It records a root
// that stayed dirty after its own pull, as GradientNode does.
//
// COST. A recompute records one tape over every root's nodes, which costs about
// 20 evaluations of them, and it happens on every change upstream of any root.
// ─────────────────────────────────────────────────────────────────────────────
class TangentNode
    : public NodeBase
    , public std::enable_shared_from_this<TangentNode>
{
public:
    /// Each root's derivative in the direction the seeds give,
    /// Σ ∂root/∂s.node · s.tangent, in the roots' order. Throws
    /// std::invalid_argument if roots or seeds is empty, or if a root or a
    /// seed's node is null. The default policy is
    /// TypedEqualityPolicy<std::vector<double>>.
    static std::shared_ptr<TangentNode> make(
        std::string name, std::vector<NodePtr> roots, std::vector<Seed> seeds,
        EqualityPolicyPtr eq = nullptr);

    ValuePtr eval(EvalContext& ctx) override;
    std::string name() const override;
    /// The roots.
    std::vector<NodePtr> inputs() const override;
    NodeKind kind() const override { return NodeKind::Compute; }

private:
    TangentNode(std::string name, std::vector<NodePtr> roots, std::vector<Seed> seeds,
                EqualityPolicyPtr eq);

    std::string                    name_;
    std::vector<NodePtr>           roots_;
    std::vector<Seed>              seeds_;
    EqualityPolicyPtr              eq_;
    ValuePtr                       cached_;
    ValueSlot<std::vector<double>> slot_;
};

using TangentNodePtr = std::shared_ptr<TangentNode>;

} // namespace dag::aad

#include "dag_aad.inl"
