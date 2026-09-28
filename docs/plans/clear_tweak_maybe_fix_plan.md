# clearTweak(): Tell Consumers "Maybe", Not "Changed"

**Status: Done (2026-09-28).** Approved 2026-09-28. All three steps landed, and the change was
released as [v0.1.3](https://github.com/tomlin256/flywheel-dag/releases/tag/v0.1.3).

Closes [flywheel-dag#8](https://github.com/tomlin256/flywheel-dag/issues/8).

## Problem

`clearTweak()` tells every consumer that the node's value changed before the node knows whether it
has:

```cpp
tweaked_ = false;
markDirty();        // must recompute from inputs on next eval
notifyDownstream(); // tell downstream to re-pull
```

`notifyDownstream()` marks each consumer `Dirty`. A `Lazy` consumer therefore reruns its functor on
its next pull, even when the value the node recomputes from its inputs equals the value it was
frozen at. Nothing is computed wrongly, because over-invalidation is safe. The rerun is wasted
work, and the tri-state protocol exists precisely to avoid it.

## The fix

Replace the two lines with `invalidate()`. That is the treatment "an input of mine definitely
changed" gets everywhere else. Once `tweaked_` is false, `propagate()` no longer absorbs, so:

- **The node** goes `Dirty` and recomputes from its inputs on its next evaluation. This is also
  what a `Lazy` tweakable node needs: left `Maybe`, it would resolve against inputs that never
  called its `invalidate()`, skip, and keep the frozen value.
- **Its consumers** go `Maybe`, through the cascade. When the node's recomputed value differs, its
  `eval()` notifies them `Dirty` as any evaluation does. When it doesn't, a `Lazy` consumer skips.
- **If a changed tweak is still pending** (the node is already `Dirty`), `propagate()` returns
  without cascading. The tweak has already told the consumers `Dirty`, so they recompute, which is
  correct.

A probe drove an `Input` → `TweakableComputeNode` (`x * 10`) → consumer (`v + 1`):

| Scenario | v0.1.2 | fixed |
|---|---|---|
| Equal tweak, then `clearTweak()`: `Lazy` consumer reruns | 1 | 0 |
| The same through the engine, with the consumer registered: reruns / callbacks | 1 / 0 | 0 / 0 |
| `tweak(99)`, pull (100), `clearTweak()`: `Lazy` consumer reruns, value | 1, 21 | 1, 21 |
| A `Lazy` tweakable node after `tweak(99)` and `clearTweak()` | 20 | 20 |
| `tweak(99)` then `clearTweak()` with no pull between: reruns, value | 1, 21 | 1, 21 |
| Equal tweak, then `clearTweak()`: `Eager` consumer reruns | 1 | 1 |

Only the needless rerun goes. `Eager` consumers are unaffected, because an `Eager` node recomputes
whenever anything upstream fired. The node's own output is unaffected too: the node is `Dirty`
either way.

**Blast radius, measured on a scratch copy with the fix applied:** all 20 ctest entries that were
run pass. No existing test changes. (The consumer subproject was not run.)

## Steps

Every commit subject is scoped to flywheel-dag#8 in the repo's `type(#N): …` form. The subjects
below omit the scope.

### Step 1 — Regression tests, then the fix

All five tests go in `test_dag.cpp`, beside the other `TweakExampleTest` tests. The first two must
fail on v0.1.2 for the reason given. The other three pass on both versions: they guard what the fix
must not break.

| Test | Asserts | v0.1.2 |
|---|---|---|
| `TweakExampleTest.ClearingAnEqualTweakSkipsALazyConsumer` | An equal tweak, then `clearTweak()`: pulling a `Lazy` consumer does not run its functor, and its value is unchanged | reruns once |
| `TweakExampleTest.ClearingAnEqualTweakSkipsALazyOutputThroughTheEngine` | The same, driven by `Engine::step()` with the consumer registered: no rerun, no callback | reruns once |
| `TweakExampleTest.ClearingAChangedTweakStillReachesALazyConsumer` | `tweak(99)`, pull, `clearTweak()`, pull: the consumer reruns once and reads the recomputed value | passes |
| `TweakExampleTest.ClearingRecomputesALazyTweakableNode` | A `Lazy` tweakable node returns its recomputed value after `clearTweak()`, not the frozen one | passes |
| `TweakExampleTest.ClearingBeforeTheTweakIsPulledGivesTheRecomputedValue` | `tweak(99)` then `clearTweak()` with no pull between: the consumer reads the recomputed value | passes |

Then replace the two lines with `invalidate()`, with a comment saying why.

**Done when:**

- the first two tests fail on v0.1.2 and pass after the fix, and all five pass;
- the full build and ctest are green: 21 of 21;
- `quickstart` prints the same transitions as before.

Commit: `fix: clearTweak() tells consumers Maybe, not changed`.

### Step 2 — Docs

- `dag.hpp`:
  - The `ITweakable` comment and `clearTweak()`'s doc: clearing marks the node dirty and its
    consumers Maybe.
  - The `clearTweak()` row of the state-machine diagram.
- `CLAUDE.md`: the "Tweakable nodes" line.

**Done when:** build and ctest are green.

Commit: `docs: say what clearTweak() tells consumers`.

### Step 3 — Release v0.1.3

1. Set the version to 0.1.3 in `CMakeLists.txt` (`project()` and the header comment) and in the
   README.
2. Build and run ctest.
3. Commit `build: release v0.1.3`, push, and wait for CI to go green on Linux and macOS.
4. Tag `v0.1.3` (annotated) and push the tag.
5. Create the GitHub release.
6. Close flywheel-dag#8 with a summary comment.

**Done when:** the release exists, CI is green at the tag, and the issue is closed.

## Self-review — risks and assumptions

- **A skip needs a pure consumer.** A `Lazy` consumer can now skip after `clearTweak()`. That is
  sound only because `Lazy` is reserved for functors that are pure functions of their declared
  inputs, which is the existing contract, unchanged here.
- **A pending tweak.** `tweak(99)` followed by `clearTweak()` before any pull leaves the consumers
  `Dirty` from the tweak, so they rerun even when the net value is unchanged. The same happens when
  an `Input` goes A→B→A between pulls. It is conservative and correct. The fifth test guards the
  value, not the rerun count.
- **The node itself stays `Dirty`, never `Maybe`.** A `Lazy` tweakable node left `Maybe` would keep
  its frozen value. The fourth test guards this.
- **Assumption.** `clearTweak()` is called on the eval thread, or while the engine is idle, as
  today.

## Progress

| Step | Status | Notes |
|---|---|---|
| 1 — Tests, then the fix | Done | Both red tests failed on v0.1.2 with one extra consumer run each: 2 runs where 1 was due. The three guards passed. After the fix all five pass, ctest passes 21 / 21, and `quickstart` output is identical |
| 2 — Docs | Done | Updated the `ITweakable` comment, the `clearTweak()` doc and the state-machine row. The `CLAUDE.md` "Tweakable nodes" paragraph now covers both tweak fixes in order. ctest 21 / 21 |
| 3 — Release v0.1.3 | Done | `d181138` is tagged `v0.1.3`. CI run 36390147347 is green on Linux and macOS. The release is published, and flywheel-dag#8 is closed |
