// =============================================================================
// flywheel-dag — A header-only C++23 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

// allocation_counter.hpp — counts the allocations a piece of code makes.
//
// Linking allocation_counter.cpp replaces all twenty of the global allocation
// functions the standard makes replaceable ([new.delete], C++23: eight
// operator new and twelve operator delete). Each counts and then calls malloc()
// or free(), so every allocation in the program pairs with its release through
// the one family, which a sanitizer still tracks.
//
// All twenty, because a sanitizer supplies its own for every one a program
// leaves out, and the standard library pairs them across forms.
// std::stable_sort takes its buffer from operator new(size_t, std::nothrow_t)
// and returns it with operator delete(void*). A counter that replaced only the
// plain functions would count nothing from the first, and AddressSanitizer would
// abort on the second, for memory from its own operator new given to free().
//
// Link it into the programs that count, and no others: the replacement is the
// whole program's. They are the suites that tests/CMakeLists.txt lists and the
// benchmark. A program that links it must not define an operator new or
// operator delete of its own, and no file but allocation_counter.cpp may:
// test_allocation_functions holds that.

#pragma once

#include <cstdint>

namespace allocation_counter {

/// Zeroes the count and counts every allocation from now on.
void start();

/// Stops counting. Returns the allocations made since start().
long stop();

/// The allocations counted since start(), whether or not it has stopped.
long count();

/// The number of allocations fn() makes. fn() must not allocate for reasons
/// unrelated to the code under test (no string building, no EXPECT inside).
template <typename Fn>
long allocations_during(Fn&& fn) {
    start();
    fn();
    return stop();
}

/// The replaceable functions of [new.delete], one enumerator each, so that a
/// test can ask which one ran. None is the state before any has.
///
/// A name says what the function adds to the plain one: NewArrayAlignedNothrow
/// is operator new[](size_t, align_val_t, nothrow_t). DeleteSized takes the
/// size, DeleteSizedAligned the size and the alignment.
enum class Function : std::uint8_t {
    None,
    // operator new
    New,
    NewArray,
    NewNothrow,
    NewArrayNothrow,
    NewAligned,
    NewArrayAligned,
    NewAlignedNothrow,
    NewArrayAlignedNothrow,
    // operator delete
    Delete,
    DeleteArray,
    DeleteSized,
    DeleteArraySized,
    DeleteNothrow,
    DeleteArrayNothrow,
    DeleteAligned,
    DeleteArrayAligned,
    DeleteSizedAligned,
    DeleteArraySizedAligned,
    DeleteAlignedNothrow,
    DeleteArrayAlignedNothrow,
};

/// The replaceable function that ran last, whether or not the counter was on.
/// Only test_allocation_counter asks, to show that each function is the
/// counter's own and not the library's or a sanitizer's.
Function last_function();

}  // namespace allocation_counter
