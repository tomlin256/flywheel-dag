// =============================================================================
// flywheel-dag — A header-only C++23 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

// test_dag_thread_fence.cpp — dag::threadFence<Order>(), the standalone fence that a GCC build under
// -fsanitize=thread and -Werror accepts (flywheel-dag#49).
//
// Coverage
// ────────
//  The signature — noexcept for every order, and the order a template argument
//  The call      — every order instantiates, links and returns
//  The ordering  — a release fence and an acquire fence pass a relaxed store from one thread to another, the
//                  way CycleSeqLock uses them. A test cannot see an ordering weaken, so this holds the pattern
//                  and not the order
//
// What this file does not hold is the warning, which is the reason the helper exists: test_tsan_fences compiles
// the helper and the engine under -fsanitize=thread, and checks that a bare fence still draws -Wtsan.

#include <gtest/gtest.h>

#include <atomic>
#include <thread>

#include "flywheel/dag_thread_fence.hpp"

TEST(ThreadFence, IsNoexceptForEveryOrder) {
    static_assert(noexcept(dag::threadFence<std::memory_order_relaxed>()));
    static_assert(noexcept(dag::threadFence<std::memory_order_acquire>()));
    static_assert(noexcept(dag::threadFence<std::memory_order_release>()));
    static_assert(noexcept(dag::threadFence<std::memory_order_acq_rel>()));
    static_assert(noexcept(dag::threadFence<std::memory_order_seq_cst>()));
    SUCCEED();
}

TEST(ThreadFence, RunsForEveryOrder) {
    std::atomic<int> returned{0};

    dag::threadFence<std::memory_order_relaxed>();
    returned.fetch_add(1, std::memory_order_relaxed);
    dag::threadFence<std::memory_order_acquire>();
    returned.fetch_add(1, std::memory_order_relaxed);
    dag::threadFence<std::memory_order_release>();
    returned.fetch_add(1, std::memory_order_relaxed);
    dag::threadFence<std::memory_order_acq_rel>();
    returned.fetch_add(1, std::memory_order_relaxed);
    dag::threadFence<std::memory_order_seq_cst>();
    returned.fetch_add(1, std::memory_order_relaxed);

    EXPECT_EQ(returned.load(std::memory_order_relaxed), 5);
}

// The pattern CycleSeqLock is built on: the writer stores relaxed, fences with release, then sets a relaxed flag;
// the reader waits on the flag, fences with acquire, then reads relaxed. A reader that saw the flag sees the store.
// Each round is fresh state and a fresh thread, and the bound is the round count: no sleep, no timeout.
TEST(ThreadFence, ReleaseAndAcquireFencesPassARelaxedStoreToAnotherThread) {
    constexpr int kRounds = 1000;

    for (int round = 1; round <= kRounds; ++round) {
        std::atomic<int>  payload{0};
        std::atomic<bool> ready{false};

        std::thread writer([&] {
            payload.store(round, std::memory_order_relaxed);
            dag::threadFence<std::memory_order_release>();
            ready.store(true, std::memory_order_relaxed);
        });

        while (!ready.load(std::memory_order_relaxed)) std::this_thread::yield();
        dag::threadFence<std::memory_order_acquire>();
        EXPECT_EQ(payload.load(std::memory_order_relaxed), round) << "round " << round;

        writer.join();
    }
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
