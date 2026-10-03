// =============================================================================
// flywheel-dag — A header-only C++17 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

#include <gtest/gtest.h>

#include "flywheel/dag.hpp"
#include "flywheel/dag_traversal.hpp"

#include <algorithm>
#include <iterator>
#include <string>
#include <type_traits>
#include <unordered_set>
#include <vector>

using namespace dag;

// ─────────────────────────────────────────────────────────────────────────────
// Test fixtures — minimal nodes that let us build arbitrary input graphs.
// ─────────────────────────────────────────────────────────────────────────────

namespace {

class TestNode : public NodeBase, public std::enable_shared_from_this<TestNode> {
public:
    static std::shared_ptr<TestNode> make(std::string name,
                                          std::vector<NodePtr> ins = {}) {
        auto n = std::shared_ptr<TestNode>(
            new TestNode(std::move(name), std::move(ins)));
        wire(n, n->inputs_);
        return n;
    }

    /// After construction, splice extra inputs in. Used by the cycle test to
    /// create a back-edge that bypasses normal wire() lifetimes.
    void appendInput(NodePtr extra) {
        inputs_.push_back(std::move(extra));
    }

    ValuePtr eval(EvalContext&) override { return cached_; }
    std::string name() const override { return name_; }
    std::vector<NodePtr> inputs() const override { return inputs_; }
    NodeKind kind() const override { return NodeKind::Compute; }

private:
    TestNode(std::string n, std::vector<NodePtr> ins)
        : name_(std::move(n)), inputs_(std::move(ins)) {}

    std::string          name_;
    std::vector<NodePtr> inputs_;
    ValuePtr             cached_;
};

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Iterator-traits invariant (static check).
// ─────────────────────────────────────────────────────────────────────────────
static_assert(std::is_same_v<
                  std::iterator_traits<traversal::BfsRange::iterator>::iterator_category,
                  std::input_iterator_tag>,
              "BfsRange::iterator must model std::input_iterator_tag");

TEST(DagTraversal, EmptyRootsIsEmpty) {
    auto bfs = traversal::bfs_upstream({});
    EXPECT_TRUE(bfs.begin() == bfs.end());
}

TEST(DagTraversal, SingleLeafYieldsSelf) {
    auto a = TestNode::make("a");
    std::vector<std::string> seen;
    for (const auto& n : traversal::bfs_upstream({a})) seen.push_back(n->name());
    EXPECT_EQ(seen, (std::vector<std::string>{"a"}));
}

// ─────────────────────────────────────────────────────────────────────────────
// LinearChainBfsOrder — A → B → C; starting from C yields {C, B, A}.
// ─────────────────────────────────────────────────────────────────────────────
TEST(DagTraversal, LinearChainBfsOrder) {
    auto a = TestNode::make("a");
    auto b = TestNode::make("b", {a});
    auto c = TestNode::make("c", {b});

    std::vector<std::string> seen;
    for (const auto& n : traversal::bfs_upstream({c})) seen.push_back(n->name());
    EXPECT_EQ(seen, (std::vector<std::string>{"c", "b", "a"}));
}

// ─────────────────────────────────────────────────────────────────────────────
// DiamondDedupes — A reachable via two paths, visited once.
// ─────────────────────────────────────────────────────────────────────────────
TEST(DagTraversal, DiamondDedupes) {
    auto a = TestNode::make("a");
    auto b = TestNode::make("b", {a});
    auto c = TestNode::make("c", {a});
    auto d = TestNode::make("d", {b, c});

    std::vector<NodePtr> all;
    for (const auto& n : traversal::bfs_upstream({d})) all.push_back(n);

    EXPECT_EQ(all.size(), 4u);
    EXPECT_EQ(std::count_if(all.begin(), all.end(),
                            [](const NodePtr& n) { return n->name() == "a"; }),
              1);
}

// ─────────────────────────────────────────────────────────────────────────────
// FeedbackCycleTerminates — back-edge in inputs() graph; BFS visits each
// node once and reaches end().
// ─────────────────────────────────────────────────────────────────────────────
TEST(DagTraversal, FeedbackCycleTerminates) {
    auto a = TestNode::make("a");
    auto b = TestNode::make("b", {a});
    a->appendInput(b);              // a now lists b as an input → cycle

    std::vector<std::string> seen;
    for (const auto& n : traversal::bfs_upstream({b})) seen.push_back(n->name());

    // BFS visits both nodes once and terminates.
    EXPECT_EQ(seen.size(), 2u);
    std::sort(seen.begin(), seen.end());
    EXPECT_EQ(seen, (std::vector<std::string>{"a", "b"}));
}

// ─────────────────────────────────────────────────────────────────────────────
// MultipleRoots — shared ancestor visited once across roots.
// ─────────────────────────────────────────────────────────────────────────────
TEST(DagTraversal, MultipleRoots) {
    auto shared = TestNode::make("shared");
    auto leftLeaf  = TestNode::make("left",  {shared});
    auto rightLeaf = TestNode::make("right", {shared});

    std::vector<NodePtr> all;
    for (const auto& n : traversal::bfs_upstream({leftLeaf, rightLeaf}))
        all.push_back(n);

    EXPECT_EQ(all.size(), 3u);
    EXPECT_EQ(std::count_if(all.begin(), all.end(),
                            [](const NodePtr& n) { return n->name() == "shared"; }),
              1);
}

// ─────────────────────────────────────────────────────────────────────────────
// RangeForLoop — compiles and visits the expected sequence.
// ─────────────────────────────────────────────────────────────────────────────
TEST(DagTraversal, RangeForLoop) {
    auto a = TestNode::make("a");
    auto b = TestNode::make("b", {a});

    int count = 0;
    for (const auto& n : traversal::bfs_upstream({b})) {
        (void)n;
        ++count;
    }
    EXPECT_EQ(count, 2);
}

TEST(DagTraversal, StdForEach) {
    auto a = TestNode::make("a");
    auto b = TestNode::make("b", {a});
    auto c = TestNode::make("c", {b});

    std::vector<std::string> seen;
    auto bfs = traversal::bfs_upstream({c});
    std::for_each(bfs.begin(), bfs.end(),
                  [&](const NodePtr& n) { seen.push_back(n->name()); });
    EXPECT_EQ(seen, (std::vector<std::string>{"c", "b", "a"}));
}

// ─────────────────────────────────────────────────────────────────────────────
// StdFindIf — find a node by name; returned iterator dereferences to it.
// ─────────────────────────────────────────────────────────────────────────────
TEST(DagTraversal, StdFindIf) {
    auto a = TestNode::make("a");
    auto b = TestNode::make("b", {a});
    auto c = TestNode::make("c", {b});

    auto bfs = traversal::bfs_upstream({c});
    auto it = std::find_if(bfs.begin(), bfs.end(),
                           [](const NodePtr& n) { return n->name() == "b"; });
    ASSERT_NE(it, bfs.end());
    EXPECT_EQ((*it)->name(), "b");
}

TEST(DagTraversal, StdCountIf) {
    auto a = TestNode::make("a");
    auto b = TestNode::make("b", {a});
    auto c = TestNode::make("c", {a});
    auto d = TestNode::make("d", {b, c});

    auto bfs = traversal::bfs_upstream({d});
    auto matches = std::count_if(bfs.begin(), bfs.end(),
                                 [](const NodePtr& n) {
                                     return n->name().size() == 1;
                                 });
    EXPECT_EQ(matches, 4);
}

// ─────────────────────────────────────────────────────────────────────────────
// StdCopyIf — filter into a vector.
// ─────────────────────────────────────────────────────────────────────────────
TEST(DagTraversal, StdCopyIf) {
    auto a = TestNode::make("alpha");
    auto b = TestNode::make("beta",  {a});
    auto c = TestNode::make("gamma", {b});

    auto bfs = traversal::bfs_upstream({c});
    std::vector<NodePtr> picked;
    std::copy_if(bfs.begin(), bfs.end(), std::back_inserter(picked),
                 [](const NodePtr& n) { return n->name()[0] == 'a'; });
    ASSERT_EQ(picked.size(), 1u);
    EXPECT_EQ(picked.front()->name(), "alpha");
}

// ─────────────────────────────────────────────────────────────────────────────
// PostIncrementMatchesPreIncrement — both walk equivalent sequences.
// ─────────────────────────────────────────────────────────────────────────────
TEST(DagTraversal, PostIncrementMatchesPreIncrement) {
    auto a = TestNode::make("a");
    auto b = TestNode::make("b", {a});
    auto c = TestNode::make("c", {b});

    std::vector<std::string> pre;
    {
        auto bfs = traversal::bfs_upstream({c});
        for (auto it = bfs.begin(); it != bfs.end(); ++it)
            pre.push_back((*it)->name());
    }

    std::vector<std::string> post;
    {
        auto bfs = traversal::bfs_upstream({c});
        for (auto it = bfs.begin(); it != bfs.end(); ) {
            auto snap = it++;
            post.push_back((*snap)->name());
        }
    }

    EXPECT_EQ(pre, post);
}

// ─────────────────────────────────────────────────────────────────────────────
// CustomNeighborsFn — neighbours function customised; traversal respects it.
//
// Filter: include input edges only when the *parent* node's name starts with
// "keep_". This prunes upstream walking at any node that doesn't qualify.
// ─────────────────────────────────────────────────────────────────────────────
TEST(DagTraversal, CustomNeighborsFn) {
    auto a = TestNode::make("a");
    auto b = TestNode::make("keep_b", {a});
    auto c = TestNode::make("c",      {b});                  // does not "keep"
    auto d = TestNode::make("keep_d", {c});

    traversal::NeighborsFn filtered = [](const INode& n) -> std::vector<NodePtr> {
        if (n.name().rfind("keep_", 0) == 0) return n.inputs();
        return {};
    };

    traversal::BfsRange bfs({d}, filtered);
    std::vector<std::string> seen;
    for (const auto& nd : bfs) seen.push_back(nd->name());

    // d → c (visited because d "keep"s) → c does NOT pass inputs through →
    // chain stops at c. b and a are not visited.
    EXPECT_EQ(seen, (std::vector<std::string>{"keep_d", "c"}));
}

TEST(DagTraversal, StdAnyOf) {
    auto a = TestNode::make("a");
    auto b = TestNode::make("b", {a});
    auto c = TestNode::make("c", {b});

    auto bfs = traversal::bfs_upstream({c});
    const bool hasA = std::any_of(bfs.begin(), bfs.end(),
                                  [](const NodePtr& n) { return n->name() == "a"; });
    EXPECT_TRUE(hasA);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
