# Take the Mean Over the Cycles That Have Ended

**Status: Proposed (2026-10-11), awaiting approval.** `Engine::meanCycleUs()` returns a mean that the
cycle's thread publishes, in one atomic word, as each cycle ends. It does not divide the total by
`cycleCount()`, which also counts the cycle a read lands in. A read from an output callback, or from
another thread inside a cycle, then returns the mean of the cycles that have ended, as the other
duration statistics do, and no read sees a total from one cycle over a count from another.

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

The guard in `cycle()` times every cycle that ends. It also counts the cycle beside its duration,
divides, and publishes the mean of the cycles counted so far in one atomic word, which
`meanCycleUs()` loads:

```cpp
// Engine, private. The cycle's thread alone touches the total and the count; the mean is the one
// word another thread reads.
uint64_t            totalCycleNs_ = 0;   // was std::atomic<uint64_t>
uint64_t            timedCycles_  = 0;
std::atomic<double> meanCycleNs_{0.0};

// the guard in cycle(), where it added to the total:
engine.totalCycleNs_ += ns;
++engine.timedCycles_;
const double mean = static_cast<double>(engine.totalCycleNs_)
                        / static_cast<double>(engine.timedCycles_);
// ... the minimum, the maximum and rollingCycles_.record(ns), as now ...
engine.meanCycleNs_.store(mean, std::memory_order_relaxed);   // last

inline double Engine::meanCycleUs() const {
    return meanCycleNs_.load(std::memory_order_relaxed) / 1'000.0;
}
```

- **The divisor is the cycles the total sums.** The guard runs on every exit, a throw included
  (flywheel-dag#21), and adds a cycle's duration to the total and one to the count together. The mean
  then covers the cycles that have ended, as the other statistics do. This is the count the issue
  sketches, written beside the total.
- **One word, since two would leave a window.** The issue's sketch stores the count beside the total,
  and `meanCycleUs()` divides one by the other. A reader on another thread that lands between the two
  stores reads the new total over the old count, a mean high by 1/n. A copy of v0.2.2 with that
  sketch applied reaches the window in every run of the prototype's stress test (below). Published
  whole, the mean is the one from before a cycle ends or the one from after, and each is the mean
  of the cycles that had ended. Every other statistic is already one word, or, for the rolling
  mean, read under the window's mutex, so after this no statistic reads torn. Two statistics are
  still two reads: `CycleSeqLock::readConsistent()` is how a reader gets a set.
- **Divide first, store last.** Stored right after the divide, the mean reads 1% to 3% slower in
  `bench_hot_path`; stored after `rollingCycles_.record(ns)` it reads within 0.2% of the issue's
  sketch (Prototype). The comment over the guard says so. The ordering is a measured effect under
  Apple Clang on arm64; the code is correct in either order.
- **The same value.** The mean is the total over the count in nanoseconds, and the getter divides by
  1,000: the operations v0.2.2 performs on the same total and count, in the same order, so a quiet
  engine reads what it read.
- **No atomic read-modify-write is added.** Only the cycle's thread touches the total and the count,
  as it alone touches `due_` and `flushList_`, so they are plain fields, and the guard's atomic add
  of the total goes. What it adds is a divide and a relaxed store, an ordinary store instruction on
  x86-64 and on arm64.
- **`cycles_` and `cycleCount()` do not change.** A callback that reads `cycleCount()` sees its own
  cycle's number, which `ReplayAlignment.TwoInputsOneQueueInterleavedSeqs` pins. Counting a cycle at
  its end instead would fix the mean with no new member, and make every such callback read one less.
- **Nothing ended reads 0,** as before: the word starts at 0.0, and nothing divides until a cycle has
  ended.
- **Comments carry the contract.** The paragraph over the statistics in `dag_engine.hpp`, which names
  this defect as open, says that every duration statistic covers the cycles that have ended and that
  `cycleCount()` alone counts a cycle as it begins. A comment over the three members says that the
  cycle's thread alone touches the total and the count, that the mean goes out in one store so a
  reader never sees them out of step, and why the divisor is not `cycles_`. The comment over the guard
  in `dag_engine.inl` says it counts the cycle beside its duration, publishes the mean, and divides
  first and stores last.
- **Source-compatible.** No signature changes, and the members are private, so this ships as v0.2.3,
  as v0.2.2 did.

## Tests

In `test_dag_async.cpp`, in the `EngineCycleStats` section after the tests of flywheel-dag#21.

Tests 1 and 2 are deterministic. A helper steps an engine through six cycles with a new value each
time. The output callback of each cycle reads the mean through a function the test passes in, and
`lastCycleUs()` read after each step is that cycle's duration. The callback of cycle k + 1 runs with
k cycles ended, so it must read 0 in the first cycle and the average of the first k durations after.
Nothing sleeps: test 2's reader runs on another thread, and the callback waits for it with
`std::async(...).get()`, so the read lands inside the cycle however the threads are scheduled.

Test 3 is a stress test, in the shape of `stressConcurrentReads`: a writer thread steps 5,000 cycles,
the calling thread reads until it finishes, and the test asserts after the join. The reader keeps a
mean when it differs from the one before. The output callback of each cycle reads the previous
cycle's duration, which gives every duration, and so the mean of the first k cycles for every k.
Each mean the reader kept must be one of those, and the last read, after the join, must be the mean
of all 5,000. The bound is a cycle count, and nothing sleeps or times out.

| # | Test | Asserts | Fails under |
|---|---|---|---|
| 1 | `EngineCycleStats.MeanReadInACallbackCoversTheCyclesThatHaveEnded` | the read in each of six callbacks is the mean of the cycles that had ended: 0, then the running average of the durations | a mean refreshed on entry from `cycles_`; a mean published a cycle late |
| 2 | `EngineCycleStats.MeanReadFromAnotherThreadInsideACycleCoversTheCyclesThatHaveEnded` | the same, with the read made by another thread while the cycle is open | the same two |
| 3 | `EngineCycleStats.MeanReadWhileAnotherThreadStepsIsTheMeanOfSomePrefixOfTheCycles` | every mean a reader keeps while another thread steps is the mean of the first k cycles for some k, and the last is the mean of all 5,000 | the same two; a total and a count stored apart; a getter that divides the plain fields |

Tests 1 and 2 read inside a cycle, so they cannot reach the end of one: a copy that stores the total
and the count apart, as the issue sketches, passes both, and test 3 alone pins the single store. Which
reads land where is up to the scheduler, so test 3 passes a defect that no read reaches. A correct
engine cannot fail it: every mean it publishes is one of the means the test checks against.

No test is modified. The 116 tests of `test_dag_async` that exist pass untouched. Four of them, the
tests of flywheel-dag#21, also pin that a cycle that throws is in the mean: they fail if the count is
bumped at the end of `cycle()` and a throw skips it.

## Prototype

Built on scratch copies of the repo (`git archive` of `ce5e5e2`, v0.2.2, and the same with the
change), with the flags of `build/compile_commands.json` and `-Werror`, under Apple Clang on arm64.
The repo tree is untouched. The code is the sketch above, and the tests are the table.

- **Tests.** A full build of the changed copy with `-Werror=dev -Werror=deprecated
  -DFLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON` prints no warnings, and ctest passes 36 of 36.
  `test_dag_async` passes 119 of 119: the 116 it has and the 3 new. Against `ce5e5e2`'s headers the
  3 new tests fail and the 116 pass.
- **Mutations.** One change per scratch copy of `include/`, against the new test file:

  | Change to the fix | Fails |
  |---|---|
  | `cycle()` also refreshes the mean on entry, dividing by `cycles_` (the defect) | 1, 2, 3 |
  | the guard publishes the mean it had before it added the cycle (a cycle late) | 1, 2, 3, and the four tests of flywheel-dag#21 |
  | the count is bumped at the end of `cycle()`, out of the guard (a throw skips it) | the four tests of flywheel-dag#21 |
  | `meanCycleUs()` divides the plain total by the plain count | 3 |
  | the total and the count stored apart, beside each other (the issue's sketch) | 3 |

  The four tests of flywheel-dag#21 are the three `ACycleThatThrowsFrom...IsTimed` tests and
  `StatsCoverACycleThatThrewAndOneThatDidNot`. Nothing else fails under any change. Storing the mean
  right after the divide changes no test.
- **Stress.** Test 3 repeated 1,000 times (`--gtest_repeat=1000`) on each copy. The change fails none,
  and none of 300 with 24 busy loops sharing the machine's 12 cores. The issue's sketch fails all
  1,000, with a median of 226 of 5,213 distinct reads off a prefix mean (88 to 294 in a run), and
  299 of the 300 under load. The plain-field getter fails all 1,000 (median 133 of 5,118); a mean
  refreshed on entry fails all 1,000 (median 4,795 of 9,788); v0.2.2 fails all 1,000 (median 4,931 of
  9,934). A mean a cycle late fails them through the last assertion. Each run takes under 2 ms.
- **ThreadSanitizer.** With the change, the 13 `EngineCycleStats` tests, among them the three that
  read the statistics while another thread steps, and the 6 `CycleSeqLock` and 4 `EngineCycleSeqLock`
  tests pass under `-fsanitize=thread` with no report. The plain-field getter draws a data race
  report from test 3. CI does not run ThreadSanitizer.
- **Hot path.** `bench_hot_path --invariants` matches `benchmarks/expected_invariants.txt` for every
  build below. The change drops a locked add from the guard and adds a divide and a store, so the
  cost is timed. A padding function of 0 to 21 nops, in steps of 3, ahead of the benchmark's other
  code moves `Engine::cycle()` through eight alignments. 15 shuffled rounds of every build at every
  alignment, a core spun for 300 ms before each run. Each cell is a build's change against
  `ce5e5e2` at the same alignment: the median over the eight, then the lowest to the highest.

  | build | chain | idle-queues | ingest |
  |---|---|---|---|
  | the change: divide first, store last | +0.21% (-0.53% to +0.57%) | +0.36% (+0.13% to +0.47%) | +0.75% (-2.21% to +1.83%) |
  | the issue's sketch: the total and the count stored apart | +0.23% (-0.08% to +0.76%) | +0.17% (-0.04% to +0.47%) | +0.58% (-1.47% to +0.83%) |
  | a control: the new member and getter, the guard as `ce5e5e2`'s | +0.25% (-0.26% to +0.42%) | -0.06% (-0.30% to +0.26%) | +0.33% (-1.56% to +1.17%) |
  | the mean stored right after the divide | +1.10% (+0.53% to +1.41%) | +1.55% (+1.32% to +1.62%) | +2.80% (+1.52% to +3.91%) |
  | the same, with the total and the count atomic | +1.16% (+0.23% to +1.60%) | +1.47% (+1.23% to +1.66%) | +3.09% (+1.19% to +4.00%) |

  The last two rows come from an earlier sweep at the same eight alignments. The control does
  `ce5e5e2`'s work, and its median reads within 0.4% on every row, so a cost that holds at every
  alignment is real. Storing the mean right after the divide, before
  `rollingCycles_.record(ns)`, costs +1.10%, +1.55% and +2.80% at the median, and is slower at every
  alignment. Moving the store after the call removes the cost, and atomic fields are no cheaper.
  The total and the count kept as doubles, to skip two integer-to-double conversions, read
  +1.50%, +2.53% and +3.61% at four alignments.
  One pair of binaries is not enough: in an earlier 40-round A/B of one binary each, a control
  with `ce5e5e2`'s own hot path (the new member and getter, nothing stored in the guard) read
  +10.6% on idle-queues.

## Steps

Every commit subject is scoped to flywheel-dag#34 in the repo's `type(#N): …` form. The subjects
below omit the scope.

### Step 1 — The tests and the fix

Written: tests 1 to 3. Changed: `dag_engine.hpp` (the members and the comments over them and over the
statistics) and `dag_engine.inl` (`meanCycleUs()`, the guard and the comment over it).

**Done when:**

- the 3 tests fail on `ce5e5e2`'s headers; each mutation in the table above fails the tests it lists
  and no others, on scratch copies of `include/`, since the repo's own tree is never mutated, with a
  control copy that passes them all; and test 3 passes 1,000 repeats on the committed tree;
- the build prints no warnings with `-DFLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON`, and ctest is green
  (36 of 36);
- the `EngineCycleStats`, `CycleSeqLock` and `EngineCycleSeqLock` tests pass under
  `-fsanitize=thread` with no report;
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
- **A consistent set of statistics without `readConsistent()`.** Each statistic is one word, so none
  reads torn, but a reader of two reads them at two moments. `CycleSeqLock` is the contract for a set,
  and this plan leaves it.
- **The pair under a lock or a sequence word,** as `RollingCycleWindow` and `CycleSeqLock` guard
  theirs. It closes the window too, and gives `meanCycleUs()` a lock or a retry loop. The single
  store needs neither.

## Self-review — risks and assumptions

- **The guard's cost.** The change reads +0.21%, +0.36% and +0.75% against `ce5e5e2`
  at the median over eight alignments, and +1.83% at the worst alignment of any row. The bar is 1%
  at the median. Ingest has the shortest cycle, so it shows a cost first, and the control reads
  -1.56% to +1.17% there at single alignments, so only the median means anything. The
  change reads within 0.2% of the issue's sketch on every row. The step 1 check repeats the timing
  on the committed tree.
- **Divide first, store last is a measured effect on one compiler.** It costs nothing here, and the
  other order costs 1% to 3%. The cause is not isolated. Another compiler may schedule the divide
  differently, and CI compares the benchmark's exact columns and never its timings. The code is
  correct in either order. The comment over the guard gives the measurement, so a later edit does
  not tidy the order away.
- **The total and the count are plain fields.** They are safe because one thread runs cycles at a
  time: `run()` is called from exactly one thread, `step()` is not for use while it runs, and
  `CycleSeqLock`'s write side already assumes a single writer. A later change that reads either from
  another thread would race. The comment over the members says so, and the plain-field getter above
  draws a ThreadSanitizer report from test 3. CI does not run ThreadSanitizer.
- **Test 3 can pass a defect.** The scheduler decides where the reads land. On this machine it fails
  the issue's sketch in 1,000 of 1,000 runs, and in 299 of 300 with 24 busy loops sharing 12 cores. A
  single-core runner with a long timeslice may land a read in the window less often. It cannot fail a
  correct engine, so its worst case is a miss and not a flake.
- **A callback now reads a larger mean than before,** by 1/n of the mean in the cycle after n have
  ended. That is the correction, and the release notes say so under **Fixed**. An application that
  logs `meanCycleUs()` from a callback will see its first values move.
- **`timedCycles_` equals `cycles_` whenever no cycle is in flight.** `cycles_` is bumped and the
  guard is built on the next line, and nothing between can throw. A destructor that throws
  terminates, as flywheel-dag#21's plan said, and the recording can throw only from the rolling
  window's mutex. No test reads `timedCycles_`, which is private; the mean pins it.
- **`std::atomic<double>` is as lock-free as the engine's other 8-byte atomics.** On a target where it
  is not, the store takes a lock and stays atomic, only slower.
- **The tests detect the defect only if the first cycle measures above zero.** A new engine's first
  cycle took 0.83 µs in the prototype's run, against a 41 ns tick, and the later cycles of test 1
  took 0.08 to 0.3 µs. A clock too coarse for that would pass tests 1 and 2 under the defect as well
  as under the fix; it could never fail the fix. `EXPECT_NEAR`'s 1e-9 is far above the rounding of an
  integer count of nanoseconds divided by 1,000.
- **Test 2 starts a thread in each of six cycles,** and the callback joins it with `get()` before it
  returns, so no thread outlives its cycle.
- **The timings are one machine and one compiler.** Apple Clang on arm64. On x86-64 the guard drops a
  `lock xadd` and the change should cost less there; that is not measured. CI's GCC leg is the first
  to build the new tests there; neither GCC-only pattern `CLAUDE.md` lists, a reference into a
  temporary or an unbraced `if` around a gtest macro, appears in them.
- **Pushing and releasing are assumed,** as in the earlier plans. If either is to be done by hand,
  step 2 stops before it.
