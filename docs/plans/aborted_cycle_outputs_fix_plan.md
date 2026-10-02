# A Cycle That Throws Leaves the Outputs It Did Not Reach Due

**Status: Approved (2026-10-02).**

Closes [flywheel-dag#20](https://github.com/tomlin256/flywheel-dag/issues/20).

## Problem

`Engine::cycle()` reads every output's dirty flag before it evaluates any of them, because
evaluating one output can pull another clean as its input. That snapshot serves one cycle, and the
next cycle overwrites it. When a node's `eval()` or an output callback throws, `cycle()` never
reaches the outputs after it. One that an earlier output had already pulled clean reads clean at
the next snapshot, so its callback misses the value its node now holds, until that value moves
again.

In the issue's graph, `b = 10·x` and `a = b + 1`, and `a`'s output is registered before `b`'s. On
the cycle where `x` goes to 2, `a` pulls `b` to 20, and then `a`'s callback throws. `b` is clean at
the next cycle, so its callback keeps 10. The same happens when `a`'s own functor throws after it
has pulled `b`.

[flywheel-dag#16](https://github.com/tomlin256/flywheel-dag/issues/16) lets `run()` start again
after a throw, so a running application can reach this, not only a test that calls `step()`.
`aad::GradientNode` and `aad::TangentNode` throw from `eval()` by contract.

## The fix

An output is due from the cycle that finds its node dirty until the engine has compared its value
with `lastSeen`. `dirtySnapshot_` becomes `due_`, the same pre-allocated `std::vector<bool>`.
`cycle()` marks entries rather than overwriting them, and clears each one when it reaches it:

```cpp
    // 2. Mark, never overwrite.
    for (std::size_t i = 0; i < outputs_.size(); ++i)
        if (outputs_[i].node->dirty()) due_[i] = true;

    // 3. Evaluate the due outputs and fire their callbacks.
    for (std::size_t i = 0; i < outputs_.size(); ++i) {
        if (!due_[i]) continue;

        ValuePtr val = outputs_[i].node->eval(ctx_);
        due_[i] = false;

        if (val != outputs_[i].lastSeen) {
            outputs_[i].lastSeen = val;
            callbacks_.fetch_add(1, std::memory_order_relaxed);
            outputs_[i].callback(val);
        }
    }
```

- **Mark, never overwrite.** A cycle that throws leaves the outputs it did not reach due, and the
  next cycle adds the outputs whose nodes are dirty then.
- **Nothing changes until a cycle throws.** A cycle that does not throw reaches every output and
  clears every entry, so the next one starts with `due_` all false, and the marking pass computes
  exactly what the snapshot did.
- **Cleared once `eval()` returns.** An output whose node throws stays due. A `ComputeNode` whose
  functor throws stays dirty, as do `aad::GradientNode` and `aad::TangentNode` when their tape
  throws, so their outputs would be retried anyway. Clearing after `eval()` keeps that true for a
  node that a throw leaves clean.
- **Cleared before the callback runs: the callback that throws has had its value.** `lastSeen`
  already holds it, as today, and the engine does not offer it again. The callback gets the next
  value its output takes. Offering it again would call a callback that throws on a value once more
  on every cycle until the value moved, and hold back every output after it for that long. A
  callback may also have acted on the value before it threw, and a second call would act again.
  The application has the exception, and the node still holds the value.
- **A due output gets the value its node holds when the engine reaches it.** If the node moved
  again before the next cycle, its callback gets the newer value, as a callback does whenever two
  changes land between cycles.
- **A new output starts not due,** as its snapshot entry does today, so the first cycle's dirty
  flag decides. An output registered on a node that is already clean then misses the value the node
  holds. That is [flywheel-dag#23](https://github.com/tomlin256/flywheel-dag/issues/23), below.

A probe on a scratch copy of the headers drove these sequences through v0.1.12 and the fix:

| Sequence | v0.1.12 | fixed |
|---|---|---|
| The issue's: `a`'s callback throws after `a` has pulled `b` to 20, then one more `step()` | `b`'s callback has seen `{10}`, and `b` holds 20 | `{10, 20}` |
| `a`'s functor throws after it has pulled `b` to 20, on three cycles running, then succeeds | `b`: `{10}` | `b`: `{10, 20}` |
| The issue's, through `run()`, with a second `run()` after the throw | `b`: `{10}` | `b`: `{10, 20}` |
| The issue's, with `x` moved to 3 before the step after the throw | `b`: `{10, 30}` | `b`: `{10, 30}` |
| `a`'s callback, which threw on 21, over the steps after the throw | `{11, 21}` | `{11, 21}` |

On the same copy, the full suite passed (36 of 36) and built with no warnings under Apple Clang with
`FLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON`. `bench_hot_path --invariants` matched
`benchmarks/expected_invariants.txt`. Over three runs of `bench_hot_path`, alternating with
v0.1.12's, the fix's median was at or below v0.1.12's on every row: chain 259.5 ns/cycle against
261.7, idle-queues 229.5 against 254.5, and ingest 111.0 against 112.2. One ingest run, at 116.0,
was above v0.1.12's highest, 114.3.

## Steps

Every commit subject is scoped to flywheel-dag#20 in the repo's `type(#N): …` form. The subjects
below omit the scope.

### Step 1 — Regression tests, then the fix

The tests go in `test_dag_async.cpp`, in a new section, "A cycle that throws", after the
`Engine::run()` section, so the `run()` test can use `runAndCatch()`. The file's coverage comment
gains the section. The first three tests build the issue's graph.

| Test | Asserts | v0.1.12 |
|---|---|---|
| `EngineAbortedCycle.ALaterOutputGetsItsValueAfterACallbackThrows` | The issue's sequence: `a`'s callback throws on the cycle where `x` goes to 2, and `b`'s callback gets 20 on the next `step()` | `b`'s callback has seen 10 only |
| `EngineAbortedCycle.ALaterOutputGetsItsValueAfterANodeThrows` | `a`'s functor throws after it has pulled `b`, on two `step()`s running. On the step where it succeeds, `a`'s callback gets 21 and `b`'s gets 20 | `b`'s callback has seen 10 only |
| `EngineAbortedCycle.ARestartedRunDeliversWhatTheAbortedCycleDidNot` | Through `run()`: `b`'s callback, on 10, sets `x` to 2, and `a`'s callback throws in the cycle that wakes. A second `run()` delivers 20 to `b`'s callback | the second `run()` returns, and `b`'s callback has seen 10 only |
| `EngineAbortedCycle.ACallbackThatThrowsHasHadItsValue` | An output on `x` alone, whose callback throws on 2. The next `step()` does not call it again, and once `x` moves to 3, it gets 3 | passes |

The `run()` test's last output is on an input, `halt`, that the test sets before the second
`run()`. Its callback stops the engine, so that run ends after its first cycle, which reaches the
other outputs first, and neither run waits on a feed. The fourth test passes on v0.1.12: it pins
the decision above, not a defect.

Then the fix, with comments in `cycle()` and on `due_` saying why an entry is marked rather than
overwritten, and why it is cleared after `eval()` and before the callback. The snapshot comment's
`dirty_=false`, a name the engine no longer uses, goes too.

Each hand-made change below must fail exactly the tests stated. After restoring a mutated file,
`touch` it before the next build: the local Make compares mtimes in whole seconds, and can keep the
mutated object.

| Change | Fails |
|---|---|
| The marking pass overwrites, as in v0.1.12: `due_[i] = outputs_[i].node->dirty()` | the first three |
| A callback that throws is offered its value again: `due_[i] = false` and `lastSeen = val` move after the callback | `ACallbackThatThrowsHasHadItsValue` |

**Done when:**

- the first three tests fail on v0.1.12 for the reason given, and all four pass after the fix;
- each hand-made change fails as stated;
- the build prints no warnings with `FLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON`, and ctest is green
  (36 of 36);
- `bench_hot_path --invariants` matches `benchmarks/expected_invariants.txt`, and no row is slower
  than v0.1.12 beyond run-to-run noise, timed on the same machine;
- CI is green on both legs.

Commit: `fix: a cycle that throws leaves the outputs it did not reach due`.

### Step 2 — Docs and release

- `dag_engine.hpp`: `addOutput()`'s doc says that a cycle a node or a callback throws out of leaves
  the outputs it did not reach for the next cycle, and that a callback that throws is not offered
  that value again. `run()`'s doc adds that the next cycle delivers what the aborted one did not
  reach. The overview's Cycle paragraph gains a sentence on it.
- `CLAUDE.md`: the sensitivity-node bullet no longer says the aborted cycle can leave another
  output's callback stale. A Key Patterns entry, "A cycle that throws", states the rule: the outputs
  it did not reach stay due, a node that throws keeps its output due, and a callback that throws
  has had its value.
- Release v0.1.13 as v0.1.12 was released: the project version and the FetchContent snippets in
  `CMakeLists.txt` and the README, the tag, and a GitHub release with notes.
- Mark this plan done, and close flywheel-dag#20 with a summary comment.

**Done when:** ctest is green (36 of 36), CI is green on the release commit on both legs, the
installed version file reports 0.1.13, the release is published, and flywheel-dag#20 is closed.

Commits: `docs: say what a cycle that throws leaves for the next`, `build: release v0.1.13` and
`docs: mark the plan done`.

## Not in this plan

- **An output registered on a node that is already clean misses the value it holds** —
  [flywheel-dag#23](https://github.com/tomlin256/flywheel-dag/issues/23), found while planning
  this. No throw is involved. An output registered after another output has pulled its node clean
  does not fire until the node moves. Starting a new output due would fix it, and breaks no test on
  a scratch copy, but whether registering an output after the engine has cycled is supported is a
  decision of its own.
- **`meanCycleUs()` counts a cycle that throws at zero duration** —
  [flywheel-dag#21](https://github.com/tomlin256/flywheel-dag/issues/21). This fix does not touch
  the statistics.
- **An output a caller evaluates between cycles still reads clean at the next one.** That is the
  tape rule in `dag_aad.hpp` and `CLAUDE.md`, which stays as it is: only a cycle that reads a node
  dirty marks its output due.

## Self-review — risks and assumptions

- **A node that throws on every evaluation holds back the outputs after it** for as long as it
  throws. It did before too. Now they are delivered once it recovers, where before an output its
  evaluation had pulled clean was lost.
- **A due output's node can be clean.** The engine then calls `eval()` on a clean node, which
  returns its cached value without recomputing. That is the path an output already takes when an
  earlier output pulled it in the same cycle. A node whose `eval()` acts when it is clean would see
  one more call after a throw. No node in the engine does.
- **`due_` is touched on the eval thread alone,** in `cycle()` and in `addOutput()`, as
  `dirtySnapshot_` was.
- **The `run()` test cannot hang.** Each run ends in its own cycles, by a throw or by `halt`'s
  callback, on v0.1.12 and with the fix. The probe ran it on both.
- **GCC is unverified locally.** The probe was built with Apple Clang alone. The change is a renamed
  `std::vector<bool>` and two stores, with nothing compiler-specific. CI's GCC 13 leg sees it at
  Step 1. [flywheel-dag#7](https://github.com/tomlin256/flywheel-dag/issues/7) moves CI to Ubuntu
  26 on 2026-10-19, which may bring a newer GCC.
- **Assumptions:** the engine stays header-only C++17. `run()` is called from one thread. Outputs
  are registered before the engine runs, which flywheel-dag#23 may change.

## Progress

| Step | Status | Notes |
|---|---|---|
| 1 — Tests, then the fix | Not started | |
| 2 — Docs and release | Not started | |
