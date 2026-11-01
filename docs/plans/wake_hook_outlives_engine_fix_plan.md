# A Source's Wake Hook Must Not Hold the Engine by Raw Pointer

**Status: Approved (2026-11-01).** The plan was approved as written: D1 to D3 stand, the engine's wake state
becomes shared, and the fix is released as v0.2.7.

Closes [flywheel-dag#42](https://github.com/tomlin256/flywheel-dag/issues/42).

## Problem

`Engine::addSource()` gives each source a wake hook, `Engine::makeWakeHook()`:

```cpp
inline std::function<void()> Engine::makeWakeHook() {
    return [this] {
        { std::lock_guard lk(mu_); hasWork_ = true; }
        cv_.notify_one();
    };
}
```

The source keeps the hook. The Engine keeps no record of it, and a source is `shared_ptr`-owned, so
whatever else holds the source can `post()` after the Engine is gone. The hook then locks a mutex and
signals a condition variable that no longer exist. `Engine::makeInput<T>()` installs the same hook, and
`FeedRegistry` hands it on to its members.

Nothing in the headers says the Engine must outlive its posters. `IFlushable::setWakeHook` says the hook is
installed "before any thread posts", which is about the start of a source's life. `Engine` has no
destructor to give an ending rule a place to live.

A reproduction that uses nothing but the engine headers (`main` at 7a43fd3):

```cpp
auto queue = dag::async::AsyncQueue<int>::make("q");
{
    dag::async::Engine engine;
    engine.addSource(queue);
}                       // the Engine is gone; the queue still holds its hook
queue->post(1);         // locks the destroyed Engine's mutex
```

On Apple Clang 21 with libc++ this terminates on 20 runs of 20 with `std::system_error: mutex lock failed:
Invalid argument`, and an `AsyncInput<int>` in place of the queue does the same. Other standard libraries
may do something else: libstdc++'s `std::mutex` has a trivial destructor, so on Linux the same code can
find a mutex that still looks valid, and ThreadSanitizer is what reports it there (a race between
`pthread_cond_destroy` and `pthread_cond_signal`).

### It does not need a contrived order

`AsyncQueue::post()` pushes under the queue's lock, releases it, and only then calls the hook. A thread can
sit between the two when the eval thread flushes the pushed value, finds the work it was waiting for, and
lets the Engine go out of scope. The window is a few instructions wide, but a thread descheduled inside it
calls the hook on a destroyed Engine, and a loaded machine or a sanitizer makes that likelier. The ordinary
shape is a worker that posts a result and a controller that steps the Engine until the result arrives and
then tears it down. A detached worker is the same shape with no controller to wait for it.

## The fix

Move the state the hook touches into a small struct owned by a `std::shared_ptr`, held by the Engine and by
every hook:

```cpp
// dag_engine.hpp, private
struct WakeState {
    std::mutex              mu;
    std::condition_variable cv;
    bool                    hasWork = false;
};
std::shared_ptr<WakeState> wake_ = std::make_shared<WakeState>();
std::atomic<bool>          running_{false};      // stays where it is
```

```cpp
inline std::function<void()> Engine::makeWakeHook() {
    return [wake = wake_] {
        { std::lock_guard lk(wake->mu); wake->hasWork = true; }
        wake->cv.notify_one();
    };
}
```

`run()` waits on `wake_->cv` with `wake_->mu`, `poke()` signals through the same state, and `stop()` notifies
`wake_->cv`. A hook that runs after the Engine is destroyed then locks a mutex that still exists, sets a
flag nobody reads, and returns.

- **No new lock on the posting path.** The hook already takes this mutex once, to set the flag.
- **No allocation per post.** The `shared_ptr` is copied once, when the hook is made at registration. The
  state is allocated once, with the Engine, so an `Engine` costs one more allocation than before and a
  cycle costs none: `bench_hot_path`'s `allocs/cycle` rows should not move, and
  `benchmarks/expected_invariants.txt` should need no regeneration.
- **No cycle.** The hook holds the state, not the Engine; the Engine holds the sources, and a source holds
  the hook.
- **A state outlives its Engine only while a source does.** It is a mutex, a condition variable and a bool.
- **`stop()` is unchanged in what it does.** It stores `running_` and notifies, as today. See "Not in this
  plan" for the one thing about it that is not this issue.

### What it does not make safe

Calling a method on a destroyed Engine (`poke()`, `stop()`, `step()`) is still undefined, as for any object.
Destroying an Engine while another thread is inside `run()` on it is still a misuse. What becomes safe is
the one thing a source does without being asked: `post()` or `set()` through the hook it was handed.

## Decisions to confirm

My defaults, each one a place to edit before approving.

- **D1. A shared wake state, not a gate.** The alternative in the issue is a mutex plus an "alive" flag that
  `~Engine()` takes and clears. It adds a lock to every post and makes `~Engine()` wait for posters, and the
  shared state needs neither. A third option, documenting that posters must finish first, is cheap and leaves
  a trap for any `shared_ptr`-owned source and any thread that is detached. I recommend the shared state.
- **D2. A patch release, v0.2.7.** The change is internal to a private member and a private function. Nothing
  public changes shape; the class layout does, and the library is header-only, so consumers rebuild.
- **D3. The documented rule goes in `dag_engine.hpp`'s thread-safety paragraph and `CLAUDE.md`'s "Threads and
  the engine"**, in one sentence each: a source may be posted to after its Engine is destroyed, and nothing
  else on an Engine may be used then.

## Steps

Every commit subject is scoped to flywheel-dag#42 in the repo's `type(#N): …` form. The subjects below omit the
scope.

### Step 0 — Approval

The plan is committed as Proposed so it can be read and edited. Nothing else lands until it is approved. A
comment on flywheel-dag#42 links the plan.

**Done when:** `git status` is clean on that commit and flywheel-dag#42 carries the link.

Commit: `docs: add plan for a wake hook that does not hold the engine`.

### Step 1 — Start from green

No commit. `cmake -B build && cmake --build build && ctest --test-dir build`, then
`cmake --build build --target bench_hot_path && ./build/bin/bench_hot_path --invariants` against
`benchmarks/expected_invariants.txt`.

**Done when:** the configure prints no warning, the build prints none with `FLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON`,
ctest passes in full (the count is recorded), and the invariants match. Anything else: stop and ask.

### Step 2 — Regression tests, red first

New tests in `tests/test_dag_async.cpp`, in a section after the `Engine::run()` tests, named
`EngineWakeHook.*`. The helper `postAfterTheEngineIsGone(source)` constructs an `Engine` by placement new in an
aligned buffer, registers the source with it, runs the destructor, then **overwrites the dead object's bytes
with `0xFF`** before the call under test. Without the overwrite a hook that still reads the dead Engine might
find a mutex that looks valid (libstdc++'s `std::mutex` has a trivial destructor), and the test would pass on
Linux on the very bug it exists for. With it, a lock on the dead mutex fails everywhere, as `EINVAL`.

| Test | Asserts | v0.2.6 |
|---|---|---|
| `EngineWakeHook.QueuePostAfterTheEngineIsGone` | `EXPECT_NO_THROW(queue->post(1))`, then the queue still holds the value | the post throws `std::system_error` (libc++), or the process dies |
| `EngineWakeHook.AsyncInputPostAfterTheEngineIsGone` | the same for an `AsyncInput<int>` | the same |
| `EngineWakeHook.RegistryMemberPostAfterTheEngineIsGone` | a queue held by a `FeedRegistry` the Engine took whole | the same |
| `EngineWakeHook.MakeInputSetAfterTheEngineIsGone` | an `Input<T>` from `Engine::makeInput` | the same |
| `EngineWakeHook.APostStillWakesARunningEngine` | `run()` on a thread, one `post()` from the test, an output callback that calls `stop()`; `join()` | passes. It guards the change: a hook that no longer signals hangs it, and ctest's 120 s timeout ends it |

None of them sleeps. The last one is event-driven, and the others are single calls. The file's coverage comment
gains the section.

Each hand-made change below must fail exactly the tests stated:

| Change | Fails |
|---|---|
| The hook captures `this` again (v0.2.6) | the first four |
| The hook sets the flag and never notifies | `APostStillWakesARunningEngine` |
| `run()` waits on the Engine's old members while the hook signals the shared state | `APostStillWakesARunningEngine` |

**Done when:** the first four fail on v0.2.6 for the reason given and `APostStillWakesARunningEngine` passes;
each hand-made change fails as stated, on a scratch copy.

Commit: `test: post to a source after its engine is gone`.

### Step 3 — The fix

`WakeState`, the shared pointer, and the four call sites (`run()`, `poke()`, `stop()`, `makeWakeHook()`), with a
comment over `makeWakeHook()` that says why the hook holds the state and not the Engine. The thread-safety
paragraph in `dag_engine.hpp` and the "Threads and the engine" section of `CLAUDE.md` gain the rule in D3.

**Done when:**

- the four tests that failed pass, and ctest is green in full;
- the build prints no warning with `FLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON`, on Apple Clang here and on GCC in CI;
- `bench_hot_path --invariants` matches `benchmarks/expected_invariants.txt` unchanged;
- the reproduction above runs to the end;
- CI is green on both legs.

Commit: `fix: a source's wake hook holds shared state, not the engine`.

### Step 4 — Release

v0.2.7 as v0.2.6 was released: the project version and the FetchContent snippets in `CMakeLists.txt` and the
README, the tag, and a GitHub release with notes. The notes say the change in one paragraph and that no
signature changed. The plan is marked done, and flywheel-dag#42 is closed with a summary comment.

**Done when:** ctest is green, CI is green on the release commit on both legs, the installed version file
reports 0.2.7, the release is published, and flywheel-dag#42 is closed.

Commits: `build: release v0.2.7` and `docs: mark the plan done`.

## Not in this plan

- **`stop()` can lose a wake-up.** It stores `running_` and calls `cv_.notify_all()` without holding the
  mutex `run()` waits under. If `run()` has evaluated its predicate (still running) and has not yet blocked
  when `stop()` stores and notifies, the notification finds no waiter and `run()` blocks until something
  else wakes it. Found while reading the code for this plan, not reproduced, and the fix is to take the
  mutex around the store. It is its own issue, [flywheel-dag#43](https://github.com/tomlin256/flywheel-dag/issues/43),
  so this change keeps `stop()` doing what it does.
- **Which threads may call `Input<T>::set()`** is [flywheel-dag#31](https://github.com/tomlin256/flywheel-dag/issues/31).
  The test above calls it from the thread that destroyed the Engine, after the Engine is gone, which needs no
  answer to that question.
- **Sanitizer builds of this repo's own tests.** ThreadSanitizer is what shows this class of bug on Linux, and
  the tests here do not run under it. That is a different plan.

## Self-review — risks and assumptions

- **The tests depend on undefined behaviour to go red.** On v0.2.6 they call a dead object's mutex, and the
  `0xFF` overwrite is what makes that fail the same way on libc++ and libstdc++. I checked libc++ here (the
  reproduction); glibc's `pthread_mutex_lock` on a mutex whose kind field is all ones returns `EINVAL`, which
  libstdc++ turns into `std::system_error`. The Linux half is unverified until CI runs Step 2 on its own, and
  Step 2 is committed on its own so that it does. If a platform returns success instead, the test is green
  on the bug there, and the macOS leg still catches it.
- **After the fix the same tests pass for the right reason.** The hook touches the shared state and nothing
  else, so the overwrite of the dead Engine's bytes changes nothing it reads.
- **`run()`'s predicate now reads two places.** `hasWork` is in the shared state and `running_` is in the Engine.
  Both are read under `wake_->mu`, as `hasWork_` and `running_` are today, so the wait loop is the same. A
  reader who moves `running_` into the state too would remove the question; I left it because `stop()` and
  `run()`'s scope guard use it without the lock.
- **One more allocation per Engine.** `make_shared<WakeState>()` in the constructor. `Engine` construction is
  not on a cycle's path, but a benchmark that constructs an Engine per iteration would see it.
- **Header-only layout.** Adding a member and removing three changes `sizeof(Engine)`. A consumer that built
  against an older header and links a newer one has no such thing to link, so the only risk is a stale build
  tree, which a pin bump rebuilds.
- **Assumptions:** the engine stays header-only C++17. `run()` is called from one thread. CI stays on
  `ubuntu-latest` and `macos-latest`.

## Progress

| Step | Status | Notes |
|---|---|---|
| 0 — Approval | Done | Approved as written on 2026-11-01. |
| 1 — Start from green | | |
| 2 — Tests, red first | | |
| 3 — The fix | | |
| 4 — Release | | |
