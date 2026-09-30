# `Engine::run()` Can Start Again After a Cycle Throws

**Status: Approved (2026-09-30).**

Closes [flywheel-dag#16](https://github.com/tomlin256/flywheel-dag/issues/16).

## Problem

`Engine::run()` sets `running_` on entry, and only `stop()` clears it. When a node's `eval()` or an
output callback throws inside `cycle()`, the exception leaves `run()` with `running_` still set.
The next `run()` then throws `Engine::run() called while already running`, although nothing is
running. A caller can work around it by calling `stop()` first.

`aad::GradientNode` and `aad::TangentNode` throw from `eval()` by contract, so a throw out of a
cycle is an outcome an application can expect, not only a programming error.

## The fix

Clear `running_` however `run()` exits, with a scope guard built right after the already-running
check:

```cpp
inline void Engine::run() {
    if (running_.exchange(true))
        throw std::runtime_error("Engine::run() called while already running");

    struct ClearOnExit {
        std::atomic<bool>& flag;
        ~ClearOnExit() { flag = false; }
    };
    const ClearOnExit clearRunning{running_};

    // ... the initial cycle and the wait loop, unchanged
}
```

- **A normal return is unchanged.** `run()` returns only once `running_` is false, so there the
  guard stores the value the flag already holds.
- **The guard is built after the check.** A `run()` refused there, such as one an output callback
  calls, then leaves the running one alone. Built before the check, the refused call would clear
  the flag on its way out, and the outer run would end after its current cycle.
- **A scope guard, not `catch (...)` and a rethrow,** as `CycleSeqLock::WriteScope` closes a cycle
  that throws. It covers every exit, including one a later edit adds.

The next `run()` starts as any run does. Its initial cycle flushes the sources and evaluates every
output that is dirty. A `ComputeNode` whose functor threw is still dirty, because it marks itself
clean only when it publishes, so that cycle retries it.

A probe on a scratch copy of the headers drove four graphs through v0.1.9 and the fix:

| Scenario | v0.1.9 | fixed |
|---|---|---|
| A node throws on every eval. A second `run()` | throws "already running" | throws the node's error again |
| A node throws once, and its callback stops the engine. A second `run()` | throws "already running" | returns, and the value is delivered |
| A callback throws once. After `x` is set, and with the callback stopping the engine, a second `run()` | throws "already running" | returns, and the new value is delivered |
| A callback calls `run()` inside the run, then sets `x`. The next value's callback stops the engine | the nested call throws "already running", and 2 values are delivered | the same |

With the guard built before the check, the last row delivers 1 value, not 2: the refused nested
call cleared the flag and ended the outer run. The fix compiled with no warnings under Apple Clang
at `-Wall -Wextra -Wpedantic -Werror`.

## Steps

Every commit subject is scoped to flywheel-dag#16 in the repo's `type(#N): …` form. The subjects
below omit the scope.

### Step 1 — Regression tests, then the fix

The tests go in `test_dag_async.cpp`, in a new `Engine::run()` section after the `CycleSeqLock`
tests, which hold `EngineCycleSeqLock.ThrowingCallbackLeavesNoCycleOpen`. The file's coverage
comment gains the section. A helper, `runAndCatch(engine)`, returns what `run()` threw, or `""`
when it returned, so a failure prints the message.

| Test | Asserts | v0.1.9 |
|---|---|---|
| `EngineRun.RunsAgainAfterANodeThrows` | A node throws while a flag is set. The first `run()` throws the node's error, and so does the second, which retries the node. With the flag cleared, a third `run()` returns once the node's value reaches its callback, which stops the engine | the second `run()` throws "already running" |
| `EngineRun.RunsAgainAfterACallbackThrows` | An output callback throws on its first call. After `x->set(2.0)`, the second `run()` returns once the callback gets `2.0` and stops the engine | the second `run()` throws "already running" |
| `EngineRun.ARunCalledWhileRunningThrowsAndLeavesTheRunGoing` | A callback's nested `run()` throws "already running". The outer run goes on to a second cycle, delivers `2.0` and returns when the callback stops it | passes |

Every `run()` in these tests ends inside its own cycles, by a throw or by a callback calling
`stop()`, so none of them waits on a feed.

Then the fix, with a comment saying why the guard exists and why it follows the check.

Each hand-made change below must fail exactly the tests stated:

| Change | Fails |
|---|---|
| No guard, as in v0.1.9 | `RunsAgainAfterANodeThrows`, `RunsAgainAfterACallbackThrows` |
| The guard built before the already-running check | `ARunCalledWhileRunningThrowsAndLeavesTheRunGoing` |

**Done when:**

- the first two tests fail on v0.1.9 for the reason given, and all three pass after the fix;
- each hand-made change fails as stated;
- the build prints no warnings with `FLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON`, and ctest is green
  (35 of 35);
- `bench_hot_path --invariants` matches `benchmarks/expected_invariants.txt`;
- CI is green on both legs.

Commit: `fix: run() can start again after a cycle throws`.

### Step 2 — Docs and release

- `dag_engine.hpp`: `run()`'s doc says that an exception from a node or an output callback ends the
  run and leaves through `run()`, which can then be called again.
- `CLAUDE.md`: the sensitivity-node bullet no longer says a throw leaves the engine running. It
  says the throw ends the run, which can be started again, and that the aborted cycle can leave
  another output's callback stale (flywheel-dag#20).
- Release v0.1.10 as v0.1.9 was released: the project version and the FetchContent snippets in
  `CMakeLists.txt` and the README, the tag, and a GitHub release with notes.
- Mark this plan done, and close flywheel-dag#16 with a summary comment.

**Done when:** ctest is green (35 of 35), CI is green on the release commit on both legs, the
installed version file reports 0.1.10, the release is published, and flywheel-dag#16 is closed.

Commits: `docs: say what run() does after a cycle throws`, `build: release v0.1.10` and
`docs: mark the plan done`.

## Not in this plan

Planning found two more defects in what an aborted cycle leaves behind. Each is its own issue:

- **A later output's callback can go stale** —
  [flywheel-dag#20](https://github.com/tomlin256/flywheel-dag/issues/20). `cycle()`'s dirty
  snapshot dies with the cycle that throws. An output that an earlier one had already pulled clean
  is clean at the next snapshot, so its callback misses the value it holds until that value moves
  again. `Engine::step()` reaches it today, and this fix lets a restarted `run()` reach it too. It
  needs a design of its own, including what the callback that threw is owed: `cycle()` sets
  `lastSeen` before it calls the callback, so that value counts as delivered.
- **`meanCycleUs()` counts a cycle that throws at zero duration** —
  [flywheel-dag#21](https://github.com/tomlin256/flywheel-dag/issues/21).

## Self-review — risks and assumptions

- **Two threads calling `run()`.** The guard stores `false` on every exit. If a second thread's
  `run()` started between the first's loop exit and that store, the store would clear the second
  run's flag. That breaks the documented contract that one thread calls `run()`, and the
  already-running check does not catch every such race today either.
- **Other throws.** A `std::system_error` from the mutex or the condition variable now also ends
  the run cleanly. Before, it left the engine marked running as well.
- **A fault that persists throws again.** The throwing node stays dirty, so each later `run()`
  retries it in its initial cycle and throws again. That is the right outcome: the application
  sees every failure, rather than an engine that cannot start. The first test asserts it.
- **Outputs the aborted cycle did not reach.** They are still dirty, so the next run's initial
  cycle delivers them. The exception is flywheel-dag#20.
- **`hasWork_`.** A wake that arrives during the aborted cycle leaves `hasWork_` set, so the next
  run wakes once more after its initial cycle. That cycle finds nothing new and fires nothing. The
  same happens today when data arrives before the first `run()`.
- **A regression could hang a test rather than fail it.** If a later change stopped the
  retried node or the new value from reaching its callback, that `run()` would wait for work and
  ctest's 120 s timeout would end it, as for `ReplayCoordinator.EngineRunCompletesUnattended`.
- **GCC is unverified.** The probe was built with Apple Clang alone. The guard is a local
  aggregate with a reference member, which is plain C++17. CI's GCC 13 leg sees it first at Step
  1. flywheel-dag#7 moves CI to Ubuntu 26 on 2026-10-19, which may bring a newer GCC.
- **Assumptions:** the engine stays header-only C++17. `run()` is called from one thread. CI stays
  on `ubuntu-latest` and `macos-latest`.

## Progress

| Step | Status | Notes |
|---|---|---|
| 1 — Tests, then the fix | Not started | |
| 2 — Docs and release | Not started | |
