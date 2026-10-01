// =============================================================================
// flywheel-dag — A header-only C++17 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

// dag_memoize.inl — implementations for dag_memoize.hpp declarations.
// Included at the bottom of dag_memoize.hpp; never include this file directly.

#pragma once

namespace dag {

// ─────────────────────────────────────────────────────────────────────────────
// Static cache accessor
// ─────────────────────────────────────────────────────────────────────────────

template<typename Out, typename... Ins>
std::unordered_map<std::size_t, Out>&
MemoizedComputeNode<Out, Ins...>::cache() {
    static std::unordered_map<std::size_t, Out> instance;
    return instance;
}

// ─────────────────────────────────────────────────────────────────────────────
// make()
// ─────────────────────────────────────────────────────────────────────────────

template<typename Out, typename... Ins>
std::shared_ptr<MemoizedComputeNode<Out, Ins...>>
MemoizedComputeNode<Out, Ins...>::make(
    std::string name, InputTuple inNodes, Fn fn,
    EqualityPolicyPtr eq)
{
    return make(std::move(name), std::move(inNodes), std::move(fn),
                std::move(eq), InvalidationMode::Eager);
}

template<typename Out, typename... Ins>
std::shared_ptr<MemoizedComputeNode<Out, Ins...>>
MemoizedComputeNode<Out, Ins...>::make(
    std::string name, InputTuple inNodes, Fn fn, InvalidationMode mode)
{
    return make(std::move(name), std::move(inNodes), std::move(fn), nullptr, mode);
}

template<typename Out, typename... Ins>
std::shared_ptr<MemoizedComputeNode<Out, Ins...>>
MemoizedComputeNode<Out, Ins...>::make(
    std::string name, InputTuple inNodes, Fn fn,
    EqualityPolicyPtr eq, InvalidationMode mode)
{
    if (!eq) eq = std::make_shared<TypedEqualityPolicy<Out>>();
    auto self = std::shared_ptr<MemoizedComputeNode>(
        new MemoizedComputeNode(
            std::move(name), std::move(inNodes), std::move(fn), std::move(eq), mode));
    wire(self, self->inputs());
    return self;
}

// ─────────────────────────────────────────────────────────────────────────────
// eval()
// ─────────────────────────────────────────────────────────────────────────────

template<typename Out, typename... Ins>
ValuePtr MemoizedComputeNode<Out, Ins...>::eval(EvalContext& ctx) {
    if (!dirty() && !ctx.forceRecompute) return cached_;
    beginEval();

    // Evaluate all upstream inputs; values are now stable for this cycle.
    // evalInputs COPIES into a tuple, so unlike ComputeNode there is no borrowed
    // reference to keep alive and the resolve check can sit here in eval().
    auto vals = evalInputs(ctx, std::index_sequence_for<Ins...>{});

    // Every input has been pulled and we are still only Maybe: nothing upstream
    // moved, so the hash would land on the same key and the cache on the same
    // value. Skipping saves the hash and the lookup as well as the functor.
    if (skipRecompute(ctx)) { endEval(); return cached_; }

    // Build cache key: hash(node_name, input_values).
    std::size_t key = detail::hash_tuple(
        name_, vals, std::index_sequence_for<Ins...>{});

    Out result;
    auto& c  = cache();
    auto  it = c.find(key);
    if (it != c.end()) {
        ++hits_;
        result = it->second;    // cache hit — functor not called
    } else {
        ++misses_;
        result = std::apply(
            [&](const Ins&... args) { return fn_(args...); }, vals);
        c.emplace(key, result); // store for future lookups
    }

    auto newV = slot_.emit(result);
    if (!eq_->equal(cached_, newV)) {
        cached_ = newV;
        notifyDownstream();
    }
    endEval();
    return cached_;
}

// ─────────────────────────────────────────────────────────────────────────────
// INode interface
// ─────────────────────────────────────────────────────────────────────────────

template<typename Out, typename... Ins>
std::string MemoizedComputeNode<Out, Ins...>::name() const { return name_; }

template<typename Out, typename... Ins>
std::vector<NodePtr> MemoizedComputeNode<Out, Ins...>::inputs() const {
    return collectInputs(std::index_sequence_for<Ins...>{});
}

// ─────────────────────────────────────────────────────────────────────────────
// Cache management
// ─────────────────────────────────────────────────────────────────────────────

template<typename Out, typename... Ins>
void MemoizedComputeNode<Out, Ins...>::clearCache() { cache().clear(); }

template<typename Out, typename... Ins>
std::size_t MemoizedComputeNode<Out, Ins...>::cacheSize() { return cache().size(); }

template<typename Out, typename... Ins>
std::size_t MemoizedComputeNode<Out, Ins...>::cacheHits() const { return hits_; }

template<typename Out, typename... Ins>
std::size_t MemoizedComputeNode<Out, Ins...>::cacheMisses() const { return misses_; }

// ─────────────────────────────────────────────────────────────────────────────
// Private helpers
// ─────────────────────────────────────────────────────────────────────────────

template<typename Out, typename... Ins>
MemoizedComputeNode<Out, Ins...>::MemoizedComputeNode(
    std::string name, InputTuple ins, Fn fn,
    EqualityPolicyPtr eq, InvalidationMode mode)
    : NodeBase(mode), name_(std::move(name)), inputs_(std::move(ins))
    , fn_(std::move(fn)), eq_(std::move(eq)) {}

template<typename Out, typename... Ins>
template<std::size_t... Is>
std::tuple<Ins...> MemoizedComputeNode<Out, Ins...>::evalInputs(
    EvalContext& ctx, std::index_sequence<Is...>)
{
    return { get_value<Ins>(std::get<Is>(inputs_)->eval(ctx))... };
}

template<typename Out, typename... Ins>
template<std::size_t... Is>
std::vector<NodePtr> MemoizedComputeNode<Out, Ins...>::collectInputs(
    std::index_sequence<Is...>) const
{
    return { std::get<Is>(inputs_)... };
}

} // namespace dag
