# `Engine::stop()` Must Not Lose Its Wake-Up

**Status: Proposed (2026-11-02), awaiting approval.** Nothing but this file has changed.

Closes [flywheel-dag#43](https://github.com/tomlin256/flywheel-dag/issues/43).

## Problem

`run()` waits for work or a stop under the wake state's mutex. `stop()` changes the flag that wait tests
without taking it:

```cpp
// Engine::run()
while (running_) {
    std::unique_lock lk(wake_->mu);
    wake_->cv.wait(lk, [this] { return wake_->hasWork || !running_; });
    if (!running_) break;
    ...

// Engine::stop()
inline void Engine::stop() {
    running_ = false;
    wake_->cv.notify_all();
}
```

A condition variable drops a notification that finds no waiter, so the state a waiter's predicate reads has to
change under the mutex the waiter holds from its test to its block. `stop()` is the one writer that does not:
`poke()` and the sources' wake hook both take the mutex around their store. The interleaving:

1. The run thread takes the mutex, evaluates the predicate (`hasWork` false, `running_` true), and is about to
   block.
2. Another thread calls `stop()` and stores `running_ = false`.
3. It calls `notify_all()`. Nothing is blocked yet, so the notification does nothing.
4. The run thread blocks, on a predicate that is now true, with nobody left to say so.

`running_` being atomic does not help; that is a statement about the store, not about the order of the store
and the wait.

**Effect.** `run()` does not return until a `poke()` or a post reaches it: the next message on a busy feed,
possibly never on an idle one. `stop()` was the thing meant to end the run, and the caller is another thread (one
that waits for a signal, or a watchdog). A `stop()` from an output callback is on the run thread, which is not
waiting, and every `stop()` in the existing tests is one of those, so none of them can reach the window.

### What I measured

Three scratch programs, none in the repo. The first two ran on Apple Clang 21 with libc++ on arm64
(`-O2 -g -DNDEBUG`), against the headers at v0.3.0.

1. **Aimed at the real window.** A thread calls `run()` once per round on an engine with one source whose
   `flush()` marks the round's first cycle. The main thread waits for the mark, spins a delay, calls `stop()`
   once, and gives `run()` 20 ms to return. Delays of 0 to 5 µs in 10 ns steps, 2,000 rounds per step:
   **0 lost wake-ups in 1,002,000 rounds.** The window is a few instructions wide, and no schedule I could aim
   reached it.
2. **The window widened.** The same program against a scratch copy whose `run()` spins about 2 µs between the
   predicate and `cv.wait()`. With `stop()` as it is: **lost in 518 of 600 rounds** (86%). With the two lines the
   issue proposes: **0 of 20,500.**
3. **The test below, prototyped** (Step 3), on Apple Clang 21 with libc++ and, in the Linux container, on GCC
   13.3 with libstdc++, with the same results on both: with the fix, six tests pass 3,000 of 3,000
   repetitions; with `stop()` storing outside the mutex, the window test fails 3,000 of 3,000, by assertion,
   without hanging. Five hand-made changes to the class are told apart (the table in Step 3).

So the hazard is real once the window exists, the fix closes it, and **a stress test of the real engine cannot go
red**: it would be green on v0.3.0 and guard nothing. That is why the issue says "not reproduced". The rest of
this plan is how to test the fix anyway.

## The fix

### The two lines

`stop()` stores under the mutex, then notifies, as `poke()` does:

```cpp
inline void Engine::stop() {
    { std::lock_guard lk(wake_->mu); running_ = false; }
    wake_->cv.notify_all();
}
```

Only `stop()` needs it. `run()`'s `exchange` and its scope guard run on the run thread, and a stale read of
`running_` there is harmless.

### The seam that lets a test hold the window

A test of those two lines has to stop the run thread between its predicate and its block, and then decide when
to let it go. It cannot let it go "after `stop()` has finished": with the fix, `stop()` cannot finish while the run
thread holds the mutex, so that wait would never end. It has to let it go once `stop()` has **either finished or
been seen blocked on the mutex**, and the standard mutex cannot say the second. So the wake state becomes a
class template over its mutex and condition variable, and a test gives it a mutex that reports contention:

```cpp
// dag_engine.hpp. dag::async::detail, because it is engine internals and not API.
template <class Mutex, class CondVar>
class BasicWakeState {
public:
    bool beginRun();        // false if a run already holds the flag (run()'s exchange)
    void endRun();          // run()'s exit; stores without the lock, so it cannot throw in a destructor
    bool running() const;

    void signalWork();      // a source's wake hook, poke(): store under mu_, then notify_one
    void requestStop();     // stop():                       store under mu_, then notify_all
    bool waitForWork();     // run(): true = work, flag consumed; false = stopped

protected:                  // a test's subclass reaches mu_ and cv_ to arm its doubles
    Mutex             mu_;
    CondVar           cv_;
    bool              hasWork_ = false;
    std::atomic<bool> running_{false};
};
```

`Engine` holds a `shared_ptr<WakeState>` as now, with `using WakeState = detail::BasicWakeState<std::mutex,
std::condition_variable>`, and loses its own `running_`. `run()` becomes:

```cpp
inline void Engine::run() {
    if (!wake_->beginRun())
        throw std::runtime_error("Engine::run() called while already running");
    struct ClearOnExit { WakeState& wake; ~ClearOnExit() { wake.endRun(); } };
    const ClearOnExit clearRunning{*wake_};

    cycle();                                              // initial cycle, as now
    while (wake_->running() && wake_->waitForWork()) cycle();
}
```

`stop()`, `poke()` and the wake hook each become one call. `Engine` cannot reach `hasWork_` or `running_`, so the
only code that can change what the predicate reads is the class's own, which takes `mu_`, and a test's subclass;
the next edit cannot reintroduce this bug by writing `running_` from `Engine`.

The doubles, in `tests/test_wake_state.cpp`:

- `ContendedMutex`: `lock()` calls `try_lock()` first, and if that fails it counts the contention, calls a
  callback the test set, and then blocks. A thread blocked on it is an event, not an absence.
- `GatedCondVar`: `wait(lk, pred)` is `while (!pred()) { hold(); inner.wait(lk); }` over
  `std::condition_variable_any`. `hold()` runs with the mutex locked and the predicate false: the window.
- The window test: the waiter calls `waitForWork()` and the gate holds it. The test starts the writer
  (`requestStop()` or `signalWork()`) on another thread. The gate lets the waiter go on once the writer has
  returned or has been seen waiting for the mutex, both events the test receives. With the fix the writer waits,
  the waiter blocks and releases the mutex, the writer stores and notifies, and the waiter returns: one
  contention. With the store outside the lock the writer returns without touching the mutex, the waiter blocks on
  a true predicate: no contention, which the test reports, after freeing the waiter so that it reports and does
  not hang.

No sleep, no timeout, no polling, and no dependence on scheduling: the waiter holds the mutex until the gate
opens, so the writer's `try_lock` fails every time.

**Why templates and not an interface.** The wake hook is the feed thread's path on every post, and a virtual call
there is a cost for a test's benefit. And `std::condition_variable` can wait only on a `std::unique_lock` of a
`std::mutex`, so a run-time substitute could not be handed to the real one. A template compiles to what the
engine compiles to today.

### What it does not change, or make safe

- `stop()` takes a mutex now, so it is a call for a thread and not for a signal handler. It never was safe in a
  handler (`notify_all()` is not async-signal-safe), and `poke()` and every wake hook take the same mutex. The
  header's thread-safety paragraph says so after this change (D5).
- A `stop()` that comes before `run()` has started is ignored, and that `run()` waits for ever. That is a
  different race, at the start of a run and not inside the wait, and a different question:
  [flywheel-dag#50](https://github.com/tomlin256/flywheel-dag/issues/50).
- Destroying an `Engine` while another thread is inside `stop()` on it is a misuse, as for any member function.

## Decisions to confirm

My defaults, each one a place to edit before approving.

- **D1. A wake-state class with doubles, not the two lines alone.** Three alternatives:
  - *The two lines and no test of the race.* Leaves a fix nothing guards, against "every change is unit-tested".
  - *The two lines and a stress test.* Measured above: green on v0.3.0.
  - *Function templates over today's struct* (`wakeStop(Wake&, atomic<bool>&)` and two more): about 25 lines
    instead of a class, and the same test. But `running_` stays a member any later edit can store without the
    lock, which is how this bug got in. I recommend the class.
- **D2. `running_` moves into the class.** It is what makes "every write the predicate can see is under `mu_`" a
  property of one class and not a convention. `run()`'s guard and loop change shape; the semantics do not.
- **D3. No red commit.** Every checkpoint is green. The tests need the class from Step 2, so they cannot compile
  against v0.3.0; the red run is on Step 2's faithful extraction (its `requestStop()` stores outside the lock,
  exactly as v0.3.0 does), shown in Step 3 and recorded in the Outcome. flywheel-dag#42 committed its red tests on
  their own; if you want that here, Step 3 splits into tests and fix.
- **D4. A patch release, v0.3.1.** Nothing public changes shape: the new class is in `detail`, `Engine`'s layout is
  private, the library is header-only, and consumers rebuild.
- **D5. The header says `stop()` is not for a signal handler.** One sentence in the thread-safety paragraph and on
  `stop()`.

## Steps

Every commit subject is scoped to flywheel-dag#43 in the repo's `type(#N): …` form. The subjects below omit the
scope.

### Step 0 — Approval

The plan is committed as Proposed so it can be read and edited, and pushed. Nothing else lands until it is
approved. A comment on flywheel-dag#43 links the plan.

**Done when:** `git status` is clean on that commit, flywheel-dag#43 carries the link, and the plan is approved.

Commit: `docs: add plan for a stop() that cannot lose its wake-up`.

### Step 1 — Start from green

No commit. On the Mac:

```bash
cmake --preset relwithdebinfo-macos-arm64 -Werror=dev -Werror=deprecated -DFLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON
cmake --build --preset relwithdebinfo-macos-arm64
ctest --preset relwithdebinfo-macos-arm64
cmake --build --preset relwithdebinfo-macos-arm64 --target bench_hot_path
./build/relwithdebinfo-macos-arm64/bin/bench_hot_path --invariants | diff - benchmarks/expected_invariants.txt
```

and the same ctest in the Linux container (GCC 13.3, libstdc++), never at the same time as the Mac's build.

**Done when:** the configure and the build print no warning, ctest passes in full on both (the count is recorded;
39 at v0.3.0), and the invariants match. Anything else: stop and ask.

### Step 2 — Extract the wake state; behaviour unchanged

`detail::BasicWakeState` as above, with `requestStop()` storing outside the lock exactly as `Engine::stop()` does
now, and a comment over it that says so. `Engine` uses `WakeState`, loses `running_`, and `run()`, `stop()`,
`poke()` and `makeWakeHook()` call the class. `tests/test_wake_state.cpp` is new and registered in
`tests/CMakeLists.txt`.

Tests written, all of which pass on the extraction:

| Test | Asserts |
|---|---|
| `WakeState.WorkSignalledBeforeTheWaitIsConsumedOnce` | `signalWork()` then `waitForWork()` is true and leaves the flag clear |
| `WakeState.AStopBeforeTheWaitEndsIt` | `requestStop()` then `waitForWork()` is false, and `running()` is false |
| `WakeState.ASecondBeginRunIsRefusedAndLeavesTheFlagSet` | the second `beginRun()` is false and `running()` stays true; `endRun()` clears it |
| `WakeState.AStopWakesABlockedWaiterOnTheRealPrimitives` | a waiter on `std::mutex` and `std::condition_variable`, a `requestStop()` from the test: the waiter returns false (a stop that never notifies hangs it, which ctest ends with its timeout) |

The existing suite is the test of `Engine`'s side: `EngineRun.*`, `EngineWakeHook.*` and the replay tests all end
or wake runs through these calls.

**Done when:** the build prints no warning with `FLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON`; ctest passes in full, one
more than Step 1; `bench_hot_path --invariants` matches the committed file; `bench_hot_path` run before and after
on the same machine differs by no more than the layout-noise floor of flywheel-dag#34 (`Engine` loses a member, so
the atomics after it move by 8 bytes, which is that noise and not a cost); and the Linux container passes.

Commit: `refactor: the engine's wake state is a class, behaviour unchanged`.

### Step 3 — The regression tests, red, then the fix

**Tests written.**

| Test | Where | Asserts |
|---|---|---|
| `WakeState.AStopBetweenTheWaitersTestAndItsBlockWaitsForTheMutex` | `test_wake_state.cpp` | the window test with `requestStop()`: one contention, the waiter returns false |
| `WakeState.AWorkSignalBetweenTheWaitersTestAndItsBlockWaitsForTheMutex` | `test_wake_state.cpp` | the same with `signalWork()`: one contention, the waiter returns true |
| `EngineStop.AStopFromAnotherThreadEndsAnIdleRun` | `test_dag_async.cpp` | `run()` on a thread, a first-cycle event releases the test, `stop()` from the test, `join()`. No existing test calls `stop()` from another thread. It cannot aim at the window; it guards the wiring from `Engine::stop()` to the wake |

**Red first.** Against Step 2, on the Mac and in the container: the `requestStop()` window test fails, by
assertion and without hanging; the `signalWork()` window test passes, because `poke()` and the hook were already
right, and now stays that way; the others pass. Recorded in the Outcome, not committed.

**The fix.** `requestStop()` stores under `mu_`. The comment block over the class says why: the state a predicate
reads changes only under the mutex the waiter holds from its test to its block, with the four-step interleaving.
In `dag_engine.hpp` the overview's "Wake mechanism" paragraph (which says the flag, mutex and condition variable
are all a feed thread touches) gains `stop()` and `poke()`, the thread-safety paragraph and `Engine::stop()` gain
D5, and the comment on the removed `running_` goes with it. No line in `CLAUDE.md`: the class comment carries the
rule, and members that `Engine` cannot reach enforce it.

**Hand-made changes**, each on a fresh scratch copy of the fixed tree, each test run on its own under a 10 s
alarm (exit 1 is an assertion, 142 a hang). The script deletes the object before each build and refuses to
report on one that did not recompile. The last column is what the prototype did, the same on libc++ and
libstdc++.

| Change | Must fail | Prototype |
|---|---|---|
| `requestStop()` stores outside the lock (v0.3.0) | the `requestStop()` window test, by assertion | assertion |
| `requestStop()` never notifies | that window test, the real-primitives test and `EngineStop.*`, by hang | hang, hang (the two class tests) |
| `signalWork()` stores outside the lock | the `signalWork()` window test, by assertion | assertion |
| `signalWork()` never notifies | that window test and `EngineWakeHook.APostStillWakesARunningEngine`, by hang | hang |
| `waitForWork()` leaves the flag set | `WorkSignalledBeforeTheWaitIsConsumedOnce`, by assertion | assertion |
| `beginRun()` ignores a run already going | `ASecondBeginRunIsRefused…` and `EngineRun.ARunCalledWhileRunningThrowsAndLeavesTheRunGoing` | not prototyped |
| `endRun()` clears nothing | `EngineRun.RunsAgainAfterANodeThrows` ("already running") | not prototyped |

**Done when:**

- the `requestStop()` window test is red on Step 2 and green now, and ctest is green in full;
- each hand-made change fails exactly the tests stated;
- the build prints no warning with `FLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON` on Apple Clang here and on GCC in the
  container, and `bench_hot_path --invariants` matches the committed file;
- the new tests pass 3,000 repetitions (`--gtest_repeat=3000`) on both;
- CI is green on both legs, the GCC 15 leg being the only evidence for warnings that GCC 15 alone reports
  (flywheel-dag#44).

Commit: `fix: stop() stores under the mutex run() waits under`.

### Step 4 — Release

v0.3.1 as v0.3.0 was released: the project version and the FetchContent tag in `CMakeLists.txt` and the README,
the tag, and a GitHub release with notes. `git log v0.3.0..HEAD` and `git diff v0.3.0..HEAD --stat -- include` are
read first, so the notes describe everything the release carries, not just this fix: as planned, this plan, the
tests, and `dag_engine.hpp` and `dag_engine.inl` under `include/`. The notes say the change in one paragraph and
that no public signature changed. The plan is marked done, and flywheel-dag#43 is closed with a summary comment.

**Done when:** ctest is green, CI is green on the release commit on both legs, the installed version file reports
0.3.1, the release is published, and flywheel-dag#43 is closed.

Commits: `build: release v0.3.1` and `docs: mark the plan done`.

## Not in this plan

- **A `stop()` before `run()`** is ignored: [flywheel-dag#50](https://github.com/tomlin256/flywheel-dag/issues/50),
  found while planning this, labelled `documentation` and `question`.
- **Sanitizer builds** ([flywheel-dag#45](https://github.com/tomlin256/flywheel-dag/issues/45)). ThreadSanitizer
  would not have found this bug: `running_` is atomic, so there is no data race to report. It is an ordering bug.
- **Which threads may call `Input<T>::set()`** is [flywheel-dag#31](https://github.com/tomlin256/flywheel-dag/issues/31).

## Self-review — risks and assumptions

- **The doubles prove the protocol, not the standard library.** The guarantee that a waiter is either before its
  test or already blocked comes from `std::mutex` and `std::condition_variable`. The tests show `stop()` takes the
  mutex the waiter holds, and `std::condition_variable_any` gives the double the same release-and-block
  contract. The engine instantiates the real types, and `AStopWakesABlockedWaiterOnTheRealPrimitives` and
  `EngineStop.*` run them.
- **The doubles know how the class locks.** A `requestStop()` that took its lock some way `ContendedMutex` does
  not see (a different type, a `try_lock` loop) would be missed. The class is ours and the comment over it says
  which way it locks.
- **The window test counts contention on `mu_`, and the count must be exactly one on the fix.** The waiter holds
  `mu_` from its first lock to its block, nothing else in the test touches `mu_`, and the waiter's re-lock after
  its wake comes after the writer has released it. The prototype held that over 3,000 runs on each standard
  library; Step 3 repeats it.
- **Red in Step 3 is on the extraction, not on v0.3.0.** They are the same behaviour by construction (D3), and
  the aimed stress above is why no test of the real engine can show it.
- **Layout.** `Engine` loses a member, so `cycles_` and its neighbours move by 8 bytes. `bench_hot_path` rows
  carry about 1% code-layout noise (flywheel-dag#34); a difference under that floor, with the exact
  `allocs/cycle`, `callbacks` and `checksum` columns unchanged, is not called a cost. `cycle()` itself is not
  touched.
- **`stop()` can now wait for the mutex.** The run thread holds it for a predicate and a flag, never across
  `cycle()` and never while calling out, so the wait is a few nanoseconds and `mu_` is a leaf lock: no lock order
  to get wrong. A `stop()` from an output callback runs outside it.
- **`endRun()` takes no lock** because it runs in a destructor, possibly during unwinding, where a throwing lock
  would terminate. That is why `running_` stays atomic.
- **A `std::function` in a test double.** GCC 15 rejects a copy of one that holds an empty lambda
  (flywheel-dag#44). The doubles' members are assigned lambdas that capture, and moved, never copied empty; CI's
  GCC 15 leg is the judge, and the container's GCC 13.3 cannot reproduce a GCC 15-only warning.
- **The Linux half of the engine is unverified until Step 1 and Step 3 run it.** The prototype of the class and its
  tests ran on both standard libraries with identical results, but the engine's own suite has not run on Linux
  since v0.3.0, and CI's GCC 15 is a newer compiler than the container's.
- **Names follow the file.** The methods are camelCase like every neighbour in `dag_engine.hpp`, not the
  snake_case table of the C++ guideline.
- **Assumptions:** the engine stays header-only C++17. `run()` is called from one thread. CI stays on
  `ubuntu-latest` and `macos-latest`.

## Progress

| Step | Status | Notes |
|---|---|---|
| 0 — Approval | Proposed | |
| 1 — Start from green | Not started | |
| 2 — Extract the wake state | Not started | |
| 3 — Tests, red, then the fix | Not started | |
| 4 — Release | Not started | |
