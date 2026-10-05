# Make FeedRegistry an IFlushable

**Status: Proposed (2026-10-05), awaiting approval.**

Closes [flywheel-dag#27](https://github.com/tomlin256/flywheel-dag/issues/27).

## Problem

`Engine::addSource(FeedRegistry&)` installs the wake hook on the registry and copies the members the
registry holds at that moment into the engine's `sources_`. A member added to the registry
afterwards gets the hook, because `FeedRegistry::add()` hands it on, so its `post()` wakes the
engine. The engine never flushes it. Its value waits in `pendingCount()` and its output never fires.

The issue's sequence on `c3965ee`, the engine as it stands, prints
`seenA=1 seenB=0 b.pendingCount=1`.

The fault is the shape of the API. `FeedRegistry` is not an `IFlushable`, so the engine needs an
overload of its own for it, and that overload can only unpack it. The comment on the issue gives the
design: a registry that is an `IFlushable` is a source like any other. The engine flushes it each
cycle, and it flushes whatever it holds then. The overload goes.

## The fix

`FeedRegistry` derives from `IFlushable`:

```cpp
class FeedRegistry : public IFlushable {
public:
    void add(std::shared_ptr<IFlushable> input);

    // ── IFlushable ────────────────────────────────────────────────────────────
    std::size_t flush() override;                            // as it is
    std::size_t pendingCount() const override;               // new: the members' counts, summed
    std::string name() const override;                       // new: "feed_registry"
    void setWakeHook(std::function<void()> hook) override;   // as it is

    bool hasPending() const;                                 // as it is
    const std::vector<std::shared_ptr<IFlushable>>& all() const;   // as it is
    // ...
};
```

`Engine::addSource(FeedRegistry&)` and its definition are removed. A registry goes in through
`addSource(std::shared_ptr<IFlushable>)`, which installs the wake hook on it, and
`FeedRegistry::setWakeHook()` hands the hook to every member present and, through `add()`, every
later one:

```cpp
auto reg = std::make_shared<async::FeedRegistry>();
reg->add(a);
engine.addSource(reg);
reg->add(b);          // flushed from the next cycle, and its post() wakes the engine
```

- **`cycle()` does not change.** It still flushes `sources_` in order. A registry flushes its
  members in the order they were added, at the registry's own place in that order, so a late member
  joins it there. `ReplayCoordinator` relies on the order: it is registered first, so its cursor
  advances before the replay sources flush.
- **The constructor stays public and default.** A registry is still a plain object for use without
  an engine, under `TickLoop`; only an engine needs it shared. The alternative, a private
  constructor and a `make()` as `ReplayCoordinator` has, would force a `shared_ptr` on those users.
- **`name()` is a constant**, `"feed_registry"`, as `ReplayCoordinator::name()` returns
  `"replay_coordinator"`. The interface asks for a name and a registry has none of its own.
- **`hasPending()` and `all()` stay.** `hasPending()` stops at the first member with work waiting,
  where `pendingCount()` sums them all. The engine no longer calls `all()`; it is public API, and
  `FeedRegistry.AllReturnsAllInputs` uses it.
- **Source-breaking.** A caller of `addSource(FeedRegistry&)` stops compiling. The migration is the
  snippet above. `CMakeLists.txt` says that before 1.0 a minor release may break the API, and that a
  request for `0.1` accepts only `0.1.z`, so this ships as v0.2.0 and a consumer pinned to a v0.1
  tag is untouched until it moves.

Three comments change, because the contract lives beside the code:

- Over `FeedRegistry` in `dag_async.hpp`: it is a source and may be a member of another; the engine
  flushes it whole, so a late member is flushed in the registry's place in the order and gets the
  wake hook; `add()` and `flush()` share the member list, so `add()` runs on the thread that
  flushes, never from another thread while the engine runs and never from a member's own `flush()`;
  and a new member is not posted to before `add()` returns, since `add()` installs its hook.
- Over `Engine::addSource()` in `dag_engine.hpp`: it takes a source or a `FeedRegistry` of them, and
  flushes a registry as a whole. The note that documents the bug (`flywheel-dag#27`) goes with the
  overload.
- The contents list at the head of `dag_async.hpp`: `FeedRegistry` is an `IFlushable` that groups
  sources.

## Tests

In `test_dag_async.cpp`, with a `LoggedSource` stub: an `IFlushable` that notes each flush in a
shared log. Each is deterministic: calls on a registry, `Engine::step()`, or a `run()` that stops
itself after two cycles. Nothing sleeps. "Fails under" names the prototype mutation that fails the
test (see Prototype).

| Test | Asserts | Fails under |
|---|---|---|
| `FeedRegistry.PendingCountSumsTheMembers` | an `AsyncInput` and an `AsyncQueue` holding one and two values: `pendingCount()` is 3, and 0 after `flush()` | `pendingCount()` returns 0 |
| `FeedRegistry.NameIsFeedRegistry` | `name()` is `"feed_registry"` | `name()` returns `""` |
| `FeedRegistry.FlushesMembersInTheOrderTheyWereAdded` | three `LoggedSource`s, the third added after a first `flush()`: the log reads in the order added | `flush()` runs the members in reverse |
| `FeedRegistry.ARegistryIsAMemberOfAnother` | a registry added to another after its hook is set: a member's `post()` fires the outer hook, and the outer `pendingCount()` and `flush()` cover the member | `pendingCount()` returns 0; `add()` skips the hook |
| `EngineFeedRegistry.AMemberAddedAfterAddSourceIsFlushed` | the issue's sequence through `step()`: both outputs fire, and `b->pendingCount()` is 0 | the engine copies a registry's members at `addSource()` |
| `EngineFeedRegistry.ALateMemberWakesRunAndIsFlushed` | a late member posts from an output callback in `run()`'s first cycle: the post wakes the engine, cycle 2 flushes it, and its callback stops the engine. Seen `{0, 2}`, in two cycles | the engine copies the members; `add()` skips the hook; `addSource()` skips the hook. Each hangs |
| `EngineFeedRegistry.ALateMemberFlushesInTheRegistrysPlace` | sources `before`, a registry and `after`, then a member added to the registry: the order is `before`, `member`, `after` | the engine copies the members |

Modified: the six `test_dag_replay.cpp` tests that build a `FeedRegistry` (`InOrderDelivery`,
`RepeatedValueEqualitySuppressed`, `BatchBoundariesPreserved`, `TwoInputsOneQueueInterleavedSeqs`,
`ShorterStreamIdlesWhileOthersContinue`, `StepDrivenMiniSession`) build it with
`std::make_shared<FeedRegistry>()`. Their assertions do not change, so they show the move changes no
replay behaviour. The seven existing `FeedRegistry` tests in `test_dag_async.cpp` keep their
stack-built registry and pass untouched.

`ALateMemberWakesRunAndIsFlushed` fails by hanging, not by a wrong value: `run()` waits on its
condition variable, and ctest reports a timeout after the 120 s default `CMakeLists.txt` sets.
`ReplayCoordinator.EngineRunCompletesUnattended` fails the same way. The wake is the one thing no
`step()` test can see, and reading the engine's work flag would take public API added for a test.

## Prototype

Built on scratch copies of `include/`, `tests/` and `benchmarks/`, with the flags of
`build/compile_commands.json` and `-Werror`, under Apple Clang. The repo tree is untouched.

- **Baseline.** On `c3965ee` the build prints no warnings and ctest is 36 of 36.
- **The bug.** The issue's sequence, run on `c3965ee`: `seenA=1 seenB=0 b.pendingCount=1`.
- **Tests.** With the change `test_dag_async` is 88 of 88, the 81 it has and the 7 above, and
  `test_dag_replay` is 14 of 14.
- **Mutations.** One flip per scratch copy of `include/`, against the 88 tests. A control copy
  passes all 88.
  - The engine copies a registry's members at `addSource()`, the original bug in the new design:
    `AMemberAddedAfterAddSourceIsFlushed` fails (`b` reads 0, its count is 1),
    `ALateMemberFlushesInTheRegistrysPlace` fails (the log lacks `member`), and
    `ALateMemberWakesRunAndIsFlushed` hangs.
  - `addSource()` skips the wake hook: `ALateMemberWakesRunAndIsFlushed` hangs, and no other test
    fails. No `step()` test sees the wake.
  - `add()` skips the hook: `SetWakeHookPropagatesNewMembers` and `ARegistryIsAMemberOfAnother`
    fail, and `ALateMemberWakesRunAndIsFlushed` hangs.
  - `pendingCount()` returns 0: `PendingCountSumsTheMembers` and `ARegistryIsAMemberOfAnother`, and
    no others.
  - `name()` returns `""`: `NameIsFeedRegistry`, and no other.
  - `flush()` runs the members in reverse: `FlushesMembersInTheOrderTheyWereAdded`, and no other.
    `ALateMemberFlushesInTheRegistrysPlace` holds one member and cannot see an order.
- **Hot path.** `bench_hot_path --invariants` matches `benchmarks/expected_invariants.txt` on
  `c3965ee` and on the prototype. 40 rounds of the benchmark, each running both builds in a random
  order. Medians in ns/cycle, `c3965ee` against the prototype: chain 263.9 and 264.0, idle-queues
  234.4 and 234.9, ingest 118.7 and 119.0.
- **The registry path**, which none of those rows uses: 32 idle `AsyncQueue`s behind one registry,
  60 shuffled rounds. On `c3965ee`, where the engine held the members itself, 233.4 ns/cycle. On
  the prototype 234.8, 1.4 ns (0.6%) more: a virtual call and a loop. One registry nested in another
  reads 235.1. Every configuration allocates 0 per cycle. A row for it is not added to
  `bench_hot_path`: the cost is one virtual call, and the committed rows stay as they are.

## Steps

Every commit subject is scoped to flywheel-dag#27 in the repo's `type(#N): …` form. The subjects
below omit the scope.

### Step 1 — The tests and the fix

Written: the seven tests above. Modified: the six replay tests. Changed: `dag_async.hpp` and
`.inl`, `dag_engine.hpp` and `.inl`, and the coverage list at the head of `test_dag_async.cpp`.

**Done when:**

- the seven new tests fail under the mutations given and pass with the fix. They are checked on
  scratch copies of `include/`, since the repo's own tree is never mutated;
- the build prints no warnings with `-DFLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON`, and ctest is green
  (36 of 36);
- `bench_hot_path --invariants` matches `benchmarks/expected_invariants.txt`, and no row is slower
  than `c3965ee` beyond run-to-run noise, timed on the same machine in alternating runs.

Commit: `fix: make FeedRegistry an IFlushable`.

### Step 2 — Release and close out

- Release v0.2.0 as v0.1.15 was released: the project version, the FetchContent tag, and the
  `find_package(flywheel_dag 0.1 …)` snippets in `CMakeLists.txt` and the README move to 0.2.0,
  v0.2.0 and `0.2`, and so does the README's note on what a request for `0.1` accepts. Then the
  tag, and a GitHub release with notes. The notes have a **Breaking** section:
  `addSource(FeedRegistry&)` is gone, and the snippet above is the migration.
- Mark this plan done.
- Push `main`, watch CI on both legs, and close flywheel-dag#27 with a summary comment.
- Stop before the push, the tag and the release for a go-ahead.

**Done when:** ctest is green (36 of 36), CI is green on the release commit on both legs, the
installed version file reports 0.2.0, the release is published, and flywheel-dag#27 is closed.

Commits: `build: release v0.2.0` and `docs: mark the plan done`.

## Not in this plan

- **A source registered twice** is flushed twice per cycle, and an `AsyncQueue` then drops its
  batch: [flywheel-dag#36](https://github.com/tomlin256/flywheel-dag/issues/36), found while
  planning this. It happens on `c3965ee` with no registry involved, and nesting adds one more way
  to reach it.
- **`add()` while the engine runs, from another thread.** The registry's comment now says it is not
  safe: `cycle()` iterates the member list with no lock. Before, a late member was never reached, so
  such a call raced nothing and did nothing. A lock would sit on the hot path of every cycle.
- **A registry that contains itself**, directly or through another, recurses without end in
  `flush()` and `setWakeHook()`. Nothing detects it. A check would catch the direct case only
  cheaply, and no caller has a reason to build it.

## Self-review — risks and assumptions

- **A source-breaking change, on purpose.** Any caller of `addSource(FeedRegistry&)` stops
  compiling. In this repo that is the six replay tests. The alternative, a deprecated overload,
  either keeps the bug for its callers or has the engine keep a reference to a registry it does not
  own. v0.2.0 carries the break, by the repo's own pre-1.0 rule.
- **The engine now keeps a promise the comments made.** `FeedRegistry`'s old comment said the hook
  reached "all current and future members", and the engine did not flush the future ones. The
  thread rule on `add()` is documented, not enforced: a call from another thread during `run()`
  races the flush. `Engine::addSource()` has the same rule over `sources_`, which no comment
  states.
- **One new test fails by hanging.** Nothing sleeps, and a pass takes no time, but a failure waits
  for ctest's 120 s timeout. It is the only test that sees the wake.
- **The numbers are one machine and one compiler.** Apple Clang, arm64. The registry's extra
  virtual call may cost more or less under GCC, and CI compares the benchmark's exact columns, never
  its timings. The new tests use neither pattern CLAUDE.md lists as GCC-only (an unbraced `if` over
  a gtest `EXPECT_`, a reference into a temporary), but CI's GCC leg is the first to build them
  there.
- **A throw out of a member's `flush()` behaves as before.** It leaves `cycle()`, and the members
  after it do not flush that cycle. They were inline in `sources_` and are now inside the registry,
  and the order and the stopping point are the same.
- **Pushing and releasing are assumed**, as in the earlier plans. If either is to be done by hand,
  step 2 stops before it.
