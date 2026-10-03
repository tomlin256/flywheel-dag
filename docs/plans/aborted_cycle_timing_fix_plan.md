# Time a Cycle That Throws

**Status: Done (2026-10-03).** Approved 2026-10-03. Both steps landed, and v0.1.15 is released.
`Engine::cycle()` times itself from a scope guard, so a cycle that a node, a callback or a source
throws out of is timed up to the throw, and every duration statistic covers every cycle that has
ended.

Closes [flywheel-dag#21](https://github.com/tomlin256/flywheel-dag/issues/21).

## Problem

`cycle()` adds one to `cycles_` on entry and records the cycle's duration in its last lines. A throw
out of a source's `flush()`, a node's `eval()` or an output callback skips those lines, so the
cycle is counted and never timed:

- `meanCycleUs()` divides the total by `cycles_`, so it counts the cycle at zero and reads low;
- `lastCycleUs()`, `minCycleUs()`, `maxCycleUs()` and `rollingMeanCycleUs()` leave it out.

The issue's sequence is one `step()` whose callback throws, then one that does not. On `ef531e6`,
the engine as it stands, `cycleCount()` is 2, `lastCycleUs()` after the throw reads 0, and after the second step
the minimum, maximum and rolling mean read 1.667 µs while `meanCycleUs()` reads 0.834, half of the
minimum.

The error is one cycle's duration per throw. [flywheel-dag#16](https://github.com/tomlin256/flywheel-dag/issues/16)
lets `run()` start again after a throw, so a long-lived engine can accumulate them.

## The fix

A guard in `cycle()` records the duration on every exit:

```cpp
inline void Engine::cycle() {
    const CycleSeqLock::WriteScope writeScope(*cycleSeqLock_);
    cycles_.fetch_add(1, std::memory_order_relaxed);

    struct TimeOnExit {
        Engine&                                     engine;
        const std::chrono::steady_clock::time_point started;
        ~TimeOnExit() {
            const auto ns = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - started).count());
            engine.lastCycleNs_.store(ns, std::memory_order_relaxed);
            // ... the total, minimum, maximum and rolling window, as the last lines of cycle() do now
        }
    };
    const TimeOnExit timeCycle{*this, std::chrono::steady_clock::now()};

    // 1. Flush ... 2. Mark ... 3. Evaluate: unchanged, minus the timing lines at the end.
}
```

It follows `run()`'s `ClearOnExit` and `CycleSeqLock::WriteScope`, which close on a throw the same
way.

- **Every counted cycle is timed.** `cycles_` still counts on entry, which callbacks rely on: one
  that reads `cycleCount()` sees its own cycle's number. The guard is built on the next line, and
  nothing between can throw.
- **Declared after `writeScope`, so destroyed before it.** The statistics are written while the
  cycle is open to a `CycleSeqLock` reader, as the last lines of `cycle()` write them today.
  Declared before it, a reader could see the cycle closed and its duration missing.
- **The destructor holds the recording itself.** A first draft called a private
  `recordCycle(ns)` from it. Apple Clang does not inline that, and `bench_hot_path`'s idle-queues
  row slowed by 3%. The body in the destructor measures the same as `ef531e6`.
- **A throwing cycle's duration is not a full cycle's.** It runs from `cycle()`'s start to the
  unwind out of it: shorter by the work it skipped, longer by the cost of the throw, which a probe
  put at 2.3 to 30.7 µs, and 96 µs for the process's first throw, against a normal cycle of about
  1.4 µs. It enters the minimum, maximum and both means as measured, and the comment on the
  statistics says so.
- **Excluding it instead is not taken.** Dividing the mean by a count of timed cycles and leaving
  every throw out would hide a slow cycle that then throws from `maxCycleUs()`, and leave the
  statistics covering fewer cycles than `cycleCount()`.

The comment over the statistics in `dag_engine.hpp` becomes:

```cpp
    // A cycle is timed when it ends, whether it returns or throws, so every statistic covers every
    // cycle that has ended. One that throws is timed up to the throw: shorter than a full cycle by
    // the work it skipped, longer by the cost of the throw. A meanCycleUs() read during a cycle
    // counts it before it is timed, and reads low (flywheel-dag#34).
```

## Tests

In `test_dag_async.cpp`, in the `EngineCycleStats` section, with `#include <algorithm>` and a
`ThrowingSource` (an `IFlushable` whose `flush()` throws). Each drives `Engine::step()` and checks
the statistics against each other and against `lastCycleUs()`, read between steps. Nothing sleeps.

| Test | Asserts | `ef531e6` |
|---|---|---|
| `ACycleThatThrowsFromACallbackIsTimed` | One `step()` throws from an output callback. `cycleCount()` is 1, `lastCycleUs()` is above 0, and the minimum, maximum, mean and rolling mean equal it | `lastCycleUs()` is 0 |
| `ACycleThatThrowsFromANodeIsTimed` | The same, with a `ComputeNode` functor that throws | `lastCycleUs()` is 0 |
| `ACycleThatThrowsFromASourceIsTimed` | The same, with a source whose `flush()` throws | `lastCycleUs()` is 0 |
| `StatsCoverACycleThatThrewAndOneThatDidNot` | The issue's sequence. After the second step the minimum and maximum are the smaller and larger of the two cycles' durations, and the mean and rolling mean are their average | the minimum is the second cycle's duration, where the throwing cycle read 0 |

One test per throw site, so a guard that covers part of `cycle()` fails the test for the part it
misses.

## Prototype

Built on scratch copies of `include/` and `tests/test_dag_async.cpp`, with the flags of
`build/compile_commands.json` and `-Werror`, under Apple Clang. The repo tree is untouched.

- **Tests.** The 77 existing tests of `test_dag_async` pass on `ef531e6` and with the fix. The four
  new tests fail on `ef531e6`, each for the reason in the table, and with the fix all 81 pass.
- **The issue's sequence with the fix.** The throwing cycle reads 95.792 µs, the process's first
  throw. The next cycle reads 1.417. The minimum is 1.417, the maximum 95.792, and the mean and
  rolling mean 48.605, their average.
- **The margin under the one absolute assertion.** The tests assert that a throwing cycle measures
  above 0. The clock's tick on this machine is 41 ns, and the shortest of 20,000 throwing cycles was
  2.291 µs, 55 ticks.
- **Hot path.** `bench_hot_path --invariants` matches `benchmarks/expected_invariants.txt`. Medians
  of 30 alternating runs, in ns/cycle, `ef531e6` against the fix: chain 256.9 and 256.2, idle-queues
  225.2 and 226.6, ingest 112.0 and 112.7.
- **The declaration order is not pinned.** Moving the guard above `writeScope` passes all 81 tests.
  A deterministic test would need a hook inside the cycle's exit, so the comment carries the rule.

## Steps

Every commit subject is scoped to flywheel-dag#21 in the repo's `type(#N): …` form. The subjects
below omit the scope.

### Step 1 — The tests and the fix

Written: the four tests above. Modified: none. The fix is in `dag_engine.inl`, and the comment over
the statistics in `dag_engine.hpp`. A comment over the guard says why it is declared after
`writeScope` and why it holds the recording.

**Done when:**

- the four tests fail on `ef531e6` for the reasons given, and pass with the fix. They are checked on
  scratch copies of the tree, since the repo's own tree is never mutated;
- the build prints no warnings with `FLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON`, and ctest is green
  (36 of 36);
- `bench_hot_path --invariants` matches `benchmarks/expected_invariants.txt`, and no row is slower
  than `ef531e6` beyond run-to-run noise, timed on the same machine in alternating runs.

Commit: `fix: time a cycle that throws`.

### Step 2 — Release and close out

- Release v0.1.15 as v0.1.14 was released: the project version and the FetchContent snippets in
  `CMakeLists.txt` and the README, the tag, and a GitHub release with notes.
- Mark this plan done.
- Push `main`, watch CI on both legs, and close flywheel-dag#21 with a summary comment.

**Done when:** ctest is green (36 of 36), CI is green on the release commit on both legs, the
installed version file reports 0.1.15, the release is published, and flywheel-dag#21 is closed.

Commits: `build: release v0.1.15` and `docs: mark the plan done`.

## Outcome

Step 1 landed as one commit and step 2 as the release commit. CI run 37142553479 on the release
commit is green on both legs, `ubuntu-latest` with GCC and `macos-latest` with Apple Clang, with
`FLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON`, so the local struct raised no GCC-only warning. Locally the
build printed no warnings, ctest was 36 of 36, and the installed version file reports 0.1.15.
`v0.1.15` is tagged on the release commit and released, and flywheel-dag#21 is closed.

The checks on scratch copies of `include/`, with the repo's own tree untouched:

- **The tests.** Built against `include/` from `ef531e6` and from v0.1.14, 77 tests of
  `test_dag_async` pass and the four new ones fail. The three site tests fail on `lastCycleUs()`
  reading 0, and `StatsCoverACycleThatThrewAndOneThatDidNot` on the minimum and the rolling mean.
  With the fix all 81 pass.
- **The hot path.** `bench_hot_path --invariants` matches `benchmarks/expected_invariants.txt`.
  Over 40 rounds in random order, medians in ns/cycle, `ef531e6` against the fix: chain 259.1 and
  259.6, idle-queues 231.6 and 232.2, ingest 116.1 and 117.0. Ingest's 0.9 ns (0.8%) is the one
  row above, inside the spread of a single series, about 6 ns.

## Not in this plan

- **`meanCycleUs()` reads low during a cycle** —
  [flywheel-dag#34](https://github.com/tomlin256/flywheel-dag/issues/34), found while planning this.
  The cycle in flight is counted before it is timed, the same defect for a cycle that has not ended.
  It needs a count of timed cycles beside the total, a design of its own. The comment above names it.
- **Injecting the engine's clock.** It would let the statistics tests assert exact values. It adds
  public API for tests, and the existing statistics tests compare the statistics with each other.

## Self-review — risks and assumptions

- **A destructor that throws terminates.** The recording can throw only if `std::mutex::lock()` does,
  inside `RollingCycleWindow::record()`. That mutex is plain, never nested and held for a few
  instructions, so it does not throw in practice. Before, such a throw would have left `cycle()` as
  an exception, and now it ends the process.
- **A throw is dearer than a cycle.** A throwing cycle often reads longer than the cheap cycles
  around it, so an application that throws often sees it in `maxCycleUs()` and the means. That is
  the cycle's real duration, and the header comment says it.
- **One assertion depends on the clock.** `lastCycleUs() > 0` for a throwing cycle fails on a clock
  whose tick exceeds a throw's cost, 41 ns against at least 2.3 µs here. It would fail the test,
  never hide the bug. The other assertions hold for any durations, so they never depend on the clock.
- **The order of the two guards has no test.** A reorder compiles and passes. A reader sees the
  effect only by landing between the two destructors, so no deterministic test reaches it, and the
  comment states the rule.
- **Timings are one machine and one compiler.** The inlining finding is Apple Clang's. GCC may
  inline the helper or not, and CI compares the exact columns of the benchmark, never its timings.
  The local struct has a reference member and a user-provided destructor, and `-Wextra` is quiet
  about it under Clang. CI's GCC leg is the first to see it there.
- **Pushing and releasing are assumed.** The earlier plans end with CI green on both legs and a
  release. If either is to be done by hand, step 2 stops before it.
