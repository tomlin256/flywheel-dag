// =============================================================================
// flywheel-dag — A header-only C++23 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

// sanitizer_canary_ubsan.cpp — a deliberate signed integer overflow.
//
// Built and run only in a -DFLYWHEEL_DAG_SANITIZE=asan tree (that mode is AddressSanitizer plus UBSan), through
// tests/run_sanitizer_canary.cmake, which requires a non-zero exit AND UBSan's "runtime error" report.
//
// The program returns 0, so the exit status is where -fno-sanitize-recover=undefined shows. UBSan's default is to print the report
// and carry on, which exits 0: a test run green over a real finding. With the option the process stops at the report and exits
// non-zero. Remove the option and this canary goes red on its exit-status requirement.
//
// noinline and volatile keep the optimiser from folding the sum, which would turn the overflow into a compile-time constant that
// nothing checks at run time.
//
// The sum is also stored to a volatile. GCC builds the overflow check into the value (an internal add-with-check function the
// optimiser treats as pure), so a sum nothing uses takes its check with it: a canary that discards its sum is still caught by Clang,
// but GCC 15.2 compiles the whole thing away and it exits 0 under a live UBSan. A volatile store is an observable effect, so the
// addition and its check have to run.

#include <climits>

namespace {

__attribute__((noinline)) int add(const int lhs, const int rhs) {
    return lhs + rhs;
}

volatile int sink = 0;  // the sum's only reader, so the sum has to be computed

}  // namespace

int main() {
    const volatile int largest = INT_MAX;
    const volatile int one = 1;
    // INT_MAX + 1: signed overflow, which is undefined behaviour.
    sink = add(largest, one);
    return 0;
}
