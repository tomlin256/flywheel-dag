# Windowed Nodes and the Engine's Cycle Window Reject a Window of `SIZE_MAX`

**Status: Proposed (2026-11-22).** Nothing past Step 0 lands until it is approved.

Closes [flywheel-dag#83](https://github.com/tomlin256/flywheel-dag/issues/83).

## Problem

`RollingStats`, `RollingSumNode` and `RollingMinMaxNode` size their ring buffers at `window + 1`, and `DelayNode` at
`delay + 1`. `ZScoreNode` and `OutlierGateNode` build a `RollingStats` with the window they are given. A window of
`SIZE_MAX` wraps that size to 0: `make()` accepts it, and the first evaluation throws the ring buffer's
`std::logic_error`, which names neither the node nor the window.

The engine has a seventh. `async::RollingCycleWindow`, which `Engine`'s `rollingWindow` argument sizes, holds the same
`window + 1` buffer. `Engine(SIZE_MAX)` builds, and its first cycle ends the process: the destructor that times the
cycle (`TimeOnExit`, in `Engine::cycle()`) calls `record()`, `record()` throws on the full buffer, and a throw from a
destructor calls `std::terminate`. The `NOLINT(bugprone-exception-escape)` over that destructor says `record()` cannot
throw, which holds for every window but this one.

Measured at `bec45b2` with Apple clang 21 (libc++) and, in `ubuntu-dev`, GCC 13.3 (libstdc++), with a throwaway
harness in the scratchpad: each node over an `Input<double>`, built with the window and evaluated three times with a
new sample each time; `RollingCycleWindow` and `Engine` in a child process. The two libraries agree:

| Built with | Result |
|---|---|
| the six nodes, `SIZE_MAX` | `make()` returns, and the first evaluation throws "RingBuffer::push_back on a full buffer (capacity 0) — …" |
| `WindowNode<double>`, `SIZE_MAX` | builds and evaluates three times |
| `RollingCycleWindow`, `SIZE_MAX` | builds, and the first `record()` throws the same message |
| `Engine`, `SIZE_MAX`, then one `step()` | builds, and the step ends the process with `SIGABRT`: the same exception, uncaught, terminates |
| the six nodes, `RollingCycleWindow` and `Engine`, `SIZE_MAX − 1` | the constructor throws `std::length_error` (libc++ "vector", libstdc++ "cannot create std::vector larger than max_size()"), and nothing is built |
| the six nodes, a window of `std::vector<double>::max_size()` (2⁶¹ − 1 on libc++, 2⁶⁰ − 1 on libstdc++) | the same `std::length_error` |
| the six nodes, one less | `std::bad_alloc` from the constructor (`std::length_error` for `RollingMinMaxNode`, whose entries are 16 bytes) |

So `SIZE_MAX` is the one window that is accepted and then fails. Every smaller window too large to hold is refused in
the constructor, by the buffer's allocation, before anything is built. No call site in the tests, the examples, the
benchmark or the README uses `SIZE_MAX` as a window.

## The fix

The six `make()` functions and `RollingCycleWindow`'s constructor throw `std::invalid_argument` for a window, or a
delay, of `SIZE_MAX`, beside the check for 0 where there is one and in its message form:

```
RollingStats 'signal.stats': window must be less than SIZE_MAX
DelayNode 'lagged': delay must be less than SIZE_MAX
RollingCycleWindow: window must be < SIZE_MAX
```

- `ZScoreNode::make(name, upstream, window)` and `OutlierGateNode::make()` check before they build the `RollingStats`
  they own, as they do for 0, so the message names the node the caller built and not `<name>.stats`.
- `Engine(SIZE_MAX)` throws from its constructor, through `RollingCycleWindow`'s, and never runs a cycle.
- `WindowNode` does not change. It keeps a `std::deque` with no spare slot, and `SIZE_MAX` is a window it never fills.
- Nothing changes on a cycle: each check runs once, at construction. `dag_timeseries.hpp` includes `<limits>` for
  `std::numeric_limits` (`dag_engine.hpp` already does).

## Decisions to confirm

My defaults, each one a place to edit before approving.

- **D1. `SIZE_MAX` alone.** It is the one window whose buffer size wraps, and the one a node accepts and then fails on.
  A smaller window too large to hold is refused by the allocation, at construction (the table above), so after the fix
  no window is accepted that fails later. Refusing by name every window a `std::vector` cannot hold would turn that
  `std::length_error` into this message, but at a bound that differs by standard library, and with `std::bad_alloc`
  still thrown just below it.
- **D2. `RollingCycleWindow` is fixed here.** It is not one of the issue's six, but it has the same `+ 1`, takes the
  same check, and fails the worst way. The alternative is an issue of its own. A comment on flywheel-dag#83 says that
  the plan covers it, and the issue gains the `engine` label once the plan is approved.
- **D3. `WindowNode` keeps accepting `SIZE_MAX`,** as the issue measured, and a test pins it, so the check cannot
  spread to it by accident.

## Steps

Every commit subject is scoped to flywheel-dag#83 in the repo's `type(#N): …` form; the subjects below omit the scope.
Every step after Step 0 ends with a full build and a full `ctest` run of `relwithdebinfo-macos-arm64`, both green, with
`-Werror` (the default), the CI flags `-Werror=author -Werror=deprecated`, and the tree's clang-tidy tests on.

### Step 0 — Approval

The plan is committed as Proposed, and is not pushed until it is approved. A comment on flywheel-dag#83 links it.

**Done when:** `git status` is clean on that commit and flywheel-dag#83 carries the link.

Commit: `docs: add plan for rejecting a window of SIZE_MAX`.

### Step 1 — The checks, tests first

In `tests/test_timeseries.cpp`, a section after `WindowOfOne`:

| Test | Asserts |
|---|---|
| `WindowOfSizeMax.RollingStatsIsRejected`, `RollingSumNodeIsRejected`, `RollingMinMaxNodeIsRejected`, `DelayNodeIsRejected`, `ZScoreNodeIsRejected`, `OutlierGateNodeIsRejected` | `make()` throws `std::invalid_argument` for `SIZE_MAX`, and the message starts with the node's kind and name (`ZScoreNode 'rejected': `, and not `RollingStats 'rejected.stats': `) and says "window must be less than SIZE_MAX" ("delay …" for `DelayNode`) |
| `WindowOfSizeMax.WindowNodeHoldsEverySample` | `WindowNode` built with `SIZE_MAX` evaluates three times, holds the three samples, and is not `full()` (D3) |
| `WindowOneBelowSizeMax.EachOfTheSixIsRefusedByItsBuffer` | `make()` throws `std::length_error` for `SIZE_MAX − 1`, the allocation's refusal of a buffer of `SIZE_MAX` |

In `tests/test_dag_async.cpp`, beside `RollingCycleWindow.ZeroWindowThrows` and in `EngineCycleStats`:

| Test | Asserts |
|---|---|
| `RollingCycleWindow.SizeMaxWindowThrows` | the constructor throws `std::invalid_argument` for `SIZE_MAX`, and the message says "window must be < SIZE_MAX" |
| `RollingCycleWindow.WindowOneBelowSizeMaxIsRefusedByItsBuffer` | the constructor throws `std::length_error` for `SIZE_MAX − 1` |
| `EngineCycleStats.SizeMaxRollingWindowIsRefusedAtConstruction` | `Engine{SIZE_MAX}` throws `std::invalid_argument` |

The two `SIZE_MAX − 1` tests pin what D1 rests on: the window below the one refused by name is refused at
construction too. They also catch a buffer two slots larger than the window, which would wrap at `SIZE_MAX − 1`.

The tests run first against the headers as they are. Then the checks go in, each with a comment that says why.

Hand-made changes, each on a scratch copy of `include/` with the repo's own test `.cpp` compiled against it (both test
files for a change to `dag_engine.inl`, which `test_timeseries.cpp` also includes), and each of the eleven new tests
run alone under a 10 s watchdog, with a control that changes nothing:

| Change | Fails |
|---|---|
| the `SIZE_MAX` check removed from one of the six `make()` functions (six changes) | that node's `…IsRejected`, and no other test. For `ZScoreNode` and `OutlierGateNode` it is the message that fails: the `RollingStats` each builds still throws, as `rejected.stats` |
| the `SIZE_MAX` check removed from `RollingCycleWindow` | `SizeMaxWindowThrows` and `SizeMaxRollingWindowIsRefusedAtConstruction` |
| `RollingSumNode`'s message without "less than SIZE_MAX" | `RollingSumNodeIsRejected` |
| a check that refuses `SIZE_MAX − 1` by name too, in `RollingStats` | `EachOfTheSixIsRefusedByItsBuffer` |
| the same, in `RollingCycleWindow` | `WindowOneBelowSizeMaxIsRefusedByItsBuffer` |
| a check for `SIZE_MAX` in `WindowNode::make()` | `WindowNodeHoldsEverySample` |
| `RollingStats`' buffer sized at `window + 2` | `EachOfTheSixIsRefusedByItsBuffer`: `SIZE_MAX − 1` wraps to 0 and is accepted |

**Done when:** against the unfixed headers the six `…IsRejected` tests, `SizeMaxWindowThrows` and
`SizeMaxRollingWindowIsRefusedAtConstruction` fail because the call returns, and the other three pass; with the checks
all eleven pass and no other test changes its result; each hand-made change fails exactly the tests in its row, and the
control none; `bench_hot_path --invariants` matches `benchmarks/expected_invariants.txt`.

Commit: `fix: refuse a window of SIZE_MAX, whose buffer size wraps to 0`.

### Step 2 — The comments

- The comment at each declaration says the window (the delay, for `DelayNode`) is less than `SIZE_MAX`, and that
  `make()`, or the constructor, throws for it: `RollingStats`, `RollingSumNode`, `RollingMinMaxNode`,
  `ZScoreNode::make(…, window, …)`, `OutlierGateNode`, `DelayNode`, `RollingCycleWindow`, and `Engine`'s constructor,
  which does not yet say that it throws for 0 either.
- The design-principles bullet at the top of `dag_timeseries.hpp` names the nodes that refuse `SIZE_MAX`, and says
  that `WindowNode` accepts it and that a smaller window too large to hold throws from the allocation, in `make()`.
- The `NOLINT` comment over `TimeOnExit`'s destructor, and the comment over
  `RollingCycleWindow.RecordNeverThrowsHoweverManyAreRecorded`, say that the constructor's refusal of `SIZE_MAX` is
  what makes "a slot more than the window" hold.
- `CLAUDE.md` and `docs/engine_rules.md` do not change: the headers carry it.

**Done when:** each comment states what is true after Step 1, and `test_comment_refs` passes.

Commit: `docs: state the SIZE_MAX rule where each window is declared`.

### Step 3 — Close

The plan gets its Outcome and its Status, and is pushed with Steps 1 and 2: once, `git fetch` first, never forced. CI
is not waited for, and a run is looked at only once it has finished. After the push, the GCC 13.3 build and `ctest` in
`ubuntu-dev`; GCC 15 and the sanitizers are CI's. flywheel-dag#83 is closed by hand after a closing comment that says
what was seen.

Commit: `docs: record the outcome`.

## Not in this plan

- **A release.** This is a fix on `main` after v1.4.0, and a tag is a separate call.

## Self-review — risks and assumptions

- **A behaviour change, in one direction.** A program that built one of the six with `SIZE_MAX` and never evaluated
  it, or an `Engine` with `SIZE_MAX` and never stepped it, ran before and throws now. One that evaluated or stepped
  already failed.
- **The `SIZE_MAX − 1` tests rest on the standard library.** libc++ and libstdc++ throw `std::length_error` for a
  vector larger than `max_size()` (measured on both). If a library threw something else, the test would say so.
- **No test allocates.** Both libraries check the size before they allocate, so the `SIZE_MAX − 1` tests ask no
  allocator for memory, and AddressSanitizer, which aborts on an allocation larger than it supports, never sees one.
  CI's sanitizer job runs them under GCC 15.
- **No test steps an `Engine` built with `SIZE_MAX`.** Against the unfixed headers,
  `SizeMaxRollingWindowIsRefusedAtConstruction` builds one and destroys it without a cycle, so it fails and does not
  end the test binary.
- **The overloads.** `ZScoreNode::make(name, upstream, window)` and `make(name, upstream, stats)` differ in the third
  argument, and the tests pass the window as a `std::size_t`.
- **The order in `RollingCycleWindow`.** The check is in the body, after `buf_(window + 1)` has run. For `SIZE_MAX`
  that is a buffer of capacity 0, which allocates nothing.
- **The message for `RollingCycleWindow`** names that class and not `Engine`, as its message for 0 does.
- **Assumptions:** `build/relwithdebinfo-macos-arm64` is configured and current, with clang-tidy on; `ubuntu-dev` is
  up.

## Progress

| Step | Status | Notes |
|---|---|---|
| 0 — Approval | In review | |
| 1 — The checks, tests first | Not started | |
| 2 — The comments | Not started | |
| 3 — Close | Not started | |
