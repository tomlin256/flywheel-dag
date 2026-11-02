// =============================================================================
// flywheel-dag — A header-only C++17 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

// test_wake_state.cpp — unit tests for dag::async::detail::BasicWakeState, the flags, mutex
// and condition variable that Engine::run() waits on and that a source's wake hook, poke() and
// stop() set (flywheel-dag#43).
//
// Coverage
// ────────
//  The flags        — a work signal is consumed by the wait that sees it, a stop ends the wait,
//                     and a second run cannot claim the run flag
//  Real primitives  — a stop wakes a waiter on std::mutex and std::condition_variable

#include <gtest/gtest.h>

#include "flywheel/dag_engine.hpp"

#include <condition_variable>
#include <future>
#include <mutex>
#include <thread>

using dag::async::detail::BasicWakeState;

namespace {

using RealWakeState = BasicWakeState<std::mutex, std::condition_variable>;

// The flags are protected, so a test reads the work flag through a subclass.
struct ObservedWakeState : RealWakeState {
    bool hasWork() const { return hasWork_; }
};

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// The flags
// ─────────────────────────────────────────────────────────────────────────────

TEST(WakeState, WorkSignalledBeforeTheWaitIsConsumedOnce) {
    ObservedWakeState wake;
    ASSERT_TRUE(wake.beginRun());

    wake.signalWork();
    EXPECT_TRUE(wake.hasWork());
    EXPECT_TRUE(wake.waitForWork());
    EXPECT_FALSE(wake.hasWork());   // consumed: the next wait blocks until new work or a stop
}

TEST(WakeState, AStopBeforeTheWaitEndsIt) {
    ObservedWakeState wake;
    ASSERT_TRUE(wake.beginRun());

    wake.requestStop();

    EXPECT_FALSE(wake.waitForWork());
    EXPECT_FALSE(wake.running());
}

// run() throws when beginRun() is refused, so the refusal must leave the flag of the run already
// going alone, or that run would stop being one.
TEST(WakeState, ASecondBeginRunIsRefusedAndLeavesTheFlagSet) {
    RealWakeState wake;
    EXPECT_FALSE(wake.running());

    EXPECT_TRUE(wake.beginRun());
    EXPECT_FALSE(wake.beginRun());
    EXPECT_TRUE(wake.running());

    wake.endRun();
    EXPECT_FALSE(wake.running());
    EXPECT_TRUE(wake.beginRun());   // free again
}

// ─────────────────────────────────────────────────────────────────────────────
// Real primitives
// ─────────────────────────────────────────────────────────────────────────────

// Whether the waiter is blocked yet or only about to be, a stop must end its wait. One that never
// notified would leave it asleep for ever, which ctest reports as a timeout, not a wrong value.
TEST(WakeState, AStopWakesAWaiterOnTheRealPrimitives) {
    RealWakeState wake;
    ASSERT_TRUE(wake.beginRun());

    std::promise<void> started;
    bool worked = true;
    std::thread waiter([&] {
        started.set_value();
        worked = wake.waitForWork();
    });
    started.get_future().wait();
    wake.requestStop();
    waiter.join();

    EXPECT_FALSE(worked);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
