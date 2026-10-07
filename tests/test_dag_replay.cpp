// =============================================================================
// flywheel-dag — A header-only C++17 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

// test_dag_replay.cpp — deterministic replay primitives.
//
// ReplayCoordinator + ReplayClock:
//   - one group delivered per flush; currentSeq advances in order
//   - wake hook fired every cycle while groups or drain cycles remain
//   - exhausted callback fires exactly once, after the configured drain
//   - empty schedule terminates cleanly
//   - clock delta mapping + monotonicity
//   - reset() rewinds
//   - Engine::run() completes unattended, with stop() called from the
//     exhausted callback
//
// ReplayInput<T> + ReplayQueue<T>: delivery gated on the coordinator's cursor.

#include <gtest/gtest.h>

#include <flywheel/dag_replay.hpp>
#include <flywheel/dag_engine.hpp>

#include <chrono>

namespace {

using namespace dag::async;
using Group = ReplayCoordinator::Group;

// ── ReplayCoordinator: cursor ──────────────────────────────────────────────────

TEST(ReplayCoordinator, OneGroupPerFlush) {
    auto coord = ReplayCoordinator::make({{1, 100}, {2, 200}, {5, 500}}, /*drain*/ 1);
    EXPECT_EQ(coord->currentSeq(), 0u);          // before first flush
    EXPECT_FALSE(coord->exhausted());

    coord->flush();
    EXPECT_EQ(coord->currentSeq(), 1u);
    coord->flush();
    EXPECT_EQ(coord->currentSeq(), 2u);
    coord->flush();
    EXPECT_EQ(coord->currentSeq(), 5u);
    EXPECT_FALSE(coord->exhausted());            // last group just delivered

    coord->flush();                              // first drain cycle
    EXPECT_TRUE(coord->exhausted());
    EXPECT_EQ(coord->currentSeq(), 0u);          // sentinel during drain
}

TEST(ReplayCoordinator, PendingCountCountsRemainingGroups) {
    auto coord = ReplayCoordinator::make({{1, 100}, {2, 200}}, 1);
    EXPECT_EQ(coord->pendingCount(), 2u);
    coord->flush();
    EXPECT_EQ(coord->pendingCount(), 1u);
    coord->flush();
    EXPECT_EQ(coord->pendingCount(), 0u);
    coord->flush();                              // drain — still 0
    EXPECT_EQ(coord->pendingCount(), 0u);
}

// ── ReplayCoordinator: self-chaining wake ──────────────────────────────────────

TEST(ReplayCoordinator, WakeHookFiredWhileWorkRemains) {
    auto coord = ReplayCoordinator::make({{1, 100}, {2, 200}}, /*drain*/ 1);
    int wakes = 0;
    coord->setWakeHook([&] { ++wakes; });

    coord->flush();  // G1  → wake
    coord->flush();  // G2  → wake
    coord->flush();  // drain → wake
    EXPECT_EQ(wakes, 3);

    coord->flush();  // exhausted → NO wake (app stops from the callback instead)
    EXPECT_EQ(wakes, 3);
}

// ── ReplayCoordinator: exhausted callback ──────────────────────────────────────

TEST(ReplayCoordinator, ExhaustedFiresOnceAfterDrain) {
    auto coord = ReplayCoordinator::make({{1, 100}, {2, 200}}, /*drain*/ 2);
    int exhausted = 0;
    coord->setExhaustedCallback([&] { ++exhausted; });

    // 2 groups + 2 drain cycles must pass before the callback fires.
    for (int i = 0; i < 4; ++i) {
        coord->flush();
        EXPECT_EQ(exhausted, 0) << "fired too early at flush " << i;
    }
    coord->flush();                 // 5th flush: drain spent → exhausted fires
    EXPECT_EQ(exhausted, 1);

    coord->flush();                 // never re-fires
    coord->flush();
    EXPECT_EQ(exhausted, 1);
}

TEST(ReplayCoordinator, EmptyScheduleTerminates) {
    auto coord = ReplayCoordinator::make({}, /*drain*/ 1);
    int exhausted = 0;
    coord->setExhaustedCallback([&] { ++exhausted; });
    EXPECT_EQ(coord->scheduleSize(), 0u);

    coord->flush();                 // straight to drain
    EXPECT_TRUE(coord->exhausted());
    EXPECT_EQ(exhausted, 0);
    coord->flush();                 // exhausted fires
    EXPECT_EQ(exhausted, 1);
}

TEST(ReplayCoordinator, ResetRewinds) {
    auto coord = ReplayCoordinator::make({{1, 100}, {2, 200}}, 1);
    for (int i = 0; i < 4; ++i) coord->flush();   // run to exhaustion
    EXPECT_TRUE(coord->exhausted());

    coord->reset();
    EXPECT_FALSE(coord->exhausted());
    EXPECT_EQ(coord->currentSeq(), 0u);
    coord->flush();
    EXPECT_EQ(coord->currentSeq(), 1u);           // delivers G1 again
}

// ── ReplayClock ────────────────────────────────────────────────────────────────

TEST(ReplayClock, DeltaMappingAndMonotonic) {
    auto coord = ReplayCoordinator::make({{1, 1000}, {2, 2000}, {3, 5000}}, /*drain*/ 0);
    auto clk   = coord->clock();

    const auto base = clk->now();                 // anchored at the first group's ts
    coord->flush();                               // ts = 1000 → offset 0
    EXPECT_EQ(clk->now(), base);
    coord->flush();                               // ts = 2000 → +1000 µs
    EXPECT_EQ(clk->now(), base + std::chrono::microseconds(1000));
    coord->flush();                               // ts = 5000 → +4000 µs
    EXPECT_EQ(clk->now(), base + std::chrono::microseconds(4000));
    EXPECT_GE(clk->now(), base);                  // never runs backward
}

// ── Unattended Engine::run() ───────────────────────────────────────────────────

TEST(ReplayCoordinator, EngineRunCompletesUnattended) {
    Engine engine;
    auto coord = ReplayCoordinator::make({{1, 100}, {2, 200}, {3, 300}}, /*drain*/ 1);

    int exhausted = 0;
    coord->setExhaustedCallback([&] {
        ++exhausted;
        engine.stop();                            // safe from a flush on the eval thread
    });
    engine.addSource(coord);

    // Must return unattended: without the coordinator's self-chaining wake, the
    // run loop would block on its condition variable forever, which ctest reports
    // as a timeout, not a wrong value.
    engine.run();

    EXPECT_EQ(exhausted, 1);
    EXPECT_EQ(engine.cycleCount(), 5u);           // 3 groups + 1 drain + 1 exhaust-detect
}

// A coordinator the engine already flushes inside a registry is not added again: flushed twice,
// it would advance two groups in a cycle, and a replayed source would never see the one between
// (flywheel-dag#36).
TEST(ReplayCoordinator, ACoordinatorAddedAgainAdvancesOneGroupPerCycle) {
    Engine engine;
    auto coord = ReplayCoordinator::make({{1, 100}, {2, 200}, {3, 300}}, /*drain*/ 1);
    auto registry = std::make_shared<FeedRegistry>();
    registry->add(coord);
    engine.addSource(registry);
    engine.addSource(coord);   // the registry holds it already

    engine.step();
    EXPECT_EQ(coord->currentSeq(), 1u);
    engine.step();
    EXPECT_EQ(coord->currentSeq(), 2u);
}

// ═══════════════════════════════════════════════════════════════════════════════
// ReplayInput<T> + ReplayQueue<T>
// ═══════════════════════════════════════════════════════════════════════════════

std::vector<Group> scheduleFromSeqs(const std::vector<std::uint64_t>& seqs) {
    std::vector<Group> g;
    g.reserve(seqs.size());
    for (auto s : seqs) g.push_back({s, static_cast<std::int64_t>(s) * 1000});
    return g;
}

// Drive an engine to completion via the coordinator's self-chaining wake.
void runToExhaustion(Engine& engine, const ReplayCoordinatorPtr& coord) {
    coord->setExhaustedCallback([&] { engine.stop(); });
    engine.run();
}

TEST(ReplayInput, InOrderDelivery) {
    auto coord = ReplayCoordinator::make(scheduleFromSeqs({1, 2, 3}));
    auto price = ReplayInput<double>::make(
        "price", {{1, 10.0}, {2, 20.0}, {3, 30.0}}, coord);

    Engine engine;
    auto reg = std::make_shared<FeedRegistry>();
    reg->add(coord);
    reg->add(price);
    engine.addSource(reg);

    std::vector<double> seq;
    engine.addOutput<double>(price, [&](const double& v) { seq.push_back(v); });
    runToExhaustion(engine, coord);

    EXPECT_EQ(seq, (std::vector<double>{10.0, 20.0, 30.0}));
}

TEST(ReplayInput, RepeatedValueEqualitySuppressed) {
    // The replay input applies its equality policy, as AsyncInput does: the
    // middle 5.0 is dropped.
    auto coord = ReplayCoordinator::make(scheduleFromSeqs({1, 2, 3}));
    auto price = ReplayInput<double>::make(
        "price", {{1, 5.0}, {2, 5.0}, {3, 7.0}}, coord);

    Engine engine;
    auto reg = std::make_shared<FeedRegistry>();
    reg->add(coord);
    reg->add(price);
    engine.addSource(reg);

    std::vector<double> seq;
    engine.addOutput<double>(price, [&](const double& v) { seq.push_back(v); });
    runToExhaustion(engine, coord);

    EXPECT_EQ(seq, (std::vector<double>{5.0, 7.0}));   // repeat suppressed
}

TEST(ReplayQueue, BatchBoundariesPreserved) {
    auto coord = ReplayCoordinator::make(scheduleFromSeqs({1, 2}));
    auto trades = ReplayQueue<int>::make(
        "trades", {{1, {10, 11, 12}}, {2, {20}}}, coord);

    Engine engine;
    auto reg = std::make_shared<FeedRegistry>();
    reg->add(coord);
    reg->add(trades);
    engine.addSource(reg);

    std::vector<std::vector<int>> batches;
    engine.addOutput<std::vector<int>>(
        trades, [&](const std::vector<int>& b) { batches.push_back(b); });
    runToExhaustion(engine, coord);

    ASSERT_EQ(batches.size(), 2u);
    EXPECT_EQ(batches[0], (std::vector<int>{10, 11, 12}));   // 3-item batch intact
    EXPECT_EQ(batches[1], (std::vector<int>{20}));           // never merged
}

TEST(ReplayAlignment, TwoInputsOneQueueInterleavedSeqs) {
    // price @ 1,3   spread @ 2,3   trades @ 1,2 — each must fire on its own cycles.
    auto coord  = ReplayCoordinator::make(scheduleFromSeqs({1, 2, 3}));
    auto price  = ReplayInput<double>::make("price",  {{1, 100.0}, {3, 101.0}}, coord);
    auto spread = ReplayInput<double>::make("spread", {{2, 0.5},   {3, 0.6}},   coord);
    auto trades = ReplayQueue<int>::make("trades",    {{1, {10, 11, 12}}, {2, {20}}}, coord);

    Engine engine;
    auto reg = std::make_shared<FeedRegistry>();
    reg->add(coord);            // coordinator FIRST
    reg->add(price);
    reg->add(spread);
    reg->add(trades);
    engine.addSource(reg);

    std::vector<std::pair<std::uint64_t, double>> priceSeq, spreadSeq;
    std::vector<std::uint64_t>                    tradeCycles;
    engine.addOutput<double>(price,  [&](const double& v) { priceSeq.push_back({engine.cycleCount(), v}); });
    engine.addOutput<double>(spread, [&](const double& v) { spreadSeq.push_back({engine.cycleCount(), v}); });
    engine.addOutput<std::vector<int>>(
        trades, [&](const std::vector<int>&) { tradeCycles.push_back(engine.cycleCount()); });
    runToExhaustion(engine, coord);

    using PV = std::pair<std::uint64_t, double>;
    // price and trades have a seq-1 sample, so they first fire real data on cycle 1.
    EXPECT_EQ(priceSeq,  (std::vector<PV>{{1, 100.0}, {3, 101.0}}));
    EXPECT_EQ(tradeCycles, (std::vector<std::uint64_t>{1, 2}));
    // spread's first sample is at seq 2, so on cycle 1 it fires its INITIAL value
    // (0.0): every node starts dirty and fires once on its first eval, as the
    // AsyncInput it replays does live.
    EXPECT_EQ(spreadSeq, (std::vector<PV>{{1, 0.0}, {2, 0.5}, {3, 0.6}}));
}

TEST(ReplayAlignment, ShorterStreamIdlesWhileOthersContinue) {
    auto coord  = ReplayCoordinator::make(scheduleFromSeqs({1, 2, 3}));
    auto price  = ReplayInput<double>::make("price",  {{1, 10.0}, {2, 20.0}, {3, 30.0}}, coord);
    auto spread = ReplayInput<double>::make("spread", {{1, 0.5}}, coord);   // only seq 1

    Engine engine;
    auto reg = std::make_shared<FeedRegistry>();
    reg->add(coord);
    reg->add(price);
    reg->add(spread);
    engine.addSource(reg);

    std::vector<double> priceSeq, spreadSeq;
    engine.addOutput<double>(price,  [&](const double& v) { priceSeq.push_back(v); });
    engine.addOutput<double>(spread, [&](const double& v) { spreadSeq.push_back(v); });
    runToExhaustion(engine, coord);

    EXPECT_EQ(priceSeq,  (std::vector<double>{10.0, 20.0, 30.0}));
    EXPECT_EQ(spreadSeq, (std::vector<double>{0.5}));   // idled after its single event
}

TEST(ReplayInput, StepDrivenMiniSession) {
    auto coord = ReplayCoordinator::make(scheduleFromSeqs({1, 2, 3}));
    auto price = ReplayInput<double>::make(
        "price", {{1, 10.0}, {2, 20.0}, {3, 30.0}}, coord);

    Engine engine;
    auto reg = std::make_shared<FeedRegistry>();
    reg->add(coord);
    reg->add(price);
    engine.addSource(reg);

    std::vector<double> seq;
    engine.addOutput<double>(price, [&](const double& v) { seq.push_back(v); });

    for (int i = 0; i < 3; ++i) engine.step();   // deliver all three groups synchronously
    EXPECT_EQ(seq, (std::vector<double>{10.0, 20.0, 30.0}));
    EXPECT_EQ(price->current(), 30.0);
}

}  // namespace

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
