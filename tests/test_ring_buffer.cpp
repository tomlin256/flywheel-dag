// =============================================================================
// flywheel-dag — A header-only C++17 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

// test_ring_buffer.cpp — RingBuffer<T> semantics, especially around the wrap.
//
// RingBuffer backs the windowed time-series nodes other than WindowNode, so the
// ordering guarantees have to be exact: index 0 is always the oldest element,
// and for_each_contiguous must present the contents in that same order across
// at most two runs. A wrap-point off-by-one here would silently corrupt every
// rolling statistic built on it.

#include <gtest/gtest.h>

#include <deque>
#include <stdexcept>
#include <string>
#include <vector>

#include "flywheel/dag_ring_buffer.hpp"

using dag::RingBuffer;

namespace {

/// Flatten via for_each_contiguous — the access path the vectorisable loops use.
template <typename T>
std::vector<T> flatten(const RingBuffer<T>& rb) {
    std::vector<T> out;
    rb.for_each_contiguous([&](const T* data, std::size_t n) {
        out.insert(out.end(), data, data + n);
    });
    return out;
}

/// Read back via operator[] — the other access path. Must always agree.
template <typename T>
std::vector<T> indexed(const RingBuffer<T>& rb) {
    std::vector<T> out;
    for (std::size_t i = 0; i < rb.size(); ++i) out.push_back(rb[i]);
    return out;
}

}  // namespace

TEST(RingBuffer, StartsEmpty) {
    RingBuffer<double> rb(4);
    EXPECT_EQ(rb.size(), 0u);
    EXPECT_EQ(rb.capacity(), 4u);
    EXPECT_TRUE(rb.empty());
    EXPECT_FALSE(rb.full());
}

TEST(RingBuffer, DefaultConstructedHasNoCapacity) {
    RingBuffer<double> rb;
    EXPECT_EQ(rb.capacity(), 0u);
    EXPECT_TRUE(rb.empty());
    // Zero capacity means full: there is nowhere to put anything.
    EXPECT_TRUE(rb.full());
    EXPECT_THROW(rb.push_back(1.0), std::logic_error);
}

TEST(RingBuffer, PushAndReadInOrder) {
    RingBuffer<int> rb(4);
    rb.push_back(1);
    rb.push_back(2);
    rb.push_back(3);

    EXPECT_EQ(rb.size(), 3u);
    EXPECT_EQ(rb.front(), 1);
    EXPECT_EQ(rb.back(), 3);
    EXPECT_EQ(indexed(rb), (std::vector<int>{1, 2, 3}));
    EXPECT_EQ(flatten(rb), (std::vector<int>{1, 2, 3}));
}

TEST(RingBuffer, PushOnFullThrowsRatherThanDroppingSilently) {
    RingBuffer<int> rb(2);
    rb.push_back(1);
    rb.push_back(2);
    ASSERT_TRUE(rb.full());

    EXPECT_THROW(rb.push_back(3), std::logic_error);
    // ...and the throw left the contents untouched.
    EXPECT_EQ(indexed(rb), (std::vector<int>{1, 2}));
}

TEST(RingBuffer, PopFrontAdvancesTheOldest) {
    RingBuffer<int> rb(4);
    for (int i = 1; i <= 4; ++i) rb.push_back(i);

    rb.pop_front();
    EXPECT_EQ(rb.front(), 2);
    EXPECT_EQ(indexed(rb), (std::vector<int>{2, 3, 4}));

    rb.pop_front();
    EXPECT_EQ(indexed(rb), (std::vector<int>{3, 4}));
}

TEST(RingBuffer, PopBackRemovesTheNewest) {
    RingBuffer<int> rb(4);
    for (int i = 1; i <= 4; ++i) rb.push_back(i);

    rb.pop_back();
    EXPECT_EQ(rb.back(), 3);
    EXPECT_EQ(indexed(rb), (std::vector<int>{1, 2, 3}));
}

// The wrap is where an off-by-one would live. Slide a window all the way round
// the buffer several times and check the contents at every step.
TEST(RingBuffer, OrderingSurvivesRepeatedWraps) {
    constexpr std::size_t kCap = 5;
    RingBuffer<int> rb(kCap);
    std::deque<int> reference;   // the container this replaces, as the oracle

    for (int i = 0; i < 200; ++i) {
        if (rb.full()) {
            rb.pop_front();
            reference.pop_front();
        }
        rb.push_back(i);
        reference.push_back(i);

        const std::vector<int> expected(reference.begin(), reference.end());
        ASSERT_EQ(indexed(rb), expected) << "index view diverged at push " << i;
        ASSERT_EQ(flatten(rb), expected) << "contiguous view diverged at push " << i;
        ASSERT_EQ(rb.front(), reference.front());
        ASSERT_EQ(rb.back(),  reference.back());
    }
}

TEST(RingBuffer, ForEachContiguousUsesAtMostTwoRuns) {
    RingBuffer<int> rb(4);
    for (int i = 1; i <= 4; ++i) rb.push_back(i);
    rb.pop_front();
    rb.push_back(5);        // now wrapped: [2,3,4] + [5]

    int runs = 0;
    std::size_t total = 0;
    rb.for_each_contiguous([&](const int*, std::size_t n) { ++runs; total += n; });

    EXPECT_LE(runs, 2);
    EXPECT_GE(runs, 1);
    EXPECT_EQ(total, rb.size());
    EXPECT_EQ(flatten(rb), (std::vector<int>{2, 3, 4, 5}));
}

TEST(RingBuffer, ForEachContiguousOnEmptyVisitsNothing) {
    RingBuffer<int> rb(4);
    int runs = 0;
    rb.for_each_contiguous([&](const int*, std::size_t) { ++runs; });
    EXPECT_EQ(runs, 0);
}

TEST(RingBuffer, AccessorsOnEmptyThrow) {
    RingBuffer<int> rb(4);
    EXPECT_THROW(rb.front(),     std::logic_error);
    EXPECT_THROW(rb.back(),      std::logic_error);
    EXPECT_THROW(rb.pop_front(), std::logic_error);
    EXPECT_THROW(rb.pop_back(),  std::logic_error);
    EXPECT_THROW(rb[0],          std::out_of_range);
}

TEST(RingBuffer, IndexBeyondSizeThrowsEvenWithinCapacity) {
    RingBuffer<int> rb(8);
    rb.push_back(1);
    rb.push_back(2);
    EXPECT_NO_THROW(rb[1]);
    // Slot 2 exists in storage but holds no element — must not be readable.
    EXPECT_THROW(rb[2], std::out_of_range);
}

TEST(RingBuffer, ClearKeepsCapacity) {
    RingBuffer<int> rb(4);
    for (int i = 1; i <= 3; ++i) rb.push_back(i);
    rb.clear();

    EXPECT_TRUE(rb.empty());
    EXPECT_EQ(rb.capacity(), 4u);
    rb.push_back(9);
    EXPECT_EQ(rb.front(), 9);
    EXPECT_EQ(indexed(rb), (std::vector<int>{9}));
}

TEST(RingBuffer, SetCapacityToTheSameValueKeepsContents) {
    RingBuffer<int> rb(4);
    rb.push_back(1);
    rb.push_back(2);

    rb.setCapacity(4);
    EXPECT_EQ(indexed(rb), (std::vector<int>{1, 2}));
}

TEST(RingBuffer, SetCapacityToADifferentValueResets) {
    RingBuffer<int> rb(4);
    rb.push_back(1);
    rb.push_back(2);

    rb.setCapacity(8);
    EXPECT_EQ(rb.capacity(), 8u);
    EXPECT_TRUE(rb.empty());
}

// Capacity 1 is the tightest wrap there is: every push wraps.
TEST(RingBuffer, CapacityOneBehavesCorrectly) {
    RingBuffer<int> rb(1);
    for (int i = 0; i < 10; ++i) {
        if (rb.full()) rb.pop_front();
        rb.push_back(i);
        ASSERT_EQ(rb.size(), 1u);
        ASSERT_EQ(rb.front(), i);
        ASSERT_EQ(rb.back(),  i);
        ASSERT_EQ(flatten(rb), std::vector<int>{i});
    }
}

// The storage is allocated once, in setCapacity. Sliding a window through the
// buffer forever must never touch the allocator again — that is the whole point.
TEST(RingBuffer, SlidingWindowDoesNotReallocate) {
    RingBuffer<double> rb(16);
    for (int i = 0; i < 16; ++i) rb.push_back(i);

    const double* const storage = &rb[0];
    for (int i = 0; i < 5000; ++i) {
        rb.pop_front();
        rb.push_back(i);
    }
    // Element 0 lives somewhere inside the same allocation it always did.
    const double* const after = &rb.front();
    EXPECT_GE(after, storage - 16);
    EXPECT_LT(after, storage + 16);
    EXPECT_EQ(rb.capacity(), 16u);
    EXPECT_EQ(rb.size(), 16u);
}

TEST(RingBuffer, WorksWithNonTrivialElementTypes) {
    RingBuffer<std::string> rb(3);
    rb.push_back("alpha");
    rb.push_back("beta");
    rb.push_back("gamma");
    EXPECT_EQ(rb.front(), "alpha");
    EXPECT_EQ(rb.back(),  "gamma");

    rb.pop_front();
    rb.push_back("delta");
    EXPECT_EQ(indexed(rb), (std::vector<std::string>{"beta", "gamma", "delta"}));
    EXPECT_EQ(flatten(rb), (std::vector<std::string>{"beta", "gamma", "delta"}));
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
