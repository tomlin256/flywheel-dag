// =============================================================================
// flywheel-dag — A header-only C++17 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

// dag_ops.inl — implementation of all dag_ops.hpp declarations
// Included at the bottom of dag_ops.hpp; never include this file directly.

#pragma once

namespace dag::ops {

// ─────────────────────────────────────────────────────────────────────────────
// OpNodeImpl<Derived>
// ─────────────────────────────────────────────────────────────────────────────

template<typename Derived>
void OpNodeImpl<Derived>::notifyDownstream(const ValuePtr& newV, const EqualityPolicyPtr& eq) {
    if (!eq->equal(cached_, newV)) {
        cached_ = newV;
        this->NodeBase::notifyDownstream();
    }
    this->markClean();
}

// ─────────────────────────────────────────────────────────────────────────────
// UnaryOpNode<T, Op>
// ─────────────────────────────────────────────────────────────────────────────

template<typename T, typename Op>
std::shared_ptr<UnaryOpNode<T,Op>> UnaryOpNode<T,Op>::make(
    std::string name, NodePtr a, EqualityPolicyPtr eq)
{
    if (!eq) eq = std::make_shared<TypedEqualityPolicy<T>>();
    auto self = std::shared_ptr<UnaryOpNode<T,Op>>(
        new UnaryOpNode<T,Op>(std::move(name), std::move(a), std::move(eq)));
    wire(self, self->inputs());
    return self;
}

template<typename T, typename Op>
UnaryOpNode<T,Op>::UnaryOpNode(std::string name, NodePtr a, EqualityPolicyPtr eq)
    : name_(std::move(name)), a_(std::move(a)), eq_(std::move(eq)) {}

template<typename T, typename Op>
ValuePtr UnaryOpNode<T,Op>::eval(EvalContext& ctx) {
    if (!this->dirty() && !ctx.forceRecompute) return this->cached_;
    // Hold the pulled value: the resolve check has to happen after the pull, and
    // the ValuePtr is what keeps the producer's ValueSlot buffer from being
    // recycled underneath the get_value() below.
    const ValuePtr av = a_->eval(ctx);
    if (this->skipRecompute(ctx)) { this->markClean(); return this->cached_; }
    T result = Op{}(get_value<T>(av));
    this->notifyDownstream(slot_.emit(std::move(result)), eq_);
    return this->cached_;
}

template<typename T, typename Op>
std::string UnaryOpNode<T,Op>::name() const { return name_; }

template<typename T, typename Op>
std::vector<NodePtr> UnaryOpNode<T,Op>::inputs() const { return {a_}; }

// ─────────────────────────────────────────────────────────────────────────────
// BinaryOpNode<T, Op>
// ─────────────────────────────────────────────────────────────────────────────

template<typename T, typename Op>
std::shared_ptr<BinaryOpNode<T,Op>> BinaryOpNode<T,Op>::make(
    std::string name, NodePtr a, NodePtr b, EqualityPolicyPtr eq)
{
    if (!eq) eq = std::make_shared<TypedEqualityPolicy<T>>();
    auto self = std::shared_ptr<BinaryOpNode<T,Op>>(
        new BinaryOpNode<T,Op>(std::move(name), std::move(a), std::move(b),
                               std::move(eq)));
    wire(self, self->inputs());
    return self;
}

template<typename T, typename Op>
BinaryOpNode<T,Op>::BinaryOpNode(std::string name, NodePtr a, NodePtr b,
                                 EqualityPolicyPtr eq)
    : name_(std::move(name)), a_(std::move(a)), b_(std::move(b)), eq_(std::move(eq)) {}

template<typename T, typename Op>
ValuePtr BinaryOpNode<T,Op>::eval(EvalContext& ctx) {
    if (!this->dirty() && !ctx.forceRecompute) return this->cached_;
    const ValuePtr av = a_->eval(ctx);
    const ValuePtr bv = b_->eval(ctx);
    if (this->skipRecompute(ctx)) { this->markClean(); return this->cached_; }
    T result = Op{}(get_value<T>(av), get_value<T>(bv));
    this->notifyDownstream(slot_.emit(std::move(result)), eq_);
    return this->cached_;
}

template<typename T, typename Op>
std::string BinaryOpNode<T,Op>::name() const { return name_; }

template<typename T, typename Op>
std::vector<NodePtr> BinaryOpNode<T,Op>::inputs() const { return {a_, b_}; }

// ─────────────────────────────────────────────────────────────────────────────
// NAryOpNode<T, Op>
// ─────────────────────────────────────────────────────────────────────────────

template<typename T, typename Op>
std::shared_ptr<NAryOpNode<T,Op>> NAryOpNode<T,Op>::make(
    std::string name, std::vector<NodePtr> ins, EqualityPolicyPtr eq)
{
    if (!eq) eq = std::make_shared<TypedEqualityPolicy<T>>();
    auto self = std::shared_ptr<NAryOpNode<T,Op>>(
        new NAryOpNode<T,Op>(std::move(name), std::move(ins), std::move(eq)));
    wire(self, self->inputs());
    return self;
}

template<typename T, typename Op>
NAryOpNode<T,Op>::NAryOpNode(std::string name, std::vector<NodePtr> ins,
                             EqualityPolicyPtr eq)
    : name_(std::move(name)), inputs_(std::move(ins)), eq_(std::move(eq)) {}

// The n-ary node is the one place the resolve check costs something, and only
// when it is Lazy. Its pulls and its fold are the same loop, so there is no
// point between "every input pulled" and "the functor has run" to check at —
// hence the separate pull pass. Two things keep the cost where it belongs:
//
//   • an Eager node never enters it, so it pays one enum compare, the same as
//     every other node;
//   • a Lazy node that does NOT skip re-pulls each input, but every one of them
//     is Clean by then, so the second pull is a virtual call returning cached_.
//
// The alternative — retaining the pulled ValuePtrs in a member vector — is
// ruled out: it pins a ValueSlot buffer and pushes the producer back into
// allocating.
template<typename T, typename Op>
ValuePtr NAryOpNode<T,Op>::eval(EvalContext& ctx) {
    if (!this->dirty() && !ctx.forceRecompute) return this->cached_;
    if (!ctx.forceRecompute) {
        for (auto& in : inputs_) in->eval(ctx);
        if (this->skipRecompute(ctx)) { this->markClean(); return this->cached_; }
    }
    T total = Op::identity();
    Op op{};
    for (auto& in : inputs_) total = op(total, get_value<T>(in->eval(ctx)));
    this->notifyDownstream(slot_.emit(std::move(total)), eq_);
    return this->cached_;
}

template<typename T, typename Op>
std::string NAryOpNode<T,Op>::name() const { return name_; }

template<typename T, typename Op>
std::vector<NodePtr> NAryOpNode<T,Op>::inputs() const { return inputs_; }

} // namespace dag::ops
