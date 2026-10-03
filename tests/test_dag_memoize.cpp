// =============================================================================
// flywheel-dag — A header-only C++17 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

#include <gtest/gtest.h>
#include "flywheel/dag_memoize.hpp"

using namespace dag;

template<typename T>
NodePtr asNode(const std::shared_ptr<T>& n) {
    return std::static_pointer_cast<INode>(n);
}

template<typename Node>
double evalDouble(const std::shared_ptr<Node>& n) {
    EvalContext ctx;
    return get_value<double>(n->eval(ctx));
}

// The cache is static per instantiation, so each test clears it first.

// ─────────────────────────────────────────────────────────────────────────────
// Basic
// ─────────────────────────────────────────────────────────────────────────────

TEST(Basic, FunctorCalledOnFirstEval) {
    MemoizedComputeNode<double, double>::clearCache();

    auto inp = Input<double>::make("x", 3.0);
    int calls = 0;
    auto node = MemoizedComputeNode<double, double>::make(
        "scale",
        std::make_tuple(asNode(inp)),
        [&](const double& x) { ++calls; return x * 2.0; });

    EXPECT_EQ(evalDouble(node), 6.0);
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(node->cacheMisses(), 1u);
    EXPECT_EQ(node->cacheHits(),   0u);
}

TEST(Basic, CacheHitOnRepeatInputs) {
    MemoizedComputeNode<double, double>::clearCache();

    auto inp = Input<double>::make("x", 4.0);
    int calls = 0;
    auto node = MemoizedComputeNode<double, double>::make(
        "scale",
        std::make_tuple(asNode(inp)),
        [&](const double& x) { ++calls; return x * 2.0; });

    // First eval — miss.
    EXPECT_EQ(evalDouble(node), 8.0);
    EXPECT_EQ(calls, 1);

    // Drive input through a different value then back to 4.0.
    inp->set(99.0);
    EXPECT_EQ(evalDouble(node), 198.0);
    EXPECT_EQ(calls, 2);

    // Back to 4.0 — cache hit, functor not called again.
    inp->set(4.0);
    EXPECT_EQ(evalDouble(node), 8.0);
    EXPECT_EQ(calls, 2);
    EXPECT_EQ(node->cacheHits(),   1u);
    EXPECT_EQ(node->cacheMisses(), 2u);
}

TEST(Basic, CacheMissOnDifferentInputs) {
    MemoizedComputeNode<double, double>::clearCache();

    auto inp = Input<double>::make("x", 1.0);
    int calls = 0;
    auto node = MemoizedComputeNode<double, double>::make(
        "scale",
        std::make_tuple(asNode(inp)),
        [&](const double& x) { ++calls; return x * 10.0; });

    evalDouble(node);           // first miss
    inp->set(2.0);
    evalDouble(node);           // second miss

    EXPECT_EQ(calls, 2);
    EXPECT_EQ(node->cacheMisses(), 2u);
    EXPECT_EQ(node->cacheHits(),   0u);
}

TEST(Basic, LazyPathSkipsHashLookup) {
    MemoizedComputeNode<double, double>::clearCache();

    auto inp = Input<double>::make("x", 5.0);
    int calls = 0;
    auto node = MemoizedComputeNode<double, double>::make(
        "scale",
        std::make_tuple(asNode(inp)),
        [&](const double& x) { ++calls; return x; });

    evalDouble(node);   // miss; the node is now clean

    // A clean node returns cached_ before it hashes: no hit, no miss.
    std::size_t hitsBefore   = node->cacheHits();
    std::size_t missesBefore = node->cacheMisses();
    evalDouble(node);

    EXPECT_EQ(node->cacheHits(),   hitsBefore);
    EXPECT_EQ(node->cacheMisses(), missesBefore);
    EXPECT_EQ(calls, 1);
}

// ─────────────────────────────────────────────────────────────────────────────
// Cache management
// ─────────────────────────────────────────────────────────────────────────────

TEST(Cache, ClearCacheForcesMiss) {
    MemoizedComputeNode<double, double>::clearCache();

    auto inp = Input<double>::make("x", 7.0);
    int calls = 0;
    auto node = MemoizedComputeNode<double, double>::make(
        "scale",
        std::make_tuple(asNode(inp)),
        [&](const double& x) { ++calls; return x; });

    evalDouble(node);   // miss
    EXPECT_EQ(calls, 1);

    MemoizedComputeNode<double, double>::clearCache();

    // Force a re-eval of the clean node.
    EvalContext ctx;
    ctx.forceRecompute = true;
    get_value<double>(node->eval(ctx));

    EXPECT_EQ(calls, 2);
    EXPECT_EQ(node->cacheMisses(), 2u);
}

TEST(Cache, CacheSizeTracked) {
    MemoizedComputeNode<double, double>::clearCache();

    auto inp = Input<double>::make("x", 1.0);
    auto node = MemoizedComputeNode<double, double>::make(
        "scale",
        std::make_tuple(asNode(inp)),
        [](const double& x) { return x; });

    inp->set(1.0); evalDouble(node);
    inp->set(2.0); evalDouble(node);
    inp->set(3.0); evalDouble(node);

    auto sz = MemoizedComputeNode<double, double>::cacheSize();
    EXPECT_EQ(sz, 3u);
}

TEST(Cache, NodeNameIsolatesEntries) {
    MemoizedComputeNode<double, double>::clearCache();

    auto inp = Input<double>::make("x", 5.0);

    auto nodeA = MemoizedComputeNode<double, double>::make(
        "nodeA", std::make_tuple(asNode(inp)), [](const double& x) { return x; });
    auto nodeB = MemoizedComputeNode<double, double>::make(
        "nodeB", std::make_tuple(asNode(inp)), [](const double& x) { return x; });

    evalDouble(nodeA);
    evalDouble(nodeB);

    // Different names → different keys → two distinct cache entries.
    auto sz2 = MemoizedComputeNode<double, double>::cacheSize();
    EXPECT_EQ(sz2, 2u);
    EXPECT_EQ(nodeA->cacheMisses(), 1u);
    EXPECT_EQ(nodeB->cacheMisses(), 1u);
}

TEST(Cache, SameNameSameInputsSharesEntry) {
    // By design, nodes with the same name and input values share an entry.
    MemoizedComputeNode<double, double>::clearCache();

    auto inp = Input<double>::make("x", 5.0);

    auto nodeA = MemoizedComputeNode<double, double>::make(
        "shared", std::make_tuple(asNode(inp)), [](const double& x) { return x * 2.0; });
    auto nodeB = MemoizedComputeNode<double, double>::make(
        "shared", std::make_tuple(asNode(inp)), [](const double& x) { return x * 2.0; });

    evalDouble(nodeA);  // miss — populates cache
    evalDouble(nodeB);  // hit  — same name + same input value

    auto sz1 = MemoizedComputeNode<double, double>::cacheSize();
    EXPECT_EQ(sz1, 1u);
    EXPECT_EQ(nodeA->cacheMisses(), 1u);
    EXPECT_EQ(nodeB->cacheHits(),   1u);
}

// ─────────────────────────────────────────────────────────────────────────────
// Equality policy
// ─────────────────────────────────────────────────────────────────────────────

TEST(EqualityPolicy, CorrectValuePropagatedWhenOutputUnchanged) {
    MemoizedComputeNode<double, double>::clearCache();

    // The functor ignores its input, so every new input value is a miss (a new
    // key) that yields the same 42.0.
    auto inp = Input<double>::make("x", 1.0);
    auto memo = MemoizedComputeNode<double, double>::make(
        "const42", std::make_tuple(asNode(inp)),
        [](const double&) { return 42.0; });

    auto downstream = ComputeNode<double, double>::make(
        "downstream", std::make_tuple(asNode(memo)),
        [](const double& v) { return v; });

    EXPECT_EQ(evalDouble(downstream), 42.0);
    inp->set(2.0);
    EXPECT_EQ(evalDouble(downstream), 42.0);
    inp->set(3.0);
    EXPECT_EQ(evalDouble(downstream), 42.0);

    // Three distinct inputs, three misses, one result: after the first eval the
    // equality check leaves cached_ alone.
    EXPECT_EQ(memo->cacheMisses(), 3u);
}

TEST(EqualityPolicy, AlwaysChangedPolicyDoesNotAffectCacheSemantics) {
    MemoizedComputeNode<double, double>::clearCache();

    // AlwaysChangedPolicy makes every result count as changed; the hit and miss
    // counts, and the skipped functor on a hit, are unaffected.
    auto inp = Input<double>::make("x", 1.0);
    int calls = 0;
    auto memo = MemoizedComputeNode<double, double>::make(
        "ac_memo", std::make_tuple(asNode(inp)),
        [&](const double& x) { ++calls; return x * 2.0; },
        std::make_shared<AlwaysChangedPolicy>());

    evalDouble(memo);       // miss
    EXPECT_EQ(calls, 1);

    inp->set(1.0);          // same value: the Input does not propagate
    evalDouble(memo);       // clean: returns cached_, no hit or miss
    EXPECT_EQ(calls, 1);

    inp->set(2.0);          // new value → miss
    evalDouble(memo);
    EXPECT_EQ(calls, 2);
    EXPECT_EQ(memo->cacheMisses(), 2u);

    inp->set(1.0);          // back to 1.0 → hit, functor skipped
    evalDouble(memo);
    EXPECT_EQ(calls, 2);
    EXPECT_EQ(memo->cacheHits(), 1u);
}

// ─────────────────────────────────────────────────────────────────────────────
// Dirty propagation
// ─────────────────────────────────────────────────────────────────────────────

TEST(Dirty, UpstreamSetMarksDirty) {
    MemoizedComputeNode<double, double>::clearCache();

    auto inp = Input<double>::make("x", 1.0);
    auto node = MemoizedComputeNode<double, double>::make(
        "dirty_test", std::make_tuple(asNode(inp)),
        [](const double& x) { return x; });

    evalDouble(node);
    EXPECT_FALSE(node->dirty());

    inp->set(2.0);              // calls invalidate() on the node
    EXPECT_TRUE(node->dirty());
}

TEST(Dirty, DownstreamInvalidatedOnOutputChange) {
    MemoizedComputeNode<double, double>::clearCache();

    auto inp = Input<double>::make("x", 1.0);
    auto memo = MemoizedComputeNode<double, double>::make(
        "pass", std::make_tuple(asNode(inp)),
        [](const double& x) { return x; });

    auto downstream = ComputeNode<double, double>::make(
        "ds", std::make_tuple(asNode(memo)),
        [](const double& v) { return v * 10.0; });

    EvalContext ctx;
    downstream->eval(ctx);
    EXPECT_FALSE(downstream->dirty());

    inp->set(5.0);
    EXPECT_TRUE(downstream->dirty());
    EXPECT_EQ(get_value<double>(downstream->eval(ctx)), 50.0);
}

TEST(Dirty, ForceRecomputeBypassesDirtyCheck) {
    MemoizedComputeNode<double, double>::clearCache();

    auto inp = Input<double>::make("x", 2.0);
    int calls = 0;
    auto node = MemoizedComputeNode<double, double>::make(
        "force_test", std::make_tuple(asNode(inp)),
        [&](const double& x) { ++calls; return x; });

    evalDouble(node);           // miss; the node is now clean
    EXPECT_EQ(calls, 1);

    // forceRecompute skips the clean-node shortcut and looks the key up.
    EvalContext ctx;
    ctx.forceRecompute = true;
    get_value<double>(node->eval(ctx));

    // The entry is cached: a hit, not a second call.
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(node->cacheHits(), 1u);
}

// ─────────────────────────────────────────────────────────────────────────────
// Multi-input
// ─────────────────────────────────────────────────────────────────────────────

TEST(MultiInput, InputOrderDistinguishesKey) {
    MemoizedComputeNode<double, double, double>::clearCache();

    auto a = Input<double>::make("a", 1.0);
    auto b = Input<double>::make("b", 2.0);
    int calls = 0;
    auto node = MemoizedComputeNode<double, double, double>::make(
        "sub",
        std::make_tuple(asNode(a), asNode(b)),
        [&](const double& x, const double& y) { ++calls; return x - y; });

    // (1, 2) → -1
    EXPECT_EQ(evalDouble(node), -1.0);
    EXPECT_EQ(calls, 1);

    // (2, 1) → different key, different result
    a->set(2.0); b->set(1.0);
    EXPECT_EQ(evalDouble(node), 1.0);
    EXPECT_EQ(calls, 2);

    // Back to (1, 2) → cache hit, result -1
    a->set(1.0); b->set(2.0);
    EXPECT_EQ(evalDouble(node), -1.0);
    EXPECT_EQ(calls, 2);
    EXPECT_EQ(node->cacheHits(), 1u);
}

TEST(MultiInput, MultiInputCacheHit) {
    MemoizedComputeNode<double, double, double>::clearCache();

    auto a = Input<double>::make("a", 3.0);
    auto b = Input<double>::make("b", 4.0);
    int calls = 0;
    auto node = MemoizedComputeNode<double, double, double>::make(
        "add",
        std::make_tuple(asNode(a), asNode(b)),
        [&](const double& x, const double& y) { ++calls; return x + y; });

    evalDouble(node);           // miss

    // Change both inputs then restore — hit on second visit to (3, 4).
    a->set(10.0); b->set(20.0);
    evalDouble(node);           // miss
    a->set(3.0);  b->set(4.0);
    evalDouble(node);           // hit

    EXPECT_EQ(calls, 2);
    EXPECT_EQ(node->cacheHits(),   1u);
    EXPECT_EQ(node->cacheMisses(), 2u);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
