# Tighten the Source Comments

**Status: In progress.** Approved 2026-10-03, with the defaults under "Decisions to confirm".
Comments only: no code, test or build change, so no release.

The engine's contract is written in its source comments (`README.md` and `CLAUDE.md` both say so).
This plan cuts each comment to what a reader of the code needs, and checks each claim against the
code beneath it.

Closes [flywheel-dag#26](https://github.com/tomlin256/flywheel-dag/issues/26) and
[flywheel-dag#25](https://github.com/tomlin256/flywheel-dag/issues/25).

## Problem

Measured at `c05963f`:

- **Volume.** 5,571 of the 22,861 lines in the C++ and CMake files are comments (24%). In
  `include/flywheel/` it is 2,767 of 8,652 (32%).
- **History.** 73 references point at 15 closed issues, and about 60 comment lines match a search
  for history phrases ("used to", "no longer", "before the fix", "regress", "the old"), 18 in
  `include/` and 39 in `tests/`. A few hits are legitimate: a compiler bug that a workaround names.
  Examples: `dag.hpp`'s "Dirtiness — three states, where there used to be two";
  `dag_timeseries.hpp`'s `RateLimiterNode` block, "It used to absorb"; `dag_aad.hpp`'s "(Before
  flywheel-dag#18's fix, …)". The reasoning behind each fix is in `docs/plans/` and git history, so
  a comment need not carry it. Only flywheel-dag#24, which is open, is cited for a limitation that
  still exists.
- **Drift.** A scripted pass over the code-like words in comments, listing those no code defines,
  already finds real errors: `dirty_` in `test_dag_async.cpp` (the member is `state_`); `tick_` in
  `dag_timeseries.hpp` (the field is `state_.tick`); `rate_` in `test_timeseries.cpp` (the field is
  `EWMATickRateNodeState::rate`); the five snake_case test names at the head of
  `test_engine_state.cpp` (they are `EngineStateTests.SaveRestoreRoundtrip` and so on); and
  `core/CLAUDE.md` in `test_dag.cpp` (flywheel-dag#25). The pass is a net, not a proof, so the rest
  needs a read.
- **Repetition.** A contract is often stated in the header, again in the `.inl`, and again in a
  test. Each copy can drift.
- **Deferred work in prose.** Two `TODO` blocks, both in `discoverStatefulNodes()`
  (`dag_engine.hpp`, `dag_engine.inl`). The issue tracker is the index of undone work.

## Rules

**Keep** what a reader cannot get from the code:

- what a caller must do or can rely on: preconditions, postconditions, what throws, ownership and
  lifetime, thread rules, an allocation or complexity guarantee that a test or the benchmark checks;
- the reason a rule exists, when it is not obvious, stated as a present fact;
- the blocks that `CLAUDE.md` and `README.md` point at: the three reasons to override `propagate()`
  or `dirty()` in `dag.hpp`, the top of `dag_aad.hpp`, the `alpha` checks in `EWMANode::make`, the
  dependency rationale in `CMakeLists.txt`;
- a compiler bug that a workaround names (`CLAUDE.md` requires the name);
- the banner, and the section titles.

**Cut:**

- history: "used to", "no longer", "now", "before the fix", "regression test for". Say what is true;
- a reference to a closed issue. A reference to an open issue stays where the comment states the
  limitation it tracks;
- a pointer to `docs/plans/` for a fixed bug;
- a sentence that restates the line below it, or narrates an obvious step;
- a contract stated again away from the place that owns it. The header block owns it. The `.inl`
  keeps only what its code makes non-obvious. A test comment says what the test pins and why, in a
  line or two;
- hedges and filler ("simply", "note that", "in practice"), and emphasis in capitals other than a
  block's label;
- a downstream application's name, module or example (`CLAUDE.md`).

**Write** each kept sentence so that it is true now, in the present tense, and spells a code name as
the code does (`Eager`, not EAGER).

**When a comment and the code disagree,** decide which is right. A wrong comment is fixed. If the
code is wrong, this plan does not touch it: open a `bug` issue, leave that comment as the contract,
and say so in the report.

## Checks at every step

1. **The code is identical.** A scratch script strips the comments from every file of the step,
   normalises whitespace, and compares the result with a snapshot taken at `c05963f`. It reads
   string, character and raw-string literals, so a `//` inside one is not a comment. It stays in
   the scratchpad: it is a verification aid, not project code.
2. **Build and tests.** `cmake -B build -DFLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON`, a full build with no
   warnings, and `ctest --test-dir build` green (36 of 36 at `c05963f`).
3. **Names resolve.** A scratch script lists every file name, test name (`Suite.Test`) and
   code-like word in the step's comments that no code defines. Each hit is fixed, or shown to be
   plain English or a CMake or standard word.
4. **No history.** A search for `used to`, `no longer`, `previously`, `formerly`, `before the fix`,
   `pre-fix`, `regress`, `the old`, `originally` and `flywheel-dag#` returns no line in the step's
   files, apart from an open issue cited for a limitation it tracks. Each hit on `now`, `new` or
   `still` is read, and stays only if it is true of the code today.
5. **Claims read against the code.** Each sentence that states a fact is read against the code it
   sits over: signatures, defaults, what throws, ordering, thread and allocation claims. Where
   reading is not enough, a throwaway probe in the scratchpad (not committed) settles it.
6. **The diff is read once more**, for a constraint that was cut and a claim added without checking.
   `CLAUDE.md`'s pointers into comment blocks still resolve.

No unit test is written or modified: nothing here has behaviour. Checks 1 to 3 stand in for them.
Each step is one checkpoint commit to `main`, subject `docs(#26): …`.

## Steps

Each step reads its files whole, code with comments, before editing. Comment-line counts are at
`c05963f`, banners included.

### Step 0 — Baseline and tools (no commit)

Take the comment-stripped snapshot of every file. Check the compare script on a scratch copy outside
the repo: it must flag a changed token, a deleted code line and a joined line, and pass a
comment-only edit. Touch a header and rebuild every translation unit with
`FLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON`.

**Done when:** the script passes its own check, the rebuild prints no warnings, and ctest is 36 of
36. (ctest was 36 of 36 on 2026-10-03.)

### Step 1 — `dag.hpp`, `dag.inl` (682)

The core contract: invalidation, equality policies, node classes. Drop the old-protocol narrative in
the `Dirtiness` block, and flywheel-dag#1, #5, #8 and #18. Name in full the test that `dag.hpp`
elides as `...DoesSuppress...`.

Commit: `docs(#26): tighten the dag.hpp and dag.inl comments`.

### Step 2 — `dag_async.*`, `dag_engine.*` (405)

Staging, flushing, the cycle and its outputs. Keep flywheel-dag#24 in `addOutput()`'s doc: it is
open. Each `TODO` becomes an `enhancement` issue and leaves the source, or shrinks to one line that
states the limitation.

**Done when,** as well: the issues are opened, and I list them in my report.

Commit: `docs(#26): tighten the async and engine comments`.

### Step 3 — Persistence, replay and the small headers (622)

`dag_state_store.*`, `dag_replay.*`, `dag_compute_module.hpp`, `dag_graph.hpp`,
`dag_traversal.*`, `dag_window_status.hpp`, `dag_ring_buffer.*`, `dag_memoize.*`.

Commit: `docs(#26): tighten the persistence, replay and utility comments`.

### Step 4 — Ops and time series (619)

`dag_ops.*`, `dag_timeseries.*`. The `RateLimiterNode` block says what the node does, not what it
used to do. `tick_` becomes what the code has.

Commit: `docs(#26): tighten the ops and time-series comments`.

### Step 5 — AAD (439)

`dag_aad.*`. The block at the top of `dag_aad.hpp` is the one `CLAUDE.md` points at: it keeps its
content.

Commit: `docs(#26): tighten the AAD comments`.

### Step 6 — Core tests (676)

`test_dag.cpp`, `test_lazy_invalidation.cpp`, `test_clean_inputs.cpp`, `test_value_slot.cpp`,
`test_nodes.hpp`. Fixes flywheel-dag#25. A test comment says what the test pins, in the present
tense: "Registered before the first `step()`, …", not "regression test for …".

Commit: `docs(#26): tighten the core test comments`.

### Step 7 — Engine, state and utility tests (857)

`test_dag_async.cpp`, `test_engine_state.cpp`, `test_engine_stateful_discovery.cpp`,
`test_dag_state_store.cpp`, `test_json_state_store.cpp`, `test_dag_replay.cpp`,
`test_compute_module.cpp`, `test_dag_graph.cpp`, `test_dag_traversal.cpp`, `test_ring_buffer.cpp`,
`test_window_status*.cpp`, `test_dag_memoize.cpp`. `dirty_` and the five test names at the head of
`test_engine_state.cpp` are fixed here.

Commit: `docs(#26): tighten the engine and state test comments`.

### Step 8 — Ops, time-series and AAD tests (805)

`test_dag_ops.cpp`, `test_timeseries.cpp`, `test_stateful_node_base.cpp`, `test_aad_*.cpp`,
`aad_test_graphs.hpp`. `rate_` is fixed here.

Commit: `docs(#26): tighten the ops, time-series and AAD test comments`.

### Step 9 — Examples, benchmarks, consumer test and CMake (466)

`examples/`, `benchmarks/`, `tests/consumer/main.cpp`, and the CMake files. The rationale for not
installing spdlog and nlohmann/json stays in `CMakeLists.txt`, where `CLAUDE.md` points. A comment
in the CMake files that cites `flywheel-dag#2` or `#4` keeps what it says and loses the number.

Commit: `docs(#26): tighten the example, benchmark and CMake comments`.

### Step 10 — Close out

Run checks 1 to 4 over the whole tree. Report the comment lines and characters before and after.
Run the `pre-push` hook's audit over the tree and the new commits. Mark this plan done, and close
flywheel-dag#25 and #26 with a summary comment. Nothing is pushed unless asked.

**Done when:** the whole-tree checks are clean, the audit passes, and both issues are closed.

Commit: `docs(#26): mark the plan done`.

## Decisions to confirm

Each has a default. Approving the plan as written takes the default.

1. **Scope.** The tests, examples, benchmarks and CMake comments are in, not only `include/`.
2. **Closed issues.** Every reference to a closed issue is cut, closed enhancements included
   (flywheel-dag#2, #4, #8, #10, #12, #14, #17). `CLAUDE.md` still cites some of them.
3. **TODOs.** The two `TODO` blocks become `enhancement` issues.
4. **Section rules.** The 678 `// ───` rule lines stay: they are how the long headers are scanned.
5. **Release and push.** Neither. The comments ship with the next release.

## Not in this plan

- `README.md`, `CLAUDE.md` and `docs/plans/`. The plans are a record. A wrong fact found in any of
  them goes into a `documentation` issue.
- Code, names, file layout, or a documentation generator.
- A bug found while reading: it gets a `bug` issue, and I tell Rob which.

## Self-review — risks and assumptions

- **A cut comment carried a constraint.** The main risk. The rules keep every contract and every
  non-obvious reason, and cut only history, restatement and duplication. Check 6 re-reads each diff
  for a lost constraint, and each commit is small enough to review (at most 857 comment lines).
- **An edit changes code** (a stray `*/`, a deleted continuation line). Check 1 compares the code,
  whitespace aside, and check 2 builds with warnings as errors, which catches `-Wcomment`.
- **A claim is judged true and is not.** Reading is the weakest check here. A probe settles what
  reading cannot, and a claim that cannot be settled is reworded to what can be shown, or raised
  with Rob. His review of the diffs is the control.
- **The comment is right and the code wrong.** Handled by the rule above: an issue, not a fix.
- **A consumer.** A consumer reads the headers at a tag. Comment-only changes alter nothing it
  builds.
- **A downstream name.** None may appear (`CLAUDE.md`). Each comment is read for one, and Step 10
  runs the `pre-push` audit.
- **GCC is unverified locally.** Builds here use Apple Clang, which also has `-Wcomment`. CI's GCC
  leg sees the commits only when pushed.
- **Assumptions:** `build/` is configured and its `_deps` are present, so no network is needed. The
  scratch scripts handle every comment form in the tree, which check 1's self-test shows.

## Progress

| Step | Status | Notes |
|---|---|---|
| 0 — Baseline and tools | Not started | ctest 36 of 36 on 2026-10-03 |
| 1 — `dag.hpp`, `dag.inl` | Not started | |
| 2 — Async and engine | Not started | |
| 3 — Persistence, replay, small headers | Not started | |
| 4 — Ops and time series | Not started | |
| 5 — AAD | Not started | |
| 6 — Core tests | Not started | |
| 7 — Engine, state and utility tests | Not started | |
| 8 — Ops, time-series and AAD tests | Not started | |
| 9 — Examples, benchmarks, consumer, CMake | Not started | |
| 10 — Close out | Not started | |
