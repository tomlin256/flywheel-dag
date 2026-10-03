# Pin Every Window-Status Companion Eager

**Status: Approved (2026-10-03).** Step 1 has not started. Tests only: the engine, the companions
and their modes do not change. One comment in `dag_timeseries.inl` is reworded, because it names a
test this plan replaces.

Closes [flywheel-dag#33](https://github.com/tomlin256/flywheel-dag/issues/33).

## Problem

Every `IWindowed::windowStatusNode()` builds its companion `Eager`. The companion's functor reads
`filled()` off the captured node, and `filled()` advances while that node's value, the companion's
declared input, sits still. A `Lazy` companion would skip exactly then and stop following the
window. `TimeSeries.WindowStatusChangesWhileItsDeclaredInputDoesNot` is meant to pin this, and does
not:

- Its mode assertion reads the companion of `RollingStats` alone. `RollingSumNode`,
  `RollingMinMaxNode`, `WindowNode<T>` and `DelayNode<T>` build a companion too.
- Its behavioural half feeds `7.0 + 1e-12 * (i + 1)`. That moves the mean by about 1e-12 a push, a
  real change, so the companion's declared input moves and a `Lazy` companion recomputes as well.
  The sequence passes for either mode.

### What catches a `Lazy` companion today

A survey on scratch copies of `include/`, with the repo tree untouched: one companion flipped to
`InvalidationMode::Lazy` per copy, and the two suites that call `windowStatusNode()`
(`test_timeseries`, `test_window_status_integration`) built against it.

| Companion flipped to `Lazy` | What fails today |
|---|---|
| `RollingStats` | `TimeSeries.WindowStatusChangesWhileItsDeclaredInputDoesNot`, by its mode assertion alone |
| `RollingSumNode` | nothing |
| `RollingMinMaxNode` | nothing |
| `WindowNode<T>` | nothing |
| `DelayNode<T>` | `WindowStatusIntegration.DelayNodeStatusNodeFiresOncePerTick`, which expects 3 callbacks and sees 1. It catches this by accident: the delayed value holds its initial value while the window fills |

So the issue's "no test checks its mode" is right for `DelayNode`, but a regression there is caught.
Three of the five are caught by nothing.

### Which feed separates `Eager` from `Lazy`

The issue proposes one constant through an `AlwaysChangedPolicy` input, run for all five. A probe
tried it. Each node got its shipped companion and a `Lazy` twin built by hand with the same
functor, window 4. A cell is the companion's `filled` after the first push and after the fourth.

| Node | Constant 7.0, `Eager` / `Lazy` | Constant 0.0, `Eager` / `Lazy` |
|---|---|---|
| `RollingStats` | 1→4 / 1→1 | 1→4 / 1→1 |
| `RollingSumNode` | 1→4 / **1→4** | 1→4 / 1→1 |
| `RollingMinMaxNode` | 1→4 / 1→1 | 1→4 / 1→1 |
| `WindowNode<double>` | 1→4 / **1→4** | 1→4 / **1→4** |
| `DelayNode<double>` | 1→4 / 1→1 | 1→4 / 1→1 |

The bold cells are runs that do not separate. Two differences from the issue follow:

- **The constant is zero.** A running sum of 7.0 moves 7, 14, 21, 28 while the window fills, so a
  `Lazy` companion recomputes. Zero is the constant that leaves all four values where they were.
- **`WindowNode` gets the mode check alone.** Its policy is `AlwaysChangedPolicy`
  (`UnchangedStatefulOutput.WindowNodeStillFiresUnderAlwaysChangedPolicy` pins that), so it
  publishes a changed value on every evaluation, and `filled()` advances only on an evaluation.
  A `Lazy` companion therefore recomputes whenever `filled()` does, and no feed tells the two apart.
  A behavioural run for it would pass for `Lazy` and look like a pin. It gets none, and the test's
  comment says why.

## The change

In `tests/test_timeseries.cpp`, replace the one test with five, in the style of the
`UnchangedStatefulOutput` tests below it: a test per node over a shared helper.

| Test | Mode check | Behavioural run | Control |
|---|---|---|---|
| `WindowStatusCompanion.RollingStats` | yes | yes | yes |
| `WindowStatusCompanion.RollingSumNode` | yes | yes | yes |
| `WindowStatusCompanion.RollingMinMaxNode` | yes | yes | yes |
| `WindowStatusCompanion.DelayNode` | yes | yes | yes |
| `WindowStatusCompanion.WindowNode` | yes | no | no |

- **Mode check.** The companion's `invalidationMode()` is `Eager`, read through `NodeBase` as the
  current test does.
- **Behavioural run.** Four zeros, one per push, through an `AlwaysChangedPolicy` input, with the
  companion evaluated after each. The status after the first push has `filled` 1 and after the
  fourth `filled` 4, and `capacity` stays 4. A `Lazy` companion stays at 1.
- **Control.** The same run against a `Lazy` companion built by hand with the shipped functor
  (`{capacity(), filled()}` read off the captured node). It must fall behind, with `filled` 1 after
  the fourth push. Without it, a later change to a node's default equality policy could leave the
  run unable to tell the modes apart, and the shipped-companion checks would still pass. That is
  how this issue arose. `WindowNode` is the node whose control would fail, which is why it has none.
- **Helper hoist.** `alwaysChangedInput` moves above the new section, unchanged, so that both sections
  use it. Nothing else in the later section changes.
- **The note in `dag_timeseries.inl`.** Its last line says "Pinned by
  TimeSeries.WindowStatusChangesWhileItsDeclaredInputDoesNot". It names the new tests instead, says
  the mode of all five is pinned and the behaviour of all but `WindowNode`, and adds why
  `WindowNode`'s input never sits still. Comment only.

Sketch of the helper, as prototyped:

```cpp
template <typename In, typename Make>
void expectCompanionFollowsTheWindow(const Make& make) {
    {
        auto in   = alwaysChangedInput(0.0);
        auto node = make(in);
        const NodePtr status = node->windowStatusNode();
        expectEager(status);
        const StatusAt at = feedZeros(in, status);   // after push 1, after push kStatusWindow
        EXPECT_EQ(at.first.filled, 1u);
        EXPECT_EQ(at.last.filled, kStatusWindow) << "the status must follow the window as it fills";
        EXPECT_EQ(at.last.capacity, kStatusWindow);
    }
    {   // The control.
        auto in   = alwaysChangedInput(0.0);
        auto node = make(in);
        const StatusAt at = feedZeros(in, lazyCompanion<In>(node));
        EXPECT_EQ(at.last.filled, at.first.filled)
            << "a Lazy companion must fall behind on this run, or the run pins nothing";
    }
}
```

### Prototype

The new tests ran on scratch copies of `include/`, one per mutation. They built with
`-Wall -Wextra -Wpedantic -Werror` under Apple Clang. Unmutated, all 50 tests of `test_timeseries`
pass: today's 46, less the one replaced, plus five. Each mutation fails the one test for its node,
and nothing else:

| Companion flipped to `Lazy` | Failing test | Mode check | Behavioural run | Control |
|---|---|---|---|---|
| `RollingStats` | `WindowStatusCompanion.RollingStats` | fails | fails | passes |
| `RollingSumNode` | `WindowStatusCompanion.RollingSumNode` | fails | fails | passes |
| `RollingMinMaxNode` | `WindowStatusCompanion.RollingMinMaxNode` | fails | fails | passes |
| `WindowNode<T>` | `WindowStatusCompanion.WindowNode` | fails | none | none |
| `DelayNode<T>` | `WindowStatusCompanion.DelayNode` | fails | fails | passes |

For the four with a behavioural run, the behavioural assertion fails on its own message, so it pins
the mode without the mode check, which is what the issue found missing.

## Steps

Every commit subject is scoped to flywheel-dag#33 in the repo's `type(#N): …` form. The subjects
below omit the scope.

### Step 1 — The tests and the note

Tests written: `WindowStatusCompanion.RollingStats`, `.RollingSumNode`, `.RollingMinMaxNode`,
`.DelayNode` and `.WindowNode`. Removed: `TimeSeries.WindowStatusChangesWhileItsDeclaredInputDoesNot`,
which they replace. Moved, unchanged: `alwaysChangedInput`. Reworded: the note in
`dag_timeseries.inl`.

**Done when:**

- the build prints no warnings with `FLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON`, and ctest is green
  (36 of 36);
- the survey above, repeated on scratch copies of the tree as committed, gives the Prototype table:
  each of the five mutations fails its one test, the four controls pass, and the unmutated copy is
  green. The repo's own tree is never mutated, so there is nothing to restore and no stale object
  for Make 3.81's whole-second mtimes to keep;
- `DelayNodeStatusNodeFiresOncePerTick` still fails under the `DelayNode` mutation, as today.

Commit: `test: pin every windowStatusNode() companion Eager`.

### Step 2 — Close out

- Mark this plan done, with the survey's results.
- Push `main` and watch CI on both legs.
- Close flywheel-dag#33 with a summary comment that corrects the issue where the probes did:
  the constant is zero, `DelayNode`'s mode is already caught by a test that counts callbacks, and
  `WindowNode` has no behavioural run to give.

**Done when:** CI is green on the pushed commit on both legs and flywheel-dag#33 is closed.

Commit: `docs: mark the plan done`.

## Not in this plan

- **Any change to the companions, their modes or `WindowNode`'s policy.** The companions are
  `Eager` already, and the tests pin it.
- **`WindowStatusIntegration.DelayNodeStatusNodeFiresOncePerTick`.** It stays as it is. It pins a
  `DelayNode` regression by counting callbacks, a different claim from the new test's, and the new
  tests do not rely on it.
- **A release.** The change is tests and one comment, so no version bumps.

## Self-review — risks and assumptions

- **The zero run is exact arithmetic.** A sum of zeros, a Welford mean of zeros, a min and max of
  zeros and a delay of zeros are all exactly 0.0 on every platform, with or without FMA
  contraction. The run does not depend on rounding, and the 1e-12 nudge it replaces did.
- **The behavioural run assumes each node's default equality policy is exact.** `TypedEqualityPolicy`
  compares with `==`. If a node's default ever becomes `AlwaysChangedPolicy`, as `WindowNode`'s is,
  its control fails, with a message that says the run pins nothing. That is the signal to drop the
  behavioural run for that node, as this plan does for `WindowNode`.
- **The control tests the run, not the library.** Its twin duplicates the shipped functor's two
  reads. If `WindowStatus` gains a field, the twin and the shipped functors change together or the
  run stops comparing like with like. The shipped checks still compare the shipped companion.
- **`WindowNode` has one line of defence.** If its mode check is deleted, nothing catches a `Lazy`
  companion there. The test's comment says that, and the mode check is two lines.
- **GCC is seen only by CI.** The prototype built under Apple Clang. Per `CLAUDE.md`, the helpers
  keep an evaluated value in a `const WindowStatus` copy instead of a reference into a temporary
  (`-Wdangling-reference`), and every `if` is braced (`-Wdangling-else`). If CI's GCC leg warns, the
  fix goes in before step 2 closes the issue.
- **Renaming a test.** Nothing outside `dag_timeseries.inl` names the old test: ctest runs one
  executable per suite, `docs/` and the README do not mention it, and the plans do not either.
- **Pushing is assumed.** The earlier plans end with CI green on both legs, which needs a push.
  If the push is to be done by hand, step 2 stops after the plan is marked done.
