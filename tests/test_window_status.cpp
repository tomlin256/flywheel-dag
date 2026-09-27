// =============================================================================
// flywheel-dag — A header-only C++17 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

// test_window_status.cpp — pure-formula unit tests for WindowStatus

#include <gtest/gtest.h>
#include "flywheel/dag_window_status.hpp"

using dag::ts::WindowStatus;

// ─────────────────────────────────────────────────────────────────────────────
// Default
// ─────────────────────────────────────────────────────────────────────────────
TEST(WindowStatus, Default)
{
    WindowStatus s;
    EXPECT_EQ(s.capacity, 0u);
    EXPECT_EQ(s.filled,   0u);
    EXPECT_FALSE(s.full());
    EXPECT_DOUBLE_EQ(s.fraction(), 0.0);
}

// ─────────────────────────────────────────────────────────────────────────────
// Full
// ─────────────────────────────────────────────────────────────────────────────
TEST(WindowStatus, Full)
{
    WindowStatus s{5, 5};
    EXPECT_TRUE(s.full());
    EXPECT_DOUBLE_EQ(s.fraction(), 1.0);
}

// ─────────────────────────────────────────────────────────────────────────────
// Partial
// ─────────────────────────────────────────────────────────────────────────────
TEST(WindowStatus, Partial)
{
    WindowStatus s{4, 1};
    EXPECT_FALSE(s.full());
    EXPECT_DOUBLE_EQ(s.fraction(), 0.25);
}

// ─────────────────────────────────────────────────────────────────────────────
// Equality
// ─────────────────────────────────────────────────────────────────────────────
TEST(WindowStatus, Equality)
{
    WindowStatus a{5, 5};
    WindowStatus b{5, 5};
    WindowStatus c{5, 4};
    WindowStatus d{4, 5};

    EXPECT_TRUE(a == b);
    EXPECT_FALSE(a != b);
    EXPECT_TRUE(a != c);
    EXPECT_TRUE(a != d);
}

// ─────────────────────────────────────────────────────────────────────────────
// OverflowProtection — filled > capacity still reports full()
// ─────────────────────────────────────────────────────────────────────────────
TEST(WindowStatus, OverflowProtection)
{
    WindowStatus s{5, 7};
    EXPECT_TRUE(s.full());
}

// ─────────────────────────────────────────────────────────────────────────────
// ZeroCapacityFractionSafe — no divide-by-zero
// ─────────────────────────────────────────────────────────────────────────────
TEST(WindowStatus, ZeroCapacityFractionSafe)
{
    WindowStatus s{0, 0};
    EXPECT_NO_THROW(s.fraction());
    EXPECT_DOUBLE_EQ(s.fraction(), 0.0);
    EXPECT_FALSE(s.full());
}

// ─────────────────────────────────────────────────────────────────────────────
// Main
// ─────────────────────────────────────────────────────────────────────────────
int main(int argc, char** argv)
{
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
