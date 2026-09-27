// =============================================================================
// flywheel-dag — A header-only C++17 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

#pragma once
// dag_ops.hpp — basic arithmetic op primitives built on dag.hpp
//
// Each op below is its own concrete C++ type — not a generic ComputeNode
// wrapping an opaque lambda — so that inputs() plus the op's identity are
// enough for a future pass (e.g. reverse-mode AAD) to attach a closed-form
// local derivative without redesigning these primitives. This file builds
// forward evaluation only: no tape, no backward pass.
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
// ─────────────────────────────────────────────────────────────────────────────
template<typename Derived>
class OpNodeImpl : public NodeBase, public std::enable_shared_from_this<Derived> {
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

} // namespace dag::ops

#include "dag_ops.inl"
