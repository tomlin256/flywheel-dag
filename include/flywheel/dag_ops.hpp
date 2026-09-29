// =============================================================================
// flywheel-dag — A header-only C++17 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

#pragma once
// dag_ops.hpp — arithmetic and trigonometric op primitives built on dag.hpp
//
// Each op below is its own concrete C++ type — not a generic ComputeNode
// wrapping an opaque lambda — so that inputs() plus the op's identity are
// enough for a pass (e.g. reverse-mode AAD) to attach a closed-form local
// derivative without redesigning these primitives. Each op supplies it through
// aad::IDifferentiable (dag.hpp), from Derivative<Op> below. This file only
// evaluates and supplies partials: it records no tape and runs no sweep.
//
// Every op below is a `using` alias over one of three arity-generic
// templates (UnaryOpNode / BinaryOpNode / NAryOpNode<T, Op>), parameterized
// by a small Op functor. Each alias still names its own concrete type —
// SumNode<T> and ProductNode<T> remain as distinguishable to the compiler
// (and to a future AAD dispatch pass) as if they were separate hand-written
// classes, because they expand to different NAryOpNode<T, Op> instantiations.
//
// Node catalogue
// ──────────────
//  SumNode<T>      — n-ary sum:      out = ins[0] + ins[1] + ... (0 ins -> T{})
//  ProductNode<T>  — n-ary product:  out = ins[0] * ins[1] * ... (0 ins -> T{1})
//  DiffNode<T>     — binary:         out = a - b
//  DivideNode<T>   — binary:         out = a / b   (unguarded — see class doc)
//  NegateNode<T>   — unary:          out = -a      (signed T only)
//  ExpNode<T>      — unary:          out = exp(a)  (floating-point T only)
//  LnNode<T>       — unary:          out = ln(a)   (floating-point T only; unguarded)
//  PowerNode<T>    — binary:         out = a ^ b   (floating-point T only; unguarded)
//  SqrtNode<T>     — unary:          out = sqrt(a) (floating-point T only; unguarded)
//  SinNode<T>      — unary:          out = sin(a)  (floating-point T only; unguarded)
//  CosNode<T>      — unary:          out = cos(a)  (floating-point T only; unguarded)
//  TanNode<T>      — unary:          out = tan(a)  (floating-point T only; unguarded)
//  AsinNode<T>     — unary:          out = asin(a) (floating-point T only; unguarded)
//  AcosNode<T>     — unary:          out = acos(a) (floating-point T only; unguarded)
//  AtanNode<T>     — unary:          out = atan(a) (floating-point T only)
//  Atan2Node<T>    — binary:         out = atan2(a, b) (floating-point T only; a is y, b is x)

#include "dag.hpp"
#include <cmath>
#include <functional>
#include <string>
#include <type_traits>
#include <vector>

namespace dag::ops {

// ─────────────────────────────────────────────────────────────────────────────
// OpNodeImpl<Derived> — CRTP helper providing dirty/downstream/kind
// boilerplate for stateless arithmetic ops. Mirrors dag::ts::NodeImpl<Derived>
// in shape; named differently (not reused) to avoid an unqualified-name
// collision if a file ever has both `dag::ts` and `dag::ops` open. Reports
// NodeKind::Compute — these are pure functions of their inputs, same bucket
// as ComputeNode/ConditionNode, not a new kind.
//
// Every op is an aad::IDifferentiable. Each arity template implements
// partials() from Derivative<Op>, and returns false for an op over a type
// other than double or an Op with no specialisation.
// ─────────────────────────────────────────────────────────────────────────────
template<typename Derived>
class OpNodeImpl
    : public NodeBase
    , public aad::IDifferentiable
    , public std::enable_shared_from_this<Derived>
{
public:
    NodeKind kind() const override { return NodeKind::Compute; }

protected:
    // ── Why every op node is Lazy, and why that is not a per-node question ────
    //
    // These nodes are STRUCTURALLY PURE. eval() computes Op{}(...) — the functor
    // is DEFAULT-CONSTRUCTED at every evaluation, so it cannot carry state from
    // one call to the next even if its author wanted it to. That is a property
    // of the arity templates, not of any one op, so the mode is fixed here
    // rather than offered at each make() where a caller could get it wrong.
    OpNodeImpl() noexcept : NodeBase(InvalidationMode::Lazy) {}

    /// Publish this evaluation's value: rebind cached_ and invalidate downstream
    /// if the policy says it changed, then mark clean. The no-argument
    /// NodeBase::notifyDownstream() it builds on stays reachable by that name.
    using NodeBase::notifyDownstream;
    void notifyDownstream(const ValuePtr& newV, const EqualityPolicyPtr& eq);

    ValuePtr cached_;
};

// ─────────────────────────────────────────────────────────────────────────────
// UnaryOpNode<T, Op> — single input: out = Op{}(a).
// Op must supply: T operator()(const T&) const.
// ─────────────────────────────────────────────────────────────────────────────
template<typename T, typename Op>
class UnaryOpNode : public OpNodeImpl<UnaryOpNode<T,Op>> {
public:
    static std::shared_ptr<UnaryOpNode> make(
        std::string name, NodePtr a, EqualityPolicyPtr eq = nullptr);

    ValuePtr eval(EvalContext& ctx) override;
    std::string name() const override;
    std::vector<NodePtr> inputs() const override;
    bool partials(EvalContext& ctx, aad::Partials& out) override;

private:
    UnaryOpNode(std::string name, NodePtr a, EqualityPolicyPtr eq);

    std::string name_;
    NodePtr a_;
    EqualityPolicyPtr eq_;
    ValueSlot<T> slot_;
};

// ─────────────────────────────────────────────────────────────────────────────
// BinaryOpNode<T, Op> — two inputs: out = Op{}(a, b).
// Op must supply: T operator()(const T&, const T&) const.
// ─────────────────────────────────────────────────────────────────────────────
template<typename T, typename Op>
class BinaryOpNode : public OpNodeImpl<BinaryOpNode<T,Op>> {
public:
    static std::shared_ptr<BinaryOpNode> make(
        std::string name, NodePtr a, NodePtr b, EqualityPolicyPtr eq = nullptr);

    ValuePtr eval(EvalContext& ctx) override;
    std::string name() const override;
    std::vector<NodePtr> inputs() const override;
    bool partials(EvalContext& ctx, aad::Partials& out) override;

private:
    BinaryOpNode(std::string name, NodePtr a, NodePtr b, EqualityPolicyPtr eq);

    std::string name_;
    NodePtr a_, b_;
    EqualityPolicyPtr eq_;
    ValueSlot<T> slot_;
};

// ─────────────────────────────────────────────────────────────────────────────
// NAryOpNode<T, Op> — runtime-sized input list, folded via Op.
// out = Op{}( ... Op{}(Op{}(identity, ins[0]), ins[1]) ..., ins[n-1])
// Op must supply: T operator()(const T&, const T&) const, and
// static constexpr T identity() — the fold seed AND the 0-input answer.
// (std::plus<T>/std::multiplies<T> model the combining rule only, not the
// empty-input case, so they can't supply identity().)
// ─────────────────────────────────────────────────────────────────────────────
template<typename T, typename Op>
class NAryOpNode : public OpNodeImpl<NAryOpNode<T,Op>> {
public:
    static std::shared_ptr<NAryOpNode> make(
        std::string name, std::vector<NodePtr> ins, EqualityPolicyPtr eq = nullptr);

    ValuePtr eval(EvalContext& ctx) override;
    std::string name() const override;
    std::vector<NodePtr> inputs() const override;
    bool partials(EvalContext& ctx, aad::Partials& out) override;

private:
    NAryOpNode(std::string name, std::vector<NodePtr> ins, EqualityPolicyPtr eq);

    std::string name_;
    std::vector<NodePtr> inputs_;
    EqualityPolicyPtr eq_;
    ValueSlot<T> slot_;
};

// ─────────────────────────────────────────────────────────────────────────────
// Op functors. std::negate<T>/std::minus<T>/std::divides<T> (<functional>)
// are reused directly below for the fixed-arity ops — they need no identity
// element. Sum/Product need one, so they get small custom
// functors carrying identity() instead of std::plus<T>/std::multiplies<T>.
// ─────────────────────────────────────────────────────────────────────────────
template<typename T>
struct PlusOp {
    static constexpr T identity() { return T{}; }
    T operator()(const T& a, const T& b) const { return a + b; }
};

template<typename T>
struct MultipliesOp {
    static constexpr T identity() { return T{1}; }
    T operator()(const T& a, const T& b) const { return a * b; }
};

// ExpOp<T>/LnOp<T> — no <functional> equivalent exists for either, so these
// wrap <cmath> directly. Neither needs an identity — both are unary.
template<typename T>
struct ExpOp {
    T operator()(const T& a) const { return std::exp(a); }
};

template<typename T>
struct LnOp {
    T operator()(const T& a) const { return std::log(a); }
};

// PowOp<T>/SqrtOp<T> — same reasoning as ExpOp/LnOp: no identity needed
// (PowOp is binary but fixed-arity, not a fold; SqrtOp is unary).
template<typename T>
struct PowOp {
    T operator()(const T& a, const T& b) const { return std::pow(a, b); }
};

template<typename T>
struct SqrtOp {
    T operator()(const T& a) const { return std::sqrt(a); }
};

// The trigonometric Ops wrap <cmath> for the same reason, in radians. None
// needs an identity: Atan2Op is binary but fixed-arity, and the rest are
// unary.
template<typename T>
struct SinOp {
    T operator()(const T& a) const { return std::sin(a); }
};

template<typename T>
struct CosOp {
    T operator()(const T& a) const { return std::cos(a); }
};

template<typename T>
struct TanOp {
    T operator()(const T& a) const { return std::tan(a); }
};

template<typename T>
struct AsinOp {
    T operator()(const T& a) const { return std::asin(a); }
};

template<typename T>
struct AcosOp {
    T operator()(const T& a) const { return std::acos(a); }
};

template<typename T>
struct AtanOp {
    T operator()(const T& a) const { return std::atan(a); }
};

// In std::atan2's order: a is y and b is x.
template<typename T>
struct Atan2Op {
    T operator()(const T& a, const T& b) const { return std::atan2(a, b); }
};

// ─────────────────────────────────────────────────────────────────────────────
// Derivative<Op> — an Op's local partial derivatives, specialised per Op.
//
// This is what the ops being their own types buys (see the top of this file).
// A pass asks a node for its partials, and the node asks this trait for its
// Op's. The primary template has none, so an op whose Op has no specialisation
// is a barrier to a pass. An application's own Op joins by specialising it.
//
// Only double has partials. A pass needs a double root, so an op over another
// type could reach one only through a conversion node, which is a barrier
// anyway. A specialisation sets `defined`, and provides the function for the
// arity its Op is used at:
//
//   unary   static double d(double a);
//   binary  static std::pair<double, double> d(double a, double b);
//   n-ary   static void d(const std::vector<double>& x, std::vector<double>& dx);
//           dx arrives sized like x, and receives ∂/∂x[i] in dx[i].
//
// Where a formula would divide zero by zero, or multiply zero by infinity, at a
// point where the function is in fact flat, the partial is the exact 0. See
// MultipliesOp and PowOp. Where the textbook form would cancel, overflow or
// underflow, the partial is computed another way. See std::divides, AsinOp and
// Atan2Op.
// ─────────────────────────────────────────────────────────────────────────────
template<typename Op>
struct Derivative {
    static constexpr bool defined = false;
};

/// 1 for each input.
template<>
struct Derivative<PlusOp<double>> {
    static constexpr bool defined = true;
    static void d(const std::vector<double>& x, std::vector<double>& dx);
};

/// The product of the other factors, from prefix and suffix products. Not
/// product / x[i], which is 0/0 when x[i] is 0.
template<>
struct Derivative<MultipliesOp<double>> {
    static constexpr bool defined = true;
    static void d(const std::vector<double>& x, std::vector<double>& dx);
};

/// 1 and −1.
template<>
struct Derivative<std::minus<double>> {
    static constexpr bool defined = true;
    static std::pair<double, double> d(double a, double b);
};

/// 1/b and −a/b².
template<>
struct Derivative<std::divides<double>> {
    static constexpr bool defined = true;
    static std::pair<double, double> d(double a, double b);
};

/// −1.
template<>
struct Derivative<std::negate<double>> {
    static constexpr bool defined = true;
    static double d(double a);
};

/// exp(a).
template<>
struct Derivative<ExpOp<double>> {
    static constexpr bool defined = true;
    static double d(double a);
};

/// 1/a.
template<>
struct Derivative<LnOp<double>> {
    static constexpr bool defined = true;
    static double d(double a);
};

/// b·a^(b−1), or 0 when b is 0. a^b·ln a, or 0 at a = 0 with b > 0.
template<>
struct Derivative<PowOp<double>> {
    static constexpr bool defined = true;
    static std::pair<double, double> d(double a, double b);
};

/// 1/(2·√a).
template<>
struct Derivative<SqrtOp<double>> {
    static constexpr bool defined = true;
    static double d(double a);
};

/// cos a.
template<>
struct Derivative<SinOp<double>> {
    static constexpr bool defined = true;
    static double d(double a);
};

/// −sin a.
template<>
struct Derivative<CosOp<double>> {
    static constexpr bool defined = true;
    static double d(double a);
};

/// 1 + tan² a.
template<>
struct Derivative<TanOp<double>> {
    static constexpr bool defined = true;
    static double d(double a);
};

/// 1/√(1 − a²), as 1/√((1 − a)(1 + a)), which does not cancel next to |a| = 1.
/// +∞ at |a| = 1, the one-sided slope, and NaN beyond.
template<>
struct Derivative<AsinOp<double>> {
    static constexpr bool defined = true;
    static double d(double a);
};

/// −1/√(1 − a²): the negative of AsinOp's.
template<>
struct Derivative<AcosOp<double>> {
    static constexpr bool defined = true;
    static double d(double a);
};

/// 1/(1 + a²).
template<>
struct Derivative<AtanOp<double>> {
    static constexpr bool defined = true;
    static double d(double a);
};

/// b/(a² + b²) and −a/(a² + b²), through hypot(a, b), which neither overflows
/// nor underflows. NaN at the origin, where the angle has no derivative.
template<>
struct Derivative<Atan2Op<double>> {
    static constexpr bool defined = true;
    static std::pair<double, double> d(double a, double b);
};

// ─────────────────────────────────────────────────────────────────────────────
// Node catalogue — public names, each a distinct concrete type via its Op.
// ─────────────────────────────────────────────────────────────────────────────
template<typename T = double, typename = std::enable_if_t<std::is_arithmetic_v<T>>>
using SumNode = NAryOpNode<T, PlusOp<T>>;

template<typename T = double, typename = std::enable_if_t<std::is_arithmetic_v<T>>>
using ProductNode = NAryOpNode<T, MultipliesOp<T>>;

template<typename T = double, typename = std::enable_if_t<std::is_arithmetic_v<T>>>
using DiffNode = BinaryOpNode<T, std::minus<T>>;

// DivideNode<T> — binary division: out = a / b.
//
// Deliberately unguarded: b == 0 propagates IEEE inf/nan rather than being
// silently substituted with a guessed fallback value, in line with the
// engine's "no silent defaults" principle — a divide-by-zero is a bug to
// surface, not hide.
template<typename T = double, typename = std::enable_if_t<std::is_arithmetic_v<T>>>
using DivideNode = BinaryOpNode<T, std::divides<T>>;

// NegateNode<T> — unary negation: out = -a.
//
// Constrained to signed T: negating an unsigned type compiles but silently
// wraps (-5u == 4294967291u), a correctness footgun the other ops don't have.
template<typename T = double, typename = std::enable_if_t<std::is_signed_v<T>>>
using NegateNode = UnaryOpNode<T, std::negate<T>>;

// ExpNode<T> — unary exp: out = exp(a).
//
// Constrained to floating-point T, tighter than the is_arithmetic_v used by
// the arithmetic ops: <cmath>'s integral-promoting overload (std::exp(int)
// returns double) would silently truncate the result back into an integral
// T, the same shape of footgun NegateNode's is_signed_v constraint guards
// against for a different reason. Unguarded at the overflow edge — a large
// a propagates IEEE inf, matching DivideNode's precedent.
template<typename T = double, typename = std::enable_if_t<std::is_floating_point_v<T>>>
using ExpNode = UnaryOpNode<T, ExpOp<T>>;

// LnNode<T> — unary natural log: out = ln(a).
//
// Same floating-point-only constraint as ExpNode, same rationale. Unguarded
// at the domain edge: a == 0 propagates IEEE -inf, a < 0 propagates NaN,
// rather than a silently-substituted fallback.
template<typename T = double, typename = std::enable_if_t<std::is_floating_point_v<T>>>
using LnNode = UnaryOpNode<T, LnOp<T>>;

// PowerNode<T> — binary power: out = a ^ b.
//
// Floating-point-only, same rationale as ExpNode/LnNode: <cmath>'s
// integral-promoting pow(int,int) overload would silently truncate the
// result back into an integral T. Unguarded: a negative base with a
// non-integer exponent propagates NaN, overflow propagates IEEE inf,
// matching DivideNode's precedent.
template<typename T = double, typename = std::enable_if_t<std::is_floating_point_v<T>>>
using PowerNode = BinaryOpNode<T, PowOp<T>>;

// SqrtNode<T> — unary square root: out = sqrt(a).
//
// Same floating-point-only constraint as ExpNode/LnNode, same rationale.
// Unguarded at the domain edge: a < 0 propagates NaN.
template<typename T = double, typename = std::enable_if_t<std::is_floating_point_v<T>>>
using SqrtNode = UnaryOpNode<T, SqrtOp<T>>;

// SinNode<T>, CosNode<T>, TanNode<T> — unary: out = sin(a), cos(a), tan(a),
// with a in radians.
//
// Same floating-point-only constraint as ExpNode, same rationale. Unguarded:
// an infinite a propagates NaN. TanNode has no pole at any double, because
// π/2 is not one: at the double nearest it, tan is about 1.6e16.
template<typename T = double, typename = std::enable_if_t<std::is_floating_point_v<T>>>
using SinNode = UnaryOpNode<T, SinOp<T>>;

template<typename T = double, typename = std::enable_if_t<std::is_floating_point_v<T>>>
using CosNode = UnaryOpNode<T, CosOp<T>>;

template<typename T = double, typename = std::enable_if_t<std::is_floating_point_v<T>>>
using TanNode = UnaryOpNode<T, TanOp<T>>;

// AsinNode<T>, AcosNode<T>, AtanNode<T> — unary: out = asin(a), acos(a),
// atan(a), in radians.
//
// Same floating-point-only constraint as ExpNode, same rationale. Unguarded
// at the domain edge: |a| > 1 propagates NaN from AsinNode and AcosNode.
// AtanNode is defined everywhere, and gives ±π/2 for an infinite a.
template<typename T = double, typename = std::enable_if_t<std::is_floating_point_v<T>>>
using AsinNode = UnaryOpNode<T, AsinOp<T>>;

template<typename T = double, typename = std::enable_if_t<std::is_floating_point_v<T>>>
using AcosNode = UnaryOpNode<T, AcosOp<T>>;

template<typename T = double, typename = std::enable_if_t<std::is_floating_point_v<T>>>
using AtanNode = UnaryOpNode<T, AtanOp<T>>;

// Atan2Node<T> — binary: out = atan2(a, b), the angle of the point (b, a), in
// radians, in [−π, π].
//
// Y COMES FIRST, in std::atan2's order: make(name, y, x). A swapped pair is
// the classic atan2 mistake, and it compiles. Same floating-point-only
// constraint as ExpNode, same rationale. Defined everywhere, the origin
// included, where the signs of the two zeros choose ±0 or ±π.
template<typename T = double, typename = std::enable_if_t<std::is_floating_point_v<T>>>
using Atan2Node = BinaryOpNode<T, Atan2Op<T>>;

} // namespace dag::ops

#include "dag_ops.inl"
