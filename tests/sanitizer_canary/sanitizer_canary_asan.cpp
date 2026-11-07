// =============================================================================
// flywheel-dag — A header-only C++23 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

// sanitizer_canary_asan.cpp — a deliberate heap-use-after-free.
//
// Built and run only in a -DFLYWHEEL_DAG_SANITIZE=asan tree, through tests/run_sanitizer_canary.cmake, which requires
// AddressSanitizer to stop it and say so. The program itself returns 0: if the sanitizer's options never reached the compiler it
// exits cleanly, the runner sees no report, and the test goes red. A sanitizer tree whose options silently did not apply is green
// on every real test and tells you nothing, which is what this program is for.
//
// noinline and volatile keep the optimiser from proving the access dead and deleting it. The block is freed behind a call, so the
// compiler cannot pair the new with the delete and drop both.

namespace {

__attribute__((noinline)) void release(int* const block) {
    delete[] block;
}

__attribute__((noinline)) int readFirst(const volatile int* const block) {
    return block[0];
}

}  // namespace

int main() {
    int* const block = new int[8]{};
    release(block);
    // Heap-use-after-free: AddressSanitizer stops here. Uninstrumented, this reads freed memory and carries on.
    const int stale = readFirst(block);
    static_cast<void>(stale);
    return 0;
}
