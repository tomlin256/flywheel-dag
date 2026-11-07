// =============================================================================
// flywheel-dag — A header-only C++23 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

// test_wake_state.cpp — unit tests for dag::async::detail::BasicWakeState, the flags, mutex
// and condition variable that Engine::run() waits on and that a source's wake hook, poke() and
// stop() set.
//
// Coverage
// ────────
//  The flags        — a work signal is consumed by the wait that sees it, a stop ends the wait,
//                     and a second run cannot claim the run flag
//  The window       — a stop or a work signal that comes between the waiter's test of its
//                     predicate and its block waits for the mutex the waiter holds, so its
//                     notification cannot fall in the gap with nobody waiting
//  Real primitives  — a stop wakes a waiter on std::mutex and std::condition_variable

#include <gtest/gtest.h>

#include "flywheel/dag_engine.hpp"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <thread>
#include <utility>

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
// The window between a waiter's test and its block
//
// A condition variable drops a notification that finds nobody waiting, so a write to what the
// waiter's predicate reads must wait for the mutex the waiter holds from its test to its block.
// The gap is a few instructions wide, and no schedule aimed at it from outside reaches it
// reliably. These tests hold the waiter in it.
//
// The waiter must be let go once the writer has either finished or been seen blocked on the mutex.
// "Finished" alone is not enough: a writer that takes the mutex cannot finish while the waiter
// holds it, so waiting for that would wait for ever. A std::mutex cannot say the second, so the
// class is given a mutex that does. Both events reach the test as they happen; nothing here sleeps,
// times out or polls, and the outcome does not depend on scheduling.
// ─────────────────────────────────────────────────────────────────────────────

namespace {

// An event: open() it once, from any thread; wait() returns once it has been opened.
class Latch {
public:
    void open() {
        {
            std::lock_guard<std::mutex> lock(mu_);
            open_ = true;
        }
        cv_.notify_all();
    }

    void wait() {
        std::unique_lock<std::mutex> lock(mu_);
        cv_.wait(lock, [this] { return open_; });
    }

private:
    std::mutex              mu_;
    std::condition_variable cv_;
    bool                    open_ = false;
};

// A mutex that says when a lock() had to wait for it, so that a test learns that a thread is blocked
// on it without waiting to see whether the thread ever comes back.
class ContendedMutex {
public:
    void lock() {
        if (mu_.try_lock()) return;
        if (onContended_) onContended_();
        mu_.lock();
    }
    bool try_lock() { return mu_.try_lock(); }
    void unlock() { mu_.unlock(); }

    // Runs on the thread that found the mutex held. Set before any other thread locks the mutex.
    void onContended(std::function<void()> callback) { onContended_ = std::move(callback); }

private:
    std::mutex            mu_;
    std::function<void()> onContended_;
};

// A condition variable that pauses between its test of the predicate and its block, with the mutex
// locked: the gap in which a notification finds no waiter. It is a condition_variable_any because a
// std::condition_variable waits only on a std::mutex, and it gives the same contract: the lock is
// released and the thread blocks as one step.
class GatedCondVar {
public:
    template<typename Predicate>
    void wait(std::unique_lock<ContendedMutex>& lock, Predicate predicate) {
        while (!predicate()) {
            if (hold_) {
                const std::function<void()> hold = std::move(hold_);
                hold_ = nullptr;
                hold();
            }
            inner_.wait(lock);
        }
    }

    void notify_one() { inner_.notify_one(); }
    void notify_all() { inner_.notify_all(); }

    // Pauses the next wait that finds its predicate false. Set before the waiter starts.
    void holdOnce(std::function<void()> hold) { hold_ = std::move(hold); }

private:
    std::condition_variable_any inner_;
    std::function<void()>       hold_;
};

struct SpyWakeState : BasicWakeState<ContendedMutex, GatedCondVar> {
    ContendedMutex& mutex() { return mu_; }
    GatedCondVar&   condVar() { return cv_; }

    // Frees a waiter whose wake-up was lost, so that a failing test reports and does not hang.
    void rescue() {
        {
            std::lock_guard<ContendedMutex> lock(mu_);
            hasWork_ = true;
        }
        cv_.notify_all();
    }
};

struct WindowOutcome {
    int  writerContentions = 0;   // times the writer found the mutex held
    bool worked = false;          // what the waiter's waitForWork() returned
};

// Holds a waiter between its test and its block, with the mutex locked, runs `writer` on another
// thread, and lets the waiter go on once the writer has returned or has been seen waiting for the
// mutex. The waiter holds the mutex until then, so a writer that takes it always finds it held.
//
// A writer that stored without the mutex returns without touching it, and the waiter then blocks on
// a predicate that is already true: no contention, which the test reports after freeing the waiter.
template<typename Writer>
WindowOutcome writeInTheWindow(SpyWakeState& wake, Writer writer) {
    Latch                        inWindow;        // the waiter holds the mutex, its predicate false
    Latch                        writerSettled;   // the writer has returned, or is waiting for the mutex
    std::atomic<std::thread::id> waiterId{};
    std::atomic<int>             writerContentions{0};

    wake.condVar().holdOnce([&] {
        inWindow.open();
        writerSettled.wait();
    });
    wake.mutex().onContended([&] {
        if (std::this_thread::get_id() == waiterId.load()) return;   // the waiter's own lock, not the writer's
        ++writerContentions;
        writerSettled.open();
    });

    WindowOutcome outcome;
    std::thread waiter([&] {
        waiterId = std::this_thread::get_id();
        outcome.worked = wake.waitForWork();
    });
    inWindow.wait();

    std::thread writerThread([&] {
        writer(wake);
        writerSettled.open();
    });
    writerThread.join();

    outcome.writerContentions = writerContentions.load();
    if (outcome.writerContentions == 0) {
        wake.rescue();
    }
    waiter.join();
    return outcome;
}

}  // namespace

TEST(WakeState, AStopBetweenTheWaitersTestAndItsBlockWaitsForTheMutex) {
    SpyWakeState wake;
    ASSERT_TRUE(wake.beginRun());

    const WindowOutcome outcome = writeInTheWindow(wake, [](SpyWakeState& w) { w.requestStop(); });

    EXPECT_GE(outcome.writerContentions, 1)
        << "stop() stored without the mutex the waiter held, so its notification could find nobody waiting";
    EXPECT_FALSE(outcome.worked);   // woken by the stop, not by work
}

TEST(WakeState, AWorkSignalBetweenTheWaitersTestAndItsBlockWaitsForTheMutex) {
    SpyWakeState wake;
    ASSERT_TRUE(wake.beginRun());

    const WindowOutcome outcome = writeInTheWindow(wake, [](SpyWakeState& w) { w.signalWork(); });

    EXPECT_GE(outcome.writerContentions, 1)
        << "signalWork() stored without the mutex the waiter held, so its notification could find nobody waiting";
    EXPECT_TRUE(outcome.worked);
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
