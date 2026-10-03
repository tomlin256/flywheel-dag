// =============================================================================
// flywheel-dag — A header-only C++17 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

// test_engine_stateful_discovery.cpp — Engine::discoverStatefulNodes() tests.
//
// Exercises the BFS discovery helper that walks INode::inputs() from every
// registered output and collects every reachable IStatefulNode with
// persistState() == true.

#include <gtest/gtest.h>

#include "flywheel/dag.hpp"
#include "flywheel/dag_engine.hpp"
#include "flywheel/dag_state_store.hpp"
#include "flywheel/dag_timeseries.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

using namespace dag;
using namespace dag::async;
using namespace dag::ts;

namespace {

// Identify a node by name in the discovery result vector.
bool contains_name(const std::vector<StatefulNodePtr>& nodes,
                   const std::string& name) {
    for (const auto& sn : nodes) {
        auto in = std::dynamic_pointer_cast<INode>(sn);
        if (in && in->name() == name) return true;
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// A stateful node that opts out of persistence via persistState() == false.
// Used to verify the opt-out path.
// ─────────────────────────────────────────────────────────────────────────────
struct OptOutState {};

class OptOutStatefulNode
    : public StatefulNodeBase<OptOutStatefulNode, double, double, OptOutState> {
public:
    using Base = StatefulNodeBase<OptOutStatefulNode, double, double, OptOutState>;
    using State = OptOutState;

    static std::shared_ptr<OptOutStatefulNode> make(std::string name, NodePtr upstream) {
        auto p = std::shared_ptr<OptOutStatefulNode>(
            new OptOutStatefulNode(std::move(name), std::move(upstream)));
        dag::wire(p, p->inputs());
        return p;
    }

    bool persistState() const override { return false; }

    double doCompute(const double& v, State&) { return v; }
    void doSaveState(INodeState&, const State&) const {}
    void doRestoreState(const INodeState&, State&) {}

private:
    OptOutStatefulNode(std::string n, NodePtr up)
        : Base(std::move(n), std::move(up)) {}
};

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// EmptyEngine — no outputs, no modules → discovery returns empty.
// ─────────────────────────────────────────────────────────────────────────────
TEST(StatefulDiscovery, EmptyEngine) {
    Engine engine;
    EXPECT_TRUE(engine.discoverStatefulNodes().empty());
}

// ─────────────────────────────────────────────────────────────────────────────
// SingleStatefulNodeReachable — Input → RollingStats → addOutput.
// Discovery returns the RollingStats exactly once.
// ─────────────────────────────────────────────────────────────────────────────
TEST(StatefulDiscovery, SingleStatefulNodeReachable) {
    auto inp   = Input<double>::make("x", 0.0);
    auto stats = RollingStats::make("stats", inp, 16);

    Engine engine;
    engine.addOutput<double>(stats, [](const double&) {});

    auto discovered = engine.discoverStatefulNodes();
    ASSERT_EQ(discovered.size(), 1u);
    EXPECT_TRUE(contains_name(discovered, "stats"));
}

// ─────────────────────────────────────────────────────────────────────────────
// StatefulNodeReachableViaTwoOutputs — same node, two outputs, returned once.
// ─────────────────────────────────────────────────────────────────────────────
TEST(StatefulDiscovery, StatefulNodeReachableViaTwoOutputs) {
    auto inp   = Input<double>::make("x", 0.0);
    auto stats = RollingStats::make("shared_stats", inp, 16);

    auto ratio1 = ComputeNode<double, double>::make(
        "out1", std::make_tuple(std::static_pointer_cast<INode>(stats)),
        [](const double& m) { return m; });
    auto ratio2 = ComputeNode<double, double>::make(
        "out2", std::make_tuple(std::static_pointer_cast<INode>(stats)),
        [](const double& m) { return m; });

    Engine engine;
    engine.addOutput<double>(ratio1, [](const double&) {});
    engine.addOutput<double>(ratio2, [](const double&) {});

    auto discovered = engine.discoverStatefulNodes();
    ASSERT_EQ(discovered.size(), 1u);
    EXPECT_TRUE(contains_name(discovered, "shared_stats"));
}

// ─────────────────────────────────────────────────────────────────────────────
// StatelessChainReturnsEmpty — Input → ComputeNode → addOutput; no IStatefulNode.
// ─────────────────────────────────────────────────────────────────────────────
TEST(StatefulDiscovery, StatelessChainReturnsEmpty) {
    auto inp = Input<double>::make("x", 0.0);
    auto cn  = ComputeNode<double, double>::make(
        "double_it",
        std::make_tuple(std::static_pointer_cast<INode>(inp)),
        [](const double& v) { return v * 2.0; });

    Engine engine;
    engine.addOutput<double>(cn, [](const double&) {});

    EXPECT_TRUE(engine.discoverStatefulNodes().empty());
}

// ─────────────────────────────────────────────────────────────────────────────
// PersistStateFalseExcludes — node returns persistState()=false → excluded.
// ─────────────────────────────────────────────────────────────────────────────
TEST(StatefulDiscovery, PersistStateFalseExcludes) {
    auto inp     = Input<double>::make("x", 0.0);
    auto stats   = RollingStats::make("keep_stats", inp, 16);
    auto opt_out = OptOutStatefulNode::make("skip_me", stats);

    Engine engine;
    engine.addOutput<double>(opt_out, [](const double&) {});

    auto discovered = engine.discoverStatefulNodes();
    ASSERT_EQ(discovered.size(), 1u);
    EXPECT_TRUE(contains_name(discovered, "keep_stats"));
    EXPECT_FALSE(contains_name(discovered, "skip_me"));
}

// ─────────────────────────────────────────────────────────────────────────────
// MixedStatefulStatelessUpstream — chain through several stateful and
// stateless nodes: discovery returns all the stateful ones.
// ─────────────────────────────────────────────────────────────────────────────
TEST(StatefulDiscovery, MixedStatefulStatelessUpstream) {
    auto inp   = Input<double>::make("x", 0.0);
    auto stats = RollingStats::make("rs", inp, 16);
    auto ewma  = EWMANode::make("ewma", stats, 0.3);
    auto delta = DeltaNode<double>::make("delta", ewma);

    auto cn = ComputeNode<double, double>::make(
        "out",
        std::make_tuple(std::static_pointer_cast<INode>(delta)),
        [](const double& v) { return v; });

    Engine engine;
    engine.addOutput<double>(cn, [](const double&) {});

    auto discovered = engine.discoverStatefulNodes();
    EXPECT_EQ(discovered.size(), 3u);
    EXPECT_TRUE(contains_name(discovered, "rs"));
    EXPECT_TRUE(contains_name(discovered, "ewma"));
    EXPECT_TRUE(contains_name(discovered, "delta"));
}

// ─────────────────────────────────────────────────────────────────────────────
// CastWorksOnEveryStatefulNodeType
//
// Constructs one of each concrete stateful node type but RollingSumNode
// (flywheel-dag#32) and confirms dynamic_pointer_cast<IStatefulNode> against the
// INode-typed shared_ptr (the form discovery uses) succeeds. Guards against a
// stateful node that bypasses StatefulNodeBase and quietly skips the discovery
// path.
// ─────────────────────────────────────────────────────────────────────────────
TEST(StatefulDiscovery, CastWorksOnEveryStatefulNodeType) {
    auto inp_d = Input<double>::make("xd", 0.0);
    auto inp_b = Input<bool>::make("xb", false);

    std::vector<NodePtr> nodes;
    nodes.push_back(WindowNode<double>::make("win",      inp_d, 4));
    nodes.push_back(RollingStats   ::make("stats",       inp_d, 4));
    nodes.push_back(RollingMinMaxNode::make("mm",        inp_d, 4));
    nodes.push_back(EWMANode       ::make("ewma",        inp_d, 0.3));
    nodes.push_back(EWMATickRateNode::make("ewmaTick",   inp_d, 0.3));
    nodes.push_back(DeltaNode<double>::make("delta",     inp_d));
    nodes.push_back(DelayNode<double>::make("delay",     inp_d, 3));
    nodes.push_back(ThresholdNode<double>::make("thr",   inp_d, 1.0,
                                                ThresholdNode<double>::Direction::Above));
    nodes.push_back(ZScoreNode     ::make("z",           inp_d, 4));
    nodes.push_back(OutlierGateNode::make("og",          inp_d, 4, 3.0));
    nodes.push_back(RateLimiterNode<double>::make("rl",  inp_d, 0.01));
    nodes.push_back(DebounceCountNode::make("dc",        inp_b, 3));
    nodes.push_back(LatchedDebounceNode::make("ld",      inp_b, 3));

    for (const auto& n : nodes) {
        auto sn = std::dynamic_pointer_cast<IStatefulNode>(n);
        EXPECT_NE(sn, nullptr) << "cast failed for " << n->name();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// FeedbackLoopTerminates — addFeedback wires output to Input<T>; the BFS
// still walks only inputs(), so no cycle exists in the traversal graph, but
// the engine accepts both with no issues.
// ─────────────────────────────────────────────────────────────────────────────
TEST(StatefulDiscovery, FeedbackLoopTerminates) {
    auto sigInput = Input<double>::make("signal", 0.0);
    auto stats    = RollingStats::make("fb_stats", sigInput, 8);

    Engine engine;
    auto fb = engine.makeInput<double>("fb", 0.0);
    engine.addFeedback<double>(stats, fb);

    auto discovered = engine.discoverStatefulNodes();
    ASSERT_EQ(discovered.size(), 1u);
    EXPECT_TRUE(contains_name(discovered, "fb_stats"));
}

// ─────────────────────────────────────────────────────────────────────────────
// SaveRestoreRoundTripsViaDiscovery — drive a save/restore round-trip using
// the discovery-sourced node list directly (the path save/restore uses).
// Demonstrates discovery returns the right set in the right shape for
// IStateStore to consume.
// ─────────────────────────────────────────────────────────────────────────────
TEST(StatefulDiscovery, SaveRestoreRoundTripsViaDiscovery) {
    const double alpha = 0.3;
    const std::vector<double> warm  = {1.0, 2.0, 3.0, 4.0, 5.0};
    const std::vector<double> extra = {6.0, 7.0};

    // ── Original engine
    auto inp1  = Input<double>::make("x", 0.0);
    auto ewma1 = EWMANode::make("ewma", inp1, alpha);

    Engine e1;
    e1.addOutput<double>(ewma1, [](const double&) {});
    auto store = std::make_shared<InMemoryStateStore>();

    EvalContext ctx;
    for (double v : warm) { inp1->set(v); ewma1->eval(ctx); }
    store->save(e1.discoverStatefulNodes());

    // ── Fresh engine
    auto inp2  = Input<double>::make("x2", 0.0);
    auto ewma2 = EWMANode::make("ewma", inp2, alpha);

    Engine e2;
    e2.addOutput<double>(ewma2, [](const double&) {});
    ASSERT_TRUE(store->restore(e2.discoverStatefulNodes()));

    // ── Drive both with extras; outputs must agree.
    for (double v : extra) {
        inp1->set(v); ewma1->eval(ctx);
        inp2->set(v); ewma2->eval(ctx);
    }
    EXPECT_NEAR(get_value<double>(ewma1->eval(ctx)),
                get_value<double>(ewma2->eval(ctx)),
                1e-9);
}

// ─────────────────────────────────────────────────────────────────────────────
// SaveRestoreSurvivesEmptyStatefulNodesOverride — a module need not list its
// stateful nodes anywhere: save and restore find them by discovery, from the
// registered outputs.
// ─────────────────────────────────────────────────────────────────────────────

namespace {

// Fixture: wires a single RollingStats through an engine output and lists it
// nowhere.
struct ForgotToListNodesModule : public IComputeModule {
    dag::InputPtr<double>      inp;
    std::shared_ptr<RollingStats> stats;

    std::string name() const override { return "forgot_list"; }

    void wire(Engine& engine) override {
        inp   = Input<double>::make("forgot_list.x", 0.0);
        stats = RollingStats::make("forgot_list.stats", inp, 32);
        engine.addOutput<double>(stats, [](const double&) {});
    }
};

} // namespace

TEST(StatefulDiscovery, SaveRestoreSurvivesEmptyStatefulNodesOverride) {
    auto store = std::make_shared<InMemoryStateStore>();

    // ── Engine 1: warm and save (Engine::saveState goes through
    //              discoverStatefulNodes()).
    {
        Engine e1;
        auto mod = std::make_shared<ForgotToListNodesModule>();
        e1.install(mod);
        e1.setStateStore(store);

        EvalContext ctx;
        for (double v : {1.0, 2.0, 3.0, 4.0, 5.0}) {
            mod->inp->set(v);
            mod->stats->eval(ctx);
        }
        e1.saveState();
    }
    EXPECT_TRUE(store->hasSavedState());

    // ── Engine 2: fresh + restore → mean must match warmed value.
    Engine e2;
    auto mod2 = std::make_shared<ForgotToListNodesModule>();
    e2.install(mod2);
    e2.setStateStore(store);
    ASSERT_TRUE(e2.restoreState());

    EXPECT_NEAR(mod2->stats->mean(), 3.0, 1e-9);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
