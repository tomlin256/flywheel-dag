# Flush a Source Once, However Many Registries Hold It

**Status: Proposed (2026-10-10), awaiting approval.** The engine flushes from a list it builds out of
the sources and registries it holds, with each source listed once. A source that two containers
hold without seeing each other is then flushed once per cycle, and an `AsyncQueue` keeps its batch.
This is not the design the issue sketches, a stamp on each source: that one costs the registry path
3.2%, and this one costs nothing measurable (see "The alternative").

Closes [flywheel-dag#37](https://github.com/tomlin256/flywheel-dag/issues/37).

## Problem

[flywheel-dag#36](https://github.com/tomlin256/flywheel-dag/issues/36) made `Engine::addSource()` and
`FeedRegistry::add()` ignore a source they already flush, directly or inside a registry at any depth
(v0.2.1). The check runs where a source is added, so it cannot see a holder the receiver does not
know about. In four wirings the second holder cannot see the first, and the source is flushed twice
per cycle. In the table `E` is the engine, `R` a registry, and `X:Y` means `X.add(Y)`, which is
`addSource()` for `E`. The last column is the flushes one `step()` gives the source on v0.2.1, from a
scratch program:

| Wiring | Why the check misses it | Flushes |
|---|---|---|
| `E:S R:S E:R` | `addSource(R)` does not look inside `R` for sources the engine holds already | 2 |
| `E:S E:R R:S` | `R` cannot see the engine | 2 |
| `E:R1 E:R2 R1:S R2:S` | `R2` cannot see `R1` | 2 |
| `R1:R2 R1:S R2:S E:R1` | `R2` cannot see `R1`, which holds it and the same source | 2 |

The other seven wirings [flywheel-dag#36's plan](source_held_once_fix_plan.md) tried read 1. A second
flush in a cycle finds nothing staged, and an `AsyncQueue` then rebinds its value to the shared `[]`
before any node reads the batch the first flush made: the output sees the `[]` it saw last cycle, no
callback fires, and the batch is gone. A `ReplayCoordinator` advances two groups in a cycle.

No check at registration can close these four. It can only turn away what is being added, and here
the repeat sits inside a registry that holds other members too, or behind a holder the receiver
cannot reach. The place every path meets is the flush.

## The fix

`Engine::cycle()` flushes from a list the engine builds, not from `sources_`. `IFlushable` gains one
virtual with a default, so that the engine can see inside a group:

```cpp
class IFlushable {
public:
    using Members = std::vector<std::shared_ptr<IFlushable>>;
    // ...
    /// The sources this one flushes, in the order it flushes them, when its flush() does no more than
    /// that. Null, the default, is a source that flushes no others.
    virtual const Members* members() const { return nullptr; }
};

inline const IFlushable::Members* FeedRegistry::members() const {
    return typeid(*this) == typeid(FeedRegistry) ? &inputs_ : nullptr;
}

// Engine, private:
std::vector<std::shared_ptr<IFlushable>> flushList_;   // sources_, each group replaced by its members
struct WatchedGroup {                                  // a group the list was built through
    std::shared_ptr<const IFlushable::Members> list;   //   its member list, kept alive by the group
    std::size_t                                size;   //   and the size it had then
};
std::vector<WatchedGroup> watched_;
bool                      flushListStale_ = false;     // addSource() sets it

// cycle(), where it flushed sources_:
if (flushListStale()) rebuildFlushList();
for (auto& s : flushList_) s->flush();
```

- **The list.** `sources_` stays the record of what was registered, and the registration check from
  flywheel-dag#36 still reads it. `flushList_` is derived from it by a walk in order: a source whose
  `members()` is null goes in the list, and a group is entered and its members walked in its place.
  A source is listed once, at the first place the walk reaches it, so a source held twice keeps its
  first place, as flywheel-dag#36 has it. A group reached again is not entered again, which also ends
  a walk through a ring of registries.
- **Rebuilt only when something changed.** `addSource()` marks the list stale. The walk records every
  group it enters with the size of its list, and `cycle()` compares those sizes before it flushes: a
  size read and a compare per group, none per source. If one differs the list is rebuilt, so a member
  added to a registry later, at any depth, is flushed from the next cycle in the registry's place
  (the contract of flywheel-dag#27). With no registry the check is an empty loop.
- **A group says so through `members()`.** The promise is that its `flush()` does no more than flush
  that list in order, that the list only grows, and that it stays at one address for the life of the
  group. A group of your own that keeps it overrides `members()` and is seen through like a
  registry. One that does not is flushed through its `flush()`, as before.
- **A class derived from `FeedRegistry` is not seen through by default.** It may override `flush()`,
  which the engine would skip if it flushed the members itself, so `FeedRegistry::members()` answers
  for an exact `FeedRegistry` only. The derived class is flushed through its `flush()`, and overrides
  `members()` to opt in.
- **The registration check stays.** `includes()`, `addSource()` and `add()` still ignore a source
  already held, which keeps `all()` and `pendingCount()` free of a plain repeat, and keeps the wake hook
  from going in twice for it. The list is what closes the four wirings the check cannot.
- **Nothing else changes.** `FeedRegistry::flush()` called on its own still flushes its members as the
  registry holds them. A source held by two registries has its wake hook installed once for each,
  with an equivalent engine hook each time. The `due_` pass and the output pass of `cycle()` are
  untouched.
- **Comments carry the contract.** Over `IFlushable`: `members()`, and that the engine flushes a source
  once per cycle however many groups hold it. Over `IFlushable::setWakeHook()`: called before any
  thread posts, once for each holder. Over `FeedRegistry`: the "add a source to one place" caveat
  goes, and a registry flushed on its own is said to flush as it holds. Over `addSource()`: the same
  for the engine. In `dag_engine.hpp`'s cycle paragraph and over step 1 of `cycle()`: the list. The
  coverage list at the head of `test_dag_async.cpp` gains the new groups. `CLAUDE.md` gets the bullet
  that says "add a source to one place" reworded: the engine flushes a source once per cycle however
  many registries hold it, and a group of your own overrides `members()` only if its `flush()` is a
  loop over that list.
- **Source-compatible.** `members()` has a default, so an `IFlushable` of your own compiles as it is.
  No signature changes, so this ships as v0.2.2, as `includes()` did in v0.2.1.

## The alternative: stamp each source

The issue sketches a flush-time design from flywheel-dag#36's prototype. Each source remembers the id
of the last cycle it flushed in. The engine names each cycle with an id and flushes a source only if
it has not flushed under it, and a registry hands the id to its members. It closes the four wirings
too, and costs more:

- **The registry path.** 32 idle `AsyncQueue`s behind one registry: 238.1 ns/cycle becomes 245.4 (+3.2%,
  and +2.9% to +4.3% at each of eight code alignments). Queues added to the engine directly: +0.5%. A
  public virtual for the hand-over cost 6% on `idle-queues` and was dropped for a private one. These
  are the earlier prototype's numbers, on this machine and compiler.
- **State in the interface.** Two private members on `IFlushable`, two `friend`s, and a private
  virtual that a registry overrides, so that a plain source takes no extra virtual call.
- **An id unique across engines.** A cycle number alone makes a source that moves to a second engine
  skip that engine's first cycle when the first engine's last cycle had the same number. The
  prototype gave each engine a base from a process-wide counter, in the high bits, which leaves an
  engine only the low bits to count its cycles in.

The flush list pays nothing because it does its work when a registry changes, not once per source
per cycle. Where the stamp is better: it does not poll for growth, and given an id of its own a
registry could dedupe a flush made outside an engine. Neither is a case flywheel-dag#37 raises.
**The plan takes the flush list.** It answers the question the issue ends on, whether closing the
four wirings is worth the cost: there is none to pay.

## Tests

In `test_dag_async.cpp` and `test_dag_replay.cpp`, with the stubs `CountedSource` and `LoggedSource`
that exist and two new ones: `ListGroup`, a group of your own that flushes its members in order, says
so through `members()` and counts the calls to it, and `CountingRegistry`, derived from
`FeedRegistry`, which counts its flushes. Each test is deterministic: it calls the registry or runs
`Engine::step()`, and nothing sleeps. The wiring tests step twice: one flush per cycle, not one flush
for ever. "Fails under" names the change to a scratch copy of `include/` that fails the test (see
Prototype). Tests 1 to 3 are step 1 and the rest step 2.

| # | Test | Asserts | Fails under |
|---|---|---|---|
| 1 | `IFlushable.ASourceHasNoMembers` | a plain source returns null | the default returns a list |
| 2 | `FeedRegistry.MembersAreTheListAllReturns` | `members()` is `&all()`, and holds a member once added | `members()` returns null |
| 3 | `FeedRegistry.AClassDerivedFromItHasNoMembers` | a registry derived from `FeedRegistry` returns null | `members()` has no exact-type check |
| 4 | `EngineSources.ASourceTheEngineHoldsIsNotFlushedAgainByARegistryAddedAfterIt` | `E:S R:S E:R`: 2 flushes over 2 cycles | the walk lists a source each time it reaches it; the walk ignores `members()`; `members()` returns null |
| 5 | `EngineSources.ASourceAddedToARegistryTheEngineHoldsIsFlushedOncePerCycle` | `E:S E:R R:S`: the same | the same three |
| 6 | `EngineSources.ASourceInTwoRegistriesIsFlushedOncePerCycle` | `E:R1 E:R2 R1:S R2:S`: the same | the same three |
| 7 | `EngineSources.ASourceInARegistryAndItsMemberRegistryIsFlushedOncePerCycle` | `R1:R2 R1:S R2:S E:R1`: the same | the same three |
| 8 | `EngineSources.AnAsyncQueueInTwoRegistriesDeliversItsBatch` | the issue's sequence: two callbacks, the second with `{1, 2}` | the same three |
| 9 | `EngineSources.ASourceHeldTwiceFlushesAtItsFirstPlaceInTheOrder` | `a, shared, b, c` from two registries that both hold `shared` | the same three; the walk keeps the last place |
| 10 | `EngineSources.AMemberAddedToARegistryAfterTheFirstCycleIsFlushedFromTheNext` | `before, late, after`: the late member flushes in the registry's place | `cycle()` does not look for growth; the rebuild does not clear the list |
| 11 | `EngineSources.AMemberAddedToANestedRegistryAfterTheFirstCycleIsFlushedFromTheNext` | a member added to an inner registry is flushed once | `cycle()` does not look for growth; the walk watches only the first group |
| 12 | `EngineSources.ASourceAddedAfterTheFirstCycleIsFlushedFromTheNext` | a source added by `addSource()` after a cycle is flushed once | `addSource()` does not mark the list stale |
| 13 | `EngineSources.ALateMemberAnotherRegistryHoldsIsFlushedOncePerCycle` | a source added late to a second registry stays at one flush per cycle | the three of tests 4 to 8; the rebuild does not clear the list |
| 14 | `EngineSources.AGroupThatReturnsItsMembersIsSeenThrough` | a `ListGroup` that holds a source the engine holds too: one flush per cycle | the walk lists a source each time it reaches it; the walk ignores `members()` |
| 15 | `EngineSources.TheEngineRebuildsItsFlushListOnlyWhenAGroupGrows` | `members()` is called 0 times by `addSource()`, once over three cycles, twice after one `add()` | the list is always stale; `cycle()` does not look for growth; the walk ignores `members()` |
| 16 | `EngineSources.ARegistryDerivedFromFeedRegistryIsFlushedThroughItsOwnFlush` | `CountingRegistry::flush()` runs once per cycle, and so does its member | `members()` has no exact-type check |
| 17 | `ReplayCoordinator.ACoordinatorInTwoRegistriesAdvancesOneGroupPerCycle` | `currentSeq()` is 1 after the first `step()` and 2 after the second | the same three as test 4 |

No test is modified. The 100 tests in `test_dag_async` and the 15 in `test_dag_replay` pass untouched.

## Prototype

Built on scratch copies of the repo (`git archive` of `45e0696`, v0.2.1, and the same with the
change), with `-DFLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON`, under Apple Clang on arm64. The repo tree is
untouched. The code is the sketch above, and the tests are the table.

- **The wirings.** The table above from the scratch program: v0.2.1 reads 2 in the four, and the
  change reads 1 in all eleven. Over two `step()`s it reads 2.
- **Tests.** With the change the build prints no warnings, `test_dag_async` is 116 of 116, the 100 it
  has and 16 new, `test_dag_replay` is 16 of 16, and ctest is 36 of 36. Against v0.2.1's headers the
  5 new tests that call `members()` do not compile; of the other 12, eight fail (seven in
  `test_dag_async` and test 17) and four pass, tests 10, 11, 12 and 16, which pin the new mechanism
  rather than the old defect.
- **Mutations.** One change per scratch copy of `include/`, against both test files. A control copy
  passes all 116 and 16. Each failure set is the "Fails under" column. The changes, with the new
  tests in `test_dag_async` and `test_dag_replay` that each fails: the walk lists a source each time
  it reaches it (8 and 1); the walk ignores `members()` (9 and 1); `FeedRegistry::members()`
  returns null (8 and 1); the walk keeps the last place (1); `cycle()` does not look for growth (3);
  the walk watches only the first group (1); the list is always stale (1); the rebuild does not
  clear the list (2); `FeedRegistry::members()` has no exact-type check (2); `addSource()` does not
  mark the list stale (1, and old tests fail with it, and a test that calls `run()` waits for ever,
  since no source is flushed); the default `members()` returns a list (test 1, and the engine
  tests, for the same reason).
- **Hot path.** `bench_hot_path --invariants` matches `benchmarks/expected_invariants.txt` for both
  builds. 30 rounds of the benchmark, each running the two builds in a random order. Medians in
  ns/cycle, v0.2.1 against the change: chain 264.5 and 262.1 (-0.9%), idle-queues 234.8 and 234.9
  (0.0%), ingest 117.5 and 116.5 (-0.9%). `bench_hot_path` carries about 1% code-layout noise.
- **The registry path,** which none of those rows uses: 32 idle `AsyncQueue`s behind one registry,
  15 shuffled rounds at each of eight code alignments. Median over alignments, v0.2.1 and the
  change: 234.35 and 233.45 ns/cycle (-0.4%, and -0.7% to +0.4% at the eight alignments). Directly
  registered queues: 233.65 and 233.38 (-0.1%). A cost would show at every alignment, and none does.
- **Rebuilding.** The first cycle after a registration walks the sources once. For 1,000 sources it
  takes 0.16 ms where v0.2.1's takes 0.03, and for 10,000 it takes 0.64 ms where v0.2.1's takes 0.14.
  A steady cycle takes the same (0.0172 ms for 1,000 and 0.074 ms for 10,000). Registration is the
  O(n^2) scan of flywheel-dag#36, unchanged: 10,000 sources take 71 ms behind a registry.

## Steps

Every commit subject is scoped to flywheel-dag#37 in the repo's `type(#N): …` form. The subjects
below omit the scope.

### Step 1 — `members()`

Written: tests 1 to 3. Changed: `dag_async.hpp` and `.inl` (`IFlushable::Members`, `members()`,
`FeedRegistry::members()`), and the coverage list at the head of `test_dag_async.cpp`. Nothing
calls `members()` yet.

**Done when:**

- the 3 tests fail under the changes given and pass with the code. They are checked on scratch copies
  of `include/`, since the repo's own tree is never mutated;
- the build prints no warnings with `-DFLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON`, and ctest is green
  (36 of 36).

Commit: `feat: expose a group's members`.

### Step 2 — The flush list

Written: tests 4 to 17. Changed: `dag_engine.hpp` and `.inl`, the comments named under "Comments carry
the contract" in `dag_async.hpp` and `dag_engine.hpp`, the coverage list, and the `CLAUDE.md`
bullet.

**Done when:**

- the 14 tests fail under the changes given and pass with the code, checked on scratch copies of
  `include/`, with a control copy that passes them all;
- the build prints no warnings with `-DFLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON`, and ctest is green
  (36 of 36);
- `bench_hot_path --invariants` matches `benchmarks/expected_invariants.txt`, and no row is slower
  than v0.2.1 beyond run-to-run noise, timed on the same machine in 40 alternating rounds, with the
  registry loop timed at eight code alignments.

Commit: `fix: flush a source once however many registries hold it`.

### Step 3 — Release and close out

- Release v0.2.2 as v0.2.1 was released: the project version and the FetchContent tag in
  `CMakeLists.txt` and the README move to 0.2.2 and v0.2.2. Then the tag, and a GitHub release with
  notes. There is no **Breaking** section: no signature changes.
- Mark this plan done, with the mutations and the timings rerun on the committed tree.
- Push `main`, watch CI on both legs, and close flywheel-dag#37 with a summary comment.
- Stop before the push, the tag and the release for a go-ahead.

**Done when:** the build prints no warnings with `-DFLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON` and ctest is
green (36 of 36), CI is green on the release commit on both legs, the installed version file reports
0.2.2, the release is published, and flywheel-dag#37 is closed.

Commits: `build: release v0.2.2` and `docs: mark the plan done`.

## Not in this plan

- **The wake hook goes in once for each holder.** A source in two registries has `setWakeHook()` called
  twice, with an equivalent engine hook. `AsyncInput` sets its hook with no lock, on the rule that it is
  set before any thread posts, so a second call after a feed thread has started would write what
  that thread reads. The wiring is setup code, which runs before the feeds start; the comment over
  `setWakeHook()` says "once for each holder". Skipping the second install would need the source to
  know it had a hook.
- **A flush made outside an engine.** `FeedRegistry::flush()` called on its own, as a `TickLoop` might,
  flushes each member as the registry holds it, so `R1:R2 R1:S R2:S` flushes `S` twice there. The
  stamp design could cover it with an id of its own; the flush list is the engine's.
- **A ring of registries,** `R1:R2 R2:R1`. The walk ends, since it enters a group once, but
  `setWakeHook()`, `includes()` and `FeedRegistry::flush()` recurse without end, as in
  flywheel-dag#36's plan, and `addSource()` reaches `setWakeHook()` first.
- **A row for the registry path in `bench_hot_path`.** Test 15 pins what an allocation column would:
  the list is not rebuilt in a cycle where nothing changed.
- **`add()` or `addSource()` while the engine runs, from another thread.** Unchanged: neither is
  locked, and both are for the thread that runs the cycles.

## Self-review — risks and assumptions

- **The engine now trusts a group's `members()`.** A group that returns a list and does more in its
  `flush()` would have the extra work skipped. The contract is in the comment, `FeedRegistry` guards
  its subclasses with the exact-type check, and test 16 pins that an override runs. A group of your
  own has to keep the promise by hand.
- **Growth is detected by size.** A group that replaces or reorders a member without changing its
  size, or moves its list, is not noticed. `FeedRegistry` only appends. The contract says "only
  grows, at one address", and the stamp design would not need it.
- **A derived registry loses the dedupe by default.** A class derived from `FeedRegistry` that adds
  only a method is flushed through its own `flush()`, which flushes its members as it holds them,
  so a source it shares with another holder is flushed twice, as in v0.2.1. It overrides `members()`
  to be seen through. That is the safe side of the choice: the other would skip an override of
  `flush()` without a word.
- **A rebuild allocates, in the one cycle that runs it.** 0.13 ms at 1,000 sources and 0.5 ms at
  10,000. A steady cycle allocates nothing, as `bench_hot_path`'s `allocs/cycle` column confirms for
  the rows it has. An application that adds a source on every cycle rebuilds on every cycle, and
  the cycle's timing statistics see it.
- **`cycle()` changes, slightly.** It iterates `flushList_` and compares group sizes first.
  flywheel-dag#36 could say `cycle()` was untouched; this cannot, and says what it costs instead.
- **A repeat is still silent,** as in flywheel-dag#36.
- **The flush list holds a `shared_ptr` to each source it flushes,** so the engine keeps a source
  alive as it did through `sources_` or the registry. A registry has no remove, so no source leaves.
- **The numbers are one machine and one compiler.** Apple Clang, arm64. The stamp design's numbers are
  the earlier prototype's, not re-measured here. The code-layout noise note applies: the registry
  loop was timed at eight alignments for that reason. CI's GCC leg is the first to build the new
  tests there; none uses the two patterns `CLAUDE.md` lists as GCC-only.
- **`typeid(*this)` needs RTTI,** which the engine already relies on (`discoverStatefulNodes()` casts
  with `dynamic_pointer_cast`).
- **Pushing and releasing are assumed,** as in the earlier plans. If either is to be done by hand,
  step 3 stops before it.
