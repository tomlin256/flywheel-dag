# Stateful Nodes: Keep the Cached Value When the Result Is Unchanged

**Status: In progress (2026-09-28).** Approved 2026-09-27. Steps 1 and 2 are done.

Closes [flywheel-dag#1](https://github.com/tomlin256/flywheel-dag/issues/1).

## Problem

`StatefulNodeBase::eval` publishes through `NodeImpl::notifyDownstream(newV, eq)`. That call
rebinds `cached_` only when the equality policy says the value changed. The next line then
rebinds it anyway:

```cpp
auto newV = slot_.emit(std::move(result));
this->notifyDownstream(newV, eq_);
this->cached_ = newV;          // unconditional
return this->cached_;
```

`ZScoreNode::eval` and `OutlierGateNode::eval` override `eval()` and repeat the line. Every other
node type rebinds only on a change: `ComputeNode`, `InPlaceComputeNode`, `MemoizedComputeNode`,
the `dag::ops` nodes, `Input` and `AsyncInput`.

The extra rebind has two effects.

1. **Spurious callbacks** (the issue as filed). `cached_` gets a new identity on every
   evaluation, and `Engine::cycle` detects change by `ValuePtr` identity. So a stateful node
   registered as an output fires its callback on every dirty cycle. A `ThresholdNode` that is
   never crossed fires 20 times in 20 cycles instead of none.
2. **A drift that never publishes** (found while planning). The policy compares the new value
   with the *previous evaluation's* value instead of the last value the node *published*. Under a
   tolerance policy, a value that moves less than the tolerance per step therefore never
   publishes, however far it moves in total. A probe used a pass-through `EWMANode` (alpha 1)
   under `EpsilonPolicy(0.6)`, with its input ramped from 0 to 3 in steps of 0.25:

   | | v0.1.0 | fixed |
   |---|---|---|
   | its `Lazy` consumer recomputes | once, and reads 0.00 throughout | at 0.75, 1.50, 2.25 and 3.00 |
   | the node's own output callback | every cycle (13) | on those 5 cycles |

   Every other node type compares against the last published value.

Under the default `TypedEqualityPolicy`, an "equal" verdict means equal values, so only the
first effect is possible. Under `AlwaysChangedPolicy` (`WindowNode`), every evaluation publishes,
so neither is.

## The fix

Delete the three unconditional rebinds. That leaves `notifyDownstream` as the only place `eval()`
rebinds `cached_`, as in `dag::ops::OpNodeImpl`. A comment at `StatefulNodeBase::eval` records why
the line must not come back.

What changes for a user:

- **Callbacks.** A stateful output's callback fires only when its value changes. The engine's
  contract in `dag_engine.hpp` and the README already say this. `Engine::callbacksFired()` falls
  to match.
- **`eval()` returns the last published value.** Under the default policy that equals the latest
  computed value. Under a tolerance or predicate policy it can trail the latest computed value by
  up to the tolerance, which every other node type already does. Accessors that read live state,
  such as `RollingStats::mean()`, are unaffected.
- **`LatchedDebounceNode`** delivers `nullopt` once after each transition and never between them.
  The docs say callbacks fire "only on actual true or false transitions", which is untrue both
  before and after the fix, so a callback must still ignore `nullopt`. The probe used
  `required = 2`, driven F T T T T F F F. v0.1.0 delivers `n n T n n F n n`; fixed, it delivers
  `n T n F n`.
- **Per-evaluation callbacks.** A consumer that wants a callback on every evaluation passes
  `AlwaysChangedPolicy`.

**Blast radius.** This was measured on a scratch copy with the three lines removed.

- One test fails: `ValueSlot.KnownBug_StatefulNodesFireCallbacksOnEveryDirtyCycle`. It pins the
  bug, and its own comment says it will fail once the bug is fixed.
- The other 19 ctest entries that were run pass, including the allocation tests. (The consumer
  subproject was not run.) `cached_` now holds one `ValueSlot` buffer across unchanged
  evaluations, which leaves the other free.
- `quickstart` prints identical output, because its callback already ignores `nullopt`.

## Steps

Every commit subject is scoped to flywheel-dag#1 in the repo's `type(#N): …` form. The subjects
below omit the scope.

### Step 1 — Regression tests, then the fix

Write the tests first and run them against today's engine. Each must fail for the reason in its
"v0.1.0" column before the fix goes in.

| Test | File | Asserts | v0.1.0 |
|---|---|---|---|
| `ValueSlot.StatefulNodeWithAnUnchangedValueFiresNoCallback` **replaces** `KnownBug_StatefulNodesFireCallbacksOnEveryDirtyCycle`, as that test's comment asks | `test_value_slot.cpp` | A never-crossed `ThresholdNode` output fires 0 extra callbacks in 20 dirty cycles, keeps the same cached pointer throughout, then fires exactly 1 on a real crossing | 20 extra |
| `StatefulNodeBase.UnchangedValueKeepsTheCachedPointer` | `test_stateful_node_base.cpp` | The toy `SumNode`, evaluated directly with no engine, returns the same `ValuePtr` across dirty evaluations while the sum is unchanged, and a new one when it changes | new pointer each time |
| `StatefulNodeBase.EqualityPolicyComparesAgainstTheLastPublishedValue` | `test_stateful_node_base.cpp` | `SumNode` under `EpsilonPolicy(0.6)`, fed +0.25 per step, publishes every third step, and a `Lazy` consumer recomputes on exactly those steps. `SumNode::make` gains an optional policy argument; this changes the test's toy node only | publishes once |
| `UnchangedStatefulOutput.<Node>`, one test per default-policy `dag::ts` node: `RollingStats`, `RollingSumNode`, `RollingMinMaxNode`, `EWMANode`, `EWMATickRateNode`, `DeltaNode`, `DelayNode`, `ThresholdNode`, `ZScoreNode`, `OutlierGateNode`, `RateLimiterNode`, `DebounceCountNode`, `LatchedDebounceNode` | `test_timeseries.cpp` | Registered as an engine output on an `AlwaysChangedPolicy` input, so that every cycle dirties it. It fires exactly 1 callback on the first cycle (the first delivery), then 0 in 20 dirty cycles once its value has settled | 20 each |
| `UnchangedStatefulOutput.WindowNodeStillFiresUnderAlwaysChangedPolicy` | `test_timeseries.cpp` | Control: `WindowNode`, whose policy is `AlwaysChangedPolicy`, still fires on all 20 | passes |
| `LatchedDebounceNodeTests.EngineCallbacksFireOnTransitionsAndOnTheReturnToNullopt` | `test_timeseries.cpp` | Through the engine, with `required = 2`, driven F T T T T F F F, the callbacks are exactly `nullopt, true, nullopt, false, nullopt` | `n n T n n F n n` |

Then delete the three rebinds.

**Done when:**

- every test in the table fails on today's engine for the stated reason, and passes after the fix;
- the full build and ctest are green: 21 of 21, including `test_consumer_subproject` and
  `example_quickstart`;
- `quickstart` prints the same transitions as before.

Commit: `fix: keep a stateful node's cached value when its result is unchanged`.

### Step 2 — Docs

- `CLAUDE.md`
  - `LatchedDebounceNode`: correct "callbacks only fire on actual true or false transitions" to
    the behaviour the Step 1 test pins. Fix the example's "should not fire but guard anyway"
    comment.
  - "What an equality policy can and cannot gate": add one sentence saying that every node
    compares a new value with the last one it *published*.
- `dag_timeseries.hpp`: make the same `nullopt` correction in the `LatchedDebounceNode` class
  comment.
- `dag.hpp`: add the published-value sentence to the `IEqualityPolicy` comment.
- `README.md`: no change. Its claims, such as "fires only when it changes", become true.

**Done when:** build and ctest are green (comments compile too).

Commit: `docs: say what a stateful output's callback delivers`.

### Step 3 — Release v0.1.1

1. Set `project(... VERSION 0.1.1)` in `CMakeLists.txt`. Change the `GIT_TAG` in its header comment
   and in the README to `v0.1.1`.
2. Build and run ctest.
3. Commit `build: release v0.1.1`, push, and wait for CI to go green on Linux and macOS.
4. Tag `v0.1.1` and push the tag.
5. Create the GitHub release. The notes say what is fixed, list the behaviour changes above, and
   drop v0.1.0's "known issue" line.
6. Close flywheel-dag#1 with a summary comment. The `type(#N):` subjects do not close it by
   keyword.

A patch release, because the change makes the implementation match the documented contract and
adds no API.

**Done when:** the release exists, CI is green at the tag, and the issue is closed.

## Out of scope

- **`TweakableComputeNode::tweak()`** has the same pattern. On an equal value it rebinds `cached_`,
  so `tweak(same)` followed by `clearTweak()` delivers an unchanged value again. Separately, a
  tweaked node registered as an output never delivers its tweaked value to its own callback. This
  fix affects neither:
  [flywheel-dag#5](https://github.com/tomlin256/flywheel-dag/issues/5).
- **Stale `RateLimiterNode` docs.** `dag.hpp`'s `NodeBase` comments and `CLAUDE.md`'s override
  table still say it overrides invalidation. It has had no override since the tri-state protocol:
  [flywheel-dag#6](https://github.com/tomlin256/flywheel-dag/issues/6).
- **Making `cached_` private to `NodeImpl`**, so that only `notifyDownstream` can rebind it. That
  would close the bug class structurally. But `RateLimiterNode::doRestoreState` seeds `cached_`,
  and application nodes derived from `StatefulNodeBase` may read it, so it is an API change rather
  than a bug fix.

## Self-review — risks and assumptions

- **Reliance on per-cycle callbacks.** A consumer may count stateful-output callbacks as a
  heartbeat; after the fix they fire only on change. The release notes say so, and
  `AlwaysChangedPolicy` restores delivery on every evaluation.
- **Tolerance policies now publish on cumulative drift.** This changes behaviour for any stateful
  node under `EpsilonPolicy` or `PredicateEqualityPolicy`. It is the correct change, because it
  matches every other node type. `eval()` returns the anchored value, not the latest one.
- **Signed zero.** `+0.0 == -0.0`, so a result that only flips the sign of zero no longer
  publishes. `ComputeNode` already behaves this way.
- **First delivery.** `cached_` is null before the first evaluation, and every built-in policy
  reports null-vs-value as changed, so the first evaluation always publishes. The per-node tests
  pin this: exactly one callback on the first cycle. A user `PredicateEqualityPolicy` that calls
  null "equal" would leave `cached_` null. That already applies to every node type and is not
  introduced here.
- **Allocation.** The existing allocation tests cover it. They pass unchanged on the scratch copy.
- **Restore.** `RateLimiterNode::doRestoreState` seeds `cached_`, so its first evaluation after a
  restore compares against the restored value. The fix does not change that.
- **Assumption.** `ZScoreNode::eval` and `OutlierGateNode::eval` differ from the base's `eval()`
  only in how they compute the value, so the same deletion is right for both. Their per-node tests
  cover it.

## Progress

| Step | Status | Notes |
|---|---|---|
| 1 — Tests, then the fix | Done | All 17 new tests failed on v0.1.0 for the stated reason. The 13 per-node outputs each fired on all 20 steady cycles, with first delivery correct. The `ThresholdNode` output fired 21 callbacks where 1 was due, on a new pointer. `SumNode` returned a new pointer every evaluation. The drift published once, and `eval()` read 3 rather than 2.5. The latch delivered `nnTnnFnn`. The `WindowNode` control passed. After deleting the three rebinds: ctest 21 / 21, and `quickstart` output is identical. The plan said 20; the 21st is `example_quickstart` |
| 2 — Docs | Done | `CLAUDE.md`, `dag.hpp` and `dag_timeseries.hpp` as listed; the README is unchanged. ctest 21 / 21 |
| 3 — Release v0.1.1 | Not started | |
