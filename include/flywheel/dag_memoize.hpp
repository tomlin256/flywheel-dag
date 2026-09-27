// =============================================================================
// flywheel-dag — A header-only C++17 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

// dag_memoize.hpp — MemoizedComputeNode: hash-keyed result caching for pure compute nodes.
// Never include dag_memoize.inl directly — always include this file.

#pragma once
#include "dag.hpp"
#include <unordered_map>

namespace dag {

// ─────────────────────────────────────────────────────────────────────────────
// detail: hash utilities
// ─────────────────────────────────────────────────────────────────────────────

namespace detail {

inline void hash_combine_impl(std::size_t& seed, std::size_t h) {
    seed ^= h + 0x9e3779b9 + (seed << 6) + (seed >> 2);
}

template<typename T>
void hash_combine(std::size_t& seed, const T& v) {
    static_assert(std::is_default_constructible_v<std::hash<T>>,
        "MemoizedComputeNode: all Ins types must be std::hash-able");
    hash_combine_impl(seed, std::hash<T>{}(v));
}

template<typename Tuple, std::size_t... Is>
std::size_t hash_tuple(const std::string& name, const Tuple& t,
                       std::index_sequence<Is...>) {
    std::size_t seed = std::hash<std::string>{}(name);
    (hash_combine(seed, std::get<Is>(t)), ...);
    return seed;
}

} // namespace detail

// ─────────────────────────────────────────────────────────────────────────────
// MemoizedComputeNode<Out, Ins...>
// ─────────────────────────────────────────────────────────────────────────────
//
// A compute node that caches results keyed by hash(node_name, input_values).
// When the same input combination is seen again the functor is skipped and the
// previously computed result is returned directly.
//
// The cache is static per <Out, Ins...> template instantiation and shared
// across all instances. The node name is folded into the hash key so
// differently named nodes do not share entries even with the same type sig.
//
// Constraints:
//   - All Ins types must be std::hash-able.
//   - The functor must be a pure function: same inputs → same output, no
//     observable side effects. This is a documentation constraint only.
//   - NOT thread-safe. eval() and cache management must only be called from
//     the eval thread.
//   - Hash collisions produce silently wrong results (astronomically unlikely
//     with 64-bit hashes over the input domain).
//   - The cache grows without bound. Call clearCache() if memory is a concern.
//   - The node name must uniquely identify the functor being memoized. Two
//     nodes with the same name but different functors will share cache entries
//     and produce incorrect results.

template<typename Out, typename... Ins>
class MemoizedComputeNode
    : public NodeBase
    , public std::enable_shared_from_this<MemoizedComputeNode<Out, Ins...>>
{
public:
    using Fn         = std::function<Out(const Ins&...)>;
    using InputTuple = std::tuple<std::conditional_t<true, NodePtr, Ins>...>;

    static std::shared_ptr<MemoizedComputeNode> make(
        std::string name, InputTuple inNodes, Fn fn,
        EqualityPolicyPtr eq = nullptr);
    static std::shared_ptr<MemoizedComputeNode> make(
        std::string name, InputTuple inNodes, Fn fn, InvalidationMode mode);
    static std::shared_ptr<MemoizedComputeNode> make(
        std::string name, InputTuple inNodes, Fn fn,
        EqualityPolicyPtr eq, InvalidationMode mode);

    // INode
    ValuePtr             eval(EvalContext& ctx) override;
    std::string          name()   const override;
    std::vector<NodePtr> inputs() const override;
    NodeKind             kind()   const override { return NodeKind::Compute; }

    // Cache management — shared per <Out, Ins...> instantiation.
    // NOT thread-safe: call only from the eval thread.
    static void        clearCache();
    static std::size_t cacheSize();

    // Per-instance hit/miss counters for observability.
    std::size_t cacheHits()   const;
    std::size_t cacheMisses() const;

private:
    MemoizedComputeNode(std::string name, InputTuple ins, Fn fn,
                        EqualityPolicyPtr eq, InvalidationMode mode);

    template<std::size_t... Is>
    std::tuple<Ins...>   evalInputs(EvalContext& ctx, std::index_sequence<Is...>);
    template<std::size_t... Is>
    std::vector<NodePtr> collectInputs(std::index_sequence<Is...>) const;

    std::string                       name_;
    InputTuple                        inputs_;
    Fn                                fn_;
    EqualityPolicyPtr  eq_;
    ValuePtr                          cached_;
    ValueSlot<Out>                    slot_;

    mutable std::size_t hits_   = 0;
    mutable std::size_t misses_ = 0;

    // One map per template instantiation. Lazy-initialised via function-static
    // to avoid static initialisation order problems.
    static std::unordered_map<std::size_t, Out>& cache();
};

} // namespace dag

#include "dag_memoize.inl"
