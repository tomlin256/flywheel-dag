# Four Windowed Nodes Reject a Window of 0 in `make()`

**Status: Done (2026-11-21), not released.** Approved as written, so D1 and D2 stood at their defaults. D3 was
resolved as (a), the guard stays, after Step 3's measurement; the Outcome at the end records what the steps found.

Closes [flywheel-dag#80](https://github.com/tomlin256/flywheel-dag/issues/80).

## Problem

`RollingStats::make()`, `RollingSumNode::make()`, `ZScoreNode::make(name, upstream, window)` and
`OutlierGateNode::make()` accept a window of 0, and the node fails later with a ring buffer's
`std::logic_error`, which names neither the node nor the window. The last two build a `RollingStats` with the
window they are given.

Measured again at `5ce4042` with Apple clang 21: each node over an `Input<double>`, built with a window of 0 and
evaluated three times with a new sample each time (a throwaway harness in the scratchpad, not in the repo). The
issue's table holds:

| Node | With a window of 0 |
|---|---|
| `RollingStats` | the first evaluation throws "RingBuffer::front on an empty buffer" |
| `ZScoreNode`, `OutlierGateNode` | the same, through their `RollingStats` |
| `RollingSumNode` | the first evaluation works, and the second throws "RingBuffer::push_back on a full buffer (capacity 1) …" |
| `RollingMinMaxNode`, `WindowNode<double>`, `DelayNode<double>` | all three evaluate three times |

No call site in the tests, the examples, the benchmark or the README builds one of the four with 0.

## The fix

The four `make()` functions throw `std::invalid_argument` for a window of 0, as `EWMANode::make()` does for an
alpha outside (0, 1], and the message names the node's kind and its name:

```
RollingStats 'signal.stats': window must be at least 1
```

It is a throw and not an `assert()`, because the default build type defines `NDEBUG`.

- `ZScoreNode::make(name, upstream, window)` and `OutlierGateNode::make()` check before they build the
  `RollingStats` they own. The message then names the node the caller built and not `<name>.stats`.
- `ZScoreNode::make(name, upstream, stats)` takes a `RollingStats` that `RollingStats::make()` has already checked.
- No cost on a cycle: the check runs once, in `make()`.

## Decisions to confirm

My defaults, each one a place to edit before approving.

- **D1. `std::invalid_argument` from `make()`**, in `EWMANode`'s message form, `<Kind> '<name>': <what> …`.
- **D2. `RollingMinMaxNode`, `WindowNode` and `DelayNode` keep accepting 0.** The issue measured that they
  evaluate, and a delay of 0 is a use (it passes each sample through, with its own reduction step). A test pins
  that these three still build and evaluate, so the check cannot spread to them by accident.
- **D3. Drop the `&& window_ > 0` guard in `RollingSumNode::doCompute()`.** Only a window of 0 reached it, and a
  window of 0 no longer gets that far. It is on the hot path of the benchmark's `signal.drift` row, so it gets
  its own step and can be left out without touching the check. The measure is the machine code (a timing cannot
  show a removed branch from layout noise of about 1%): the function that holds `doCompute()` in
  `bench_hot_path`, disassembled before and after, is expected to lose one compare and branch and nothing else,
  and `--invariants` is expected to match `benchmarks/expected_invariants.txt` unchanged. **Resolved at Step 3 as
  (a): the guard stays, with a comment.**

## Steps

Every commit subject is scoped to flywheel-dag#80 in the repo's `type(#N): …` form. The subjects below omit the
scope. Every step after Step 0 ends with a full build and a full `ctest` run, both green, with `-Werror` (the
default) and the CI flags `-Werror=author -Werror=deprecated`.

### Step 0 — Approval

The plan is committed as Proposed. Nothing else lands until it is approved. A comment on flywheel-dag#80 links it.

**Done when:** `git status` is clean on that commit and flywheel-dag#80 carries the link.

Commit: `docs: add plan for rejecting a window of 0`.

### Step 1 — The checks, tests first

New tests in `tests/test_timeseries.cpp`, in a window-validation section after the alpha tests. They drive the
nodes with `Input<double>::set()` and `eval()`, so nothing waits.

| Test | Asserts |
|---|---|
| `WindowOfZero.RollingStatsIsRejected`, `RollingSumNodeIsRejected`, `ZScoreNodeIsRejected`, `OutlierGateNodeIsRejected` | `make()` throws `std::invalid_argument` for a window of 0, and the message starts with the node's kind and name (`ZScoreNode 'rejected': `, and not `RollingStats 'rejected.stats': `) and says "window must be at least 1" |
| `WindowOfZero.RollingMinMaxNodeWindowNodeAndDelayNodeEvaluate` | the three that keep accepting 0 build with it and evaluate three times with no throw, and the delay passes each sample through (D2) |
| `WindowOfOne.EachOfTheFourEvaluatesAtTheLatestSample` | each of the four built with 1 and evaluated three times: the mean and the sum are the latest sample, the z-score is 0, and the gate passes the sample |

They are run first against the headers as they are, then the four checks go in `dag_timeseries.inl`.

Each hand-made change below is made on a scratch copy of `include/`, with the repo's own `test_timeseries.cpp`
compiled against it and each new test run alone under a watchdog. A control with no change is run the same way.

| Change | Fails |
|---|---|
| the check removed from one of the four `make()` functions | that node's `…IsRejected` and no other. For `ZScoreNode` and `OutlierGateNode` it is the message that fails: `RollingStats` still throws, as `rejected.stats`, and a bare `EXPECT_THROW` would pass |
| a check that refuses a window of 1 too, in one of the four | `EachOfTheFourEvaluatesAtTheLatestSample` |
| a message without "window must be at least 1", in one of the four | that node's `…IsRejected` |
| a check for 0 added to `RollingMinMaxNode`, `WindowNode` or `DelayNode` | `RollingMinMaxNodeWindowNodeAndDelayNodeEvaluate` |

**Done when:** against the unfixed headers the four `…IsRejected` tests fail, because `make()` returns a node, and
the other two pass; with the checks all six pass and no other test changes result; each hand-made change fails
exactly the tests in the table, and the control none.

Commit: `fix: make() rejects a window of 0 for the four nodes that cannot hold one`.

### Step 2 — The comments

The comment at each declaration (`RollingStats`, `RollingSumNode`, `ZScoreNode`, `OutlierGateNode`) says the
window must be at least 1 and that `make()` throws. The design principles at the top of `dag_timeseries.hpp` gain
one bullet: which nodes refuse a window of 0 and which accept it. `CLAUDE.md` and `docs/engine_rules.md` do not
change: the header carries it.

**Done when:** each comment states what is true after Step 1, and `test_comment_refs` passes (it holds a comment
to citing no closed issue).

Commit: `docs: state the window rule where each node is declared`.

### Step 3 — The dead guard (D3)

As planned: remove `&& window_ > 0` from `RollingSumNode::doCompute()`.

**Done when (D3 as written):** the disassembly before and after differs by what D3 expects, the invariants match,
and the `RollingSumNode` tests (`test_timeseries`, `test_aad_timeseries`, `test_dag_reduce_timeseries`) pass.

**Stopped here, for a decision.** The disassembly does not differ by what D3 expects. `doCompute()` is inlined
into `StatefulNodeBase<RollingSumNode, …>::eval`, which was disassembled in `bench_hot_path` (Apple clang 21,
arm64) with the guard and without it, and the two listings compared with each branch target reduced to the index
of its instruction, so that a shifted address is not a difference. With `window_ > 0` gone the compiler can no
longer see that `buf.front()` is never called on an empty buffer, so it keeps `front()`'s emptiness check: a `cbz`
on the path every warm tick takes, and a cold block that calls `RingBuffer::throwEmpty`.

| Variant | Instructions in `eval` | The eviction test | Cold code | `--invariants` |
|---|---|---|---|---|
| the guard kept (the code as it is) | 204 | `cmp`, `ccmp`, `b.eq` | none | matches |
| the guard removed (D3 as written) | 209 | `cmp`, `b.ne`, `cbz` | a block that calls `RingBuffer::throwEmpty` | matches |
| `[[assume(window_ > 0)]];` in its place | 203 | `cmp`, `b.ne` | none | matches |

The third is D3's expected result, one compare fewer and nothing else (the only other difference is which of two
loads comes first). It tells the optimiser what `make()` now guarantees, and nothing outside the class can break
that, because the constructor is private. The second adds a branch to the hot path, and a measured cost there
counts however small. No timing is offered for any of them: the difference is one instruction, below the layout
noise of about 1% that `bench_hot_path` carries.

The choices are **(a)** keep the guard, with a comment that says why a check that always passes is there;
**(b)** replace it with `[[assume(window_ > 0)]];`; **(c)** remove it, as D3 was written. My recommendation is (a):
it changes nothing on the hot path and adds no assumption for the optimiser to hold the code to, and (b) saves
one instruction.

**Decided: (a).** The question went unanswered, so my recommendation was taken: it changes nothing on the hot
path, and it is the easiest of the three to reverse. A comment over the guard says why a check that always passes
is there. The disassembly of `eval` is identical to the one before the comment, with each branch target reduced
to the index of its instruction: the same 204 instructions and no difference. `--invariants` matches, the build
has no warning, and ctest passes 150 of 150. If (b) is wanted, it is one line and a new measurement, and the table
above is its baseline.

Commit: `docs: say why RollingSumNode keeps its window_ > 0 test`.

### Step 4 — Close

The plan gets its Outcome (the red-first result, the disassembly, what differed from this plan) and its Status.
Push, with `git fetch` first, once for the last two commits: Rob is out of CI compute, and each push starts runs.
CI is not consulted. flywheel-dag#80 is closed by hand after a closing comment. The GCC leg, the sanitizers and
`ubuntu-dev` are left for later, as optional, and the Outcome says so.

Commit: `docs: record the outcome`.

## Not in this plan

- **A window of `SIZE_MAX`** wraps the ring buffer's `window + 1` to 0 in six nodes (these four,
  `RollingMinMaxNode` and `DelayNode`), and they fail at the first evaluation with the same kind of message. Found
  while measuring for this plan, and measured at `5ce4042`. It is
  [flywheel-dag#83](https://github.com/tomlin256/flywheel-dag/issues/83) (`bug`, `nodes`, Hardening), because it
  also covers the two nodes that this plan leaves accepting 0.
- **A release.** This is a fix on `main` after v1.4.0, and a tag is a separate call.

## Self-review — risks and assumptions

- **A behaviour change, in one direction.** A program that built one of the four with 0 and never evaluated it
  ran before and throws now. One that evaluated it already threw (the second evaluation, for `RollingSumNode`).
  The full test run is the proof that nothing in the repo does this.
- **The overloads.** `ZScoreNode::make(name, upstream, window)` and `make(name, upstream, stats)` differ in the
  third argument, and a bare `0` can name either, so the tests pass the window as a `std::size_t`.
- **Two checks on the same window.** A `RollingStats` that `ZScoreNode` shares is already checked, and the one a
  `ZScoreNode` or an `OutlierGateNode` builds is never reached with 0. The checks are not redundant, because the
  first is what names the node the caller built.
- **The disassembly is an expectation.** If the guard's removal changes more than one compare and branch (the
  loop around it, an inlined call), Step 3 stops and reports it, and D3 goes back to Rob.
- **Assumptions:** `<stdexcept>` is already in scope in `dag_timeseries.inl`, where `EWMANode::make()` throws the
  same type; the build tree `build/relwithdebinfo-macos-arm64` is configured and current.

## Progress

| Step | Status | Notes |
|---|---|---|
| 0 — Approval | Done | Approved 2026-11-21 as written. flywheel-dag#80 links this file, and flywheel-dag#83 is the `SIZE_MAX` edge. |
| 1 — The checks, tests first | Done | `c6dd416`. The four rejection tests red on the unfixed headers, fifteen hand-made changes each failing exactly the tests predicted, ctest 150 of 150, no warnings. |
| 2 — The comments | Done | `039ef5a`. ctest 150 of 150. |
| 3 — The dead guard | Done | `1e69ea8` records the measurement, which was not what D3 expected, and `0a1b287` is the comment: D3 resolved as (a), the guard stays. |
| 4 — Close | Done | This commit, pushed together with the one before it. flywheel-dag#80 is closed by hand after the push, with a comment that links this file. |

## Outcome

Fixed on `main`, not released. flywheel-dag#80 is closed by hand after the push, with a comment that links this file.

| Commit | Step | What |
|---|---|---|
| `86c51a1`, `3552cd2`, `23f1ddd` | 0 | the plan, the approval, and the plan's commit subjects without a scope (below) |
| `c6dd416` | 1 | the four checks and their six tests |
| `039ef5a` | 2 | the comments at the declarations |
| `1e69ea8` | 3 | what the disassembly showed |
| `0a1b287` | 3 | the comment over the guard |
| this commit | 4 | this Outcome |

**The reproduction.** At `5ce4042`, built with Apple clang 21, a throwaway harness in the scratchpad gave the
table in "Problem" exactly: the first evaluation of `RollingStats`, `ZScoreNode` and `OutlierGateNode` throws, the
second of `RollingSumNode` throws, and the other three evaluate.

**Step 1, red first.** Against the unfixed headers the four `WindowOfZero.…IsRejected` tests failed with
"`<Kind>::make() accepted a window of 0`", and `RollingMinMaxNodeWindowNodeAndDelayNodeEvaluate` and
`WindowOfOne.EachOfTheFourEvaluatesAtTheLatestSample` passed. With the checks all six pass.

**The hand-made changes.** Each was made on a scratch copy of `include/`, with the repo's own `test_timeseries.cpp`
compiled against it and each of the six tests run alone under a 10 s watchdog. 16 objects were compiled, the
control and 15 changes, and every result was the one predicted:

| Change | Failed |
|---|---|
| none (the control) | none |
| the check removed from `RollingStats`, `RollingSumNode`, `ZScoreNode` or `OutlierGateNode` (four changes) | that node's `…IsRejected`, and no other test |
| a check that refuses a window of 1 too, in each of the four (four) | `EachOfTheFourEvaluatesAtTheLatestSample` |
| the message without "window must be at least 1", in each of the four (four) | that node's `…IsRejected` |
| a check for 0 added to `RollingMinMaxNode`, `WindowNode` or `DelayNode` (three) | `RollingMinMaxNodeWindowNodeAndDelayNodeEvaluate` |

With the check removed from `ZScoreNode` or `OutlierGateNode`, `make()` still throws, from the `RollingStats` it
builds, as `RollingStats 'rejected.stats': window must be at least 1`. Only the message check can fail there, and a
bare `EXPECT_THROW` would have passed.

**The gates, on Apple clang 21 and CMake 4.4.5 here.** Configure with `-Werror=author -Werror=deprecated` reported
no diagnostic, each build printed no warning, and ctest passed 150 of 150 after each of Steps 1, 2 and 3, with the
clang-tidy tests among them. `--invariants` matches `benchmarks/expected_invariants.txt`. CMake 4.5.0 is not among
them: it fails at Configure in CI (flywheel-dag#84).

**Step 3, D3.** Removing `&& window_ > 0` was not free: with it gone the compiler keeps `buf.front()`'s emptiness
check, so `eval` gains a `cbz` and a cold block (the table under Step 3). The plan's own stop condition held, and
the choice was put to Rob. It went unanswered, so (a) was taken, the option that changes nothing on the hot path.
The comment is the only change, and the disassembly of `eval` is identical to the one before it, branch targets
reduced to instruction indices.

**Where the work differs from the approved plan.** `c6dd416` brought the plan's Step 1 up to what was built.

- Six tests and not four. The approved plan had `RejectsAWindowOfZero` for all four nodes, a separate
  `MessageNamesTheNodeTheCallerBuilt`, `AWindowOfOneIsAccepted` and `AWindowOfZeroStillBuildsWhereItDid`. As built,
  each of the four nodes has its own rejection test that asserts both the throw and the message, and the other two
  are the window of 0 that stays accepted and the window of 1.
- The hand-made changes were added to Step 1 while it was built. The approved plan named tests only.
- The plan's sentence that a rejected call wires nothing to `upstream` was dropped. It holds, because each check is
  the first statement of its `make()`, ahead of anything built or wired, but none of the six tests looks at it.
- Step 3 stopped, and D3 was resolved by default as (a). Its commit is a comment and not the removal.
- The first push was refused by the pre-push hook, which rejects a bare issue reference in the tree, and the plan's
  commit subjects had them. They omit the scope now, as other plans in `docs/plans/` do (`23f1ddd`).

**Found outside the plan.**
[flywheel-dag#83](https://github.com/tomlin256/flywheel-dag/issues/83) (`bug`, `nodes`, Hardening): a window of
`SIZE_MAX` wraps the ring buffer's `window + 1` to 0 in six nodes.
[flywheel-dag#84](https://github.com/tomlin256/flywheel-dag/issues/84) (`bug`, `build`, Hardening): a CI job whose
runner has CMake 4.5.0 fails at Configure, on `non-target-directive`, under `-Werror=author`.

**CI.** Looked at in passing up to `1e69ea8`, then not at all, on Rob's instruction: he is out of CI compute. The
macOS leg of the run on `1e69ea8` finished green (Configure, Build, 98 of 98 tests, Benchmark) on a runner with
CMake 4.4.5. Every ubuntu job in the runs measured, and each macOS job that got CMake 4.5.0, failed at Configure
(flywheel-dag#84), so no GCC build has seen these changes. The two commits after `1e69ea8` are pushed together, and
their runs are not looked at.

**Left for later.** The new tests under GCC in `ubuntu-dev`, and the sanitizers: both optional, and neither run.
Each of flywheel-dag#83 and flywheel-dag#84 needs a plan of its own.
