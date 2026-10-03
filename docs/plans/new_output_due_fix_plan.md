# A New Output Gets the Value Its Node Holds

**Status: Done (2026-10-03).** Approved 2026-10-03. Both steps landed, and v0.1.14 is released.
A new output starts due, so its callback fires on the first cycle after it is registered, with the
value its node holds then. Registering an output between cycles is supported.

Closes [flywheel-dag#23](https://github.com/tomlin256/flywheel-dag/issues/23).

## Problem

`Engine::cycle()` evaluates an output only when the output is due. An output becomes due when a
cycle finds its node dirty, and stays due until a cycle reaches it
([flywheel-dag#20](https://github.com/tomlin256/flywheel-dag/issues/20)). `addOutput()` starts a
new output not due. Its node may already be clean, because an earlier cycle pulled it as another
output's input, or because a caller evaluated it. The output is then not due at the next cycle, and
its callback waits for the node to move. It never sees the value the node held when the output was
registered.

In the issue's graph, `b = 10·x` and `a = b + 1`. `a`'s output is registered, and a `step()` pulls
`b` clean. An output registered on `b` after that sees nothing over two more `step()`s, though `b`
holds 10. Registered before the first `step()`, it gets 10 on that step. So whether a callback sees
its node's starting value depends on when it was registered. `cycle()`'s comment, "The first time
lastSeen is null, so every output fires once", holds only for an output whose node is dirty at its
first cycle.

## The fix

A new output starts due:

```cpp
inline void Engine::addOutput(
    NodePtr node, std::function<void(const ValuePtr&)> cb)
{
    outputs_.push_back({ std::move(node), nullptr, std::move(cb) });
    due_.push_back(true);
}
```

- **Every output fires once, on the first cycle after it is registered,** with the value its node
  holds when that cycle reaches it. Its `lastSeen` is null, so any value differs from it. After
  that, its callback fires only when the value changes, as today.
- **`cycle()` does not change.** It treats a new output as it treats one that a throw left due
  (flywheel-dag#20). It evaluates the node, which returns its cached value if it is clean, clears
  the mark, and calls the callback. A node that throws keeps the new output due, and the next cycle
  retries it.
- **Nothing changes for an output whose node is dirty at its first cycle.** The marking pass would
  have marked it due anyway. Every node in the engine starts dirty, so that covers every output
  registered before the first cycle on a node that no one has evaluated. The fix reaches only an
  output whose node is clean when its first cycle starts.
- **`addFeedback()` and `install()` get it too,** since both register through `addOutput()`. A
  feedback registered after a cycle sets its input to the node's value on its first cycle. The
  input's consumers see that on the cycle after, the one-cycle lag `addFeedback()` documents.
- **An output that a cycle has reached is unchanged.** A caller that evaluates its node between
  cycles leaves it clean at the next cycle, so its callback misses the change. The tape rule in
  `dag_aad.hpp` and `CLAUDE.md` stays: never make a tape evaluate a dirty root for its caller. Its
  wording narrows to an output that a cycle has reached.

### Registering an output after the engine has cycled is supported

The issue asks for this decision. An output may be registered between cycles: before the first
cycle, between `step()`s, and once `run()` has returned, which
[flywheel-dag#16](https://github.com/tomlin256/flywheel-dag/issues/16) lets an application call
again. With the fix, when an output was registered no longer decides what its callback sees.
`install()`, which keeps the module and calls its `wire()`, follows the same rule. Its "Must be
called before run()" predates a `run()` that can be called again.

Registering during a cycle, from an output callback or a node's `eval()`, is not supported, and
neither is registering from another thread while `run()` is going.

- A callback that calls `addOutput()` can read freed memory:
  [flywheel-dag#24](https://github.com/tomlin256/flywheel-dag/issues/24), found while planning
  this. The fix does not change that.
- Another thread's call races `cycle()`'s reads of `outputs_` and `due_`.

The alternative, refusing a registration after the first cycle, would break any caller that
registers an output between `step()`s or installs a module between runs, for no gain. No test here
does either today. Starting a new output due costs nothing in `cycle()`.

### Probe

A probe on a scratch copy of the repo drove these sequences through v0.1.13 and the fix. Each
cell is what the output's callback saw, or the value of `y`.

| Sequence | v0.1.13 | fixed |
|---|---|---|
| The issue's: `b`'s output registered after `a`'s cycle pulled `b` clean, then two `step()`s | `{}` | `{10}` |
| A caller evaluates `b`, then registers `b`'s output, then one `step()` | `{}` | `{10}` |
| `b`'s output registered between two `run()`s, each ended by its initial cycle | `{}` | `{10}` |
| A second output on `b`, registered after the first has had 10, then `x` moves to 2 | `{20}` | `{10, 20}` |
| `addFeedback(b, y)` registered after a cycle, then one `step()` | `y` is 0 | `y` is 10 |
| An `AsyncQueue` output registered after a cycle, then one batch of one value | sizes `{1}` | sizes `{0, 1}` |
| `b`'s output registered before any cycle | `{10}` | `{10}` |
| `b`'s output registered after a cycle, once `x` has moved to 2 | `{20}` | `{20}` |
| An `AsyncQueue` output registered before any cycle, then one batch of one value | sizes `{0, 1}` | sizes `{0, 1}` |
| `b`'s output, which a cycle has reached, after `x` moves and a caller evaluates `b` | `{10}` | `{10}` |

The last four rows are unchanged. An `AsyncQueue` registered after a cycle gets the empty batch
once, as one registered before the first cycle already does.

On the same copy, the full suite passed (36 of 36) and built with no warnings under Apple Clang
with `FLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON`. `bench_hot_path --invariants` matched
`benchmarks/expected_invariants.txt`. Every `bench_hot_path` row registers its outputs before its
warm-up, on nodes that start dirty.

## Steps

Every commit subject is scoped to flywheel-dag#23 in the repo's `type(#N): …` form. The subjects
below omit the scope.

### Step 1 — Regression tests, then the fix

The tests go in `test_dag_async.cpp`, in a new section, "An output registered on a clean node",
after "A cycle that throws". The file's coverage comment gains the section. Each test builds the
issue's graph, `b = 10·x` and `a = b + 1`, and all but the second register `a`'s output first.

| Test | Asserts | v0.1.13 |
|---|---|---|
| `EngineNewOutput.GetsTheValueItsNodeHoldsAfterAnotherOutputPulledIt` | The issue's sequence. Over the two `step()`s after `b`'s output is registered, its callback gets 10, once. When `x` moves to 2, it gets 20 | `b`'s callback has seen nothing after the two `step()`s |
| `EngineNewOutput.GetsTheValueItsNodeHoldsAfterACallerEvaluatedIt` | The test evaluates `b` before the first `step()`, then registers `b`'s output. The `step()` delivers 10 | nothing |
| `EngineNewOutput.ARunStartedAgainDeliversAnOutputRegisteredBetweenRuns` | Through `run()`. `b`'s output is registered after the first `run()` returns, and the second `run()` delivers 10 | the second `run()` returns, and `b`'s callback has seen nothing |
| `EngineNewOutput.AFeedbackRegisteredAfterACycleSetsItsInput` | `addFeedback(b, y)` after a `step()`. The next `step()` sets `y` to 10 | `y` stays 0 |
| `EngineNewOutput.AModuleInstalledAfterACycleGetsItsOutputsValue` | A module that is given `b` and registers an output on it in `wire()`, as a module downstream of another does, is installed after a `step()`. The next `step()` delivers 10 | nothing |

The `run()` test registers an output on an input, `halt`, before the first `run()`. `halt` starts
dirty, and the test sets it again before the second. Its callback stops the engine, which ends the
run once that cycle has reached every output, `b`'s included. So each run ends after its initial
cycle, and neither waits on a feed.

Then the fix, with comments in `addOutput()`, on `due_`, and on `cycle()`'s "every output fires
once", saying that a new output starts due and why.

**Done when:**

- the five tests fail on v0.1.13 for the reason given, and pass after the fix;
- the build prints no warnings with `FLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON`, and ctest is green
  (36 of 36);
- `bench_hot_path --invariants` matches `benchmarks/expected_invariants.txt`;
- CI is green on both legs.

No timing: `cycle()` does not change.

Commit: `fix: a new output gets the value its node holds`.

### Step 2 — Docs and release

- `dag_engine.hpp`:
  - `addOutput()`'s doc says that the callback fires on the first cycle after the output is
    registered, with the value its node holds then, and after that only when the value changes. It
    says when an output may be registered: between cycles, never from inside one
    (flywheel-dag#24), and never from another thread while `run()` is going.
  - `install()`'s "Must be called before run()" becomes "between cycles, as `addOutput()`".
  - The overview's Cycle paragraph gains a sentence on a new output.
- `dag_engine.inl`: the `discoverStatefulNodes()` TODO no longer says that `install` and
  `addOutput` are "documented as pre-run". It says that outputs change only when one is
  registered, between cycles. The memo it proposes already invalidates from `install()` and
  `addOutput()`.
- `dag_aad.hpp` and `CLAUDE.md`: the tape rule says that an output a cycle has reached misses the
  change. A new output does not.
- `CLAUDE.md`: the last sentence of the "A cycle that throws" entry states the new rule: a new
  output starts due, so its first cycle delivers the value its node holds then. Outputs are
  registered between cycles, never from inside one (flywheel-dag#24).
- Release v0.1.14 as v0.1.13 was released: the project version and the FetchContent snippets in
  `CMakeLists.txt` and the README, the tag, and a GitHub release with notes. The notes say that an
  output registered on a clean node now fires once on its first cycle.
- Mark this plan done, and close flywheel-dag#23 with a summary comment.

**Done when:** ctest is green (36 of 36), CI is green on the release commit on both legs, the
installed version file reports 0.1.14, the release is published, and flywheel-dag#23 is closed.

Commits: `docs: say when an output may be registered and what it first gets`,
`build: release v0.1.14` and `docs: mark the plan done`.

## Not in this plan

- **`addOutput()` from inside an output callback can read freed memory** —
  [flywheel-dag#24](https://github.com/tomlin256/flywheel-dag/issues/24), found while planning
  this. `cycle()` calls each callback where it is stored, and a registration that reallocates
  `outputs_` frees the running callback's storage. This plan only documents the rule. Refusing a
  registration during a cycle, deferring it, or keeping the entries in storage that does not move
  is a decision of its own, and two of the three change what `cycle()` iterates.
- **Registering from another thread while `run()` is going.** It would need a lock that `cycle()`
  takes, on the hot path.
- **An output that a cycle has reached still misses a value a caller evaluated between cycles.**
  That is the tape rule, which stays as it is: only a new output is due without a dirty node.

## Self-review — risks and assumptions

- **An application's callback may see one call it did not see before.** An output registered on a
  clean node now fires once on its first cycle, with the value its node holds. A callback that
  relied on silence until the node moved relied on when it was registered, which is the defect. An
  `AsyncQueue`'s callback gets the empty batch, as one registered before the first cycle already
  does. A feedback registered after a cycle sets its input, which re-dirties the input's
  consumers, once.
- **A new output's node can be clean when the cycle evaluates it.** `eval()` then returns its cached
  value without recomputing, the path an output already takes after a throw (flywheel-dag#20) or
  when an earlier output pulled it in the same cycle.
- **An output registered from inside a callback is now due in that same cycle,** since `cycle()`'s
  loop reads `outputs_.size()` on every pass. That registration is unsupported and can read freed
  memory either way (flywheel-dag#24). The fix does not change that.
- **`due_` is touched on the eval thread alone,** in `cycle()` and in `addOutput()`, as before.
- **The `run()` test cannot hang.** Each run ends in its own initial cycle, through `halt`'s
  callback, on v0.1.13 and with the fix. The probe ran the same sequence on both.
- **GCC is unverified locally.** The probe was built with Apple Clang alone. The change is one
  `bool`, with nothing compiler-specific. CI's GCC 13 leg sees it at Step 1.
  [flywheel-dag#7](https://github.com/tomlin256/flywheel-dag/issues/7) moves CI to Ubuntu 26 on
  2026-10-19, which may bring a newer GCC.
- **Assumptions:** the engine stays header-only C++17. `run()` is called from one thread. Outputs
  are registered between cycles.

## Progress

| Step | Status | Notes |
|---|---|---|
| 1 — Tests, then the fix | Done | ctest 36 / 36, and `--invariants` is unchanged. The build prints no warnings with `FLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON`. `test_dag_async` gains the five `EngineNewOutput` tests. On v0.1.13 all five failed: `b`'s callback at `{}` after another output pulled `b` clean, after a caller evaluated it, and between two `run()`s, `y` at 0, and the module's callback at `{}`. The first also saw `{20}` once `x` moved, where the fix gives `{10, 20}`. CI run 37114001467 is green on both legs, at 36 / 36 with no compiler warnings |
| 2 — Docs and release | Done | `dag_engine.hpp`'s `addOutput()` doc says what a new output first gets and when an output may be registered, `install()`'s says between cycles, and the overview says a new output starts due. The `discoverStatefulNodes()` TODO no longer calls them pre-run. The tape rule, in `dag_aad.hpp` and `CLAUDE.md`, narrows to an output a cycle has reached, and `CLAUDE.md`'s "A cycle that throws" entry ends with the new rule. ctest 36 / 36. The docs and release commits were pushed together, and CI run 37114246910 on the release commit is green on both legs, at 36 / 36 with no compiler warnings. The installed version file reports 0.1.14. `v0.1.14` is tagged and released, and flywheel-dag#23 is closed |
