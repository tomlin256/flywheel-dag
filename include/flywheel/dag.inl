// =============================================================================
// flywheel-dag — A header-only C++17 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

// dag.inl — implementation of all dag.hpp declarations
// Included at the bottom of dag.hpp; never include this file directly.

#pragma once

namespace dag {

// ─────────────────────────────────────────────────────────────────────────────
// TypedValue<T>
// ─────────────────────────────────────────────────────────────────────────────

template<typename T>
TypedValue<T>::TypedValue(T v) : IValue(typeid(T)), value_(std::move(v)) {}

template<typename T>
const T& TypedValue<T>::get() const { return value_; }

// ─────────────────────────────────────────────────────────────────────────────
// make_value / get_value
// ─────────────────────────────────────────────────────────────────────────────

template<typename T>
ValuePtr make_value(T v) { return std::make_shared<TypedValue<T>>(std::move(v)); }

// Hot path: called once per input per node per cycle. The dynamic_cast this
// replaced walked the RTTI hierarchy on every one of those reads (~15% of the
// eval cycle). TypedValue<T> is final and is the only IValue implementation, so
// an exact type_index compare accepts and rejects exactly the same values the
// cast did — including both throw paths.
template<typename T>
const T& get_value(const ValuePtr& v) {
    if (!v) throw std::runtime_error("Null value");
    if (v->type() != std::type_index(typeid(T))) throw std::bad_cast();
    return static_cast<const TypedValue<T>*>(v.get())->get();
}

// ─────────────────────────────────────────────────────────────────────────────
// AlwaysChangedPolicy
// ─────────────────────────────────────────────────────────────────────────────

inline bool AlwaysChangedPolicy::equal(const ValuePtr&, const ValuePtr&) const { return false; }

// ─────────────────────────────────────────────────────────────────────────────
// TypedEqualityPolicy<T>
// ─────────────────────────────────────────────────────────────────────────────

template<typename T>
bool TypedEqualityPolicy<T>::equal(const ValuePtr& a, const ValuePtr& b) const {
    if (!a && !b) return true;
    if (!a || !b) return false;
    try { return get_value<T>(a) == get_value<T>(b); }
    catch (...) { return false; }
}

// ─────────────────────────────────────────────────────────────────────────────
// PredicateEqualityPolicy
// ─────────────────────────────────────────────────────────────────────────────

inline PredicateEqualityPolicy::PredicateEqualityPolicy(Pred p) : pred_(std::move(p)) {}

inline bool PredicateEqualityPolicy::equal(const ValuePtr& a, const ValuePtr& b) const {
    return pred_(a, b);
}

// ─────────────────────────────────────────────────────────────────────────────
// EpsilonPolicy<T>
// ─────────────────────────────────────────────────────────────────────────────

template<typename T, typename E>
EpsilonPolicy<T, E>::EpsilonPolicy(T e) : eps_(e) {}

template<typename T, typename E>
bool EpsilonPolicy<T, E>::equal(const ValuePtr& a, const ValuePtr& b) const {
    if (!a || !b) return false;
    try { return std::abs(get_value<T>(a) - get_value<T>(b)) < eps_; }
    catch (...) { return false; }
}

// ─────────────────────────────────────────────────────────────────────────────
// wire()
// ─────────────────────────────────────────────────────────────────────────────

inline void wire(const NodePtr& self, const std::vector<NodePtr>& ins) {
    for (const auto& in : ins) in->addDownstream(self);
}

// ─────────────────────────────────────────────────────────────────────────────
// Input<T>
// ─────────────────────────────────────────────────────────────────────────────

template<typename T>
std::shared_ptr<Input<T>> Input<T>::make(
    std::string name, T initial,
    EqualityPolicyPtr eq)
{
    if (!eq) eq = std::make_shared<TypedEqualityPolicy<T>>();
    return std::shared_ptr<Input<T>>(
        new Input<T>(std::move(name), std::move(initial), std::move(eq)));
}

template<typename T>
void Input<T>::set(T newVal) {
    auto newV = make_value(std::move(newVal));
    if (!eq_->equal(value_, newV)) {
        value_ = std::move(newV);
        markDirty();
        notifyDownstream();
        if (wakeHook_) wakeHook_();
    }
}

template<typename T>
const T& Input<T>::get() const { return get_value<T>(value_); }

template<typename T>
void Input<T>::setWakeHook(std::function<void()> hook) { wakeHook_ = std::move(hook); }

template<typename T>
ValuePtr Input<T>::eval(EvalContext&) { markClean(); return value_; }

template<typename T>
std::string Input<T>::name() const { return name_; }

template<typename T>
std::vector<NodePtr> Input<T>::inputs() const { return {}; }

template<typename T>
Input<T>::Input(std::string n, T initial, EqualityPolicyPtr eq)
    : name_(std::move(n)), value_(make_value(std::move(initial)))
    , eq_(std::move(eq)) {}

// ─────────────────────────────────────────────────────────────────────────────
// ComputeNode<Out, Ins...>
// ─────────────────────────────────────────────────────────────────────────────

template<typename Out, typename... Ins>
ComputeNodePtr<Out, Ins...> ComputeNode<Out, Ins...>::make(
    std::string name, InputTuple inNodes, Fn fn,
    EqualityPolicyPtr eq)
{
    return make(std::move(name), std::move(inNodes), std::move(fn),
                std::move(eq), InvalidationMode::Eager);
}

template<typename Out, typename... Ins>
ComputeNodePtr<Out, Ins...> ComputeNode<Out, Ins...>::make(
    std::string name, InputTuple inNodes, Fn fn, InvalidationMode mode)
{
    return make(std::move(name), std::move(inNodes), std::move(fn), nullptr, mode);
}

template<typename Out, typename... Ins>
ComputeNodePtr<Out, Ins...> ComputeNode<Out, Ins...>::make(
    std::string name, InputTuple inNodes, Fn fn,
    EqualityPolicyPtr eq, InvalidationMode mode)
{
    if (!eq) eq = std::make_shared<TypedEqualityPolicy<Out>>();
    auto self = std::shared_ptr<ComputeNode>(
        new ComputeNode(std::move(name), std::move(inNodes), std::move(fn),
                        std::move(eq), mode));
    wire(self, self->inputs());
    return self;
}

template<typename Out, typename... Ins>
ValuePtr ComputeNode<Out, Ins...>::eval(EvalContext& ctx) {
    if (!dirty() && !ctx.forceRecompute) return cached_;
    return applyInputs(ctx, std::index_sequence_for<Ins...>{});
}

template<typename Out, typename... Ins>
ValuePtr ComputeNode<Out, Ins...>::publish(Out&& result) {
    auto newV = slot_.emit(std::move(result));
    if (!eq_->equal(cached_, newV)) {
        cached_ = newV;
        notifyDownstream();
    }
    markClean();
    return cached_;
}

template<typename Out, typename... Ins>
std::string ComputeNode<Out, Ins...>::name() const { return name_; }

template<typename Out, typename... Ins>
std::vector<NodePtr> ComputeNode<Out, Ins...>::inputs() const {
    return collectInputs(std::index_sequence_for<Ins...>{});
}

template<typename Out, typename... Ins>
ComputeNode<Out, Ins...>::ComputeNode(
    std::string name, InputTuple ins, Fn fn, EqualityPolicyPtr eq,
    InvalidationMode mode)
    : NodeBase(mode), name_(std::move(name)), inputs_(std::move(ins))
    , fn_(std::move(fn)), eq_(std::move(eq)) {}

// Two paths, chosen at compile time by how expensive the input types are to copy.
//
// The old code always built a std::tuple<Ins...> from the get_value() references,
// which copies every input by value. For a double that is free; for a
// std::vector batch or a std::deque window it is a deep copy of the whole
// container on every cycle — and the same batch is copied once per consuming node.
//
// Binding references instead needs the referenced values to stay alive AND stay
// unmodified for the duration of the call. The `held` array does that: it keeps
// each TypedValue alive and pushes its use_count above 1, so no ValueSlot can
// recycle a buffer a reference points into. That last part matters when
// ctx.forceRecompute is set and two inputs share an upstream node — evaluating
// the second input re-runs the first input's producer, which then emits into a
// different buffer precisely because the first one is still referenced.
//
// The array is DEFENSIVE, not load-bearing: written as one expression,
// `fn_(get_value<Ins>(in->eval(ctx))...)` is already correct, because C++17
// keeps the eval() temporaries alive to the end of the full-expression, which
// includes the fn_ call. Verified — removing the array leaves every test green.
// It is kept because that guarantee evaporates the moment someone splits this
// into separate statements, and the array makes the requirement visible at the
// point where it has to hold.
//
// Trivially-copyable inputs keep taking the copy: a shared_ptr costs an atomic
// refcount bump, which is not worth paying to avoid copying a double.
//
// WHY THE RESOLVE CHECK LIVES IN HERE AND NOT IN eval(). On the reference-binding
// path the `held` array is what keeps the pulled values alive, so the decision to
// skip has to be made while it is still in scope. Splitting "pull" from "call"
// into two functions would either dangle those references or force an
// Out-shaped return slot on the skip path; keeping the whole tail here costs two
// duplicated lines and no abstraction.
template<typename Out, typename... Ins>
template<std::size_t... Is>
ValuePtr ComputeNode<Out, Ins...>::applyInputs(
    EvalContext& ctx, std::index_sequence<Is...>)
{
    if constexpr ((std::is_trivially_copyable_v<Ins> && ...)) {
        // Cheap to copy, and no atomics. Braced init guarantees left-to-right
        // evaluation, so each value is copied out before the next input runs.
        const std::tuple<Ins...> vals{
            get_value<Ins>(std::get<Is>(inputs_)->eval(ctx))... };
        // Every input has now been pulled, so any of them that really changed
        // has already called our invalidate(). Still only Maybe means nothing
        // moved.
        if (skipRecompute(ctx)) { markClean(); return cached_; }
        return publish(fn_(std::get<Is>(vals)...));
    } else {
        const ValuePtr held[] = { std::get<Is>(inputs_)->eval(ctx)... };
        if (skipRecompute(ctx)) { markClean(); return cached_; }
        return publish(fn_(get_value<Ins>(held[Is])...));
    }
}

template<typename Out, typename... Ins>
template<std::size_t... Is>
std::vector<NodePtr> ComputeNode<Out, Ins...>::collectInputs(
    std::index_sequence<Is...>) const
{
    return { std::get<Is>(inputs_)... };
}

// ─────────────────────────────────────────────────────────────────────────────
// InPlaceComputeNode<Out, Ins...>
// ─────────────────────────────────────────────────────────────────────────────

template<typename Out, typename... Ins>
std::shared_ptr<InPlaceComputeNode<Out, Ins...>> InPlaceComputeNode<Out, Ins...>::make(
    std::string name, InputTuple inNodes, Fn fn,
    EqualityPolicyPtr eq)
{
    return make(std::move(name), std::move(inNodes), std::move(fn),
                std::move(eq), InvalidationMode::Eager);
}

template<typename Out, typename... Ins>
std::shared_ptr<InPlaceComputeNode<Out, Ins...>> InPlaceComputeNode<Out, Ins...>::make(
    std::string name, InputTuple inNodes, Fn fn, InvalidationMode mode)
{
    return make(std::move(name), std::move(inNodes), std::move(fn), nullptr, mode);
}

template<typename Out, typename... Ins>
std::shared_ptr<InPlaceComputeNode<Out, Ins...>> InPlaceComputeNode<Out, Ins...>::make(
    std::string name, InputTuple inNodes, Fn fn,
    EqualityPolicyPtr eq, InvalidationMode mode)
{
    if (!eq) eq = std::make_shared<TypedEqualityPolicy<Out>>();
    auto self = std::shared_ptr<InPlaceComputeNode>(
        new InPlaceComputeNode(std::move(name), std::move(inNodes),
                               std::move(fn), std::move(eq), mode));
    wire(self, self->inputs());
    return self;
}

// The one line that differs from ComputeNode::eval, and the reason this class
// exists: emit takes a const lvalue, so ValueSlot copy-ASSIGNS scratch_ into
// the recycled TypedValue buffer. std::vector::operator= reuses the
// destination's storage whenever its capacity suffices, so neither the buffer
// nor scratch_ gives its allocation back. ComputeNode's emit(std::move(result))
// frees the buffer's storage and installs the temporary's instead, which is
// exactly the allocation this avoids.
template<typename Out, typename... Ins>
ValuePtr InPlaceComputeNode<Out, Ins...>::eval(EvalContext& ctx) {
    if (!dirty() && !ctx.forceRecompute) return cached_;
    return applyInputs(ctx, std::index_sequence_for<Ins...>{});
}

template<typename Out, typename... Ins>
ValuePtr InPlaceComputeNode<Out, Ins...>::publish() {
    auto newV = slot_.emit(std::as_const(scratch_));
    if (!eq_->equal(cached_, newV)) {
        cached_ = newV;
        notifyDownstream();
    }
    markClean();
    return cached_;
}

template<typename Out, typename... Ins>
std::string InPlaceComputeNode<Out, Ins...>::name() const { return name_; }

template<typename Out, typename... Ins>
std::vector<NodePtr> InPlaceComputeNode<Out, Ins...>::inputs() const {
    return collectInputs(std::index_sequence_for<Ins...>{});
}

template<typename Out, typename... Ins>
InPlaceComputeNode<Out, Ins...>::InPlaceComputeNode(
    std::string name, InputTuple ins, Fn fn, EqualityPolicyPtr eq,
    InvalidationMode mode)
    : NodeBase(mode), name_(std::move(name)), inputs_(std::move(ins))
    , fn_(std::move(fn)), eq_(std::move(eq)) {}

// Same two paths as ComputeNode::applyInputs, chosen the same way — see the
// long note on that definition for why the `held` array is there and why
// trivially-copyable inputs are copied instead.
template<typename Out, typename... Ins>
template<std::size_t... Is>
ValuePtr InPlaceComputeNode<Out, Ins...>::applyInputs(
    EvalContext& ctx, std::index_sequence<Is...>)
{
    if constexpr ((std::is_trivially_copyable_v<Ins> && ...)) {
        const std::tuple<Ins...> vals{
            get_value<Ins>(std::get<Is>(inputs_)->eval(ctx))... };
        // Skipping leaves scratch_ holding the previous evaluation's value,
        // which is exactly what cached_ already points at — the class contract
        // (the functor must overwrite everything it owns) is about the functor
        // RUNNING, and it did not run.
        if (skipRecompute(ctx)) { markClean(); return cached_; }
        fn_(scratch_, std::get<Is>(vals)...);
    } else {
        const ValuePtr held[] = { std::get<Is>(inputs_)->eval(ctx)... };
        if (skipRecompute(ctx)) { markClean(); return cached_; }
        fn_(scratch_, get_value<Ins>(held[Is])...);
    }
    return publish();
}

template<typename Out, typename... Ins>
template<std::size_t... Is>
std::vector<NodePtr> InPlaceComputeNode<Out, Ins...>::collectInputs(
    std::index_sequence<Is...>) const
{
    return { std::get<Is>(inputs_)... };
}

// ─────────────────────────────────────────────────────────────────────────────
// aad::Partials
// ─────────────────────────────────────────────────────────────────────────────

inline void aad::Partials::add(std::size_t input, double d) {
    entries_.push_back({input, d});
}

inline const std::vector<aad::Partials::Entry>& aad::Partials::entries() const noexcept {
    return entries_;
}

inline void aad::Partials::clear() noexcept { entries_.clear(); }

// ─────────────────────────────────────────────────────────────────────────────
// TweakableComputeNode<Out, Ins...>
// ─────────────────────────────────────────────────────────────────────────────

template<typename Out, typename... Ins>
std::shared_ptr<TweakableComputeNode<Out, Ins...>> TweakableComputeNode<Out, Ins...>::make(
    std::string name, InputTuple inNodes, Fn fn,
    EqualityPolicyPtr eq)
{
    return make(std::move(name), std::move(inNodes), std::move(fn),
                std::move(eq), InvalidationMode::Eager);
}

template<typename Out, typename... Ins>
std::shared_ptr<TweakableComputeNode<Out, Ins...>> TweakableComputeNode<Out, Ins...>::make(
    std::string name, InputTuple inNodes, Fn fn, InvalidationMode mode)
{
    return make(std::move(name), std::move(inNodes), std::move(fn), nullptr, mode);
}

template<typename Out, typename... Ins>
std::shared_ptr<TweakableComputeNode<Out, Ins...>> TweakableComputeNode<Out, Ins...>::make(
    std::string name, InputTuple inNodes, Fn fn,
    EqualityPolicyPtr eq, InvalidationMode mode)
{
    if (!eq) eq = std::make_shared<TypedEqualityPolicy<Out>>();
    auto self = std::shared_ptr<TweakableComputeNode>(
        new TweakableComputeNode(
            std::move(name), std::move(inNodes), std::move(fn), std::move(eq), mode));
    wire(self, self->inputs());
    return self;
}

template<typename Out, typename... Ins>
ValuePtr TweakableComputeNode<Out, Ins...>::eval(EvalContext& ctx) {
    if (tweaked_) {
        // Return the frozen value without touching the inputs. Marking clean
        // completes the delivery of a new tweak, which tweak() leaves dirty for
        // the engine to collect.
        markClean();
        return cached_;
    }
    if (!dirty() && !ctx.forceRecompute) return cached_;

    // Normal computation path.
    return applyInputs(ctx, std::index_sequence_for<Ins...>{});
}

template<typename Out, typename... Ins>
ValuePtr TweakableComputeNode<Out, Ins...>::publish(Out&& result) {
    auto newV = slot_.emit(std::move(result));
    if (!eq_->equal(cached_, newV)) {
        cached_ = newV;
        notifyDownstream();
    }
    markClean();
    return cached_;
}

template<typename Out, typename... Ins>
void TweakableComputeNode<Out, Ins...>::propagate(Dirtiness incoming) {
    if (tweaked_) return;   // ← absorb while frozen, on BOTH entry points
    NodeBase::propagate(incoming);
}

template<typename Out, typename... Ins>
std::string TweakableComputeNode<Out, Ins...>::name() const { return name_; }

template<typename Out, typename... Ins>
std::vector<NodePtr> TweakableComputeNode<Out, Ins...>::inputs() const {
    return collectInputs(std::index_sequence_for<Ins...>{});
}

template<typename Out, typename... Ins>
void TweakableComputeNode<Out, Ins...>::tweak(Out val) {
    auto newV = make_value(std::move(val));
    tweaked_  = true;

    // An equal tweak only freezes. cached_ keeps its identity, because the
    // engine detects change by pointer identity and a new pointer for the same
    // value reads as a change (flywheel-dag#5, the contract flywheel-dag#1 set
    // for stateful nodes). The state is left alone too. A delivery still
    // pending from an earlier tweak must survive, and an input change pending
    // from before the freeze is harmless: eval() now returns the frozen value.
    if (eq_->equal(cached_, newV)) return;

    // A new value is published as an evaluation would publish it. Downstream is
    // notified. This node stays dirty until it is evaluated, which propagate()
    // preserves by absorbing everything while frozen, so the engine evaluates
    // it once more and delivers the tweak to its own output. Left clean, it was
    // never evaluated again while frozen, and its callback never saw the tweak.
    cached_ = std::move(newV);
    markDirty();
    notifyDownstream();
}

template<typename Out, typename... Ins>
void TweakableComputeNode<Out, Ins...>::clearTweak() {
    if (!tweaked_) return;
    tweaked_ = false;
    // The frozen value no longer applies, but whether the recomputed one
    // differs is not known until the node recomputes. invalidate() says exactly
    // that (flywheel-dag#8). This node goes Dirty, so it recomputes, even when
    // it is Lazy, and its consumers go Maybe. Its eval() then tells them Dirty
    // only if the value moved, and a Lazy consumer skips when it did not.
    // Marking them Dirty here made every Lazy consumer rerun regardless. If a
    // changed tweak is still pending, the node is already Dirty, and the tweak
    // has already told its consumers.
    invalidate();
}

template<typename Out, typename... Ins>
bool TweakableComputeNode<Out, Ins...>::isTweaked() const { return tweaked_; }

template<typename Out, typename... Ins>
std::optional<Out> TweakableComputeNode<Out, Ins...>::tweakValue() const {
    if (!tweaked_) return std::nullopt;
    return get_value<Out>(cached_);
}

// A frozen value depends on no input, so it names none, and a pass stops here
// without reading the inputs the freeze is ignoring. Those inputs can be dirty:
// propagate() absorbs their invalidations while frozen. Untweaked, the functor
// is opaque, so the node cannot say and is a barrier.
template<typename Out, typename... Ins>
bool TweakableComputeNode<Out, Ins...>::partials(EvalContext&, aad::Partials&) {
    return tweaked_;
}

template<typename Out, typename... Ins>
TweakableComputeNode<Out, Ins...>::TweakableComputeNode(
    std::string name, InputTuple ins, Fn fn,
    EqualityPolicyPtr eq, InvalidationMode mode)
    : NodeBase(mode), name_(std::move(name)), inputs_(std::move(ins))
    , fn_(std::move(fn)), eq_(std::move(eq)) {}

// See ComputeNode::applyInputs for why the two paths differ, and why the resolve
// check lives here rather than in eval().
template<typename Out, typename... Ins>
template<std::size_t... Is>
ValuePtr TweakableComputeNode<Out, Ins...>::applyInputs(
    EvalContext& ctx, std::index_sequence<Is...>)
{
    if constexpr ((std::is_trivially_copyable_v<Ins> && ...)) {
        const std::tuple<Ins...> vals{
            get_value<Ins>(std::get<Is>(inputs_)->eval(ctx))... };
        if (skipRecompute(ctx)) { markClean(); return cached_; }
        return publish(fn_(std::get<Is>(vals)...));
    } else {
        const ValuePtr held[] = { std::get<Is>(inputs_)->eval(ctx)... };
        if (skipRecompute(ctx)) { markClean(); return cached_; }
        return publish(fn_(get_value<Ins>(held[Is])...));
    }
}

template<typename Out, typename... Ins>
template<std::size_t... Is>
std::vector<NodePtr> TweakableComputeNode<Out, Ins...>::collectInputs(
    std::index_sequence<Is...>) const
{
    return { std::get<Is>(inputs_)... };
}

// ─────────────────────────────────────────────────────────────────────────────
// ConditionNode
// ─────────────────────────────────────────────────────────────────────────────

inline std::shared_ptr<ConditionNode> ConditionNode::make(
    std::string name, NodePtr condition, NodePtr trueBranch, NodePtr falseBranch,
    EqualityPolicyPtr eq)
{
    if (!eq) eq = std::make_shared<AlwaysChangedPolicy>();
    auto self = std::shared_ptr<ConditionNode>(
        new ConditionNode(std::move(name), condition, trueBranch, falseBranch,
                          std::move(eq)));
    // Not wire(): the condition reaches the node itself, but each branch reaches
    // it through its listener, which drops what the branch not taken says.
    self->onTrue_  = std::make_shared<BranchListener>(self, true);
    self->onFalse_ = std::make_shared<BranchListener>(self, false);
    condition->addDownstream(self);
    trueBranch->addDownstream(self->onTrue_);
    falseBranch->addDownstream(self->onFalse_);
    return self;
}

// The one node that does NOT pull every declared input: only the taken branch is
// evaluated, which is the property dag_timeseries.hpp opens by relying on
// ("un-observed branches never compute"). So an untaken branch can be left
// stale, and a later switch to it can arrive Dirty when nothing about the branch
// itself moved. That is a spurious recompute, never a stale value — the branch
// is pulled before its value is used — and it is asserted as such in
// test_lazy_invalidation.cpp.
//
// taken_ is set before the branch is pulled, so that a branch which changes as
// it is pulled is heard, as any input's change is. taken_ is out of date only
// once the condition has published a new value, which made this node Dirty, and
// a Dirty node drops whatever a listener passes on.
inline ValuePtr ConditionNode::eval(EvalContext& ctx) {
    if (!dirty() && !ctx.forceRecompute) return cached_;
    const bool cond = get_value<bool>(condition_->eval(ctx));
    taken_ = cond;
    const ValuePtr newV = cond ? trueBranch_->eval(ctx) : falseBranch_->eval(ctx);
    if (skipRecompute(ctx)) { markClean(); return cached_; }
    if (!eq_->equal(cached_, newV)) {
        cached_ = newV;
        notifyDownstream();
    }
    markClean();
    return cached_;
}

inline std::string ConditionNode::name() const { return name_; }

inline std::vector<NodePtr> ConditionNode::inputs() const {
    return { condition_, trueBranch_, falseBranch_ };
}

// The derivative of a selection is the derivative of the branch it took. The
// condition is a bool, so it has no partial, and the other branch gets no entry,
// so a pass never reads it. That is the property eval() keeps: an untaken branch
// may be stale, and reading it would evaluate it. The condition is pulled only
// to learn which branch was taken. The node is clean when a pass asks, so the
// pull returns the value its last eval() chose by.
inline bool ConditionNode::partials(EvalContext& ctx, aad::Partials& out) {
    const bool cond = get_value<bool>(condition_->eval(ctx));
    out.add(cond ? 1 : 2, 1.0);
    return true;
}

// Lazy, fixed in the class: this is pure selection over three inputs, with no
// functor at all for an author to get wrong. The one asymmetry — only the taken
// branch is pulled — predates lazy invalidation and is what
// dag_timeseries.hpp's "un-observed branches never compute" relies on. It can
// cost a spurious recompute after a switch, because the branch is evaluated on
// the way past (test_lazy_invalidation.cpp Case6). Its consumers never see a
// stale value from it, because the node does not hear the branch it did not
// take (flywheel-dag#18).
inline ConditionNode::ConditionNode(
    std::string name, NodePtr cond, NodePtr tb, NodePtr fb,
    EqualityPolicyPtr eq)
    : NodeBase(InvalidationMode::Lazy), name_(std::move(name)), condition_(cond)
    , trueBranch_(tb), falseBranch_(fb), eq_(std::move(eq)) {}

inline ConditionNode::BranchListener::BranchListener(
    std::weak_ptr<ConditionNode> owner, bool branch)
    : owner_(std::move(owner)), branch_(branch) {}

inline ValuePtr ConditionNode::BranchListener::eval(EvalContext&) {
    throw std::logic_error("ConditionNode: " + name() + " is not a node and has no value");
}

inline std::string ConditionNode::BranchListener::name() const {
    const auto owner = owner_.lock();
    return (owner ? owner->name() : std::string("a released node"))
         + (branch_ ? "'s true-branch listener" : "'s false-branch listener");
}

inline std::vector<NodePtr> ConditionNode::BranchListener::inputs() const { return {}; }

// The same kind passes on: a branch's "changed" is the node's "an input of mine
// changed", and its "maybe" the node's "maybe". The node's own state then does
// what it does for any input.
inline void ConditionNode::BranchListener::propagate(Dirtiness incoming) {
    const auto owner = owner_.lock();
    if (!owner || owner->taken_ != branch_) return;
    if (incoming == Dirtiness::Dirty) owner->invalidate();
    else                              owner->invalidateMaybe();
}

// ─────────────────────────────────────────────────────────────────────────────
// Graph
// ─────────────────────────────────────────────────────────────────────────────

inline void Graph::addNode(NodePtr node) { nodes_.push_back(std::move(node)); }
inline ValuePtr Graph::eval(const NodePtr& root) { EvalContext ctx; return root->eval(ctx); }
inline ValuePtr Graph::eval(const NodePtr& root, EvalContext ctx) { return root->eval(ctx); }

} // namespace dag
