# Hold a Source Once

**Status: Done (2026-10-08).** Approved 2026-10-07. Both steps landed, and v0.2.1 is released.
The engine and a registry ignore a source they already flush, so a source registered twice is
flushed once per cycle and an `AsyncQueue` keeps its batch. This design was chosen over a
flush-time one. Both were prototyped (see "The alternative"), and
[flywheel-dag#37](https://github.com/tomlin256/flywheel-dag/issues/37) holds the flush-time design.

Closes [flywheel-dag#36](https://github.com/tomlin256/flywheel-dag/issues/36).

## Problem

`Engine::addSource()` adds whatever it is given to `sources_`, and `cycle()` flushes each entry. A
source added twice is flushed twice per cycle. `flush()` drains, so the second call finds nothing
staged. For an `AsyncQueue` it then refreshes the cached value to the shared `[]` before any node
has read the batch the first call made. The output sees the same `[]` pointer it saw last cycle, no
callback fires, and the batch is gone.

The issue's sequence on `1f79d99` prints `calls=1 last.size=0`. With the source registered once it
is `calls=2` and `last={1, 2}`.

A queue is not the only source it hurts. `ReplayCoordinator::flush()` advances one group per call,
so a coordinator flushed twice advances two groups in a cycle. On the unfixed headers its
`currentSeq()` reads 2 after the first `step()`, not 1, and a source that flushes after both calls
never sees group 1. `IFlushable::setWakeHook()` says it is called once, so a source is meant to be
registered once, and nothing holds the engine to it.

The same source reaches the engine by more than one path. A scratch program built eleven wirings
(`E` is the engine, `R` a registry, and `X:Y` means `X.add(Y)`, which is `addSource()` for `E`) and
counted the flushes in one `step()`:

| Wiring | `1f79d99` | This fix |
|---|---|---|
| `E:S E:S` | 2 | 1 |
| `R:S R:S E:R` | 2 | 1 |
| `R:S E:R E:S` | 2 | 1 |
| `E:S R:S E:R` | 2 | **2** |
| `E:R R:S E:S` | 2 | 1 |
| `E:S E:R R:S` | 2 | **2** |
| `E:R1 E:R2 R1:S R2:S` | 2 | **2** |
| `R1:R2 R1:S R2:S E:R1` | 2 | **2** |
| `R1:R2 R2:S E:R1 E:S` | 2 | 1 |
| `R:S E:R E:R` | 2 | 1 |
| `R2:S R1:R2 R1:S E:R1` | 2 | 1 |

## The fix

The two places that take a source ignore one they already flush. `IFlushable` gains one virtual with
a default, so that they can see inside a registry:

```cpp
class IFlushable {
    // ...
    /// True when flushing this source also flushes `src`: this source itself or, for a source that
    /// flushes others, one of them.
    virtual bool includes(const IFlushable& src) const { return this == &src; }
};

inline bool FeedRegistry::includes(const IFlushable& src) const {
    if (this == &src) return true;
    for (const auto& inp : inputs_) if (inp->includes(src)) return true;
    return false;
}

inline void FeedRegistry::add(std::shared_ptr<IFlushable> input) {
    if (includes(*input)) return;
    // ... as it is
}

inline void Engine::addSource(std::shared_ptr<IFlushable> src) {
    for (const auto& s : sources_)
        if (s->includes(*src)) return;
    // ... as it is
}
```

- **A repeat is ignored, not thrown on.** The issue offers either. A repeat adds nothing: the source
  is flushed once per cycle already. A throw would break wiring that works today for a source whose
  second flush is harmless, such as two modules that each register a shared `AsyncInput`, and
  `addSource()` has no throw of its own.
- **The check sees through registries at any depth.** `R:S E:R E:S` and `E:R R:S E:S` both leave
  the engine holding the source once, whether the registry took it before `addSource()` or after.
  A group of your own overrides `includes()` to be seen through as a registry is; one that does
  not is a source like any other.
- **The wake hook goes in once.** An ignored repeat does not reach `setWakeHook()`. `AsyncInput`
  sets its hook with no lock, on the rule that it is set before any thread posts. A second call
  after a feed thread has started would write what that thread reads.
- **The source keeps its first place in the order.** `ReplayCoordinator` relies on the order, since
  it is added first so that its cursor moves before the sources it gates.
- **`cycle()` does not change,** so the flush loop costs what it did.
- **A registry added to itself is ignored,** as a result: it includes itself. `flush()` and
  `includes()` would otherwise recurse without end, the case
  [flywheel-dag#27's plan](feed_registry_iflushable_fix_plan.md) left undetected. A cycle through
  two registries is still not caught (see "Not in this plan").
- **Comments carry the contract.** Over `IFlushable`: the engine flushes a source once per cycle, why,
  and that `includes()` is how the engine and a registry hold it once. Over `FeedRegistry`: `add()`
  ignores what the registry includes, and the check sees only that registry. Over `addSource()`:
  the same for the engine, and that it is called between cycles on the thread that runs them, the
  rule `addOutput()` states and `addSource()` did not. The coverage list at the head of
  `test_dag_async.cpp` gains the new groups. `CLAUDE.md` gets a clause on the bullet that already
  warns about a redundant flush: the engine and a registry ignore a source they already include, so
  a source goes in one place.
- **It does not cover four wirings:** the ones marked 2 above. In each, the second holder cannot see
  the first. A registry cannot see the engine or another registry, and `addSource()` does not look
  inside a registry for sources the engine holds already. The comments say so, in the words "add a
  source to one place".
- **Source-compatible.** `includes()` has a default, so an `IFlushable` of your own compiles as it
  is. No signature changes, so this ships as v0.2.1.

## The alternative: dedupe at flush time

The registration check cannot close the four wirings, because it runs where the second holder cannot
see the first. The alternative runs where every path meets, in the flush. Each source remembers the
id of the last cycle it flushed in. The engine names each cycle with an id and flushes a source only
if it has not flushed under it, and a registry hands the id to its members. It closes all eleven
wirings, in any order, and makes `includes()` unnecessary.

It costs more:

- **The id must be unique across engines.** A cycle number alone fails: a source added to a second
  engine is skipped in that engine's first cycle when the first engine's last cycle had the same
  number. Two engines of one cycle each flushed the source once, not twice. The prototype gives each
  engine a base from a process-wide counter, in the high bits.
- **It puts state in the interface:** two private members on `IFlushable`, two `friend`s, and a
  private virtual that a registry overrides, so that a plain source takes no extra virtual call. A
  public virtual for the hand-over costs 6% on `idle-queues` (measured, then dropped).
- **A registry's members cost more to flush:** 245.4 ns/cycle against 238.1 for 32 idle queues
  behind one registry, +3.2% (+2.9% to +4.3% at each of eight code alignments). Queues added to the
  engine directly read +0.5%, and `bench_hot_path`'s rows are within the run-to-run noise of a
  build whose `cycle()` is unchanged.
- **It leaves a repeat in place.** `all()` lists it, `pendingCount()` counts it twice, and its hook is
  installed twice.

**The plan takes the registration check:** it costs the flush loop nothing, it is the fix the issue
proposes, and it covers the wirings an engine's own setup code is likely to produce.
[flywheel-dag#37](https://github.com/tomlin256/flywheel-dag/issues/37) records the four it leaves,
with the flush-time design in it.

## Tests

In `test_dag_async.cpp` and `test_dag_replay.cpp`, with a `CountedSource` stub: an `IFlushable`
that counts its flushes and the wake hooks it is given. Each test is deterministic: it calls the
registry or runs `Engine::step()`, and nothing sleeps. "Fails under" names the prototype change that
fails the test (see Prototype).

| Test | Asserts | Fails under |
|---|---|---|
| `IFlushable.IncludesItselfAndNoOtherSource` | a plain source includes itself and not another | the default returns false |
| `FeedRegistry.IncludesItselfAndItsMembersAtAnyDepth` | a registry includes itself, a member, a member registry and its member, and not a stranger or a sibling | `includes()` sees direct members only; omits the registry itself; the default returns false |
| `FeedRegistry.AMemberAddedTwiceIsHeldOnce` | `all().size()` is 1, one flush, one hook install | `add()` skips the check; installs the hook first; the default returns false |
| `FeedRegistry.AMemberOfAMemberRegistryIsNotAddedAgain` | a source inside a member registry is not added to the outer one | `add()` skips the check or compares the pointer only; `includes()` sees direct members only; the default returns false |
| `FeedRegistry.ARegistryAddedToItselfIsIgnored` | `all()` is empty after `reg->add(reg)`, checked before anything else is added or flushed | `add()` skips the check or compares the pointer only; `includes()` omits the registry itself |
| `EngineSources.ASourceAddedTwiceIsFlushedOncePerCycle` | two `step()`s give two flushes, and one hook install | `addSource()` skips the check or installs the hook first; the default returns false |
| `EngineSources.AnAsyncQueueAddedTwiceDeliversItsBatch` | the issue's sequence: two callbacks, the second with `{1, 2}` | `addSource()` skips the check; the default returns false |
| `EngineSources.ARepeatKeepsTheSourcesFirstPlaceInTheOrder` | `a, b, a` flushes `a`, `b` | `addSource()` skips the check or checks only the last source; the default returns false |
| `EngineSources.ASourceInsideARegistryIsNotAddedAgain` | `R:S E:R E:S`: one flush | `addSource()` skips the check or compares the pointer only; the default returns false |
| `EngineSources.ASourceAddedToARegistryAfterItsAddSourceIsNotAddedAgain` | `E:R R:S E:S`: one flush | `addSource()` skips the check or compares the pointer only; the default returns false |
| `EngineSources.ASourceInsideANestedRegistryIsNotAddedAgain` | `R1:R2 R2:S E:R1 E:S`: one flush | `addSource()` skips the check or compares the pointer only; `includes()` sees direct members only; the default returns false |
| `EngineSources.ARegistryAddedTwiceIsFlushedOnce` | `R:S E:R E:R`: one flush | `addSource()` skips the check; `includes()` omits the registry itself |
| `ReplayCoordinator.ACoordinatorAddedAgainAdvancesOneGroupPerCycle` | `R:coord E:R E:coord`: `currentSeq()` is 1 after the first `step()` and 2 after the second | `addSource()` skips the check or compares the pointer only; the default returns false |

No test is modified. The 88 tests in `test_dag_async` and the 14 in `test_dag_replay` pass untouched.

## Prototype

Built on scratch copies of the repo, with `-DFLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON`, under Apple Clang.
The repo tree is untouched.

- **Baseline.** On `1f79d99` the build prints no warnings and ctest is 36 of 36.
- **The bug.** The issue's sequence prints `calls=1 last.size=0`; with the fix, `calls=2 last.size=2`.
- **The wirings.** The table above, from the scratch program. The flush-time design reads 1 in all
  eleven.
- **Tests.** With the change the build prints no warnings, `test_dag_async` is 100 of 100, the 88 it
  has and 12 new, `test_dag_replay` is 15 of 15, and ctest is 36 of 36. Run against the unfixed
  headers, the 11 new tests that do not call `includes()` fail, 10 in `test_dag_async` and 1 in
  `test_dag_replay`. The other two test `includes()` itself and do not compile there.
- **Mutations.** One change per scratch copy of `include/`, against both test files. A control copy
  passes all 100 and 15. Each failure set is the "Fails under" column. The changes: `addSource()`
  skips the check (7 and 1 fail); `add()` skips it (3); `includes()` sees direct members only (3);
  omits the registry itself (3); the default returns false (10 and 1); `addSource()` installs the
  hook before the check (1); `add()` does (1); `addSource()` checks only the last source (1);
  `addSource()` compares the pointer only (3 and 1); `add()` does (2).
- **Hot path.** `bench_hot_path --invariants` matches `benchmarks/expected_invariants.txt`. 40 rounds
  of the benchmark, each running the builds in a random order. Medians in ns/cycle, `1f79d99`
  against the fix: chain 264.7 and 266.4 (+0.6%), idle-queues 234.9 and 234.9 (0.0%), ingest 121.0
  and 122.0 (+0.9%). `cycle()` does not change, so those are code layout. The flush-time design
  reads 265.0, 236.5 and 122.0.
- **The registry path,** which none of those rows uses: 32 idle `AsyncQueue`s behind one registry,
  15 shuffled rounds at each of eight code alignments. Median over alignments, `1f79d99` and the
  fix: 238.1 and 238.1 ns/cycle (+0.1%). One alignment of the fix read +6.4% with `cycle()`
  unchanged, so the noise bound for this tight loop is wider than for the rows. The flush-time design
  reads 245.4 (+3.2%). Directly registered queues: 237.1 and 237.6 (+0.1%), and 238.1 for the
  flush-time design (+0.5%). A row for the registry is not added to `bench_hot_path`.
- **Registration.** Each call scans what the engine or registry holds, so n calls cost O(n²):
  1,000 sources take 1.5 ms through `addSource()` where they took 0.03 ms, and 10,000 take 67 ms
  where they took 0.24 ms. A registry is similar (38 ms for 10,000). It runs once, at setup.

## Steps

Every commit subject is scoped to flywheel-dag#36 in the repo's `type(#N): …` form. The subjects
below omit the scope.

### Step 1 — The tests and the fix

Written: the 13 tests above. Changed: `dag_async.hpp` and `.inl`, `dag_engine.hpp` and `.inl`, the
coverage list at the head of `test_dag_async.cpp`, and the `CLAUDE.md` bullet.

**Done when:**

- the 13 tests fail under the changes given and pass with the fix. They are checked on scratch
  copies of `include/`, since the repo's own tree is never mutated;
- the build prints no warnings with `-DFLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON`, and ctest is green
  (36 of 36);
- `bench_hot_path --invariants` matches `benchmarks/expected_invariants.txt`, and no row is slower
  than `1f79d99` beyond run-to-run noise, timed on the same machine in alternating runs.

Commit: `fix: hold a source once`.

### Step 2 — Release and close out

- Release v0.2.1 as v0.1.15 was released: the project version and the FetchContent tag in
  `CMakeLists.txt` and the README move to 0.2.1 and v0.2.1. Then the tag, and a GitHub release with
  notes. There is no **Breaking** section: no signature changes.
- Mark this plan done.
- Push `main`, watch CI on both legs, and close flywheel-dag#36 with a summary comment.
- Stop before the push, the tag and the release for a go-ahead.

**Done when:** ctest is green (36 of 36), CI is green on the release commit on both legs, the
installed version file reports 0.2.1, the release is published, and flywheel-dag#36 is closed.

Commits: `build: release v0.2.1` and `docs: mark the plan done`.

## Outcome

Step 1 landed as one commit and step 2 as the release commit. CI run 37745040031 on the release
commit is green on both legs, `ubuntu-latest` with GCC and `macos-latest` with Apple Clang, with
`FLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON`, so the new tests raised no GCC-only warning. Both jobs passed
first time. Locally the build printed no warnings, ctest was 36 of 36, and the installed version
file reports 0.2.1. `v0.2.1` is tagged on the release commit and released, and flywheel-dag#36 is
closed.

The checks on scratch copies of `include/`, with the repo's own tree untouched:

- **The mutations,** rerun on the committed tree: the same results as the prototype's. The control
  copy passes all 100 `test_dag_async` and 15 `test_dag_replay` tests, and each change fails the
  tests the "Fails under" column names.
- **The hot path.** `bench_hot_path --invariants` matches `benchmarks/expected_invariants.txt`.
  Over 40 rounds in random order, medians in ns/cycle, `1f79d99` against the committed tree: chain
  259.4 and 260.0, idle-queues 232.0 and 231.6, ingest 115.9 and 115.3. The engine's headers are
  the same in `1f79d99` and v0.2.0, so those are v0.2.0's numbers too.

The comments over `FeedRegistry` and `addSource()`, and the `CLAUDE.md` clause, point at
flywheel-dag#37 for the four wirings the check misses.

## Not in this plan

- **The four wirings marked 2.**
  [flywheel-dag#37](https://github.com/tomlin256/flywheel-dag/issues/37), opened once this design
  was approved, holds the flush-time design. Until then the comments tell a caller to add a source
  to one place.
- **A cycle through two registries,** `R1:R2 R2:R1`. It recurses without end in `flush()` and
  `setWakeHook()`, as before, and `includes()` and so `add()` now recurse too, where `add()` did not.
  Refusing a member that includes the registry (`input->includes(*this)`) would stop the cycle
  forming. It is not here: the cycle has no use, and what to do with one, ignore or throw, is
  its own decision.
- **`add()` or `addSource()` while the engine runs, from another thread.** Unchanged: the rule is
  now stated for `addSource()`, and neither is locked.

## Self-review — risks and assumptions

- **A repeat is silent.** Nothing tells a caller that a source was already held. That is the cost
  of ignoring over throwing. An `addSource()` that returns whether it added the source is a cheap
  addition if callers should be told.
- **A known hole stays open.** Four wirings still lose a queue's batch. The recommendation rests on
  the registration check being what the issue asked for and free, and on the wirings it misses
  needing two containers that cannot see each other. Section "The alternative" has the price of
  closing them.
- **The order can surprise.** An ignored repeat keeps the first place, so a caller who added the
  source again to move it later gets no move. The order only matters between a coordinator and its
  sources, which already add the coordinator first.
- **A group of your own is not seen through** unless it overrides `includes()`. Its members can be
  flushed twice, as they could before.
- **The numbers are one machine and one compiler.** Apple Clang, arm64. `cycle()` is unchanged, so
  CI compares only the benchmark's exact columns, as before. The new tests use neither pattern
  CLAUDE.md lists as GCC-only (an unbraced `if` over a gtest `EXPECT_`, a reference into a
  temporary), but CI's GCC leg is the first to build them there.
- **Registration is O(n²).** Harmless at the sizes measured. An index would make it linear, and is
  not worth its code until a setup of tens of thousands of sources appears.
- **Pushing and releasing are assumed**, as in the earlier plans. If either is to be done by hand,
  step 2 stops before it.
