// =============================================================================
// flywheel-dag — A header-only C++23 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

#pragma once

// The C++23 guard of dag.hpp, which this header does not reach.
#if __cplusplus <= 202002L
#error "flywheel-dag requires C++23: compile with -std=c++23, or link flywheel::dag from CMake, which asks for it"
#endif

// dag_thread_fence.hpp — std::atomic_thread_fence for a program that is built under -fsanitize=thread with -Werror.
//
// The problem
// ───────────
// ThreadSanitizer does not model a standalone fence: it sees no ordering through one. GCC (11 and later) says so with
// -Wtsan, at every fence it compiles under -fsanitize=thread, and -Werror makes that an error. The warning is true.
// Where the fence orders atomic accesses only, it costs nothing: ThreadSanitizer never reports on an atomic access,
// with a fence or without one, so there is no race it could miss and none it could invent. CycleSeqLock is such a
// place.
//
// What this does
// ──────────────
// threadFence<Order>() is std::atomic_thread_fence(Order) with -Wtsan off for that one call, and only on GCC: Clang has
// no -Wtsan and rejects the name in the pragma. The object code is the same, ThreadSanitizer sees what it saw before,
// and every other fence in the program still draws the warning.
//
// When not to use it
// ──────────────────
// Use it for a fence that orders atomic accesses. For plain data ThreadSanitizer cannot see the ordering and reports
// races that are not there; the warning is then what points at the cause, and this would hide it. Use a lock there, or
// a release or acquire operation, which ThreadSanitizer models.
//
// Why the order is a template argument
// ────────────────────────────────────
// GCC compiles a fence whose order it cannot see at compile time as seq_cst. A function that took the order at run time
// would be a full fence wherever it was not inlined (GCC 13.3, x86-64, -O1 and above: lock orq or mfence, where a
// constant acquire fence compiles to nothing). A template argument is a constant inside this function whatever the
// caller does.
//
// Not covered
// ───────────
// -flto with GCC. The sanitizer passes then run at link time, where no pragma applies, and the warning stops the link
// as it does without this header. Such a build keeps -Wno-error=tsan.

#include <atomic>

namespace dag {

/// std::atomic_thread_fence(Order), with GCC's -Wtsan off for this one call. For a fence that orders atomic accesses
/// only; see the comment above.
template<std::memory_order Order>
inline void threadFence() noexcept {
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wtsan"
#endif
    std::atomic_thread_fence(Order);
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
}

}  // namespace dag
