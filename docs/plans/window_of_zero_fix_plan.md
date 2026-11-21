# Four Windowed Nodes Reject a Window of 0 in `make()`

**Status: Approved (2026-11-21), as written, so D1 to D3 stand at their defaults.** The Progress table at the end
says where each step stands.

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
  `RollingStats` they own. The message then names the node the caller built and not `<name>.stats`, and a rejected
  call wires nothing to `upstream`.
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
  and `--invariants` is expected to match `benchmarks/expected_invariants.txt` unchanged.

## Steps

Every commit subject is scoped to flywheel-dag#80 in the repo's `type(#N): …` form. The subjects below omit the
scope. Every step after Step 0 ends with a full build and a full `ctest` run, both green, with `-Werror` (the
default) and the CI flags `-Werror=author -Werror=deprecated`.

### Step 0 — Approval

The plan is committed as Proposed. Nothing else lands until it is approved. A comment on flywheel-dag#80 links it.

**Done when:** `git status` is clean on that commit and flywheel-dag#80 carries the link.

Commit: `docs: add plan for rejecting a window of 0`.

### Step 1 — The checks, tests first

New tests in `tests/test_timeseries.cpp`, in a `WindowValidation` section after the alpha tests. They drive the
nodes with `Input<double>::set()` and `eval()`, so nothing waits.

| Test | Asserts |
|---|---|
| `RejectsAWindowOfZero` | `make()` throws `std::invalid_argument` for each of the four |
| `MessageNamesTheNodeTheCallerBuilt` | the message has the kind, the name and "window", and for `ZScoreNode` and `OutlierGateNode` it names that node and not `<name>.stats` |
| `AWindowOfOneIsAccepted` | each of the four built with 1 and evaluated three times: the mean and the sum are the latest sample, the z-score is 0, and the gate passes the sample |
| `AWindowOfZeroStillBuildsWhereItDid` | `RollingMinMaxNode`, `WindowNode<double>` and `DelayNode<double>` built with 0 and evaluated three times, with no throw (D2) |

They are run first against the headers as they are, then the four checks go in `dag_timeseries.inl`.

**Done when:** against the unfixed headers `RejectsAWindowOfZero` and `MessageNamesTheNodeTheCallerBuilt` fail,
because `make()` returns a node, and the other two pass; with the checks all four pass, and no other test
changes result.

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

Remove `&& window_ > 0` from `RollingSumNode::doCompute()`.

**Done when:** the disassembly before and after differs by what D3 expects, the invariants match, and the
`RollingSumNode` tests (`test_timeseries`, `test_aad_timeseries`, `test_dag_reduce_timeseries`) pass.

Commit: `refactor: RollingSumNode's eviction no longer guards a window of 0`.

### Step 4 — Close

The plan gets its Outcome (the red-first result, the disassembly, what differed from this plan) and its Status.
Push, with `git fetch` first. CI is looked at in passing and not waited for. flywheel-dag#80 is closed by hand
after a closing comment. The GCC leg, the sanitizers and `ubuntu-dev` are left for later, as optional, and the
Outcome says so.

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
  third argument, so the tests spell the window `std::size_t{0}`.
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
| 1 — The checks, tests first | Not started | |
| 2 — The comments | Not started | |
| 3 — The dead guard | Not started | |
| 4 — Close | Not started | |
