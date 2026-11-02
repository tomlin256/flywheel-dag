# The Allocation Counter Must Replace Every Allocation Function

**Status: Proposed (2026-11-02).** Nothing below is implemented. D1 to D5 are my defaults, each a place to
edit before approving.

Closes [flywheel-dag#47](https://github.com/tomlin256/flywheel-dag/issues/47).

## Problem

`test_value_slot` and `test_aad_node` count allocations by defining the global `operator new(std::size_t)`
and two `operator delete` functions with `malloc()` and `free()`. That is three of the twenty global
allocation functions the standard makes replaceable ([new.delete]: eight `operator new`, twelve
`operator delete`). A program that replaces some gets the standard library's default for the rest, and the
defaults forward to the replaced ones, so in a plain build the counter sees everything.

AddressSanitizer does not forward. It supplies its own version of all twenty, so in a sanitized build every
one a program leaves out is the sanitizer's. `TestSuite::Run()` sorts the test list with `std::stable_sort`,
which takes its temporary buffer from `operator new(std::size_t, const std::nothrow_t&)` (the sanitizer's,
because the suite did not replace it) and returns it with `operator delete(void*)` (the suite's, which is
`free()`). The sanitizer sees `free()` given memory from its own `operator new` and aborts before any test
body runs:

```text
ERROR: AddressSanitizer: alloc-dealloc-mismatch (operator new vs free)
allocated by operator new(unsigned long, std::nothrow_t const&) in std::get_temporary_buffer
```

### What I measured

At `893ffec`, in the `ubuntu-dev` container (GCC 13.3, aarch64), tree `build/asan-linux-arm64`
(`RelWithDebInfo`, `-fsanitize=address,undefined -fno-omit-frame-pointer`):

- **`ctest` with AddressSanitizer's default options passes 38 of 40.** The two failures are `test_aad_node`
  (7 tests) and `test_value_slot` (21), each aborting in its first `TestSuite::Run()` with the report above.
  With `ASAN_OPTIONS=alloc_dealloc_mismatch=0` both pass, as flywheel-dag#47 says.
- **`bench_hot_path` has the same three-function counter and does not trip on it.** It builds in that tree and
  runs 2,000 cycles to exit 0. Nothing it runs reaches a nothrow `new`.
- **The Mac is not affected.** Apple Clang 21's AddressSanitizer and UndefinedBehaviorSanitizer run both
  unchanged suites to the end (21 and 7 tests pass).
- Neither suite starts a thread.

A prototype of the fix in the session scratchpad, with the repo tree untouched (it is Step 2 and Step 3
below), built by hand against each tree's `libgtest.a` with `-Wall -Wextra -Wpedantic -Werror`:

| Build | `test_allocation_counter` (new, 7 tests) | `test_value_slot` (21) | `test_aad_node` (7) |
|---|---|---|---|
| GCC 13.3, plain `RelWithDebInfo` flags | pass | pass | pass |
| GCC 13.3, AddressSanitizer + UndefinedBehaviorSanitizer, default options | exit 0 | exit 0 | exit 0 |
| GCC 13.3, ThreadSanitizer (without `-Werror`, see flywheel-dag#49) | pass | pass | pass |
| Apple Clang 21, plain | pass | pass | pass |
| Apple Clang 21, AddressSanitizer + UndefinedBehaviorSanitizer | pass | pass | pass |

No build printed a warning. The AddressSanitizer runs have leak detection on, so every release form frees
what its allocating form took. On GCC 13.3 the GCC bug 103993 pragma (`-Wmismatched-new-delete`) is not
needed: the bug needs the replacement in the same translation unit as the code that inlines it, and the
counter now has one of its own.

**The hand-made changes.** Each is a scratch copy of the prototype, compiled by hand and run alone under a
20 s alarm. The copy with no change passes.

| Change | Result |
|---|---|
| The nothrow `operator new` left out (flywheel-dag#47's gap) | `EveryReplaceableFunctionIsTheCounters` fails in a plain build. Under GCC AddressSanitizer all three binaries abort with flywheel-dag#47's report |
| The sized `operator delete(void*, size_t)` left out | `EveryReplaceableFunctionIsTheCounters` fails |
| `operator new[](size_t, align_val_t, nothrow_t)` left out | `EveryReplaceableFunctionIsTheCounters` fails |
| `operator new[]` counts twice | `EachAllocatingFormCountsOnce` and `NewExpressionsAreCounted` fail |
| A release counts | `NoReleaseCounts` and `NewExpressionsAreCounted` fail |
| The aligned forms ignore their alignment | `AlignedFormsReturnAlignedMemory` fails |
| `stop()` keeps counting | `CountsOnlyBetweenStartAndStop` fails |
| `start()` does not zero the count | `CountsOnlyBetweenStartAndStop`, `EachAllocatingFormCountsOnce`, `NoReleaseCounts` and `NewExpressionsAreCounted` fail |
| `operator delete(void*)` does not free | passes in a plain build; LeakSanitizer in the container reports 38,377 bytes in 135 allocations from `test_allocation_counter` alone |
| `ValueSlot` never recycles (a scratch copy of `include/`) | the **unchanged** suites fail 7 tests of `test_value_slot` and 1 of `test_aad_node`; the **prototype** suites fail the identical sets |

The last row is what the move has to keep: a suite that counts through the shared counter catches exactly
what it caught before.

## The fix

Two files and a test for them in `tests/`, and the two suites drop their own copy.

### `tests/allocation_counter.hpp` and `.cpp`

The `.cpp` defines all twenty replaceable functions. Each is a count and a `malloc()` or a `free()`, so every
allocation in the program pairs with its release through the one family, which a sanitizer still tracks. The
header is the whole interface:

```cpp
namespace allocation_counter {

void start();   ///< Zeroes the count and counts every allocation from now on.
long stop();    ///< Stops counting. Returns the allocations made since start().
long count();   ///< The allocations counted since start(), stopped or not.

/// The number of allocations fn() makes. fn() must not allocate for reasons
/// unrelated to the code under test (no string building, no EXPECT inside).
template <typename Fn>
long allocations_during(Fn&& fn) { start(); fn(); return stop(); }

/// One enumerator per replaceable function (20), and None before any has run.
enum class Function { None, New, NewArray, NewNothrow, NewArrayNothrow, /* … */ };

/// The replaceable function that ran last, counted or not. Only
/// test_allocation_counter asks, to prove each function is the counter's own.
Function last_function();

}  // namespace allocation_counter
```

The `.cpp` is three primitives and twenty one-line definitions in the order of [new.delete]:

```cpp
void* take(Function fn, std::size_t size) noexcept {        // every allocation, counted here only
    g_last.store(fn, std::memory_order_relaxed);
    if (g_counting.load(std::memory_order_relaxed)) g_count.fetch_add(1, std::memory_order_relaxed);
    return std::malloc(size != 0 ? size : 1);               // 0 bytes still gets a pointer
}
// take_aligned(): the same over posix_memalign(). give(): g_last, then std::free(p).
// or_throw(): the throwing forms throw std::bad_alloc where the nothrow forms return null.

void* operator new(std::size_t n)                                 { return or_throw(take(Function::New, n)); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { return take(Function::NewNothrow, n); }
void  operator delete(void* p) noexcept                           { give(Function::Delete, p); }
// … seventeen more
```

`g_counting`, `g_count` and `g_last` are relaxed atomics with constant initialisation, so a call during
static initialisation or from another thread is safe. The counter is linked into the suites that count and no
others, because the replacement is the whole program's:

```cmake
set(FLYWHEEL_DAG_COUNTING_TESTS test_aad_node test_allocation_counter test_value_slot)
# in the foreach() that adds each test:
  if(test IN_LIST FLYWHEEL_DAG_COUNTING_TESTS)
    target_sources(${test} PRIVATE allocation_counter.cpp)
  endif()
```

`test_allocation_counter` is added to `FLYWHEEL_DAG_TESTS`.

### `tests/test_allocation_counter.cpp`

Seven tests, none with a thread, a sleep or a clock. Each function is called by name, `::operator new(8,
std::nothrow)` and so on, so the test does not depend on which form the compiler picks for an expression.

| Test | Pins |
|---|---|
| `EveryReplaceableFunctionIsTheCounters` | after each of the twenty calls, `last_function()` is that function: none is the sanitizer's or the library's |
| `EachAllocatingFormCountsOnce` | each of the eight allocating forms counts 1 |
| `NoReleaseCounts` | each of the twelve releasing forms counts 0 |
| `CountsOnlyBetweenStartAndStop` | a stopped counter does not count and `start()` zeroes |
| `NewExpressionsAreCounted` | `new`, `new[]`, nothrow and over-aligned (`alignas(64)`) expressions, and `make_shared`, count 1 each. The pointer escapes through a `volatile` so that allocation elision ([expr.new]) cannot remove the call |
| `AlignedFormsReturnAlignedMemory` | alignments 16, 64, 256 and 4096 from each aligned form |
| `ZeroBytesStillReturnsADistinctPointer` | `operator new(0)` is non-null and unique |

### The two suites

`test_value_slot.cpp` drops lines 22 to 55 (its counter, its three functions and the GCC pragma) and its
`allocationsDuring()`. `test_aad_node.cpp` drops lines 13 to 43 and its helper. Each includes
`allocation_counter.hpp`, adds `using allocation_counter::allocations_during;`, and renames its call sites
(9 and 1): the C++ guidelines name functions in `snake_case`. No assertion changes.

### `CLAUDE.md`

Under Conventions, one line, because a test author who writes their own counter will not meet the header:

> A test that counts allocations links `tests/allocation_counter.cpp` and calls
> `allocation_counter::allocations_during()`; it never defines a global `operator new` or `operator delete`
> of its own. A sanitizer supplies its own for every form a program leaves out, and the suite then aborts
> under AddressSanitizer (flywheel-dag#47).

and the pointer in the warnings rule to the one silenced site that remains, from `test_value_slot.cpp` to
`benchmarks/bench_hot_path.cpp`.

## Decisions to confirm

- **D1. Replace all twenty functions.** Alternatives, each worse:
  - *Add the nothrow pair to each of the three copies.* The smallest diff, and it ends this abort. It leaves
    each copy a partial set, so the next form a library pairs across families fails the same way, and
    nothing says which forms are replaced.
  - *Count through a sanitizer hook.* The plain build and the sanitized build would then assert "allocates
    nothing" by two different mechanisms.
  - *Build the counter only when no sanitizer is on.* The allocation tests would be skipped or empty in the
    sanitizer job, which flywheel-dag#45 says no test may be.
- **D2. One counter in `tests/`, as `.hpp` and `.cpp`, linked into the suites that count.** Not a header of
  definitions: non-templated bodies belong in a `.cpp`, and in its own translation unit the replacement can
  no longer trigger GCC bug 103993, so the pragma goes from the tests. `benchmarks/bench_report.{hpp,cpp}` is
  the precedent for a support unit that two executables link.
- **D3. The aligned forms are replaced too,** though nothing in the engine or the suites is over-aligned.
  It keeps "every allocation is counted and pairs through `malloc()` and `free()`" true without arguing
  case by case, for twelve one-line functions. They use `posix_memalign()`, and the presets are macOS and
  Linux. Without them the set is the ten default-alignment functions, and an over-aligned allocation in a
  steady state would escape the count.
- **D4. `bench_hot_path.cpp` keeps its own counter.** It runs clean under AddressSanitizer today, CI diffs its
  output, and its timings carry layout noise (flywheel-dag#34); moving it is a measurement of its own. A
  follow-up issue records the gap, opened when this plan is approved.
- **D5. No release.** Nothing under `include/` changes, and the header's comment carries the rationale.
  `CLAUDE.md` gains the one line above.

## Steps

Every commit subject is scoped to flywheel-dag#47 in the repo's `type(#N): …` form. The subjects below omit
the scope. Each step's commit goes to `main` and is pushed when the step is done; approving the plan
approves that.

### Step 0 — Approval

The plan is committed as Proposed. Nothing else lands until it is approved. On approval the commit is pushed,
a comment on flywheel-dag#47 links the plan, and the follow-up issue D4 names is opened.

**Done when:** `git status` is clean on that commit, flywheel-dag#47 carries the link, and the plan is
approved.

Commit: `docs: add plan for an allocation counter that replaces every allocation function`.

### Step 1 — Start from green

No commit. On the Mac:

```bash
cmake --preset relwithdebinfo-macos-arm64 -Werror=dev -Werror=deprecated -DFLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON
cmake --build --preset relwithdebinfo-macos-arm64
ctest --preset relwithdebinfo-macos-arm64
```

and the same in the container on `relwithdebinfo-linux-arm64`, never at the same time as the Mac's build. The
AddressSanitizer baseline is the one measured above.

**Done when:** the configure and the build print no warning, `ctest` passes 40 of 40 on both, and the
AddressSanitizer tree passes 38 of 40 with `test_aad_node` and `test_value_slot` the two failures. Anything
else: stop and ask.

### Step 2 — The counter, with its tests

`tests/allocation_counter.hpp`, `tests/allocation_counter.cpp`, `tests/test_allocation_counter.cpp` and the
CMake lines above. The suites are untouched.

**Tests written:** the seven in `test_allocation_counter.cpp`.

**Mutants.** The nine counter changes in the hand-made table (every row but the last), applied to scratch
copies of the committed files and run alone under a 20 s alarm; the copy with no change must pass.

**Done when:**

- the mutants fail as the table says and the unchanged copy passes;
- the strict build prints no warning with Apple Clang here and GCC in the container, and `ctest` passes
  41 of 41 on both (40 and `test_allocation_counter`), `test_warning_flags` included;
- `test_allocation_counter` exits 0 in the AddressSanitizer tree with default options.

Commit: `test: add an allocation counter that replaces every global allocation function`.

### Step 3 — The two suites count through it

`tests/test_value_slot.cpp`, `tests/test_aad_node.cpp`, the CMake link for both, and `CLAUDE.md`.

**Tests modified:** the 28 in the two suites, by renaming one call. No assertion changes, no new test; the
`ValueSlot` mutant is the evidence that they still count.

**Done when:**

- the strict build prints no warning on both compilers and `ctest` passes 41 of 41 on both;
- in the container's AddressSanitizer tree `ctest` passes **41 of 41 with default options** (38 of 40
  today), and UndefinedBehaviorSanitizer prints nothing;
- the `ValueSlot` mutant (a scratch copy of `include/`, never restored) fails 7 tests of `test_value_slot` and
  1 of `test_aad_node`, and the unchanged tree passes;
- CI is green on both legs on the pushed commit. The GCC 15 leg is the only evidence for a warning GCC 15
  alone reports (flywheel-dag#44).

Commit: `test: test_value_slot and test_aad_node count through the allocation counter`.

### Step 4 — Close

The plan is marked done with an Outcome section. flywheel-dag#47 is closed with a summary comment, and
flywheel-dag#45 gets a comment that this blocker is cleared and the suite passes in full under
AddressSanitizer in the container.

**Done when:** the plan is committed as Done and flywheel-dag#47 is closed.

Commit: `docs: mark the plan done`.

## Not in this plan

- **The AddressSanitizer job and its presets** are
  [flywheel-dag#45](https://github.com/tomlin256/flywheel-dag/issues/45). This plan clears the last finding
  I know of for the container's AddressSanitizer run and adds none of the job's machinery.
- **The ThreadSanitizer finding** that `atomic_thread_fence` is invisible to it is
  [flywheel-dag#49](https://github.com/tomlin256/flywheel-dag/issues/49).
- **`bench_hot_path.cpp`'s counter** (D4).
- **LeakSanitizer on the Mac.** Apple Clang's AddressSanitizer answers `detect_leaks=1` with "not supported on
  this platform", so leak evidence is the container's alone.

## Self-review — risks and assumptions

- **A replacement is the whole program's.** The counter links into three executables. A suite that links it
  and defines an `operator new` of its own fails at link time, loudly.
- **The three binaries lose two of AddressSanitizer's checks:** that `delete[]` matches `new[]`, and that a
  sized `delete` is given the right size. Every allocation is `malloc()` and `free()` to the sanitizer. The
  suites already lacked the size check, since they replaced the sized `operator delete`, and the other 37
  binaries keep both.
- **`posix_memalign()` is POSIX.** A Windows port would need `_aligned_malloc()` and `_aligned_free()`, a
  different release function, so `give()` would change too.
- **A zero-byte request is not pinned by a mutant.** `malloc(0)` returns a non-null pointer on glibc and on
  macOS, so dropping the `size != 0 ? size : 1` cannot fail a test on either. The standard requires it.
- **The counter does not call the `new_handler`.** A replacement need not, and no test exhausts memory.
- **`last_function()` is read straight after a call.** An assertion that allocated in between would
  overwrite it. gtest allocates only to describe a failure, after the failure is recorded.
- **Allocation elision.** If a compiler removed an escaped `new` anyway, `NewExpressionsAreCounted` would
  fail with a count of 0 rather than pass.
- **GCC 15 warnings.** The change adds atomics, `posix_memalign()` and one-line functions: no
  `std::function`, no reference into a temporary, no unbraced `if` over a gtest macro (the rules in
  `CLAUDE.md`). GCC 13.3 is the container's, so a GCC 15-only warning is CI's to find.
- **The prototype is not the commit.** Its files live in a scratchpad. Every number above is re-run from the
  committed files in Steps 2 and 3.
- **Assumptions:** nothing else in the 40 tests is affected (measured: 38 pass today); neither suite nor the
  engine has an over-aligned type (`grep` for `alignas` over `include/`, `tests/`, `benchmarks/` and
  `examples/` finds one use, a local buffer in `test_dag_async.cpp`, which is not a heap allocation).

## Progress

| Step | Status | Notes |
|---|---|---|
| 0 — Approval | Proposed | |
| 1 — Start from green | Not started | |
| 2 — The counter, with its tests | Not started | |
| 3 — The two suites count through it | Not started | |
| 4 — Close | Not started | |
