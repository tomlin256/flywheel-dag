# Take the Mean Over the Cycles That Have Ended

**Status: Proposed (2026-10-11), awaiting approval.** `Engine::meanCycleUs()` divides the total by a
count of the cycles that have ended, written beside the total, and not by `cycleCount()`, which also
counts the cycle a read lands in. A read from an output callback, or from another thread inside a
cycle, then returns the mean of the cycles that have ended, as the other duration statistics do.

Closes [flywheel-dag#34](https://github.com/tomlin256/flywheel-dag/issues/34).

## Problem

`Engine::cycle()` adds one to `cycles_` on entry, and its scope guard adds the cycle's duration to
`totalCycleNs_` as the cycle ends. `meanCycleUs()` divides the total by `cycles_`, so a read while a
cycle is in flight counts that cycle at zero duration: for n ended cycles it returns n/(n+1) of their
mean. `lastCycleUs()`, `minCycleUs()`, `maxCycleUs()` and `rollingMeanCycleUs()` are written when a
cycle ends, so they leave the cycle in flight out, and `meanCycleUs()` alone counts it.

Two reads reach it: an output callback, which runs on the cycle's own thread, and a read from
another thread that lands inside a cycle. `CycleSeqLock::readConsistent()` makes a reader on another
thread wait the cycle out, but it returns at once on the cycle's own thread, so a callback cannot get
the mean of the ended cycles through it.

On `ce5e5e2`, the new tests read `meanCycleUs()` from an output callback, in µs, against the mean of
the cycles that had ended. The ratios are the issue's:

| cycle | `meanCycleUs()` | mean of the ended cycles | ratio |
|---|---|---|---|
| 2 | 0.4170 | 0.8340 | 0.500 |
| 3 | 0.3477 | 0.5215 | 0.667 |
| 4 | 0.3338 | 0.4450 | 0.750 |
| 5 | 0.2836 | 0.3545 | 0.800 |
| 6 | 0.2850 | 0.3420 | 0.833 |

The error is one cycle's duration in n + 1, so it fades as an engine runs. It matters in the first
cycles, and for a callback that reports the mean on every cycle.
[flywheel-dag#21](https://github.com/tomlin256/flywheel-dag/issues/21) was the same defect for a
cycle that throws, and v0.1.15 fixed it.

## The fix

A count of the cycles the total sums, recorded by the guard that records the total:

```cpp
// Engine, private, beside totalCycleNs_:
std::atomic<uint64_t> timedCycles_{0};

// the guard in cycle(), after the total:
engine.totalCycleNs_.fetch_add(ns, std::memory_order_relaxed);
engine.timedCycles_.fetch_add(1, std::memory_order_relaxed);

inline double Engine::meanCycleUs() const {
    const uint64_t timed = timedCycles_.load(std::memory_order_relaxed);
    return (timed == 0) ? 0.0
        : static_cast<double>(totalCycleNs_.load(std::memory_order_relaxed))
            / static_cast<double>(timed) / 1'000.0;
}
```

- **The divisor is the cycles the total covers.** The guard runs on every exit, a throw included
  (flywheel-dag#21), so every cycle that ends adds its duration to the total and one to the count.
  The mean then covers the cycles that have ended, as the other statistics do. This is the fix the
  issue sketches.
- **`cycles_` and `cycleCount()` do not change.** A callback that reads `cycleCount()` sees its own
  cycle's number, which `ReplayAlignment.TwoInputsOneQueueInterleavedSeqs` pins. Counting a cycle at
  its end instead would fix the mean with no new atomic, and make every such callback read one less.
- **Nothing ended reads 0,** as before: the count is 0, and the test for it keeps the division off.
- **Two stores, total first.** A reader on another thread that lands between them reads the new total
  over the old count, a mean high by 1/n. The window is two adjacent stores, once per cycle, where
  the defect it replaces is open for a whole cycle. `CycleSeqLock::readConsistent()` waits it out,
  since the guard records while the cycle is open to a seqlock reader. Closing it would take a mean
  that is one atomic, which restructures a statistic that is advisory; it is not taken.
- **Comments carry the contract.** The paragraph over the statistics in `dag_engine.hpp`, which names
  this defect as open, says that every duration statistic covers the cycles that have ended and that
  `cycleCount()` alone counts a cycle as it begins. A comment over `totalCycleNs_` and `timedCycles_`
  says why the divisor is not `cycles_`, and the window. The comment over the guard in
  `dag_engine.inl` says the guard counts the cycle in `timedCycles_` beside its duration.
- **Source-compatible.** No signature changes and one private member, so this ships as v0.2.3, as
  v0.2.2 did.

## Tests

In `test_dag_async.cpp`, in the `EngineCycleStats` section after the tests of flywheel-dag#21. A
helper steps an engine through six cycles with a new value each time. The output callback of each
cycle reads the mean through a function the test passes in, and `lastCycleUs()` read after each step
is that cycle's duration. The callback of cycle k + 1 runs with k cycles ended, so it must read 0 in
the first cycle and the average of the first k durations after. Nothing sleeps: the second test's
reader runs on another thread, and the callback waits for it with `std::async(...).get()`, so the
read lands inside the cycle however the threads are scheduled.

| # | Test | Asserts | Fails under |
|---|---|---|---|
| 1 | `EngineCycleStats.MeanReadInACallbackCoversTheCyclesThatHaveEnded` | the read in each of six callbacks is the mean of the cycles that had ended: 0, then the running average of the durations | `meanCycleUs()` divides by `cycles_`; the count is bumped on entry, beside `cycles_` |
| 2 | `EngineCycleStats.MeanReadFromAnotherThreadInsideACycleCoversTheCyclesThatHaveEnded` | the same, with the read made by another thread while the cycle is open | the same two |

No test is modified. The 116 tests of `test_dag_async` that exist pass untouched. Four of them, the
tests of flywheel-dag#21, also pin that a cycle that throws is in the divisor: they fail if the count
is bumped at the end of `cycle()` and a throw skips it.

## Prototype

Built on scratch copies of the repo (`git archive` of `ce5e5e2`, v0.2.2, and the same with the
change), with the flags of `build/compile_commands.json` and `-Werror`, under Apple Clang on arm64.
The repo tree is untouched. The code is the sketch above, and the tests are the table.

- **Tests.** With the change the build prints no warnings and `test_dag_async` passes 118 of 118:
  the 116 it has and the 2 new. Against `ce5e5e2`'s headers the 2 new tests fail, with the reads of
  the table above, and the 116 pass.
- **Mutations.** One change per scratch copy of `include/`, against the new test file. Divide by
  `cycles_`: tests 1 and 2 fail, and nothing else. Bump the count on entry, beside `cycles_`: the
  same. Bump the count at the end of `cycle()`, out of the guard: the three `...IsTimed` tests and
  `StatsCoverACycleThatThrewAndOneThatDidNot` fail, and tests 1 and 2 pass.
- **ThreadSanitizer.** With the change, the 12 `EngineCycleStats` tests, among them the two that read
  the statistics while another thread steps, and the 6 `CycleSeqLock` tests pass under
  `-fsanitize=thread` with no report.
- **Hot path.** `bench_hot_path --invariants` matches `benchmarks/expected_invariants.txt` for every
  build below. The change adds one atomic add per cycle, to the guard, so the cost is timed.
  - *One binary each.* 40 rounds, each running four builds in a random order: `ce5e5e2`; the change;
    a control with the new member and the getter and nothing stored in the guard, whose hot path is
    `ce5e5e2`'s; and the change with the count written as a load and a store. Medians in ns/cycle,
    chain, idle-queues and ingest. `ce5e5e2`: 257.4, 230.2, 113.8. The change: 258.1, 231.1, 114.3
    (+0.3%, +0.4%, +0.4%). The load and store: 257.4, 231.4, 114.7. The control: 258.2, 254.8, 113.2
    (+0.3%, +10.6%, -0.5%). The control does the same work as `ce5e5e2` and reads 10.6% slower on
    idle-queues, so one pair of binaries reads code layout as often as cost.
  - *Eight code alignments.* A padding function of 0 to 21 nops, in steps of 3, ahead of the
    benchmark's other code moves `Engine::cycle()` through eight alignments. 15 shuffled rounds of
    every build at every alignment. The change against `ce5e5e2` at the same alignment, as the median
    over the eight and then the lowest and highest: chain -0.21% (-0.53% to +0.46%), idle-queues
    +0.06% (-0.68% to +0.51%), ingest +0.47% (-0.93% to +1.28%). Ingest has the shortest cycle, 117
    ns, so one more add shows there first; it reads above zero at six alignments and below at two.
    The load and store: -0.44%, -0.02% and -0.09%, no cheaper, so the guard keeps `fetch_add` as its
    neighbours do.

## Steps

Every commit subject is scoped to flywheel-dag#34 in the repo's `type(#N): …` form. The subjects
below omit the scope.

### Step 1 — The tests and the fix

Written: tests 1 and 2. Changed: `dag_engine.hpp` (the member and the comments over it and over the
statistics) and `dag_engine.inl` (`meanCycleUs()`, the guard and the comment over it).

**Done when:**

- the 2 tests fail on `ce5e5e2`'s headers and under the two changes that divide by `cycles_` or bump
  the count on entry, and pass with the fix; the four tests of flywheel-dag#21 fail when the count is
  bumped out of the guard. They are checked on scratch copies of `include/`, since the repo's own tree
  is never mutated, with a control copy that passes them all;
- the build prints no warnings with `-DFLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON`, and ctest is green
  (36 of 36);
- `bench_hot_path --invariants` matches `benchmarks/expected_invariants.txt`, and no row is more than
  1% slower than `ce5e5e2` at the median over eight code alignments, timed on the same machine in
  shuffled rounds.

Commit: `fix: take meanCycleUs() over the cycles that have ended`.

### Step 2 — Release and close out

- Release v0.2.3 as v0.2.2 was released: the project version and the FetchContent tag in
  `CMakeLists.txt` and the README move to 0.2.3 and v0.2.3. Then the tag, and a GitHub release with
  notes. There is no **Breaking** section: no signature changes.
- Mark this plan done, with the mutations and the timings rerun on the committed tree.
- Push `main`, watch CI on both legs, and close flywheel-dag#34 with a summary comment.
- Stop before the push, the tag and the release for a go-ahead.

**Done when:** the build prints no warnings with `-DFLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON` and ctest is
green (36 of 36), CI is green on the release commit on both legs, the installed version file reports
0.2.3, the release is published, and flywheel-dag#34 is closed.

Commits: `build: release v0.2.3` and `docs: mark the plan done`.

## Not in this plan

- **Injecting the engine's clock.** It would let the statistics tests assert exact values. It adds
  public API for tests, and the new tests compare the statistics with each other, as the existing ones
  do.
- **A mean that is one atomic.** The cycle's thread would keep the total and the count in plain
  fields and publish the finished mean in one `std::atomic<double>`, which no reader could see torn.
  It closes the window between the two stores, and it replaces the total with fields and a divide in
  the guard. The statistic is advisory and `readConsistent()` gives a reader a whole set, so the plan
  takes the count the issue sketches.

## Self-review — risks and assumptions

- **One more atomic add per cycle.** Within the 1% bar at the median on every row, at eight
  alignments; ingest, the shortest cycle, reads +0.5% and may carry a real cost of that size. The
  step 1 check repeats the timing on the committed tree.
- **A reader between the two stores reads the mean high by 1/n.** The window is two adjacent stores
  per cycle. No test reaches it: a deterministic one would need a hook inside the guard. The comment
  over the members states it, and that `readConsistent()` waits it out.
- **A callback now reads a larger mean than before,** by 1/n of the mean in the cycle after n have
  ended. That is the correction, and the release notes say so under **Fixed**. An application that
  logs `meanCycleUs()` from a callback will see its first values move.
- **`timedCycles_` equals `cycles_` whenever no cycle is in flight.** `cycles_` is bumped and the
  guard is built on the next line, and nothing between can throw. A destructor that throws
  terminates, as flywheel-dag#21's plan said, and the recording can throw only from the rolling
  window's mutex. No test reads `timedCycles_`, which is private; the mean pins it.
- **The tests detect the defect only if the first cycle measures above zero.** A new engine's first
  cycle took 0.83 µs in the prototype's run, against a 41 ns tick, and the later cycles of test 1
  took 0.08 to 0.3 µs. A clock too coarse for that would pass the tests under the defect as well as
  under the fix; it could never fail the fix. `EXPECT_NEAR`'s 1e-9 is far above the rounding of an
  integer count of nanoseconds divided by 1,000.
- **Test 2 starts a thread in each of six cycles,** and the callback joins it with `get()` before it
  returns, so no thread outlives its cycle.
- **The timings are one machine and one compiler.** Apple Clang on arm64. CI compares the benchmark's
  exact columns and never its timings. CI's GCC leg is the first to build the new tests there; neither
  GCC-only pattern `CLAUDE.md` lists, a reference into a temporary or an unbraced `if` around a gtest
  macro, appears in them.
- **Pushing and releasing are assumed,** as in the earlier plans. If either is to be done by hand,
  step 2 stops before it.
