// =============================================================================
// flywheel-dag — A header-only C++23 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

// sanitizer_canary_lsan.cpp — a deliberate memory leak.
//
// Built and run only in a -DFLYWHEEL_DAG_SANITIZE=asan tree on Linux, through tests/run_sanitizer_canary.cmake, which requires
// LeakSanitizer to stop it and say so. LeakSanitizer is part of AddressSanitizer's runtime, looks for leaks as the process exits, and
// exists only on Linux. It is on there unless ASAN_OPTIONS says detect_leaks=0, which leaves this program exiting 0 with no report and
// the test red. A Linux asan run whose leak detector is off is green on every real test and finds no leak, which is what this
// program is for.
//
// The pointer is volatile so that the allocation is observable and the optimiser cannot drop the new. It is cleared before main
// returns, so the block is unreachable at exit.

namespace {

int* volatile pointer = nullptr;

}  // namespace

int main() {
    pointer = new int[8]{};
    // Leak: the block has no pointer left to it. LeakSanitizer reports it at exit. Uninstrumented, or with detect_leaks=0, nothing
    // says so.
    pointer = nullptr;
    return 0;
}
