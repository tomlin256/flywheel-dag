# The Stateful-Node Walk Must Run Once per Registration, Not Once per Call

**Status: Proposed (2026-11-03).** Nothing is implemented. "Decisions to confirm" holds my defaults; edit any of
them before approving.

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
| 0 — Approval | Proposed | |
| 1 — Start from green | Not started | |
| 2 — Tests that pin | Not started | |
| 3 — The cache | Not started | |
| 4 — Measure | Not started | |
| 5 — Release | Not started | |
