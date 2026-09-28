# TweakableComputeNode: Keep the Cached Value on an Equal Tweak, and Deliver a Tweak to Its Own Output

**Status: In progress (2026-09-28).** Approved 2026-09-28. Step 1 is done.

Closes [flywheel-dag#5](https://github.com/tomlin256/flywheel-dag/issues/5).

## Problem

`TweakableComputeNode::tweak()` has two defects. Both concern what a registered engine output
sees.

1. **An equal tweak rebinds `cached_`.** The equal branch still assigns the new pointer, so
   `cached_` gets a new identity with the same value. After `clearTweak()`, the recomputed value
   is "equal", `eval()` returns the tweak's pointer, and the engine delivers an unchanged value
   again. This is the pattern fixed for stateful nodes in flywheel-dag#1: an "equal" verdict
   followed by a rebind.
2. **A tweak never reaches the node's own output callback.** `tweak()` calls `markClean()`, and
   `propagate()` absorbs every invalidation while the node is frozen. The node is therefore never
   dirty while tweaked, so `Engine::cycle` never evaluates it. Downstream nodes are notified and
   see the tweak; the node's own callback does not.

A probe drove an `Input` → `TweakableComputeNode` (`x * 10`) registered as an output, with
`Engine::step()`:

| Scenario | v0.1.1 | fixed |
|---|---|---|
| `eval()` pointer across `tweak(sameValue)` | a new pointer | the same pointer |
| `tweak(20)`, step, `clearTweak()`, step (20 already delivered) | 20 delivered again | nothing |
| `tweak(99)`, step; step; `clearTweak()`, step | nothing; nothing; 20 | 99; nothing; 20 |
| `tweak(99)` twice, then step | nothing | 99, once |
| `tweak(99)` then `tweak(77)`, then step | nothing | 77, once |
| the node and a consumer (`v + 1`) both registered: `tweak(99)`, step | consumer 100 only | node 99, consumer 100 |

## The fix

`tweak()` becomes:

- **Equal value:** freeze only. There is no rebind and no state change. A delivery still pending
  from an earlier tweak must survive, which is why this branch cannot mark the node clean.
- **New value:** rebind `cached_`, mark the node dirty without cascading, and notify downstream,
  as now.

The node stays dirty because the engine evaluates only dirty outputs. While tweaked, `eval()`
returns the frozen value and marks the node clean, so the engine delivers the tweak exactly once,
on its next cycle. Meanwhile the absorbing `propagate()` keeps upstream from touching the state.

This settles the issue's open question: deliver the tweak, rather than documenting that a tweak
never reaches the node's own output. The engine's contract is that an output's callback fires when
its value changes, and a tweak changes it.

What changes for a user:

- A tweaked output's callback fires once with the tweaked value, on the engine's next cycle.
  `tweak()` does not wake a running engine, so the delivery waits for the next cycle, as for any
  change made outside a source.
- An equal tweak or re-tweak changes nothing observable.
- `dirty()` can now be true while the node is tweaked, until it is evaluated. Before, it was
  always false.
- `clearTweak()`, `isTweaked()` and `tweakValue()` are unchanged.

**Blast radius, measured on a scratch copy with the fix applied.** One test fails:
`LazyInvalidation.TweakedNodeAbsorbsTheTransitiveCascadeNotJustTheDirectHop`, at
`EXPECT_FALSE(frozen->dirty())`. The test never evaluates `frozen` after its tweak, so the delivery
is still pending when it checks. Evaluating `frozen` after the tweak, as the engine would, makes it
pass. It still fails if `propagate()` stops absorbing, which is what it exists to catch. The other
19 ctest entries that were run pass, including the five `TweakExampleTest` tests. (The consumer
subproject was not run.)

## Steps

Every commit subject is scoped to flywheel-dag#5 in the repo's `type(#N): …` form. The subjects
below omit the scope.

### Step 1 — Regression tests, then the fix

Write the new tests first and run them against today's engine. Each must fail for the reason in
its "v0.1.1" column.

| Test | File | Asserts | v0.1.1 |
|---|---|---|---|
| `TweakExampleTest.EqualTweakKeepsTheCachedPointer` | `test_dag.cpp` | `eval()` returns the same `ValuePtr` before and after `tweak(sameValue)` | a new pointer |
| `TweakExampleTest.EqualTweakThenClearFiresNoExtraCallback` | `test_dag.cpp` | Engine: after the first delivery, `tweak(sameValue)`, step, `clearTweak()` and step deliver nothing more | delivered twice |
| `TweakExampleTest.TweakedOutputDeliversItsValueOnce` | `test_dag.cpp` | Engine: `tweak(99)` then step delivers 99 exactly once, the next step delivers nothing, and `clearTweak()` then step delivers the recomputed value | nothing delivered |
| `TweakExampleTest.EqualRetweakKeepsAPendingDelivery` | `test_dag.cpp` | Engine: `tweak(99)` twice, then step, delivers 99 exactly once | nothing delivered |
| `TweakExampleTest.TweakedOutputAndItsConsumerDeliverInOneCycle` | `test_dag.cpp` | Engine: with the node and a consumer both registered, one step after `tweak(99)` delivers 99 and 100 | consumer only |

One existing test changes its setup only:
`LazyInvalidation.TweakedNodeAbsorbsTheTransitiveCascadeNotJustTheDirectHop` evaluates `frozen`
after its tweak, so its `dirty()` assertion checks only the absorbing override. It passes on
v0.1.1 as well; the assertion is unchanged.

**Done when:**

- the five new tests fail on v0.1.1 for the stated reasons, and pass after the fix;
- the full build and ctest are green: 21 of 21;
- `quickstart` prints the same transitions as before.

Commit: `fix: keep an equal tweak's cached value, and deliver a tweak to its own output`.

### Step 2 — Docs

- `dag.hpp`:
  - The `ITweakable` comment and `tweak()`'s doc: a new tweak reaches the node's own output on the
    engine's next cycle, and an equal tweak changes nothing.
  - The `TweakableComputeNode` state-machine diagram, which says the state "is always Clean"
    while tweaked and that `tweak()` calls `markClean()`.
- `CLAUDE.md`: the "Tweakable nodes" line.

**Done when:** build and ctest are green.

Commit: `docs: say what a tweaked output delivers`.

### Step 3 — Release v0.1.2

1. Set `project(... VERSION 0.1.2)` in `CMakeLists.txt`. Change the `GIT_TAG` in its header
   comment and in the README to `v0.1.2`.
2. Build and run ctest.
3. Commit `build: release v0.1.2`, push, and wait for CI to go green on Linux and macOS.
4. Tag `v0.1.2` (annotated, like v0.1.1) and push the tag.
5. Create the GitHub release with notes on both fixes and the `dirty()` change.
6. Close flywheel-dag#5 with a summary comment.

A patch release, because it fixes behaviour and adds no API.

**Done when:** the release exists, CI is green at the tag, and the issue is closed.

## Out of scope

- **`clearTweak()` marks consumers Dirty rather than Maybe,** so a `Lazy` consumer reruns even when
  the unfrozen value equals the frozen one. This is safe but wasteful, and this change does not
  touch it: [flywheel-dag#8](https://github.com/tomlin256/flywheel-dag/issues/8).
- **Thread safety and waking.** `tweak()` is for the eval thread, or for an idle engine, as today.
  It still does not wake a running engine.

## Self-review — risks and assumptions

- **Behaviour change.** A tweaked output's callback now fires. A consumer that tweaks a node which
  is also a registered output gets one more callback per tweak. That callback is the point of the
  fix, and the release notes say so.
- **`dirty()` while tweaked.** It stays true until the node is evaluated. Only three places read
  it: the engine's output snapshot (intended), the `eval()` guard (the tweaked path returns
  first), and `skipRecompute`, which a frozen node never reaches.
- **An out-of-engine pull.** If application code evaluates the node between cycles, it is marked
  clean and the engine will not deliver the tweak. The same holds for any node evaluated outside
  the engine between cycles.
- **A→B→A across a cycle.** `tweak(99)` followed by `clearTweak()` before a cycle delivers the
  recomputed value, even when it equals the value last delivered. It carries a new pointer, and
  change detection is by pointer identity. `Input::set` behaves the same way.
- **Allocation.** `tweak()` still allocates through `make_value`. It is not on the steady-state
  eval path, so this is unchanged.
- **Assumption.** `tweak()` and `clearTweak()` are called on the eval thread, or while the engine
  is idle, as today.

## Progress

| Step | Status | Notes |
|---|---|---|
| 1 — Tests, then the fix | Done | All five new tests failed on v0.1.1 for the stated reasons. The equal tweak returned a new pointer, 20 was delivered twice, and `{}` arrived where `{99}` was due, three times. The adjusted absorbing test passes on both. After the fix, ctest passes 21 / 21 and `quickstart` output is identical. The comment on `eval()`'s tweaked branch no longer says the node is "always clean while tweaked" |
| 2 — Docs | Not started | |
| 3 — Release v0.1.2 | Not started | |
