// =============================================================================
// flywheel-dag — A header-only C++23 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

// test_allocation_counter.cpp — pins the allocation counter's contract.
//
// Every suite that asserts "allocates nothing" stands on three things, and each
// is a test below:
//
//   1. All twenty replaceable functions are the counter's own. A sanitizer
//      supplies its own for any that are not, and the standard library pairs
//      them across forms (flywheel-dag#47).
//   2. An allocation counts once, in every form, and a release never counts.
//   3. What an allocating form returns, its release takes back, aligned or not.
//
// The sized releases, ::operator delete(p, std::size_t{8}) and the like, are
// declared by libstdc++'s <new> only when __cpp_sized_deallocation is defined,
// and Clang leaves that off before version 19. This file does not compile on
// that pair, which is not a supported build (flywheel-dag#58).
//
// Each function is called by name, ::operator new(8, std::nothrow) and so on,
// so that no test depends on which form the compiler picks for an expression.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>

#include <gtest/gtest.h>

#include "allocation_counter.hpp"

using allocation_counter::allocations_during;
using allocation_counter::Function;

namespace {

/// True if `expected` is the replaceable function that ran last. Ask straight
/// after the call: anything that allocated in between would have replaced it.
bool ran(const Function expected) { return allocation_counter::last_function() == expected; }

/// A type no ordinary allocation fits: new Wide asks for the aligned forms.
struct alignas(64) Wide {
    char byte;
};

/// Somewhere for a pointer to go, so that a compiler may not drop a new
/// expression whose result nothing reads (allocation elision, [expr.new]).
void* volatile g_escape = nullptr;

}  // namespace

// ─────────────────────────────────────────────────────────────────────────────
// 1. Every replaceable function is the counter's own.
// ─────────────────────────────────────────────────────────────────────────────

TEST(AllocationCounter, EveryReplaceableFunctionIsTheCounters) {
    const std::align_val_t align{64};

    void* p = ::operator new(8);
    EXPECT_TRUE(ran(Function::New));
    ::operator delete(p);
    EXPECT_TRUE(ran(Function::Delete));

    p = ::operator new(8);
    ::operator delete(p, std::size_t{8});
    EXPECT_TRUE(ran(Function::DeleteSized));

    p = ::operator new[](8);
    EXPECT_TRUE(ran(Function::NewArray));
    ::operator delete[](p);
    EXPECT_TRUE(ran(Function::DeleteArray));

    p = ::operator new[](8);
    ::operator delete[](p, std::size_t{8});
    EXPECT_TRUE(ran(Function::DeleteArraySized));

    p = ::operator new(8, std::nothrow);
    EXPECT_TRUE(ran(Function::NewNothrow));
    ::operator delete(p, std::nothrow);
    EXPECT_TRUE(ran(Function::DeleteNothrow));

    p = ::operator new[](8, std::nothrow);
    EXPECT_TRUE(ran(Function::NewArrayNothrow));
    ::operator delete[](p, std::nothrow);
    EXPECT_TRUE(ran(Function::DeleteArrayNothrow));

    p = ::operator new(8, align);
    EXPECT_TRUE(ran(Function::NewAligned));
    ::operator delete(p, align);
    EXPECT_TRUE(ran(Function::DeleteAligned));

    p = ::operator new(8, align);
    ::operator delete(p, std::size_t{8}, align);
    EXPECT_TRUE(ran(Function::DeleteSizedAligned));

    p = ::operator new[](8, align);
    EXPECT_TRUE(ran(Function::NewArrayAligned));
    ::operator delete[](p, align);
    EXPECT_TRUE(ran(Function::DeleteArrayAligned));

    p = ::operator new[](8, align);
    ::operator delete[](p, std::size_t{8}, align);
    EXPECT_TRUE(ran(Function::DeleteArraySizedAligned));

    p = ::operator new(8, align, std::nothrow);
    EXPECT_TRUE(ran(Function::NewAlignedNothrow));
    ::operator delete(p, align, std::nothrow);
    EXPECT_TRUE(ran(Function::DeleteAlignedNothrow));

    p = ::operator new[](8, align, std::nothrow);
    EXPECT_TRUE(ran(Function::NewArrayAlignedNothrow));
    ::operator delete[](p, align, std::nothrow);
    EXPECT_TRUE(ran(Function::DeleteArrayAlignedNothrow));
}

// ─────────────────────────────────────────────────────────────────────────────
// 2. An allocation counts once, and a release never does.
// ─────────────────────────────────────────────────────────────────────────────

TEST(AllocationCounter, EachAllocatingFormCountsOnce) {
    const std::align_val_t align{64};
    void* p = nullptr;

    EXPECT_EQ(allocations_during([&] { p = ::operator new(8); }), 1);
    ::operator delete(p);
    EXPECT_EQ(allocations_during([&] { p = ::operator new[](8); }), 1);
    ::operator delete[](p);
    EXPECT_EQ(allocations_during([&] { p = ::operator new(8, std::nothrow); }), 1);
    ::operator delete(p, std::nothrow);
    EXPECT_EQ(allocations_during([&] { p = ::operator new[](8, std::nothrow); }), 1);
    ::operator delete[](p, std::nothrow);
    EXPECT_EQ(allocations_during([&] { p = ::operator new(8, align); }), 1);
    ::operator delete(p, align);
    EXPECT_EQ(allocations_during([&] { p = ::operator new[](8, align); }), 1);
    ::operator delete[](p, align);
    EXPECT_EQ(allocations_during([&] { p = ::operator new(8, align, std::nothrow); }), 1);
    ::operator delete(p, align, std::nothrow);
    EXPECT_EQ(allocations_during([&] { p = ::operator new[](8, align, std::nothrow); }), 1);
    ::operator delete[](p, align, std::nothrow);
}

TEST(AllocationCounter, NoReleaseCounts) {
    const std::align_val_t align{64};
    void* const a = ::operator new(8);
    void* const b = ::operator new(8);
    void* const c = ::operator new[](8);
    void* const d = ::operator new[](8);
    void* const e = ::operator new(8, std::nothrow);
    void* const f = ::operator new[](8, std::nothrow);
    void* const g = ::operator new(8, align);
    void* const h = ::operator new(8, align);
    void* const i = ::operator new[](8, align);
    void* const j = ::operator new[](8, align);
    void* const k = ::operator new(8, align, std::nothrow);
    void* const l = ::operator new[](8, align, std::nothrow);

    const long counted = allocations_during([&] {
        ::operator delete(a);
        ::operator delete(b, std::size_t{8});
        ::operator delete[](c);
        ::operator delete[](d, std::size_t{8});
        ::operator delete(e, std::nothrow);
        ::operator delete[](f, std::nothrow);
        ::operator delete(g, align);
        ::operator delete(h, std::size_t{8}, align);
        ::operator delete[](i, align);
        ::operator delete[](j, std::size_t{8}, align);
        ::operator delete(k, align, std::nothrow);
        ::operator delete[](l, align, std::nothrow);
    });
    EXPECT_EQ(counted, 0);
}

TEST(AllocationCounter, CountsOnlyBetweenStartAndStop) {
    allocation_counter::start();
    void* const counted = ::operator new(8);
    EXPECT_EQ(allocation_counter::stop(), 1);

    void* const uncounted = ::operator new(8);   // stopped: not counted
    EXPECT_EQ(allocation_counter::count(), 1);

    allocation_counter::start();                 // zeroes the count
    EXPECT_EQ(allocation_counter::count(), 0);
    EXPECT_EQ(allocation_counter::stop(), 0);

    ::operator delete(counted);
    ::operator delete(uncounted);
}

TEST(AllocationCounter, NewExpressionsAreCounted) {
    EXPECT_EQ(allocations_during([] { int* p = new int(1);                  g_escape = p; delete p; }), 1);
    EXPECT_EQ(allocations_during([] { int* p = new int[4];                  g_escape = p; delete[] p; }), 1);
    EXPECT_EQ(allocations_during([] { int* p = new (std::nothrow) int(1);   g_escape = p; delete p; }), 1);
    EXPECT_EQ(allocations_during([] { int* p = new (std::nothrow) int[4];   g_escape = p; delete[] p; }), 1);
    EXPECT_EQ(allocations_during([] { Wide* p = new Wide;                   g_escape = p; delete p; }), 1);
    EXPECT_EQ(allocations_during([] { Wide* p = new Wide[2];                g_escape = p; delete[] p; }), 1);
    EXPECT_EQ(allocations_during([] { Wide* p = new (std::nothrow) Wide;    g_escape = p; delete p; }), 1);
    EXPECT_EQ(allocations_during([] { Wide* p = new (std::nothrow) Wide[2]; g_escape = p; delete[] p; }), 1);
    // A shared_ptr's control block and its object are one allocation.
    EXPECT_EQ(allocations_during([] { const auto p = std::make_shared<int>(1); g_escape = p.get(); }), 1);
}

// ─────────────────────────────────────────────────────────────────────────────
// 3. What an allocating form returns, its release takes back.
// ─────────────────────────────────────────────────────────────────────────────

TEST(AllocationCounter, AlignedFormsReturnAlignedMemory) {
    for (const std::size_t alignment : {std::size_t{16}, std::size_t{64}, std::size_t{256}, std::size_t{4096}}) {
        const std::align_val_t align{alignment};

        void* p = ::operator new(8, align);
        EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p) % alignment, 0u);
        ::operator delete(p, align);

        p = ::operator new[](8, align);
        EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p) % alignment, 0u);
        ::operator delete[](p, align);

        p = ::operator new(8, align, std::nothrow);
        EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p) % alignment, 0u);
        ::operator delete(p, align, std::nothrow);

        p = ::operator new[](8, align, std::nothrow);
        EXPECT_EQ(reinterpret_cast<std::uintptr_t>(p) % alignment, 0u);
        ::operator delete[](p, align, std::nothrow);
    }
}

TEST(AllocationCounter, ZeroBytesStillReturnsADistinctPointer) {
    void* const first  = ::operator new(0);
    void* const second = ::operator new(0);
    EXPECT_NE(first, nullptr);
    EXPECT_NE(second, nullptr);
    EXPECT_NE(first, second);
    ::operator delete(first);
    ::operator delete(second);

    void* const nothrow = ::operator new(0, std::nothrow);
    EXPECT_NE(nothrow, nullptr);
    ::operator delete(nothrow, std::nothrow);
}

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
