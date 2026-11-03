// =============================================================================
// flywheel-dag — A header-only C++23 reactive DAG computation engine
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
// persistState() == true. The StatefulDiscoveryCache tests, at the end, pin what
// the engine keeps of that walk between registrations and what keeping it must
// not change.

#include <gtest/gtest.h>

#include "flywheel/dag.hpp"
#include "flywheel/dag_engine.hpp"
#include "flywheel/dag_state_store.hpp"
#include "flywheel/dag_timeseries.hpp"

#include <algorithm>
#include <atomic>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
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

// ─────────────────────────────────────────────────────────────────────────────
// StatefulDiscoveryCache — what the engine keeps of the walk between
// registrations, and what keeping it must not change.
//
// A walk calls inputs() once on each node it reaches, so a node that counts its
// inputs() calls counts the walks that reach it. Nothing here steps an engine,
// so no probe is evaluated.
// ─────────────────────────────────────────────────────────────────────────────

namespace {

// A node whose inputs() adds one to walks(). inputs() is const, so the count is
// mutable: the call is what the tests observe, and a const member has no other
// way to record that it was made.
class CountingNode : public NodeBase {
public:
    static std::shared_ptr<CountingNode> make(std::string name,
                                              std::vector<NodePtr> ins = {}) {
        return std::shared_ptr<CountingNode>(
            new CountingNode(std::move(name), std::move(ins)));
    }

    std::size_t walks() const { return walks_.load(); }

    ValuePtr eval(EvalContext&) override {
        throw std::logic_error("CountingNode " + name_ + " is never evaluated");
    }
    std::string name() const override { return name_; }
    std::vector<NodePtr> inputs() const override {
        ++walks_;
        return ins_;
    }
    NodeKind kind() const override { return NodeKind::Compute; }

protected:
    CountingNode(std::string name, std::vector<NodePtr> ins)
        : name_(std::move(name)), ins_(std::move(ins)) {}

private:
    std::string                      name_;
    std::vector<NodePtr>             ins_;
    mutable std::atomic<std::size_t> walks_{0};
};

// The same node as an IStatefulNode. It keeps one number, and persistState()
// answers as the test last set it.
class StatefulProbe : public CountingNode, public IStatefulNode {
public:
    static std::shared_ptr<StatefulProbe> make(std::string name,
                                               std::vector<NodePtr> ins = {}) {
        return std::shared_ptr<StatefulProbe>(
            new StatefulProbe(std::move(name), std::move(ins)));
    }

    void   set_level(double v) { level_ = v; }
    double level() const { return level_; }
    void   set_persist(bool v) { persist_ = v; }

    void saveState(INodeState& s) const override { s["level"] = level_; }
    void restoreState(const INodeState& s) override {
        level_ = s["level"].as<double>();
        invalidate();
    }
    bool persistState() const override { return persist_; }

private:
    StatefulProbe(std::string name, std::vector<NodePtr> ins)
        : CountingNode(std::move(name), std::move(ins)) {}

    double level_   = 0.0;
    bool   persist_ = true;
};

// Registers two outputs, reading the discovery after each: what a module that
// asks the engine about its own nodes sees while it is being installed.
class TwoOutputModule : public IComputeModule {
public:
    std::vector<StatefulNodePtr> after_first;
    std::vector<StatefulNodePtr> after_second;

    std::string name() const override { return "two_outputs"; }
    void wire(Engine& engine) override {
        engine.addOutput(CountingNode::make("out_a", {StatefulProbe::make("a")}),
                         [](const ValuePtr&) {});
        after_first = engine.discoverStatefulNodes();
        engine.addOutput(CountingNode::make("out_b", {StatefulProbe::make("b")}),
                         [](const ValuePtr&) {});
        after_second = engine.discoverStatefulNodes();
    }
};

// The names of the nodes, in order: the nodes in these tests have unique names.
std::vector<std::string> names(const std::vector<StatefulNodePtr>& nodes) {
    std::vector<std::string> result;
    result.reserve(nodes.size());
    for (const auto& sn : nodes)
        result.push_back(std::dynamic_pointer_cast<INode>(sn)->name());
    return result;
}

using Names = std::vector<std::string>;

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// AnOutputRegisteredLaterIsFoundInBreadthFirstOrder
//
//   out1 -> a -> b -> leaf1          out2 -> c -> leaf2
//
// From the roots [out1, out2] the walk visits out1, out2, a, c, b: the order
// across all the outputs together. Depth first, or the second output's nodes
// appended to the first result, would put b before c.
// ─────────────────────────────────────────────────────────────────────────────
TEST(StatefulDiscoveryCache, AnOutputRegisteredLaterIsFoundInBreadthFirstOrder) {
    auto leaf1 = Input<double>::make("leaf1", 0.0);
    auto b     = StatefulProbe::make("b", {leaf1});
    auto a     = StatefulProbe::make("a", {b});

    Engine engine;
    engine.addOutput(CountingNode::make("out1", {a}), [](const ValuePtr&) {});
    EXPECT_EQ(names(engine.discoverStatefulNodes()), (Names{"a", "b"}));

    auto leaf2 = Input<double>::make("leaf2", 0.0);
    auto c     = StatefulProbe::make("c", {leaf2});
    engine.addOutput(CountingNode::make("out2", {c}), [](const ValuePtr&) {});
    EXPECT_EQ(names(engine.discoverStatefulNodes()), (Names{"a", "c", "b"}));
}

// ─────────────────────────────────────────────────────────────────────────────
// InstallAndAddFeedbackAreFoundToo — the other routes to a registration. A
// module's wire() reads the discovery between its own two registrations, and
// addFeedback() registers an output as addOutput() does.
// ─────────────────────────────────────────────────────────────────────────────
TEST(StatefulDiscoveryCache, InstallAndAddFeedbackAreFoundToo) {
    auto x = StatefulProbe::make("x");

    Engine engine;
    engine.addOutput(CountingNode::make("out_x", {x}), [](const ValuePtr&) {});
    ASSERT_EQ(names(engine.discoverStatefulNodes()), (Names{"x"}));

    auto mod = std::make_shared<TwoOutputModule>();
    engine.install(mod);
    EXPECT_EQ(names(mod->after_first),  (Names{"x", "a"}));
    EXPECT_EQ(names(mod->after_second), (Names{"x", "a", "b"}));

    auto f  = StatefulProbe::make("f");
    auto fb = engine.makeInput<double>("fb", 0.0);
    engine.addFeedback<double>(CountingNode::make("out_f", {f}), fb);
    EXPECT_EQ(names(engine.discoverStatefulNodes()), (Names{"x", "a", "b", "f"}));
}

// ─────────────────────────────────────────────────────────────────────────────
// PersistStateIsAskedOnEveryCall — persistState() is a virtual, and a node may
// answer differently over time. A registration is not what changes the answer.
// ─────────────────────────────────────────────────────────────────────────────
TEST(StatefulDiscoveryCache, PersistStateIsAskedOnEveryCall) {
    auto p = StatefulProbe::make("p");

    Engine engine;
    engine.addOutput(CountingNode::make("out", {p}), [](const ValuePtr&) {});

    EXPECT_EQ(names(engine.discoverStatefulNodes()), (Names{"p"}));
    p->set_persist(false);
    EXPECT_TRUE(engine.discoverStatefulNodes().empty());
    p->set_persist(true);
    EXPECT_EQ(names(engine.discoverStatefulNodes()), (Names{"p"}));
}

// ─────────────────────────────────────────────────────────────────────────────
// CallsBetweenRegistrationsDoNotWalk — the walk runs on the first call after a
// registration, and a later call reuses its result.
// ─────────────────────────────────────────────────────────────────────────────
TEST(StatefulDiscoveryCache, CallsBetweenRegistrationsDoNotWalk) {
    auto probe = StatefulProbe::make("probe", {Input<double>::make("leaf", 0.0)});

    Engine engine;
    engine.addOutput(probe, [](const ValuePtr&) {});

    const std::vector<StatefulNodePtr> first = engine.discoverStatefulNodes();
    ASSERT_EQ(first.size(), 1u);
    EXPECT_EQ(probe->walks(), 1u);

    EXPECT_EQ(engine.discoverStatefulNodes(), first);
    EXPECT_EQ(engine.discoverStatefulNodes(), first);
    EXPECT_EQ(probe->walks(), 1u);
}

// ─────────────────────────────────────────────────────────────────────────────
// ARegistrationWalksOnceMore — an output registered after a call makes the next
// call walk again, and only that one.
// ─────────────────────────────────────────────────────────────────────────────
TEST(StatefulDiscoveryCache, ARegistrationWalksOnceMore) {
    auto probe = StatefulProbe::make("probe");

    Engine engine;
    engine.addOutput(probe, [](const ValuePtr&) {});
    EXPECT_EQ(engine.discoverStatefulNodes().size(), 1u);
    EXPECT_EQ(engine.discoverStatefulNodes().size(), 1u);
    EXPECT_EQ(probe->walks(), 1u);

    engine.addOutput(Input<double>::make("other", 0.0), [](const ValuePtr&) {});
    EXPECT_EQ(engine.discoverStatefulNodes().size(), 1u);
    EXPECT_EQ(engine.discoverStatefulNodes().size(), 1u);
    EXPECT_EQ(probe->walks(), 2u);
}

// ─────────────────────────────────────────────────────────────────────────────
// AnEmptyResultIsKept — a graph with no stateful node has an empty result, which
// is a result like any other and is not walked for again.
// ─────────────────────────────────────────────────────────────────────────────
TEST(StatefulDiscoveryCache, AnEmptyResultIsKept) {
    auto plain = CountingNode::make("plain");

    Engine engine;
    engine.addOutput(plain, [](const ValuePtr&) {});
    EXPECT_TRUE(engine.discoverStatefulNodes().empty());
    EXPECT_TRUE(engine.discoverStatefulNodes().empty());
    EXPECT_EQ(plain->walks(), 1u);
}

// ─────────────────────────────────────────────────────────────────────────────
// SaveAndRestoreShareTheWalk — saveState() and restoreState() reach the stateful
// nodes through discoverStatefulNodes(), so they share its walk.
// ─────────────────────────────────────────────────────────────────────────────
TEST(StatefulDiscoveryCache, SaveAndRestoreShareTheWalk) {
    auto probe = StatefulProbe::make("probe");

    Engine engine;
    engine.addOutput(probe, [](const ValuePtr&) {});
    engine.setStateStore(std::make_shared<InMemoryStateStore>());

    probe->set_level(5.0);
    engine.saveState();
    engine.saveState();
    engine.saveState();
    probe->set_level(0.0);
    EXPECT_TRUE(engine.restoreState());
    EXPECT_DOUBLE_EQ(probe->level(), 5.0);
    EXPECT_EQ(probe->walks(), 1u);
}

// ─────────────────────────────────────────────────────────────────────────────
// ThreadsOnAColdCacheWalkOnce — eight threads reach a cold cache together, and
// the walk runs once between them.
//
// The graph is wide and shallow: the walk over 20,000 leaves outlasts the start
// of the other seven threads, so each finds the cache cold, and building and
// releasing the graph does not recurse. With the fill locked the first thread
// walks and the rest wait for its result; unlocked, each would walk. The
// assertions hold in every interleaving of a correct engine, with no sleep and
// no barrier.
// ─────────────────────────────────────────────────────────────────────────────
TEST(StatefulDiscoveryCache, ThreadsOnAColdCacheWalkOnce) {
    constexpr std::size_t kLeaves  = 20'000;
    constexpr std::size_t kThreads = 8;

    std::vector<NodePtr> leaves;
    leaves.reserve(kLeaves);
    for (std::size_t i = 0; i < kLeaves; ++i)
        leaves.push_back(Input<double>::make("leaf" + std::to_string(i), 0.0));
    auto probe = StatefulProbe::make("probe", std::move(leaves));

    Engine engine;
    engine.addOutput(probe, [](const ValuePtr&) {});

    std::vector<std::vector<StatefulNodePtr>> results(kThreads);
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (std::size_t i = 0; i < kThreads; ++i)
        threads.emplace_back([&engine, &results, i] { results[i] = engine.discoverStatefulNodes(); });
    for (auto& t : threads) t.join();

    for (const auto& r : results) {
        ASSERT_EQ(r.size(), 1u);
        EXPECT_EQ(r[0].get(), static_cast<IStatefulNode*>(probe.get()));
    }
    EXPECT_EQ(probe->walks(), 1u);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
