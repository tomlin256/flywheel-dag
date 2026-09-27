// =============================================================================
// flywheel-dag — A header-only C++17 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

#include <gtest/gtest.h>
#include "flywheel/dag_graph.hpp"
#include "flywheel/dag_async.hpp"
#include "flywheel/dag_timeseries.hpp"

using namespace dag;

// ── Helpers ───────────────────────────────────────────────────────────────────

static std::shared_ptr<Input<double>> makeInput(const std::string& name, double val = 0.0) {
    return Input<double>::make(name, val);
}

static ComputeNodePtr<double, double, double> makeCompute(
    const std::string& name,
    NodePtr a,
    NodePtr b)
{
    return ComputeNode<double, double, double>::make(
        name, { a, b },
        [](const double& x, const double& y) { return x + y; });
}

static ComputeNodePtr<double, double> makePassthrough(
    const std::string& name,
    NodePtr in)
{
    return ComputeNode<double, double>::make(
        name, { in },
        [](const double& x) { return x; });
}

// ── Test 1: all four nodes appear in the DOT output ──────────────────────────

TEST(DagGraph, AllNodesPresent) {
    auto a = makeInput("a");
    auto b = makeInput("b");
    auto c = makeCompute("c", a, b);
    auto d = makePassthrough("d", c);

    std::string dot = GraphExporter::toDot({ d });

    EXPECT_NE(dot.find("\"a\""), std::string::npos);
    EXPECT_NE(dot.find("\"b\""), std::string::npos);
    EXPECT_NE(dot.find("\"c\""), std::string::npos);
    EXPECT_NE(dot.find("\"d\""), std::string::npos);
}

// ── Test 2: edges appear in the DOT output ───────────────────────────────────

TEST(DagGraph, EdgesPresent) {
    auto a = makeInput("a");
    auto b = makeInput("b");
    auto c = makeCompute("c", a, b);
    auto d = makePassthrough("d", c);

    std::string dot = GraphExporter::toDot({ d });

    EXPECT_NE(dot.find("\"a\" -> \"c\""), std::string::npos);
    EXPECT_NE(dot.find("\"b\" -> \"c\""), std::string::npos);
    EXPECT_NE(dot.find("\"c\" -> \"d\""), std::string::npos);
}

// ── Test 3: Input nodes use ellipse shape ─────────────────────────────────────

TEST(DagGraph, InputNodeShape) {
    auto a = makeInput("a");
    auto b = makeInput("b");
    auto c = makeCompute("c", a, b);

    std::string dot = GraphExporter::toDot({ c });

    // Both 'a' and 'b' should have ellipse shape
    // The DOT attribute appears in the cluster block for the node
    EXPECT_NE(dot.find("shape=ellipse"), std::string::npos);
}

// ── Test 4: Compute nodes use box shape ───────────────────────────────────────

TEST(DagGraph, ComputeNodeShape) {
    auto a = makeInput("a");
    auto c = makePassthrough("c", a);

    std::string dot = GraphExporter::toDot({ c });

    EXPECT_NE(dot.find("shape=box"), std::string::npos);
}

// ── Test 5: Clustering by name prefix ────────────────────────────────────────

TEST(DagGraph, ClusteringByPrefix) {
    // X.a, X.b, X.c → cluster_X
    // Y.d           → cluster_Y
    auto xa = makeInput("X.a");
    auto xb = makeInput("X.b");
    auto xc = makeCompute("X.c", xa, xb);
    auto yd = makePassthrough("Y.d", xc);

    std::string dot = GraphExporter::toDot({ yd });

    EXPECT_NE(dot.find("cluster_X"), std::string::npos);
    EXPECT_NE(dot.find("cluster_Y"), std::string::npos);
}

// ── Test 6: Nodes without a dot go into cluster_global ───────────────────────

TEST(DagGraph, GlobalCluster) {
    auto a = makeInput("fees");
    auto b = makePassthrough("total", a);

    std::string dot = GraphExporter::toDot({ b });

    EXPECT_NE(dot.find("cluster_global"), std::string::npos);
}

// ── Test 7: Multiple roots — all are included ────────────────────────────────

TEST(DagGraph, MultipleRoots) {
    auto a = makeInput("a");
    auto b = makePassthrough("b", a);
    auto c = makePassthrough("c", a);

    std::string dot = GraphExporter::toDot({ b, c });

    EXPECT_NE(dot.find("\"a\""), std::string::npos);
    EXPECT_NE(dot.find("\"b\""), std::string::npos);
    EXPECT_NE(dot.find("\"c\""), std::string::npos);
}

// ── Test 8: Shared upstream node is deduplicated ──────────────────────────────

TEST(DagGraph, SharedUpstreamDeduplicated) {
    auto a = makeInput("shared");
    auto b = makePassthrough("b", a);
    auto c = makePassthrough("c", a);
    auto d = makeCompute("d", b, c);

    std::string dot = GraphExporter::toDot({ d });

    // "shared" should appear exactly once as a node declaration
    // Count occurrences of the node declaration (the one inside the cluster block)
    std::string needle = "\"shared\"";
    std::size_t count = 0;
    std::size_t pos = 0;
    while ((pos = dot.find(needle, pos)) != std::string::npos) {
        ++count;
        pos += needle.size();
    }
    // node declaration + 2 edge references = 3 occurrences minimum,
    // but the node itself appears only once in a cluster block.
    // The edge "shared" -> "b" and "shared" -> "c" also contain the name.
    // Key check: the node is not declared twice.
    // We verify by counting [shape= ... ] attribute lines — only one for "shared".
    std::size_t attrCount = 0;
    pos = 0;
    std::string declNeedle = "\"shared\" [shape=";
    while ((pos = dot.find(declNeedle, pos)) != std::string::npos) {
        ++attrCount;
        pos += declNeedle.size();
    }
    EXPECT_EQ(attrCount, 1u);
}

// ── Test 9: NodeKind::Input returns correct kind ──────────────────────────────

TEST(DagGraph, NodeKindInput) {
    auto a = Input<double>::make("a");
    EXPECT_EQ(a->kind(), NodeKind::Input);
}

// ── Test 10: NodeKind::Compute returns correct kind ───────────────────────────

TEST(DagGraph, NodeKindCompute) {
    auto a = Input<double>::make("a");
    auto b = ComputeNode<double, double>::make("b", { a },
                 [](const double& x) { return x; });
    EXPECT_EQ(b->kind(), NodeKind::Compute);
}

// ── Test 11: NodeKind::AsyncInput returns correct kind ───────────────────────

TEST(DagGraph, NodeKindAsyncInput) {
    auto n = dag::async::AsyncInput<double>::make("ai");
    EXPECT_EQ(n->kind(), NodeKind::AsyncInput);
}

// ── Test 12: NodeKind::AsyncQueue returns correct kind ───────────────────────

TEST(DagGraph, NodeKindAsyncQueue) {
    auto n = dag::async::AsyncQueue<double>::make("aq");
    EXPECT_EQ(n->kind(), NodeKind::AsyncQueue);
}

// ── Test 13: NodeKind::TimeSeries returns correct kind ───────────────────────

TEST(DagGraph, NodeKindTimeSeries) {
    auto inp = Input<double>::make("x");
    auto ewma = dag::ts::EWMANode::make("ewma", inp, 0.1);
    EXPECT_EQ(ewma->kind(), NodeKind::TimeSeries);
}

// ── Test 14: slash in node name is replaced with underscore in DOT output ────

TEST(DagGraph, SlashInNameReplacedWithUnderscore) {
    auto raw    = makeInput("left/right.raw");
    auto smooth = makePassthrough("left/right.smooth", raw);

    std::string dot = GraphExporter::toDot({ smooth });

    // The slash should not appear anywhere in the DOT source
    EXPECT_EQ(dot.find('/'), std::string::npos);

    // The sanitised forms should appear instead
    EXPECT_NE(dot.find("left_right.raw"),    std::string::npos);
    EXPECT_NE(dot.find("left_right.smooth"), std::string::npos);

    // Edge between the two nodes uses sanitised names
    EXPECT_NE(dot.find("\"left_right.raw\" -> \"left_right.smooth\""), std::string::npos);
}

// ── Test 15: hyphen in cluster prefix is quoted, not left as a bare DOT id ───
// Regression: an unquoted "subgraph cluster_my-module" is invalid DOT (dot
// rejects unquoted ids containing '-'). A node-name prefix of the form
// "<hyphenated-module>/" followed by a dotted child name (e.g. ".fast") is
// common, so clusterOf() strips a hyphenated, slash-bearing prefix — exactly
// this shape.

TEST(DagGraph, HyphenInClusterPrefixIsQuoted) {
    auto fast = makeInput("my-module/signal.fast");
    auto slow = makePassthrough("my-module/signal.slow", fast);

    std::string dot = GraphExporter::toDot({ slow });

    // Cluster identifier must be quoted (valid DOT even with '-' inside).
    EXPECT_NE(dot.find("subgraph \"cluster_my-module_signal\""),
              std::string::npos);

    // Must never regress to the pre-fix bare/unquoted form.
    EXPECT_EQ(dot.find("subgraph cluster_my-module"), std::string::npos);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
