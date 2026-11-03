// =============================================================================
// flywheel-dag — A header-only C++23 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

// allocation_counter.cpp — see allocation_counter.hpp.

#include "allocation_counter.hpp"

#include <stdlib.h>   // posix_memalign, which <cstdlib> need not declare

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <new>

namespace {

using allocation_counter::Function;

// Constant-initialised, so a call during static initialisation is safe, and
// relaxed, so that a call on another thread is not a race. Nothing orders the
// count against any other memory: a suite reads it on the thread that asked.
std::atomic<bool>     g_counting{false};
std::atomic<long>     g_count{0};
std::atomic<Function> g_last{Function::None};

/// Every allocation is recorded here and nowhere else.
void note_allocation(const Function fn) noexcept {
    g_last.store(fn, std::memory_order_relaxed);
    if (g_counting.load(std::memory_order_relaxed)) g_count.fetch_add(1, std::memory_order_relaxed);
}

void* take(const Function fn, const std::size_t size) noexcept {
    note_allocation(fn);
    return std::malloc(size != 0 ? size : 1);   // a request for 0 bytes still gets a pointer
}

void* take_aligned(const Function fn, const std::size_t size, const std::align_val_t alignment) noexcept {
    note_allocation(fn);
    // posix_memalign() wants a power of two that is a multiple of sizeof(void*).
    const std::size_t align = std::max(static_cast<std::size_t>(alignment), sizeof(void*));
    void* p = nullptr;
    return posix_memalign(&p, align, size != 0 ? size : 1) == 0 ? p : nullptr;
}

/// Every release is recorded here and not counted.
void give(const Function fn, void* const p) noexcept {
    g_last.store(fn, std::memory_order_relaxed);
    std::free(p);
}

/// The throwing forms report a failed allocation by throwing, where the nothrow
/// forms return null.
void* or_throw(void* const p) {
    if (p == nullptr) throw std::bad_alloc();
    return p;
}

}  // namespace

namespace allocation_counter {

void start() {
    g_count.store(0, std::memory_order_relaxed);
    g_counting.store(true, std::memory_order_relaxed);
}

long stop() {
    g_counting.store(false, std::memory_order_relaxed);
    return g_count.load(std::memory_order_relaxed);
}

long     count()         { return g_count.load(std::memory_order_relaxed); }
Function last_function() { return g_last.load(std::memory_order_relaxed); }

}  // namespace allocation_counter

// ─────────────────────────────────────────────────────────────────────────────
// The twenty replaceable functions, in the order of [new.delete]. Each is one
// call to a primitive above, tagged with the function it is.
// ─────────────────────────────────────────────────────────────────────────────

void* operator new(std::size_t n)                                   { return or_throw(take(Function::New, n)); }
void* operator new[](std::size_t n)                                 { return or_throw(take(Function::NewArray, n)); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept   { return take(Function::NewNothrow, n); }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { return take(Function::NewArrayNothrow, n); }

void* operator new(std::size_t n, std::align_val_t a) {
    return or_throw(take_aligned(Function::NewAligned, n, a));
}
void* operator new[](std::size_t n, std::align_val_t a) {
    return or_throw(take_aligned(Function::NewArrayAligned, n, a));
}
void* operator new(std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
    return take_aligned(Function::NewAlignedNothrow, n, a);
}
void* operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
    return take_aligned(Function::NewArrayAlignedNothrow, n, a);
}

void operator delete(void* p) noexcept                          { give(Function::Delete, p); }
void operator delete[](void* p) noexcept                        { give(Function::DeleteArray, p); }
void operator delete(void* p, std::size_t) noexcept             { give(Function::DeleteSized, p); }
void operator delete[](void* p, std::size_t) noexcept           { give(Function::DeleteArraySized, p); }
void operator delete(void* p, const std::nothrow_t&) noexcept   { give(Function::DeleteNothrow, p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { give(Function::DeleteArrayNothrow, p); }

void operator delete(void* p, std::align_val_t) noexcept {
    give(Function::DeleteAligned, p);
}
void operator delete[](void* p, std::align_val_t) noexcept {
    give(Function::DeleteArrayAligned, p);
}
void operator delete(void* p, std::size_t, std::align_val_t) noexcept {
    give(Function::DeleteSizedAligned, p);
}
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept {
    give(Function::DeleteArraySizedAligned, p);
}
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept {
    give(Function::DeleteAlignedNothrow, p);
}
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept {
    give(Function::DeleteArrayAlignedNothrow, p);
}
