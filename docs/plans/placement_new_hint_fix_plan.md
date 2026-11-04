# Build the Wake-Hook Test's Engine Without Placement New

**Status: Proposed.** Awaiting approval.

Closes [flywheel-dag#61](https://github.com/tomlin256/flywheel-dag/issues/61).

## Problem

clangd 23.1.4, the version the editor starts, puts a hint on the `<new>` include of
`tests/test_dag_async.cpp` (line 51):

> Included header new is not used directly [unused-includes]

The include is used. `wireThenDestroyTheEngine()` builds an `Engine` in a buffer of its own with
`new (storage) Engine`, and the placement forms of `operator new` are declared in `<new>`. The issue
gives line 2051; the file has grown since, and the call is at line 2070 now.

The tool is wrong, and about placement new alone. Each row is a clangd 23.1.4 LSP session that opens
the file as an editor does and reads what the server publishes, or the named tool run by hand:

| Probe | Result |
|---|---|
| clangd, `tests/test_dag_async.cpp` | 1 diagnostic: `unused-includes` at 51:1 |
| clangd, a six-line file: `#include <new>` and one `new (storage) int(1)` | the same hint |
| clangd, a file that uses `new (std::nothrow) int(1)` | none |
| clangd, the other three files that include `<new>` (`tests/allocation_counter.cpp`, `tests/test_allocation_counter.cpp`, `benchmarks/bench_hot_path.cpp`) | none |
| `clang-include-cleaner` and clang-tidy's `misc-include-cleaner`, `tests/test_dag_async.cpp` | no unused include (both list includes the file lacks, which this issue is not about) |

So the hint is clangd's alone, and this file is its only site in the repository.

## The change

In `wireThenDestroyTheEngine()`:

```cpp
// was: Engine* engine = new (storage) Engine;   ...   engine->~Engine();
Engine* const engine = std::construct_at(reinterpret_cast<Engine*>(storage));
wire(*engine);
std::destroy_at(engine);
```

and `#include <new>` goes. `<memory>`, which the file includes, declares both functions. Nothing else in
the file uses `<new>` (no `std::nothrow`, `std::launder`, `std::bad_alloc` or the like), so there is
nothing left for the tool to miss. A sentence in the helper's comment says why it is not placement new,
so that nobody "simplifies" it back and the hint returns.

The fill of the dead Engine's bytes with `0xFF`, and the volatile pointer it goes through, are not
touched. They are what makes the tests fail the same way on every platform.

**Prototype** on a scratch copy of the test, repository untouched: clangd publishes 0 diagnostics for it
(1 before), and the copy compiles clean with the Linux tree's flags (`-O2 -Wall -Wextra -Wpedantic
-Werror`) under GCC 13.3.0 in `ubuntu-dev` and GCC 15.2.0 in a throwaway `ubuntu:26.04`.

## Decisions

- **D1. Replace the placement new; do not silence the hint.** `// IWYU pragma: keep` on the include
  would hide it, and leave an include that only a tool which cannot see its use is happy to keep, which
  outlives the placement new if the helper ever changes. The replacement is three lines and removes an
  include.
- **D2. `reinterpret_cast`.** `storage` is `alignas(Engine) unsigned char[sizeof(Engine)]`, so the cast
  is to an aligned address. `construct_at` returns the pointer to the new object, and that is the one
  used, so no `std::launder` is needed.
- **D3. Same construction.** `Engine`'s constructor is user-provided (`explicit Engine(std::size_t =
  kDefaultRollingCycleWindow)`), so `construct_at(p)`, which value-initialises, calls the constructor
  the old line did, with the same argument.

## Steps

### Step 1 — The plan

Commit: `docs: add plan for building the wake-hook test's Engine without placement new`. **Done when:**
the plan is committed and a comment on the issue gives its path.

### Step 2 — The change

The edit above, and the comment. **Tests written or modified: none.** The four `EngineWakeHook` tests
that call the helper (`QueuePostAfterTheEngineIsGone`, `AsyncInputPostAfterTheEngineIsGone`,
`RegistryMemberPostAfterTheEngineIsGone`, `MakeInputSetAfterTheEngineIsGone`) are its tests, and the
hand-made change below shows they still bite.

**Done when:**

- the build prints no warning and `ctest --preset relwithdebinfo-macos-arm64` passes in full (46 of 46
  today);
- clangd publishes 0 diagnostics for the file;
- clang-tidy, as the `clang_tidy_` tests run it (`.clang-tidy`, `--warnings-as-errors=*`), reports
  nothing for the file;
- the file compiles clean with the Linux tree's flags under GCC 13.3.0 and GCC 15.2.0;
- the hand-made change fails as stated, on a scratch copy of `include/`, with the Mac's compiler and
  with the container's GCC:

  | Change | Fails |
  |---|---|
  | `Engine::makeWakeHook()` captures `this` again (`[this] { wake_->signalWork(); }`) | the first four `EngineWakeHook` tests, and not `APostStillWakesARunningEngine` |

- CI is green on both legs after the push.

Commit: `test: build the wake-hook test's Engine with construct_at, not placement new`.

### Step 3 — Close out

Status Done, the Outcome, and the issue closed once the push's run is green. A docs commit; no file the
build reads changes.

## Self-review — risks and assumptions

- **The engine is not touched.** Only a test helper changes, so the benchmark's invariants and
  `Engine::cycle()` cannot move, and I do not run them.
- **The tests could lose their teeth silently.** Nothing asserts the helper still constructs, destroys
  and fills. The hand-made change is the check: its hook reads the Engine's `wake_` through `this`, which
  is a live pointer if the Engine was never destroyed, and a freed `WakeState` that still reads fine if
  its bytes were never overwritten. Lose either step and the four tests pass on the bug. The fill loop
  itself is unchanged.
- **GCC reads the new code differently.** A cast pointer into a stack array, then a write after
  `destroy_at`, is the shape GCC's `-Wmaybe-uninitialized`, `-Wdangling-pointer` and `-Wuse-after-free`
  look at. The prototype is clean under 13.3 and 15.2, and the final file is compiled again.
- **A fixed clangd.** If a later clangd counts placement new as a use, nothing here is wrong: the code
  is right without `<new>`.
- **Out of scope.** `misc-include-cleaner` lists includes this file lacks (`<cstddef>`, `<mutex>`,
  `<condition_variable>` and others). That is the Include Cleaner decision the clang-tidy plan left
  open, not this issue.
