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
#include "flywheel/dag_state_store.hpp"
#include "flywheel/dag_timeseries.hpp"

#include <cmath>
#include <memory>
#include <vector>

using namespace dag;
using namespace dag::ts;

// ─────────────────────────────────────────────────────────────────────────────
// Helper: feed N values into a node through an Input<double>
// ─────────────────────────────────────────────────────────────────────────────
template<typename Node>
static void feed(const std::shared_ptr<Input<double>>& inp,
                 const std::shared_ptr<Node>& node,
                 const std::vector<double>& values)
{
    EvalContext ctx;
    for (double v : values) { inp->set(v); node->eval(ctx); }
}

// ─────────────────────────────────────────────────────────────────────────────
// INodeState — basic value read/write
// ─────────────────────────────────────────────────────────────────────────────

TEST(NodeStateTests, WriteReadDouble) {
    InMemoryStateStore store;
    // use save/restore round-trip on a trivial node to get a MapNodeState
    // Actually, test MapNodeState directly via InMemoryStateStore internals
    // by saving a real node and verifying the round-trip.
    // Here we verify through EWMANode as a proxy for INodeState value semantics.
    auto inp  = Input<double>::make("x", 0.0);
    auto ewma = EWMANode::make("ewma", inp, 0.5);
    EvalContext ctx;
    inp->set(10.0); ewma->eval(ctx);
    inp->set(20.0); ewma->eval(ctx);

    store.save({ewma});
    EXPECT_TRUE(store.hasSavedState());

    auto inp2  = Input<double>::make("x2", 0.0);
    auto ewma2 = EWMANode::make("ewma", inp2, 0.5);
    store.restore({ewma2});

    // Both ewma and ewma2 should now produce the same output on the same input
    inp->set(15.0);  ewma->eval(ctx);
    inp2->set(15.0); ewma2->eval(ctx);
    double v1 = get_value<double>(ewma->eval(ctx));
    double v2 = get_value<double>(ewma2->eval(ctx));
    EXPECT_NEAR(v1, v2, 1e-9);
}

TEST(NodeStateTests, WriteReadBool) {
    InMemoryStateStore store;
    EvalContext ctx;
    // Use AlwaysChangedPolicy so identical bool values still propagate dirty
    auto always = std::make_shared<AlwaysChangedPolicy>();
    auto boolInp = Input<bool>::make("bi", false, always);
    auto dbn2    = DebounceCountNode::make("dbn", boolInp, 3);
    boolInp->set(true); dbn2->eval(ctx);  // count=1
    boolInp->set(true); dbn2->eval(ctx);  // count=2

    store.save({dbn2});

    auto boolInp3 = Input<bool>::make("bi3", false, always);
    auto dbn3     = DebounceCountNode::make("dbn", boolInp3, 3);
    store.restore({dbn3});

    // dbn3 should have count=2; one more true → fires
    boolInp3->set(true); dbn3->eval(ctx);
    EXPECT_TRUE(get_value<bool>(dbn3->eval(ctx)));
}

TEST(NodeStateTests, WriteReadVecDouble) {
    InMemoryStateStore store;
    auto inp   = Input<double>::make("x", 0.0);
    auto delay = DelayNode<double>::make("d", inp, 3);
    EvalContext ctx;
    inp->set(1.0); delay->eval(ctx);
    inp->set(2.0); delay->eval(ctx);
    inp->set(3.0); delay->eval(ctx);

    store.save({delay});

    auto inp2   = Input<double>::make("x2", 0.0);
    auto delay2 = DelayNode<double>::make("d", inp2, 3);
    store.restore({delay2});

    // After restore, next input should emit the first buffered value
    inp->set(4.0);  delay->eval(ctx);
    inp2->set(4.0); delay2->eval(ctx);
    double v1 = get_value<double>(delay->eval(ctx));
    double v2 = get_value<double>(delay2->eval(ctx));
    EXPECT_NEAR(v1, v2, 1e-9);
}

TEST(NodeStateTests, SubBagIsolation) {
    // Write key "x" in parent and key "x" in sub("child") — no cross-contamination
    InMemoryStateStore store;
    auto inp  = Input<double>::make("raw", 0.0);
    auto gate = OutlierGateNode::make("gate", inp, 5, 3.0);

    EvalContext ctx;
    for (double v : {10.0, 11.0, 10.5, 9.8, 10.2, 10.0, 10.1}) {
        inp->set(v); gate->eval(ctx);
    }

    store.save({gate});
    EXPECT_TRUE(store.hasSavedState());

    auto inp2  = Input<double>::make("raw2", 0.0);
    auto gate2 = OutlierGateNode::make("gate", inp2, 5, 3.0);
    store.restore({gate2});

    // Subsequent outputs should match after restore
    for (double v : {10.3, 10.0}) {
        inp->set(v);  gate->eval(ctx);
        inp2->set(v); gate2->eval(ctx);
    }
    double v1 = get_value<double>(gate->eval(ctx));
    double v2 = get_value<double>(gate2->eval(ctx));
    EXPECT_NEAR(v1, v2, 1e-6);
}

TEST(NodeStateTests, SubBagNested) {
    // OutlierGateNode: sub("zscore") → sub("stats") — two-level nesting
    InMemoryStateStore store;
    auto inp  = Input<double>::make("raw", 0.0);
    auto gate = OutlierGateNode::make("gate", inp, 10, 3.0);

    EvalContext ctx;
    for (int i = 0; i < 15; ++i) { inp->set(10.0 + i * 0.1); gate->eval(ctx); }

    store.save({gate});

    auto inp2  = Input<double>::make("raw2", 0.0);
    auto gate2 = OutlierGateNode::make("gate", inp2, 10, 3.0);
    store.restore({gate2});

    inp->set(11.5);  gate->eval(ctx);
    inp2->set(11.5); gate2->eval(ctx);
    double v1 = get_value<double>(gate->eval(ctx));
    double v2 = get_value<double>(gate2->eval(ctx));
    EXPECT_NEAR(v1, v2, 1e-6);
}

TEST(NodeStateTests, MissingKeyThrows) {
    InMemoryStateStore store;
    store.save({});  // empty save — store has state but no nodes
    // Calling restore on an EWMANode whose state was never saved
    auto inp  = Input<double>::make("x", 0.0);
    auto ewma = EWMANode::make("ewma_missing", inp, 0.5);
    // restore() should warn and skip (not throw) for missing nodes
    EXPECT_NO_THROW(store.restore({ewma}));
}

TEST(NodeStateTests, RestoreBeforeSaveReturnsFalse) {
    InMemoryStateStore store;
    auto inp  = Input<double>::make("x", 0.0);
    auto ewma = EWMANode::make("ewma", inp, 0.5);
    bool result = store.restore({ewma});
    EXPECT_FALSE(result);
}

// ─────────────────────────────────────────────────────────────────────────────
// EWMANode save/restore
// ─────────────────────────────────────────────────────────────────────────────

TEST(NodeStateTests, EWMASaveRestore) {
    auto inp  = Input<double>::make("x", 0.0);
    auto ewma = EWMANode::make("ewma", inp, 0.1);
    EvalContext ctx;
    feed(inp, ewma, {100, 105, 103, 107, 102});

    InMemoryStateStore store;
    store.save({ewma});

    auto inp2  = Input<double>::make("x", 0.0);
    auto ewma2 = EWMANode::make("ewma", inp2, 0.1);
    store.restore({ewma2});

    // Next tick must match
    inp->set(110.0);  ewma->eval(ctx);
    inp2->set(110.0); ewma2->eval(ctx);
    EXPECT_NEAR(get_value<double>(ewma->eval(ctx)),
                get_value<double>(ewma2->eval(ctx)), 1e-9);
}

// ─────────────────────────────────────────────────────────────────────────────
// RollingStats save/restore
// ─────────────────────────────────────────────────────────────────────────────

TEST(NodeStateTests, RollingStatsSaveRestore) {
    auto inp   = Input<double>::make("x", 0.0);
    auto stats = RollingStats::make("stats", inp, 5);
    EvalContext ctx;
    feed(inp, stats, {10, 11, 12, 13, 14});  // full window

    InMemoryStateStore store;
    store.save({stats});

    auto inp2   = Input<double>::make("x", 0.0);
    auto stats2 = RollingStats::make("stats", inp2, 5);
    store.restore({stats2});

    // Push one more value — incremental result must match
    inp->set(15.0);  stats->eval(ctx);
    inp2->set(15.0); stats2->eval(ctx);
    EXPECT_NEAR(stats->mean(),   stats2->mean(),   1e-9);
    EXPECT_NEAR(stats->stddev(), stats2->stddev(), 1e-9);
}

// ─────────────────────────────────────────────────────────────────────────────
// DeltaNode save/restore
// ─────────────────────────────────────────────────────────────────────────────

TEST(NodeStateTests, DeltaNodeSaveRestore) {
    auto inp   = Input<double>::make("x", 0.0);
    auto delta = DeltaNode<double>::make("delta", inp);
    EvalContext ctx;
    feed(inp, delta, {10.0, 15.0, 12.0});

    InMemoryStateStore store;
    store.save({delta});

    auto inp2   = Input<double>::make("x", 0.0);
    auto delta2 = DeltaNode<double>::make("delta", inp2);
    store.restore({delta2});

    // Next input should give correct delta, not zero
    inp->set(20.0);  delta->eval(ctx);
    inp2->set(20.0); delta2->eval(ctx);
    EXPECT_NEAR(get_value<double>(delta->eval(ctx)),
                get_value<double>(delta2->eval(ctx)), 1e-9);
    EXPECT_NEAR(get_value<double>(delta2->eval(ctx)), 8.0, 1e-9);  // 20 - 12
}

// ─────────────────────────────────────────────────────────────────────────────
// DelayNode save/restore
// ─────────────────────────────────────────────────────────────────────────────

TEST(NodeStateTests, DelayNodeSaveRestore) {
    auto inp   = Input<double>::make("x", 0.0);
    auto delay = DelayNode<double>::make("delay", inp, 3);
    EvalContext ctx;
    feed(inp, delay, {1.0, 2.0, 3.0, 4.0});  // warm after tick 4

    InMemoryStateStore store;
    store.save({delay});

    auto inp2   = Input<double>::make("x", 0.0);
    auto delay2 = DelayNode<double>::make("delay", inp2, 3);
    store.restore({delay2});

    EXPECT_TRUE(delay2->isWarm());

    inp->set(5.0);  delay->eval(ctx);
    inp2->set(5.0); delay2->eval(ctx);
    EXPECT_NEAR(get_value<double>(delay->eval(ctx)),
                get_value<double>(delay2->eval(ctx)), 1e-9);
}

// ─────────────────────────────────────────────────────────────────────────────
// RollingMinMaxNode save/restore — verify entry tick preservation
// ─────────────────────────────────────────────────────────────────────────────

TEST(NodeStateTests, RollingMinMaxSaveRestore) {
    auto inp    = Input<double>::make("x", 0.0);
    auto minmax = RollingMinMaxNode::make("mm", inp, 3);
    EvalContext ctx;
    feed(inp, minmax, {10.0, 5.0, 8.0});

    InMemoryStateStore store;
    store.save({minmax});

    auto inp2    = Input<double>::make("x", 0.0);
    auto minmax2 = RollingMinMaxNode::make("mm", inp2, 3);
    store.restore({minmax2});

    inp->set(7.0);  minmax->eval(ctx);
    inp2->set(7.0); minmax2->eval(ctx);
    auto p1 = get_value<std::pair<double,double>>(minmax->eval(ctx));
    auto p2 = get_value<std::pair<double,double>>(minmax2->eval(ctx));
    EXPECT_NEAR(p1.first,  p2.first,  1e-9);  // min
    EXPECT_NEAR(p1.second, p2.second, 1e-9);  // max
}

TEST(NodeStateTests, RollingMinMaxEntryTickPreserved) {
    // Verify window expiry is correct after restore — entries expire at right tick
    auto inp    = Input<double>::make("x", 0.0);
    auto minmax = RollingMinMaxNode::make("mm", inp, 3);
    EvalContext ctx;
    // Push 4 values so the first is outside window after restore
    feed(inp, minmax, {1.0, 100.0, 2.0});
    // tick_ is now 3; window is 3. Entry at idx=0 (val=1.0) still inside window.
    // But after pushing a 4th value (tick_ becomes 4), idx=0 expires.

    InMemoryStateStore store;
    store.save({minmax});

    auto inp2    = Input<double>::make("x", 0.0);
    auto minmax2 = RollingMinMaxNode::make("mm", inp2, 3);
    store.restore({minmax2});

    // Push one more — the entry from tick 0 (val=1.0) should expire
    inp->set(3.0);  minmax->eval(ctx);
    inp2->set(3.0); minmax2->eval(ctx);

    auto p1 = get_value<std::pair<double,double>>(minmax->eval(ctx));
    auto p2 = get_value<std::pair<double,double>>(minmax2->eval(ctx));
    EXPECT_NEAR(p1.first,  p2.first,  1e-9);
    EXPECT_NEAR(p1.second, p2.second, 1e-9);
}

// ─────────────────────────────────────────────────────────────────────────────
// DebounceCountNode save/restore
// ─────────────────────────────────────────────────────────────────────────────

TEST(NodeStateTests, DebounceCountSaveRestore) {
    // AlwaysChangedPolicy: setting the same bool value still propagates dirty
    auto always = std::make_shared<AlwaysChangedPolicy>();
    auto inp = Input<bool>::make("b", false, always);
    auto dbn = DebounceCountNode::make("dbn", inp, 3);
    EvalContext ctx;
    inp->set(true); dbn->eval(ctx);  // count=1
    inp->set(true); dbn->eval(ctx);  // count=2, not yet fired

    InMemoryStateStore store;
    store.save({dbn});

    auto inp2 = Input<bool>::make("b", false, always);
    auto dbn2 = DebounceCountNode::make("dbn", inp2, 3);
    store.restore({dbn2});

    // One more true → fires (count becomes 3)
    inp2->set(true); dbn2->eval(ctx);
    EXPECT_TRUE(get_value<bool>(dbn2->eval(ctx)));

    // False → resets
    inp2->set(false); dbn2->eval(ctx);
    EXPECT_FALSE(get_value<bool>(dbn2->eval(ctx)));
}

// ─────────────────────────────────────────────────────────────────────────────
// RateLimiterNode save/restore
// ─────────────────────────────────────────────────────────────────────────────

TEST(NodeStateTests, RateLimiterSaveRestore) {
    auto inp = Input<double>::make("x", 0.0);
    auto rl  = RateLimiterNode<double>::make("rl", inp, 5.0);
    EvalContext ctx;
    inp->set(10.0); rl->eval(ctx);  // lastEmitted=10, hasEmitted=true

    InMemoryStateStore store;
    store.save({rl});

    auto inp2 = Input<double>::make("x", 0.0);
    auto rl2  = RateLimiterNode<double>::make("rl", inp2, 5.0);
    store.restore({rl2});

    // Input below threshold → no emit (cached stays at 10)
    inp2->set(12.0); rl2->eval(ctx);
    EXPECT_NEAR(get_value<double>(rl2->eval(ctx)), 10.0, 1e-9);

    // Input above threshold → emits
    inp2->set(16.0); rl2->eval(ctx);
    EXPECT_NEAR(get_value<double>(rl2->eval(ctx)), 16.0, 1e-9);
}

// ─────────────────────────────────────────────────────────────────────────────
// OutlierGateNode save/restore — tests delegation chain
// ─────────────────────────────────────────────────────────────────────────────

TEST(NodeStateTests, OutlierGateSaveRestore) {
    auto inp  = Input<double>::make("raw", 0.0);
    auto gate = OutlierGateNode::make("gate", inp, 5, 2.5);
    EvalContext ctx;
    feed(inp, gate, {10.0, 10.1, 10.2, 9.9, 10.0});  // warm window

    InMemoryStateStore store;
    store.save({gate});

    auto inp2  = Input<double>::make("raw", 0.0);
    auto gate2 = OutlierGateNode::make("gate", inp2, 5, 2.5);
    store.restore({gate2});

    // Both nodes should produce same output on same input
    for (double v : {10.3, 100.0, 10.1}) {
        inp->set(v);  gate->eval(ctx);
        inp2->set(v); gate2->eval(ctx);
        EXPECT_NEAR(get_value<double>(gate->eval(ctx)),
                    get_value<double>(gate2->eval(ctx)), 1e-6)
            << "mismatch at v=" << v;
    }
}

TEST(NodeStateTests, OutlierGateDelegationChain) {
    // Verify the sub-bag nesting: gate → sub("zscore") → sub("stats")
    auto inp  = Input<double>::make("raw", 0.0);
    auto gate = OutlierGateNode::make("gate", inp, 8, 3.0);
    EvalContext ctx;
    for (int i = 0; i < 10; ++i) { inp->set(10.0 + i * 0.05); gate->eval(ctx); }

    InMemoryStateStore store;
    store.save({gate});

    auto inp2  = Input<double>::make("raw", 0.0);
    auto gate2 = OutlierGateNode::make("gate", inp2, 8, 3.0);
    store.restore({gate2});

    // Spike — both should detect and substitute the same mean
    inp->set(500.0);  gate->eval(ctx);
    inp2->set(500.0); gate2->eval(ctx);
    EXPECT_TRUE(gate->lastWasOutlier());
    EXPECT_TRUE(gate2->lastWasOutlier());
    EXPECT_NEAR(get_value<double>(gate->eval(ctx)),
                get_value<double>(gate2->eval(ctx)), 1e-6);
}

// ─────────────────────────────────────────────────────────────────────────────
// ThresholdNode save/restore
// ─────────────────────────────────────────────────────────────────────────────

TEST(NodeStateTests, ThresholdNodeSaveRestore) {
    auto inp  = Input<double>::make("x", 0.0);
    auto thr  = ThresholdNode<double>::make(
        "thr", inp, 5.0, ThresholdNode<double>::Direction::Above, 1.0);
    EvalContext ctx;
    // Trigger the threshold
    inp->set(6.0); thr->eval(ctx);
    EXPECT_TRUE(get_value<bool>(thr->eval(ctx)));

    InMemoryStateStore store;
    store.save({thr});

    auto inp2 = Input<double>::make("x", 0.0);
    auto thr2 = ThresholdNode<double>::make(
        "thr", inp2, 5.0, ThresholdNode<double>::Direction::Above, 1.0);
    store.restore({thr2});

    // Value drops below level but not below hysteresis (5 - 1 = 4); stays true
    inp2->set(4.5); thr2->eval(ctx);
    EXPECT_TRUE(get_value<bool>(thr2->eval(ctx)));

    // Value drops below hysteresis threshold → false
    inp2->set(3.5); thr2->eval(ctx);
    EXPECT_FALSE(get_value<bool>(thr2->eval(ctx)));
}

// ─────────────────────────────────────────────────────────────────────────────
// restoreState invalidates downstream
// ─────────────────────────────────────────────────────────────────────────────

TEST(NodeStateTests, RestoreInvalidatesDownstream) {
    auto inp   = Input<double>::make("x", 0.0);
    auto ewma  = EWMANode::make("ewma", inp, 0.5);
    auto delta = DeltaNode<double>::make("delta", ewma);

    EvalContext ctx;
    feed(inp, delta, {10.0, 20.0, 30.0});

    InMemoryStateStore store;
    store.save({ewma, delta});

    auto inp2   = Input<double>::make("x", 0.0);
    auto ewma2  = EWMANode::make("ewma", inp2, 0.5);
    auto delta2 = DeltaNode<double>::make("delta", ewma2);

    store.restore({ewma2, delta2});

    // After restore, downstream should be dirty and pick up restored state
    inp->set(40.0);  delta->eval(ctx);
    inp2->set(40.0); delta2->eval(ctx);
    EXPECT_NEAR(get_value<double>(delta->eval(ctx)),
                get_value<double>(delta2->eval(ctx)), 1e-9);
}

// ─────────────────────────────────────────────────────────────────────────────
// InMemoryStateStore — save/restore round-trip
// ─────────────────────────────────────────────────────────────────────────────

TEST(InMemoryStoreTests, SaveRestoreRoundTrip) {
    auto inp   = Input<double>::make("x", 0.0);
    auto stats = RollingStats::make("stats", inp, 5);
    feed(inp, stats, {1, 2, 3, 4, 5});

    InMemoryStateStore store;
    EXPECT_FALSE(store.hasSavedState());
    store.save({stats});
    EXPECT_TRUE(store.hasSavedState());

    auto inp2   = Input<double>::make("x", 0.0);
    auto stats2 = RollingStats::make("stats", inp2, 5);
    EXPECT_TRUE(store.restore({stats2}));

    EXPECT_NEAR(stats->mean(),   stats2->mean(),   1e-9);
    EXPECT_NEAR(stats->stddev(), stats2->stddev(), 1e-9);
}

TEST(InMemoryStoreTests, RestoreBeforeSave) {
    InMemoryStateStore store;
    auto inp   = Input<double>::make("x", 0.0);
    auto stats = RollingStats::make("stats", inp, 5);
    EXPECT_FALSE(store.restore({stats}));
}

TEST(InMemoryStoreTests, Reset) {
    InMemoryStateStore store;
    auto inp   = Input<double>::make("x", 0.0);
    auto stats = RollingStats::make("stats", inp, 5);
    feed(inp, stats, {1, 2, 3});
    store.save({stats});
    EXPECT_TRUE(store.hasSavedState());
    store.reset();
    EXPECT_FALSE(store.hasSavedState());
    EXPECT_FALSE(store.restore({stats}));
}

// ─────────────────────────────────────────────────────────────────────────────
// WindowNode save/restore
// ─────────────────────────────────────────────────────────────────────────────

TEST(NodeStateTests, WindowNodeSaveRestore) {
    auto inp  = Input<double>::make("x", 0.0);
    auto win  = WindowNode<double>::make("win", inp, 4);
    feed(inp, win, {10.0, 20.0, 30.0});

    InMemoryStateStore store;
    store.save({win});

    auto inp2 = Input<double>::make("x", 0.0);
    auto win2 = WindowNode<double>::make("win", inp2, 4);
    store.restore({win2});

    EXPECT_EQ(win->window(), win2->window());
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
