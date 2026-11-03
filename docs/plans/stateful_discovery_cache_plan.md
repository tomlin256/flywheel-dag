# The Stateful-Node Walk Must Run Once per Registration, Not Once per Call

**Status: Done (2026-11-03), released as v0.3.2.** The plan was approved as written: D1 to D5 stood, so the cache
holds the walk and the `persistState()` test runs on every call, behind a mutex; `addOutput()` alone resets it; the
tests landed in two green commits; and the fix is in
[v0.3.2](https://github.com/tomlin256/flywheel-dag/releases/tag/v0.3.2). The Outcome section at the end records what
the steps found and where they differ from this text.

Closes [flywheel-dag#29](https://github.com/tomlin256/flywheel-dag/issues/29).

## Problem

`Engine::discoverStatefulNodes()` walks `inputs()` breadth-first from every registered output, with a
`dynamic_pointer_cast` per node, on every call. `saveState()` and `restoreState()` each call it. The answer
changes only when an output is registered: `addOutput()` is the one place `outputs_` grows, and graphs are not
rewired at runtime (`dag.hpp`, over `notifyDownstream()`: "a node's downstream set is built once, by `wire()`").
A host that saves on a timer pays for the whole walk each time, stateless nodes included.

### What I measured

A scratch program (not committed), `-O2 -DNDEBUG`, Apple Clang 21, arm64, `main` at 4228e77. N modules, each
`Input → RollingStats → EWMANode → DeltaNode → ComputeNode`, one output per module: five nodes and three stateful
ones per module. Each figure is the mean of a timed loop after a warm-up, and three runs agree within about 10%.

| Nodes | Stateful | `discoverStatefulNodes()` | `saveState()`, `InMemoryStateStore` | `saveState()`, `JsonFileStateStore` |
|---:|---:|---:|---:|---:|
| 50 | 30 | 4.5 µs | 10 µs | 125 µs |
| 500 | 300 | 43 µs | 102 µs | 340 µs |
| 5,000 | 3,000 | 460 µs | 1.08 ms | 2.3 ms |
| 50,000 | 30,000 | 5.0 ms | 11.1 ms | 23.2 ms |

- The walk costs 90 to 100 ns per node, so it is linear. It is 42% to 45% of an in-memory save at every size,
  and 4% to 22% of a save to a JSON file, where the store's own work and the file write are the rest.
- Saving once a second, 5,000 nodes spend 0.05% of a core on the walk. The issue is right that a small graph
  does not feel it. The saving grows with the graph and with the save rate, and `restoreState()` pays the same
  walk at startup.

## The fix

The engine keeps the result of the walk and drops it when an output is registered. Only the walk is kept. The
`persistState()` test still runs on every call, because `persistState()` is a public virtual with no promise
that it is constant, and caching the filtered list would change what a node that changes its answer gets.

```cpp
// dag_engine.hpp, private, declared after every existing member
using StatefulList    = std::vector<dag::StatefulNodePtr>;
using StatefulListPtr = std::shared_ptr<const StatefulList>;

mutable std::mutex      discoveryMu_;
mutable StatefulListPtr discovered_;   // null until a call walks; guarded by discoveryMu_
```

```cpp
// dag_engine.inl
inline std::vector<dag::StatefulNodePtr> Engine::discoverStatefulNodes() const {
    StatefulListPtr reached;
    {
        const std::lock_guard<std::mutex> lk(discoveryMu_);
        if (!discovered_) discovered_ = std::make_shared<const StatefulList>(walkStatefulNodes());
        reached = discovered_;
    }
    std::vector<dag::StatefulNodePtr> result;
    result.reserve(reached->size());
    for (const auto& sn : *reached)
        if (sn->persistState()) result.push_back(sn);   // outside the lock: this is user code
    return result;
}
```

`walkStatefulNodes()` is today's body without the `persistState()` test: private, `const`, same BFS, same order.
`addOutput()` resets `discovered_` under the lock, straight after `outputs_.push_back()`. `install()` and
`addFeedback()` register through `addOutput()`, so it is the one place that needs the reset.

### What stays the same

- The signature, the BFS order, the dedupe, and the filter.
- `cycle()`. The new members come last, after `cycleSeqLock_`, so no offset it uses moves. Step 4 proves it by
  disassembly: the repo's rows carry about 1% code-layout noise, which a timing could not see through.
- What may be called from which thread. Registering from another thread while `run()` goes stays unsupported.

### Why `mutable`, and why a mutex

`saveState()` and `discoverStatefulNodes()` are `const` and public, so the cache has to be filled from a `const`
member: `mutable` is the only way that keeps the signatures (making them non-const would break a caller that
holds a `const Engine&`). Until now they only read, so two threads calling them did not race; an unlocked fill
would make them race on the first call after a registration. The lock is held for the walk, which calls `inputs()`
on every node, so an `inputs()` that called back into the engine would deadlock. None does, and `persistState()`
runs outside the lock for the same reason.

## Decisions to confirm

My defaults, each one a place to edit before approving.

- **D1. Cache the walk and filter on every call.** The issue says "cache the result". The filter costs one
  virtual call per stateful node, which Step 4 measures, and it keeps a `persistState()` that changes its answer
  honoured. I recommend it.
- **D2. A mutex, not the issue's bare `mutable std::optional`.** I recommend the mutex, for the reason above.
  Its cost is one uncontended lock per call and per registration.
- **D3. Reset from `addOutput()` alone.** The issue names `install()` too. A reset in `install()` as well would be
  a second place to forget when a third registration route appears; hand-made change M3 below shows what the
  tests do when it is the only one.
- **D4. Two green commits, not a red one first.** Step 2 lands the tests that pass on today's code and pin what
  the cache must keep. Step 3 lands the cache with the tests that fail without it. The red run is on a scratch
  copy of the old headers and goes in the Outcome. flywheel-dag#42 committed its red tests first on purpose; say
  so if you want that here.
- **D5. A patch release, v0.3.2, as Step 5.** Nothing under `include/` has changed since v0.3.1, so the release
  carries this change from the headers alone, plus the test and plan commits since the tag. No signature changes.
  Strike Step 5 to leave it for the next release.

## Steps

Every commit subject is scoped to flywheel-dag#29 in the repo's `type(#N): …` form. The subjects below omit the
scope. Each step ends with a full build and a full ctest run, and nothing is pushed until its step is green.

### Step 0 — Approval

The plan is committed as Proposed so it can be read and edited. Nothing else lands until it is approved. A
comment on flywheel-dag#29 links the plan, and a commit records the approval here.

**Done when:** `git status` is clean on that commit and flywheel-dag#29 carries the link.

Commit: `docs: add plan for caching the stateful nodes the engine finds`.

### Step 1 — Start from green, and record the baseline

No commit. `cmake --preset relwithdebinfo-macos-arm64`, build, `ctest --preset …`. Then build with
`-Werror=dev -Werror=deprecated -DFLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON`, build `bench_hot_path`, run
`--invariants` against `benchmarks/expected_invariants.txt`, and save the disassembly of `Engine::cycle()` from
the benchmark binary (`xcrun llvm-objdump -d --no-show-raw-insn`, one symbol, addresses stripped) as the
reference for Step 4. Rerun the scratch program for the table above.

**Done when:** no configure or build warning, ctest passes in full (41 of 41 at the last commit), the invariants
match, and the reference disassembly and the baseline table are saved. Anything else: stop and ask.

### Step 2 — Tests that pin what the cache must keep

New tests in `tests/test_engine_stateful_discovery.cpp`, a section named `StatefulDiscoveryCache.*`. They
pass on today's code, because today's code walks every time and so cannot return a stale answer.

| Test | Asserts |
|---|---|
| T1 `AnOutputRegisteredLaterIsFound` | a call, then an output that reaches a new stateful node, then a call: the second result is both nodes in breadth-first order, element for element |
| T2 `InstallAndAddFeedbackAreFoundToo` | after a call, `install()` of a module whose `wire()` registers two outputs and calls `discoverStatefulNodes()` between them, then `addFeedback()` of a node reaching another stateful node: each call sees exactly the nodes registered so far |
| T3 `PersistStateIsAskedOnEveryCall` | a probe whose `persistState()` a test flips: included, excluded, included again, with no registration between the calls |

They share two helpers local to the file: `CountingNode`, a `NodeBase` whose `inputs()` adds one to a counter the
test holds (the counter is a `std::shared_ptr<std::atomic<std::size_t>>`, since `inputs()` is `const`), and
`StatefulProbe`, the same node with `IStatefulNode` and a flag behind `persistState()`. Nothing steps an engine, so
neither evaluates.

**Done when:** T1 to T3 pass on the unchanged headers and ctest is green in full.

Commit: `test: pin what discoverStatefulNodes() returns after a registration and as persistState() changes`.

### Step 3 — The cache

The members, `walkStatefulNodes()`, the new `discoverStatefulNodes()`, and the reset in `addOutput()`. Over the
members, a comment that says why they are `mutable` and why the fill is locked. The public doc of
`discoverStatefulNodes()` says the walk runs once after a registration and `persistState()` is asked on every
call. The doc of `INode::inputs()` in `dag.hpp` says the edges are fixed once the node is built, since the cache
now depends on it. `CLAUDE.md`'s "State persistence" paragraph gains one sentence to the same effect.

Tests, in the same section, that **fail on the old headers**:

| Test | Asserts |
|---|---|
| T4 `CallsBetweenRegistrationsDoNotWalk` | three calls leave the probe's counter at 1, and the three results are equal |
| T5 `ARegistrationWalksOnceMore` | call, call, `addOutput()`, call, call: the counter reads 2 |
| T6 `AnEmptyResultIsKept` | a graph with no stateful node: two calls, one walk |
| T7 `SaveAndRestoreShareTheWalk` | with an `InMemoryStateStore`, `saveState()` three times and `restoreState()` once leave the counter at 1, and the store round-trips the probe's state |
| T8 `ThreadsOnAColdCacheWalkOnce` | eight threads call once each on a cold cache, over a graph with 20,000 leaves so the walk outlasts the other threads' start; every result equals the expected list, and the counter reads 1 |

T8 has no sleep and no barrier: its assertions hold in every interleaving of a correct implementation, and the
wide graph is what makes an unlocked fill overlap. The graph is shallow, so building and destroying it does not
recurse.

**Done when:**

- T4 to T8 fail on the old headers (run on a scratch copy of them, first) and pass on the new; T1 to T3 still
  pass; ctest is green in full;
- the build prints no warning with `FLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON` on Apple Clang here and on GCC 13.3 in
  `ubuntu-dev`, and CI is green on both legs (GCC 15.2 and macOS), pushed on its own so the next push does not
  cancel it;
- each hand-made change below fails exactly the tests stated.

Each change is made on a scratch copy of `include/` and never restored (Make 3.81 compares mtimes in whole
seconds), and each test runs alone under a watchdog:

| Change | Fails |
|---|---|
| M1 no cache: every call walks (today's code) | T4, T5, T6, T7, T8 |
| M2 `addOutput()` never resets | T1, T2, T5 |
| M3 only `install()` resets | T1, T2, T5 |
| M4 the cache holds the filtered list | T3 |
| M5 an empty result counts as not computed | T6 |
| M6 the fill takes no lock | T8, and ThreadSanitizer reports the race |
| M7 `saveState()` walks for itself | T7 |

Commit: `perf: discoverStatefulNodes() walks once per registration`.

### Step 4 — Measure the gain and the cost

No commit. On the fixed headers:

- the scratch program again: the call between registrations, and `saveState()` with both stores;
- `bench_hot_path --invariants` against the committed file, unchanged;
- the disassembly of `Engine::cycle()` against Step 1's;
- `test_engine_stateful_discovery` under ThreadSanitizer in `ubuntu-dev` (GCC 13.3, tree `build/tsan-linux-arm64`)
  with the fix, and with M6;
- `ctest` in the container's `relwithdebinfo-linux-arm64` tree and its AddressSanitizer tree.

**Done when:** the call between registrations takes at most a fifth of the baseline's time at 500, 5,000 and
50,000 nodes; the invariants match; `cycle()`'s disassembly is identical after addresses are stripped;
ThreadSanitizer is silent with the fix and reports M6; both container trees pass in full. Any other result: stop
and report it.

### Step 5 — Release

v0.3.2 as v0.3.1 was released: the project version in `CMakeLists.txt` and the two FetchContent snippets
(`CMakeLists.txt`, `README.md`), the tag, and a GitHub release with notes. Before the notes say "no signature
changed", run `git log v0.3.1..HEAD` and `git diff v0.3.1..HEAD --stat -- include`. The plan is marked done with
an Outcome section, and flywheel-dag#29 is closed with a summary comment.

**Done when:** ctest is green, CI is green on the release commit on both legs, the installed version file reports
0.3.2, the release is published, and flywheel-dag#29 is closed.

Commits: `build: release v0.3.2` and `docs: mark the plan done`.

## Designs I rejected

- **Rebuild in `addOutput()`.** It needs no `mutable` and no lock, but a module that registers k outputs
  walks k times over a graph that grows, and a host that never saves pays too. The lazy fill costs nothing
  until someone asks.
- **Extend the cache from the new output's walk.** It makes registration linear overall, but it appends the new
  output's nodes after every earlier one, and the documented result is breadth-first from all outputs together.
  That order is an observable property, so changing it is a behaviour change this issue does not ask for.
- **Return a `shared_ptr` to the cached list.** It would save the copy, but it changes a public signature that
  the suite and a host's code use, to save work that is now proportional to the stateful nodes only.

## Not in this plan

- **The orphan detector** that the removed `TODO` also described: [flywheel-dag#28](https://github.com/tomlin256/flywheel-dag/issues/28).
- **No sanitizer build of the engine's own tests in CI:** [flywheel-dag#45](https://github.com/tomlin256/flywheel-dag/issues/45).
  Step 4 runs ThreadSanitizer by hand once.
- **Registering an output from another thread while `run()` goes.** Unsupported, as before; the mutex covers the
  cache and not `outputs_`.

## Self-review — risks and assumptions

- **The edges are fixed once wired.** The cache is wrong for a node whose `inputs()` changes after an output
  reaches it, until the next `addOutput()`. `dag.hpp` already says graphs are not rewired at runtime, every
  `inputs()` in `include/` returns members its constructor set (I grepped for a later assignment and found
  none), and `wire()` builds the downstream links once from them, so such a node would already miss
  invalidations. Step 3 writes the rule on `INode::inputs()`, where a node author reads it.
- **`persistState()` is not assumed constant,** which is why D1 filters on every call. T3 pins it.
- **The cache holds strong references** to the stateful nodes until the next registration or the engine's
  destruction. Every edge in `include/` is a strong member, so the output already owns each of them and the
  cache extends no lifetime. A node of a user's own that returns a `weak_ptr`'s `lock()` from `inputs()` would be
  kept alive by it.
- **The lock covers the walk,** so `inputs()` must not call into the engine (above). It also means two threads on a
  cold cache walk once between them, which T8 asserts.
- **`addOutput()` takes a lock** it did not take before. Registration is not a hot path, and the cost is two atomic
  operations an output.
- **`sizeof(Engine)` grows** by a mutex and a pointer. The library is header-only, so a consumer rebuilds.
- **A cached call is not free.** It copies the stateful nodes' `shared_ptr`s and asks each `persistState()`, so
  it is linear in the stateful nodes, not in the graph. Step 4 measures what that leaves.
- **T8 is probabilistic about the bug and deterministic about the fix.** On a correct implementation it passes in
  every interleaving. Against M6 it fails when the threads overlap, which the wide graph makes the usual case,
  and ThreadSanitizer is what makes M6 certain. That is why Step 4 runs it.
- **The benefit is modest below a few thousand nodes.** 0.46 ms a save at 5,000 nodes is not a problem for most
  hosts. The change is about thirty lines and costs nothing in the cycle, which is why I recommend it, but the
  table is the evidence, and if it does not justify a mutex in `Engine` the answer to the plan is to close the
  issue as not worth it.
- **Assumptions:** the engine stays header-only C++17; the suite's tests keep running under `ctest` with no
  sleeps; CI stays on GCC 15.2 and Apple Clang.

## Progress

| Step | Status | Notes |
|---|---|---|
| 0 — Approval | Done | Approved as written on 2026-11-03. |
| 1 — Start from green | Done | ctest 41 of 41, no warnings with warnings as errors, invariants match; the reference disassembly saved. |
| 2 — Tests that pin | Done | `b90252c`. T1 to T3, green on v0.3.1's engine. |
| 3 — The cache | Done | `bb63d88`. T4 to T8 red on the old engine and green on the new; eight hand-made changes fail as stated; CI green on both legs. |
| 4 — Measure | Done | No commit. A cached call takes a fiftieth of the old walk; `cycle()` is identical; the sanitizers are clean. |
| 5 — Release | Done | `4b4e62e`, tag `v0.3.2`, release published. |

## Outcome

Released as [v0.3.2](https://github.com/tomlin256/flywheel-dag/releases/tag/v0.3.2), tag `v0.3.2` on `4b4e62e`.

| Commit | Step | What |
|---|---|---|
| `06f23fc` | 0 | the plan, proposed |
| `a525c90` | 0 | the approval, recorded |
| `b90252c` | 2 | the `StatefulDiscoveryCache` tests T1 to T3, which pass on v0.3.1's engine |
| `bb63d88` | 3 | the cache, T4 to T8, and the comments that state the rule |
| `4b4e62e` | 5 | release v0.3.2 |

The tests, by the numbers used below: T1 `AnOutputRegisteredLaterIsFoundInBreadthFirstOrder`, T2
`InstallAndAddFeedbackAreFoundToo`, T3 `PersistStateIsAskedOnEveryCall`, T4 `CallsBetweenRegistrationsDoNotWalk`, T5
`ARegistrationWalksOnceMore`, T6 `AnEmptyResultIsKept`, T7 `SaveAndRestoreShareTheWalk` and T8
`ThreadsOnAColdCacheWalkOnce`.

**Step 1.** The baseline was green on the Mac: no configure or build warning with `-Werror=dev -Werror=deprecated
-DFLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON`, ctest 41 of 41, and `bench_hot_path --invariants` matching the committed file.
The disassembly of `Engine::cycle()` (185 lines once addresses are stripped) and of the destructor of its `TimeOnExit`
guard (77) was saved as the reference.

**Step 2.** T1 to T3 passed on the unchanged engine, as they must: it walks on every call, so it cannot be stale, and
they are what a cache has to keep. Found on the way: [flywheel-dag#54](https://github.com/tomlin256/flywheel-dag/issues/54),
two unused includes in the test file that this plan did not put there.

**Step 3, red first.** T4 to T8 were written and run on the unchanged engine, before a header was edited. All five failed
on the walk count the probe read: 3 where 1 is right (T4), 4 where 2 (T5), 2 where 1 (T6), 4 where 1 (T7) and 8 where 1
(T8: each of the eight threads walked). T1 to T3 passed. With the cache the suite passes 18 of 18 and ctest 41 of 41.

Eight hand-made changes, each built from a scratch copy of `include/` and the test, with every test run alone under a
30 s watchdog. M0, the fixed code unchanged, passes all 18.

| Change | Fails |
|---|---|
| M1 no cache: every call walks | T4, T5, T6, T7, T8 |
| M2 `addOutput()` never resets | T1, T2, T5 |
| M3 only `install()` resets | T1, T2, T5 |
| M4 the cache holds the filtered list | T3 |
| M5 an empty result counts as not computed | T6 |
| M6 the fill takes no lock | T8 |
| M7 `saveState()` walks for itself | T7 |
| M8 `restoreState()` walks for itself | T7 |

M6 fails T8 on 40 of 40 runs on the Mac, with the probe's walk count at 8 where 1 is right, and the fixed code passes
40 of 40.

**Step 4, measured.** The scratch program of "What I measured", on the fixed headers, three runs that agree within about
4% (apart from the first row of the first run of a process). Before is the table in "Problem":

| Nodes | A call between registrations | `saveState()`, `InMemoryStateStore` | `saveState()`, `JsonFileStateStore` |
|---:|---:|---:|---:|
| 50 | 4.7 µs → 0.1 µs | 10.3 µs → 5.7 µs | 124 µs → 123 µs |
| 500 | 43 µs → 0.7 to 1.0 µs | 102 µs → 60 µs | 340 µs → 292 µs |
| 5,000 | 460 µs → 9.4 µs | 1.08 ms → 0.63 ms | 2.32 ms → 1.90 ms |
| 50,000 | 5.0 ms → 0.10 ms | 11.2 ms → 6.4 ms | 23.4 ms → 18.4 ms |

- The bar was a fifth of the baseline. A call takes a forty-third to a sixty-first of it at 500 nodes and a fiftieth at
  5,000 and 50,000. What is left is the copy and the `persistState()` test, about 3 ns for each stateful node.
- The first call after a registration costs 5% to 10% more than before (5,000 nodes: 513 to 554 µs against 464 to 501;
  50,000: 5.5 to 5.6 ms against 5.0 to 5.4), since it makes the list and then copies and filters it.
- `bench_hot_path --invariants` matches the committed file, which is unchanged.
- The disassembly of `Engine::cycle()` is identical to Step 1's once addresses are stripped: the same SHA-1 over the 185
  lines. The `TimeOnExit` destructor is identical but for one instruction pair, the address of the string literal that
  the empty-buffer branch of `RingBuffer::front()` passes to `throwEmpty()`, which moved by 176 bytes in the constant pool.
- In `ubuntu-dev` (GCC 13.3, aarch64): `relwithdebinfo-linux-arm64` with the CI warning flags builds with no warning and
  passes 41 of 41; the AddressSanitizer and UndefinedBehaviorSanitizer tree passes 41 of 41 with default options and no
  report; the ThreadSanitizer tree runs the discovery suite 18 of 18 with no report. M6 built there with ThreadSanitizer
  fails T8 on 30 of 30 runs, and ThreadSanitizer reports a data race in 13 of them.
- **CI.** [Run 41628014858](https://github.com/tomlin256/flywheel-dag/actions/runs/41628014858) on `bb63d88` and
  [run 41629104701](https://github.com/tomlin256/flywheel-dag/actions/runs/41629104701) on `4b4e62e` are green on both
  legs, GCC 15.2.0 on ubuntu-latest and Apple Clang 21.0.0 on macOS: 41 of 41 each, no compiler warning, and the
  benchmark step passing.

**Step 5, release.** ctest 41 of 41 with no warning, and the installed version file reports `0.3.2`. `git diff
v0.3.1..v0.3.2 --stat -- include` names `dag.hpp` (a comment), `dag_engine.hpp` and `dag_engine.inl` alone. The commits
since v0.3.1 were read first: this plan, and the test-only work of flywheel-dag#47, flywheel-dag#48 and flywheel-dag#53.

**Where the work differs from the plan.**

- **The local checks of Step 4 ran before Step 3's push.** The plan had CI under Step 3 and the measurements after. Running
  the cheap ones first meant a failure would not cost a CI run. Steps 2 and 3 and the two plan commits went in one
  push, and nothing else was pushed until its CI had finished.
- **ThreadSanitizer is not what makes M6 certain,** as the self-review had it. It reported the unlocked fill in 13 of 30
  runs, and why it misses the others is not established: the probe's atomic walk counter gives the threads
  happens-before edges, which may hide some of the accesses, and the detector keeps only a few past accesses for each
  word. T8's walk count is the guard, and it fails on every run. ThreadSanitizer is a second signal, and silent on the
  fixed engine.
- **One more hand-made change than planned:** M8, `restoreState()` walking for itself, which T7 also catches.
- **The red run was on the unchanged tree,** not on a scratch copy of the old headers, since no header had been edited
  yet: it is the same engine.
- **The `TimeOnExit` destructor is not byte-identical** to Step 1's; the plan asked that of `cycle()`, which is.
- **flywheel-dag#54** was not in the plan.

**Left open.** [flywheel-dag#28](https://github.com/tomlin256/flywheel-dag/issues/28) (the orphan detector),
[flywheel-dag#45](https://github.com/tomlin256/flywheel-dag/issues/45) (no sanitizer build of the engine's own tests in
CI; the thread test is one more reason for it) and flywheel-dag#54 (two unused includes).
