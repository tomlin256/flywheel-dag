# Refuse a Cycle Begun Inside a Cycle

**Status: Approved (2026-10-11).** `Engine::step()` and `Engine::run()` throw `std::runtime_error`
while a cycle is in flight, from an output callback, a node's `eval()` or a source's `flush()`, and
begin no cycle. Today such a call runs a cycle inside the cycle, which flushes every source a second
time, drops an `AsyncQueue`'s batch and leaves `CycleSeqLock` out of step. The test is the one
[flywheel-dag#24](https://github.com/tomlin256/flywheel-dag/issues/24) added for a registration, the
two counters `Engine` keeps already, and it goes first in `cycle()`, which both calls end in. The check
adds four instructions to `cycle()`, and `bench_hot_path` reads inside the spread of two controls built
from `22aeb99`'s own headers. The plan refuses rather than makes a nested call work, and "The
decision" gives the reasons. The plan was approved as written, with the check at the top of `cycle()`
and the nested call refused rather than made to work by a deferral or a nested cycle.

Closes [flywheel-dag#39](https://github.com/tomlin256/flywheel-dag/issues/39).

## Problem

`Engine::step()` and `Engine::run()` both end in the private `cycle()`, and nothing stops either from
being called while a cycle is in flight: from an output callback, from a node's `eval()` or from a
source's `flush()`. The call begins a cycle inside the one running. Neither call is documented as
allowed there: `step()` is "for deterministic testing only", and `run()` "must be called from exactly
one thread". On `22aeb99`, which has v0.2.4's engine, a nested cycle breaks three things:

1. **It flushes every source a second time in one cycle.** An `AsyncQueue` that the outer cycle has
   already flushed finds nothing staged and rebinds its value to the shared empty batch, so its batch is
   gone before its output is reached. That is the mechanism of
   [flywheel-dag#37](https://github.com/tomlin256/flywheel-dag/issues/37), reached by another route. In
   the issue's program a callback calls `step()` once while two values wait in a queue. Without the
   nested `step()` the queue's output sees a batch of 2. With it, the output finds the shared empty
   batch it saw last time and never fires, and the engine has counted four cycles where two were asked
   for.
2. **It leaves `CycleSeqLock` out of step.** A cycle holds the lock open with a `WriteScope`. Its
   constructor adds one to the sequence word, which is odd while a cycle is open, and records the
   cycle's thread; its destructor adds one again and clears the record. Two scopes do not nest. When
   the inner one ends it clears the record while the outer cycle is still open, so a `readConsistent()`
   from the same callback finds an odd word and no sign that the cycle is its own, and waits for a cycle
   that cannot end until the read returns. Without the nested `step()` the same call returns at once.
   With it, the issue's second program prints that it is reading and never prints the result, and is
   killed after 5 seconds.
3. **It tells a reader on another thread that no cycle is open.** The inner scope's constructor makes
   the sequence word even again. Until the inner cycle ends, a `readConsistent()` on another thread
   sees an even word and accepts a read taken in the middle of the outer cycle, the torn read the lock
   exists to reject. In a program whose outer callback changes two values together, with a reader on
   another thread released from the inner cycle, the read returns `(1, 0)` in one call and is accepted.
   With the nested `step()` refused it returns `(1, 1)`.

`run()` called from a callback of a `step()`-driven engine starts a run loop inside the cycle in the
same way. A caller with no `AsyncQueue` among its sources, and no read of the lock from inside the
cycle, sees none of this: its nested cycle delivers the outputs the outer cycle had not reached yet,
before the rest of the callback that stepped.

## The decision

The issue names one fix, to refuse a nested `step()` or `run()` as the plan for flywheel-dag#24 refuses
a registration, by the same test: a cycle is in flight while `cycles_` differs from `timedCycles_`. What
is left to decide is where the check goes and whether to refuse at all. Each cell is the change against
`22aeb99` in the same run, as the median over 16 alignments of `Engine::cycle()` and then the lowest to
the highest (see "Prototype" for the method). The rows come from one sweep of 20 rounds, with two
controls built from `22aeb99`'s own headers:

| Design | A `step()` or `run()` inside a cycle | What the engine pays | chain | idle-queues | ingest |
|---|---|---|---|---|---|
| Check at the top of `cycle()` (this plan) | throws, begins nothing | two loads, a compare and a branch in every cycle | +0.05% (-0.49% to +19.34%) | +0.13% (-10.03% to +0.78%) | -0.53% (-1.31% to +2.27%) |
| Check in `step()`, and at the entry of `run()` | throws, begins nothing | the same in `step()`; nothing in `cycle()` or in `run()`'s loop | -0.04% (-0.75% to +0.93%) | +0.01% (-9.99% to +0.51%) | +0.20% (-2.00% to +2.52%) |
| Control 1: `22aeb99`'s headers, built again | as today | nothing | +0.06% (-0.41% to +19.29%) | -0.02% (-0.40% to +0.44%) | -0.47% (-2.58% to +2.31%) |
| Control 2: the same | as today | nothing | -0.06% (-0.45% to +0.36%) | +0.02% (-0.42% to +0.45%) | -0.87% (-2.53% to +2.56%) |

The base times, in ns per cycle, are 265 (chain), 236 (idle-queues) and 122 (ingest). The controls are
the same code as the base, so their rows are noise: they read from -0.87% to -0.47% on `ingest`, and
one alignment of `chain` read +19% for a control and for the check at the top of `cycle()`, a layout
effect that only the median over alignments survives. An earlier sweep of 15 rounds with one control
read the control at +0.42% on `ingest`, the check in `step()` at +1.09% and the check at the top of
`cycle()` at +0.42%. So `ingest` moves by about 1% between builds of the same code, and a cost inside
that is unmeasured, not zero.

**The plan refuses, at the top of `cycle()`,** for these reasons:

- **It enforces what is written.** Neither call is allowed inside a cycle, and nothing enforces it. A
  caller inside the rule is not affected. One outside it gets an exception that names the two calls,
  where today it gets a dropped batch, a hang or a torn read, depending on what else the engine holds.
- **One check closes every way in.** `cycle()` is private and only `step()` and `run()` call it, so a
  check at its top covers both, and any caller added later. It comes before the scope on `CycleSeqLock`,
  the `cycles_` bump and the guard, so a refused call has begun nothing.
- **Where it goes is measured, and the measurement does not separate the two places.** Both read inside
  the spread of the controls on all three rows, in both sweeps. The check at the top of `cycle()` read
  lower on `ingest` than the check in `step()` in both, by about 0.6 ns a cycle (-0.53% against +0.20%
  here, +0.42% against +1.09% in the earlier sweep), which the controls' own spread of a nanosecond
  cannot confirm. The check in `step()` has one advantage that is not in doubt: `cycle()` and `run()`'s
  loop stay as they are, and only `step()`, which is for testing, pays. It has two drawbacks that are not
  in doubt either. It is two places, and `run()`'s has to sit between its two guards: after the
  "already running" check, whose message the suite pins, and after the `ClearOnExit`, which test 4 pins.
  And it is a place that a third caller of `cycle()` can forget. The plan takes the single check,
  because no cost separates them.
- **Making a nested call work is a design, and nothing asks for it.** A deferral records the request
  in `step()` and returns, and the cycle runs once more when the one in flight ends. A real nested cycle
  runs inside. The deferral turns a call whose doc says "execute one cycle synchronously" into one that
  returns before its cycle has run, and nothing tells the caller. A real nested cycle has to say what it
  flushes, what it delivers and in what order, and what `CycleSeqLock` shows, and flywheel-dag#37's rule
  that a source is flushed once per cycle has no answer for it. A running engine already has a way to
  ask for another cycle, `poke()`. Neither is built here. A deferral needs the same test, to know that
  it should defer, and a look for a request after every cycle, so it cannot cost less than the
  refusal. A real nested cycle needs a depth in `WriteScope` and a flush that knows it is nested, which
  is a design of its own.

To get another cycle after this one, a caller calls `step()` once the cycle has returned, or `poke()`s
an engine that `run()` drives.

## The fix

```cpp
// Engine, private.
bool cycleInFlight() const { return cycles_.load(std::memory_order_relaxed) != timedCycles_; }
[[noreturn]] static void throwNestedCycle();

inline void Engine::cycle() {
    if (cycleInFlight()) throwNestedCycle();   // first: a refused call begins nothing
    const CycleSeqLock::WriteScope writeScope(*cycleSeqLock_);
    cycles_.fetch_add(1, std::memory_order_relaxed);
    /* ... as today ... */
}

[[noreturn]] inline void Engine::throwNestedCycle() {
    throw std::runtime_error("Engine::step() or Engine::run() called during a cycle");
}

// refuseInCycle() asks the same question:
inline void Engine::refuseInCycle(const std::string_view call) const {
    if (cycleInFlight())
        throw std::runtime_error(
            std::string("Engine::").append(call).append("() called during a cycle"));
}
```

- **The counters are the test, as for a registration.** `cycles_` counts a cycle as it begins and
  `timedCycles_` as its guard ends it
  ([flywheel-dag#34](https://github.com/tomlin256/flywheel-dag/issues/34)), so they differ from the
  `cycles_.fetch_add()` at the top of `cycle()` to the count in the guard's destructor, whether the
  cycle returns or throws. A callback, a node's `eval()` and a source's `flush()` all run in that span.
  `cycleInFlight()` is that one question, asked by `cycle()` and by `refuseInCycle()`, so the two
  cannot come to disagree.
- **A refused call changes nothing.** The check comes before the scope on the lock, the count and the
  guard, so the refusal has taken no scope, bumped no count and flushed no source. A registration after
  a refused `step()` is refused still, and the existing
  `EngineRegistrationInCycle.ARegistrationAfterANestedStepIsStillRefused` pins it. It passes as it
  stands: its nested `step()` throws now where it used to run. Its comment says that a cycle opened
  inside a cycle leaves the counters apart when it ends. That cannot happen any more, so the comment
  changes and no assertion does.
- **`step()` and `run()` need no change.** `run()` called from a callback of a `step()`-driven engine
  sets its flag, reaches `cycle()`, and the refusal leaves through the `ClearOnExit` that exists
  already, which clears the flag again (test 4). Called from a callback of a running engine it stops at
  "already running" before it gets that far, as it does today
  (`EngineRun.ARunCalledWhileRunningThrowsAndLeavesTheRunGoing` pins it).
- **One message for both calls:** `"Engine::step() or Engine::run() called during a cycle"`. `cycle()`
  does not know which of the two called it, and passing the name would cost an argument on every cycle
  to say what the message says already, since these are the only two ways in. The exception type is
  `std::runtime_error`, as for a registration.
- **The throw is out of line.** `throwNestedCycle()` is `[[noreturn]]`, so the check that `cycle()`
  carries is two loads, a compare and a branch around a call that the compilers place away from the
  path.
- **A refusal is a throw out of a callback like any other.** The cycle ends, and the outputs it did
  not reach stay due and get their value next cycle (test 6). Under `run()` the exception leaves through
  `run()`, which can be called again, as after any throw out of a callback
  (`EngineRun.RunsAgainAfterACallbackThrows`). A callback that wants to carry on catches it (tests 1, 5
  and 7).
- **Per engine.** The counters belong to the engine, so a callback of one engine may step another that
  has no cycle in flight (test 8).
- **Another thread is not covered,** and nothing changes for it: `step()` while `run()` is going on
  another thread is documented as unsupported, and it races the cycle. It reads the counters like any
  other call, so it may be refused when it lands inside a cycle, and nothing promises that.
- **Comments carry the contract.** `step()`'s and `run()`'s docs say that inside a cycle they throw and
  begin nothing, and what to do instead; the overview's cycle paragraph names them beside
  `addOutput()`; the comment over `refuseInCycle()` is rewritten around `cycleInFlight()`, without the
  sentence about a cycle opened inside a cycle and without the claim that the counters add nothing to
  `cycle()`; `WriteScope`'s comment says that scopes on one lock must not nest, since the engine no
  longer nests them; the coverage list at the head of `test_dag_async.cpp` gains the new section.
  `CLAUDE.md`'s rule says that `step()` and `run()` throw inside a cycle with the three registration
  calls.
- **Source-compatible.** Two private member functions and no signature changes, so this ships as
  v0.2.5, as v0.2.4 did. A caller that stepped inside a cycle and got away with it now gets an
  exception, which the release notes say under **Changed**.

## Tests

In `test_dag_async.cpp`, in a new section after the `EngineRegistrationInCycle` tests, with one helper:
`SteppingSource`, a source whose `flush()` steps the engine once. `stepAndCatch()` and `runAndCatch()`
exist already. Every test is deterministic: `Engine::step()` drives the cycle, and nothing sleeps. Test 1
is the issue's program. Eight tests, in the suite `EngineNestedCycle`. "Fails under" names the changes
of "Prototype" (the mutations) that fail it:

| # | Test | Asserts | Fails under |
|---|---|---|---|
| 1 | `ACallbackThatStepsTheEngineIsRefused` | the issue's program: the callback's `step()` throws `Engine::step() or Engine::run() called during a cycle`, begins no cycle (`cycleCount()` is 2 after two `step()`s), and the queue's output gets its batch of 2 | a, e, f, k, m, n |
| 2 | `ANodeThatStepsTheEngineInEvalIsRefused` | a `step()` from a node's `eval()` throws; the node stays dirty, and the next cycle delivers its value | a, e, f, g, k, m, n |
| 3 | `ASourceThatStepsTheEngineInFlushIsRefused` | a `step()` from a source's `flush()`, the first phase of the cycle, is refused too, and the cycle delivers | a, e, m, n |
| 4 | `ACallbackThatRunsTheEngineIsRefusedAndRunStaysUsable` | `run()` from a callback of a stepping engine throws and starts no loop, and a `run()` after the cycle starts, where a flag left set would say "already running" | a, e, f, k, m, n, p |
| 5 | `ACallbackOfARunningEngineThatStepsIsRefused` | the same for an engine that `run()` drives: the callback sees the refusal, stops the run, and `run()` returns normally | a, e, m, n |
| 6 | `ARefusedStepLeavesTheEngineUsable` | uncaught, the refusal ends the cycle at that callback, and the output it did not reach gets its value next cycle | a, e, f, g, k, m, n |
| 7 | `AReadAfterARefusedStepInTheCallbackReturnsAtOnce` | `readConsistent()` after the refusal runs its function once and returns | a, e, i, m (these wait for ever), n |
| 8 | `AnotherEnginesStepMayBeCalledFromACallback` | a callback of one engine steps another, which delivers | h |

Test 8 passes on `22aeb99`'s headers. It pins that the refusal is per engine, and fails only under a
refusal that is not. Two existing tests are pinned by the change and need none of their assertions
touched: `EngineRun.ARunCalledWhileRunningThrowsAndLeavesTheRunGoing`, which says that "already
running" still comes first, and `EngineRegistrationInCycle.ARegistrationAfterANestedStepIsStillRefused`,
which says that a refused `step()` changes nothing. The second gets a new comment, as "The fix" says.

## Prototype

Built on scratch copies of the repository (`git archive` of `22aeb99`, and the same with each change),
with the flags of `build/compile_commands.json` and `-Werror`, under Apple Clang 21 on arm64. The
repository tree is untouched. The code is the sketch above, and the tests are the table.

- **Tests.** A full build of the change with `-Werror=dev -Werror=deprecated
  -DFLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON` prints no warnings, and ctest passes 36 of 36. `test_dag_async`
  passes 139 of 139: the 131 it has and the 8 new. Against `22aeb99`'s headers the 131 pass and, run one
  at a time, tests 1 to 6 fail, test 7 waits for ever and test 8 passes.
- **Mutations.** One change per scratch copy of `include/`, against the new test file, with a control
  copy that passes all 139. Every test ran on its own under a watchdog, so a crash or a hang loses only
  that test:

  | | Change to the fix | Fails |
  |---|---|---|
  | a | `cycle()` does not check (the defect) | 1 to 6; 7 waits for ever |
  | e | the check never fires | 1 to 6; 7 waits for ever; eight tests of flywheel-dag#24, of which six fail and two crash |
  | f | the check fires once any cycle has begun | 1, 2, 4, 6; 38 existing tests; 4 existing tests abort |
  | g | a throw skips the count: it is bumped at the end of `cycle()`, outside the guard | 2, 6; 13 existing tests |
  | h | the check reads a depth that every engine on the thread shares | 8 |
  | i | `cycle()` checks after it has taken its scope on the lock | 7 waits for ever |
  | k | a refusal closes the cycle: it bumps `timedCycles_` | 1, 2, 4, 6; `ARegistrationAfterANestedStepIsStillRefused` crashes |
  | m | one level of nesting is allowed | 1 to 6; 7 waits for ever |
  | n | the message names only `step()` | 1 to 7 |
  | p | `ClearOnExit` does not clear the flag | 4; `EngineRun.RunsAgainAfterANodeThrows`, `EngineRun.RunsAgainAfterACallbackThrows` and `EngineAbortedCycle.ARestartedRunDeliversWhatTheAbortedCycleDidNot` |

  The existing tests that f and g fail are the ones that step twice or that throw out of a cycle, among
  them the four of [flywheel-dag#21](https://github.com/tomlin256/flywheel-dag/issues/21). The first run
  of this table showed that test 4 did not see p: the output that stops a run which was not refused
  stopped the refused one too, which cleared the flag the refusal had left set. It acts only inside a run
  that was not refused now.
- **The programs.** The two programs of the issue, and a third that shows the effect the issue does not
  name, built against `22aeb99`'s headers and against the change:

  | Program | On `22aeb99` | With the change |
  |---|---|---|
  | A callback steps while two values wait in a queue | the queue's output sees no batch; 4 cycles counted | a batch of 2; 2 cycles |
  | A read from the callback after the nested `step()` | prints that it is reading, never the result; killed after 5 s | reads 1 |
  | A reader on another thread released from the inner cycle | `(1, 0)` in one call, accepted | `(1, 1)` |

- **Sanitizers.** `test_dag_async` passes under `-fsanitize=address,undefined` with no report (139 of
  139). The issue's first program, built with the change, ends in the refusal at `-O0` and at `-O2` with
  no report. The 55 tests of `CycleSeqLock`, `EngineCycleSeqLock`, `EngineRegistrationInCycle`,
  `EngineNestedCycle`, `EngineCycleStats`, `EngineRun`, `EngineNewOutput` and `EngineAbortedCycle` pass
  under `-fsanitize=thread` with no report. CI runs neither sanitizer.
- **Hot path.** `bench_hot_path --invariants` matches `benchmarks/expected_invariants.txt`.
  `llvm-objdump` of `Engine::cycle()` in the benchmark, built from `22aeb99`'s headers and from the
  change, shows 199 and 205 instructions: the check's `ldr`, `ldr`, `cmp` and `b.ne` at the top, a call
  to `throwNestedCycle()` among the function's other cold code, and the landing pads moved around it.
  `TimeOnExit::~TimeOnExit` is the same 80 instructions in both, and differs in the address of one
  constant. A padding function of 0 to 15 nops ahead of the benchmark's other code moves
  `Engine::cycle()` through sixteen alignments mod 64, 4 bytes apart. A build's binaries are paired with
  the other builds' by where `cycle()` lands (read with `nm`), not by the padding, since the builds differ
  in size. 15 or 20 shuffled rounds, a core spun for 300 ms before each run, the median per binary, then
  the change per alignment against the base, then the median and range over the alignments. "The
  decision" has the numbers.

## Steps

Every commit subject is scoped to flywheel-dag#39 in the repository's `type(#N): …` form. The subjects
below omit the scope.

### Step 1 — The tests and the fix

Written: tests 1 to 8 and their helper, `SteppingSource`. Changed: `dag_engine.hpp` (the docs, the
overview, `cycleInFlight()`, `throwNestedCycle()`, and `WriteScope`'s comment), `dag_engine.inl`
(`cycle()`, `throwNestedCycle()` and `refuseInCycle()` with its comment), the comment over
`EngineRegistrationInCycle.ARegistrationAfterANestedStepIsStillRefused`, the coverage list in
`test_dag_async.cpp`, and `CLAUDE.md` (the rule).

**Done when:**

- tests 1 to 7 fail on `22aeb99`'s headers and test 8 passes, as the table says; each mutation above
  fails the tests it lists and no others, on scratch copies of `include/`, since the repository's own
  tree is never mutated, with a control copy that passes them all;
- the build prints no warnings with `-DFLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON`, and ctest is green
  (36 of 36);
- `test_dag_async` passes under `-fsanitize=address,undefined`, the issue's program ends in the refusal
  with no report, and the tests of the suites named above pass under `-fsanitize=thread`;
- `bench_hot_path --invariants` matches `benchmarks/expected_invariants.txt`, and no row is more than
  1% slower than `22aeb99` at the median over 16 alignments, with a control built from `22aeb99`'s
  headers in the same rounds;

Commit: `fix: refuse a cycle begun inside a cycle`.

### Step 2 — Release and close out

- Release v0.2.5 as v0.2.4 was released: the project version and the FetchContent tag in
  `CMakeLists.txt` and the README move to 0.2.5 and v0.2.5. Then the tag, and a GitHub release with
  notes. Under **Changed** they say that a `step()` or `run()` made inside a cycle, which ran a cycle
  inside the cycle, now throws `std::runtime_error`. There is no **Breaking** section: no signature
  changes.
- Mark this plan done, with the mutations and the timings rerun on the committed tree.
- Push `main`, watch CI on both legs, and close flywheel-dag#39 with a summary comment.
- Stop before the push, the tag and the release for a go-ahead.

**Done when:** the build prints no warnings with `-DFLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON` and ctest is
green (36 of 36), CI is green on the release commit on both legs, the installed version file reports
0.2.5, the release is published, and flywheel-dag#39 is closed.

Commits: `build: release v0.2.5` and `docs: mark the plan done`.

## Not in this plan

- **Making a nested `step()` work.** "The decision" has the two designs and why neither is built. Either
  is a plan of its own, and it replaces this check.
- **A `WriteScope` that nests.** A depth count would keep the sequence word odd, and the record of the
  cycle's thread set, through an inner scope. It costs `cycle()` a load and a branch on every cycle, and
  nothing reaches it once the engine refuses to nest. `WriteScope`'s comment says that scopes on one
  lock must not nest.
- **A `step()` on another thread while `run()` is going.** Unchanged, and documented as unsupported.
  Refusing it reliably needs the sequence word to be taken with a compare-and-swap in `cycle()`, which
  is more than the check here costs.

## Self-review — risks and assumptions

- **A caller that stepped inside a cycle and got away with it now gets an exception.** The exception
  names both calls. Under `run()` it ends the run like any throw out of a callback, and `run()` can be
  called again. The release notes say so under **Changed**. This repository's suite and examples never
  step or run inside a cycle (ctest passes with the refusal, 36 of 36), and neither does the benchmark
  (its invariants match).
- **Every cycle pays for the check,** a load pair, a compare and a branch at the top of `cycle()`,
  `run()`'s cycles included. The benchmark reads it inside the spread of the controls, in two sweeps
  ("The decision"), and a cycle under `run()` costs more than the benchmark's, which strips out
  everything but the cycle, so the check is a smaller share there. The same check in `step()` and at the
  entry of `run()` would leave `cycle()` as it is; "The decision" has what it read and why the plan does
  not take it.
- **The counters are the check, so what they mean is a contract.** `timedCycles_` equals `cycles_`
  whenever no cycle is in flight, because `cycles_` is bumped and the guard is built on the next line,
  and nothing between can throw (flywheel-dag#34's plan). A later change that broke that would make
  `cycle()` refuse for ever or never, and mutations e and f show that the tests catch both.
- **A plain field is read.** The thread that runs cycles writes `timedCycles_`, and `cycle()` reads it
  on that thread. A call from another thread while a cycle runs reads it with no ordering, on top of
  racing the cycle. That call is unsupported already, and nothing promises that it is refused.
- **A callback that swallows exceptions swallows this one too.** A callback with a blanket
  `catch (...)` around a `step()` carries on without the cycle, silently, as it would for any exception.
  Tests 1, 5 and 7 show the refusal can be caught, and that is the price of an exception.
- **Test 7 waits for ever when the fix is absent,** because the read it makes is the one that hangs.
  `ReadInsideItsOwnOpenCycleReturnsAtOnce` has the same shape. A regression hangs `test_dag_async` where
  it should fail one test, and ctest's default timeout of 120 seconds ends it. The mutation run puts a
  watchdog on every binary.
- **GCC is unverified locally.** The prototype was built with Apple Clang alone. The change is a
  comparison and a call to a `[[noreturn]]` function, with nothing compiler-specific, and the new
  tests contain neither GCC-only pattern `CLAUDE.md` lists, a reference into a temporary or an unbraced
  `if` around a gtest macro. CI's GCC leg builds them.
- **The timings are one machine and one compiler,** Apple Clang 21 on arm64. `ingest` moves by about
  0.5% between runs of the same code, so only the median means anything.
- **Assumptions:** the engine stays header-only C++17. One thread runs cycles. `step()` and `run()` are
  called between cycles. Pushing and releasing are assumed, as in the earlier plans; if either is to be
  done by hand, step 2 stops before it.
