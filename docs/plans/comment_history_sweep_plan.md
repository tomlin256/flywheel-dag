# Comments State the Present, Not the History

**Status: In progress. Approved 2026-11-07, with the defaults under "Decisions to confirm".**
Comments, one test and one line of `CLAUDE.md`: no engine code changes, so no release.

Closes [flywheel-dag#71](https://github.com/tomlin256/flywheel-dag/issues/71).

A comment says what is true of the code now. This plan reads every comment line in the tracked
C++, CMake, CI and tool-config files against that rule, rewrites the ones that narrate how the code
got here, corrects any claim the code no longer bears out, and adds a test that keeps issue
references from coming back. [flywheel-dag#26](https://github.com/tomlin256/flywheel-dag/issues/26)
did the same sweep on 2026-10-03 (`docs/plans/comment_tightening_plan.md`) and cut every
closed-issue reference. Nothing durable stated the rule, and 69 are back.

## Problem

Measured at `6cf5282`:

- **Volume.** 6,309 full-line comment lines (524,424 characters) in 94 files: `include/` 2,869
  lines, `tests/` 3,002, `benchmarks/` and `examples/` 226, and the root CMake files, `cmake/`, CI
  and the two tool configs 212. Since the last sweep (`2c768e0`) 165 commits have added 1,633 of
  them, mostly to `test_dag_async.cpp`, `test_dag_graph.cpp`, `check_presets.cmake`,
  `test_wake_state.cpp`, `test_engine_stateful_discovery.cpp` and `dag_engine.*`.
- **Issue references.** 72 `flywheel-dag#N` in comments. 69 point at 23 closed issues
  (flywheel-dag#24, flywheel-dag#27, flywheel-dag#34, flywheel-dag#36, flywheel-dag#37,
  flywheel-dag#39, flywheel-dag#41 to flywheel-dag#44, flywheel-dag#47 to flywheel-dag#49,
  flywheel-dag#56 to flywheel-dag#63, flywheel-dag#68, flywheel-dag#70); 3 point at open ones
  (flywheel-dag#31 twice, flywheel-dag#32). `dag_engine.hpp` carries 14, `test_dag_async.cpp` 10,
  `dag_engine.inl` 6.
- **Narrated bugs.** A test comment that tells the bug the test was written for:
  `test_dag_async.cpp:905` ("The issue's sequence: the queue was flushed twice in the second cycle,
  …"), `:1058`, `:2121` ("Guards the fix, not the bug"), `:2673` ("The issue's program."),
  `test_aad_dual.cpp:176` ("was absolute below a slope of 1 until flywheel-dag#41: it accepted
  that"), `check_cmake_diagnostics.cmake:102` ("The issue's symptom") and `tests/CMakeLists.txt:339`
  ("none of the checks .clangd used to remove").
- **Plan pointers.** Three comments name a file in `docs/plans/` (`check_platform.cmake:12`,
  `check_presets.cmake:12` and `:14`).
- **The scan is a net.** A search for history phrases returns about 80 lines, and most are plain
  English about the code ("the oldest element", "an earlier cycle", "the node is now clean"). The
  reading decides, not the search.

## Rules

**A comment keeps** what a reader cannot get from the code: what a caller must do or can rely on
(preconditions, postconditions, what throws, ownership, lifetime, thread rules, an allocation or
complexity guarantee that a test or the benchmark checks), and the reason a rule exists when it is
not obvious, stated as a present fact. It keeps the name of a compiler bug that a workaround is for
(`CLAUDE.md` requires it).

**A comment loses:**

- history: "used to", "no longer" about a change, "previously", "now does", "the old", "was changed",
  "fixed in", "regression test for", "guards the fix", "the issue's sequence". Say what is true.
- a reference to a closed issue, and a pointer to `docs/plans/`. An open issue stays where the
  comment states the limitation it tracks.
- the retelling of the bug a test was written for. A test comment says what the test pins and why
  the behaviour matters, in the present tense. Example: "The issue's sequence: the queue was flushed
  twice in the second cycle, and the second flush replaced its batch with []" becomes "A queue added
  twice is flushed once per cycle, so the output reads the batch the first flush staged."
- a number a reader cannot check, and a sentence that is no longer true.

**The test of a sentence:** would it still be true if the code had never had the bug? "The node is
now clean" in a test's sequence passes. "This used to read freed memory" does not.

**When a comment and the code disagree,** decide which is right. A wrong comment is fixed. If the
code is wrong, this plan does not touch it: a `bug` issue is opened, the comment stays as the
contract, and the report says so.

Test names are code and stay. An assertion message (the string after `<<`) that tells history
changes, and the compare script lists each such message for review.

## Checks at every step

1. **The code is identical.** A scratch script strips the comments from each file of the step
   (string, character and raw-string literals read as literals; CMake line and bracket comments;
   YAML comments), collapses whitespace, and compares the result with a snapshot taken at
   `6cf5282`. A changed assertion message is listed for review; any other token fails.
2. **Build and tests.** `cmake --preset relwithdebinfo-macos-arm64
   -DFLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON`, a full build with no warning (`-Wcomment` catches a stray
   continuation), and `ctest --preset relwithdebinfo-macos-arm64` green: 55 of 55 at `6cf5282`.
3. **Names resolve.** A scratch script lists every file name, test name (`Suite.Test`) and code-like
   word in the step's comments that no code defines. Each hit is fixed or shown to be English, CMake
   or the standard library.
4. **No history.** A search for the phrases above, `flywheel-dag#` and `docs/plans` returns nothing
   in the step's files except open-issue citations, and each hit on `now`, `new`, `still`,
   `already`, `no longer` and `previously` has been read and is true of the code today.
5. **Claims read against the code.** Each sentence that states a fact is read against the code under
   it: signatures, defaults, what throws, ordering, thread and allocation claims. Where reading is
   not enough, a throwaway probe in the scratchpad (not committed) settles it.
6. **The diff is read once more,** for a constraint that was cut and a claim added unchecked.
   `CLAUDE.md`'s pointers into comment blocks still resolve.

The scratch scripts (Python, run through `pipenv`) stay in the scratchpad: they are verification
aids, not project code. Steps 1 to 10 write no test, because nothing in them has behaviour; checks 1
to 3 stand in. Step 11 writes one.

Each step is one checkpoint commit to `main`, subject `docs` scoped to this issue's number, pushed
once the build and tests are green. CI is read in passing: the next push cancels the previous run,
so the final run covers the macOS leg of an earlier checkpoint.

## Steps

Comment-line counts are at `6cf5282`. They add up to 6,309.

| Step | Files | Lines |
|---|---|---|
| 0 | Baseline and tools (no commit) | |
| 1 | `dag.hpp`, `dag.inl`, `dag_async.hpp`, `dag_async.inl` | 792 |
| 2 | `dag_engine.*`, `dag_graph.hpp`, `dag_thread_fence.hpp`, `dag_state_store.*` | 660 |
| 3 | `dag_replay.*`, `dag_compute_module.hpp`, `dag_traversal.*`, `dag_window_status.hpp`, `dag_ring_buffer.*`, `dag_memoize.*`, `dag_aad.*` | 824 |
| 4 | `dag_ops.*`, `dag_timeseries.*` | 593 |
| 5 | `test_dag`, `test_lazy_invalidation`, `test_clean_inputs`, `test_value_slot`, `test_nodes.hpp`, `test_stateful_node_base`, `test_engine_state` | 630 |
| 6 | `test_dag_async`, `test_wake_state`, `test_allocation_counter`, `allocation_counter.*`, `test_dag_thread_fence` | 514 |
| 7 | `test_engine_stateful_discovery`, `test_dag_graph`, `test_dag_state_store`, `test_json_state_store`, `test_dag_traversal`, `test_compute_module`, `test_dag_replay`, `test_dag_memoize`, `test_window_status*`, `test_ring_buffer` | 648 |
| 8 | `test_timeseries`, `test_dag_ops`, `test_aad_*`, `aad_test_graphs.hpp` | 598 |
| 9 | root `CMakeLists.txt`, `cmake/`, `ci.yml`, `.clang-tidy`, `.clangd`, `benchmarks/`, `examples/` | 438 |
| 10 | `tests/CMakeLists.txt`, `tests/check_*.cmake`, `install_dependencies.cmake`, `tests/consumer/`, `tests/clang_tidy_canary/` | 612 |
| 11 | `test_comment_refs` and a line in `CLAUDE.md` | |
| 12 | Close out | |

**Every step from 1 to 10** lists the step's comment lines with file and line number, reads all of
them, reads the code beneath each comment whose claim is not plain, rewrites, and runs checks 1 to 6.
**Done when** the six checks pass and the report lists each comment that kept a history word or an
open-issue citation, with the reason.

Notes on particular steps:

- **Step 0.** Snapshot every file. Check the compare script on a scratch copy outside the repo: it
  must flag a changed token, a deleted code line and a joined line, and pass a comment-only edit.
  Configure with `-DFLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON`, build, and run ctest.
  **Done when** the script passes its own check, the build prints no warning, and ctest is 55 of 55.
- **Step 3.** The block at the top of `dag_aad.hpp` is the one `CLAUDE.md` points at: it keeps its
  content.
- **Step 7.** `test_dag_graph.cpp` and `test_engine_stateful_discovery.cpp` grew most since the last
  sweep and get the slowest read.
- **Step 10.** `check_presets.cmake` and `check_platform.cmake` lose their pointers to
  `docs/plans/`. Where a pointer is the only place a rule is defined (`check_presets.cmake` calls
  one rule `R8`), the script states the rule itself.
- **Step 11.** `tests/check_comment_refs.cmake` fails on a `flywheel-dag#N` outside an allowlist of
  open issues (31 and 32 at this plan's start) and on `docs/plans` in `include/`, `tests/`,
  `benchmarks/`, `examples/`, `cmake/`, the root CMake files, `.github/` and the tool configs. The
  allowlist is an argument, so fixtures under `tests/comment_refs/` use their own. The script and
  its fixtures build the pattern from pieces so they do not match themselves.
  `CLAUDE.md` gains one line under Conventions (about 250 bytes, within the 6,500-byte budget):
  a comment states the present, cites no closed issue and no plan, and `test_comment_refs` holds it.
  **Tests first:** register the tests and fixtures with the script absent, and watch every one fail;
  then write the script. Fixtures: a clean file, a closed-issue reference in a C++ comment, one in a
  CMake comment, one in a YAML comment, an allowlisted reference (passes), and a plan pointer. Each
  rule is mutated by hand (allowlist ignored, plan rule removed, one directory not scanned, the
  number pattern cut to one digit) and the fixture that names it must fail.
  **Done when** the real tree passes, every mutant is killed, and `CLAUDE.md` is within budget.
- **Step 12.** Run checks 1 to 4 over the whole tree. Record in the Outcome: comment lines and
  characters before and after, references before and after, the issues opened, the comments that
  kept a history word and why. Run the leak audit over the tree. Mark this plan done, and close
  flywheel-dag#71 with a summary comment (no closing keyword in any commit).
  **Done when** the whole-tree checks are clean, the audit passes and the issue is closed.

## Decisions to confirm

Each has a default. Approving the plan as written takes the default.

1. **Closed issues.** Every reference to a closed issue is cut, as flywheel-dag#26 decided, and so
   are the three `docs/plans/` pointers. Open flywheel-dag#31 and flywheel-dag#32 stay where they
   state a limitation.
2. **Scope.** The 94 tracked code and config files. Not `README.md`, `CLAUDE.md` (apart from the one
   line in Step 11), `docs/engine_rules.md`, `docs/releasing.md` or `docs/plans/`: the plans are the
   record.
3. **Reading depth.** Every comment line, not only the 1,633 added since the last sweep.
4. **The guard test.** Step 11 is in. Without it nothing but the written rule stops the references
   from coming back, as they did after flywheel-dag#26. Dropping it ends the plan at Step 10 and
   Step 12.
5. **Push and release.** Each checkpoint is pushed. No release: the comments ship with the next one.

## Not in this plan

- Code, names, test names or test behaviour.
- `README.md`, `docs/` and `docs/plans/`. A wrong fact found in one goes into a `documentation`
  issue.
- A bug found while reading: it gets a `bug` issue and the report names it.

## Self-review — risks and assumptions

- **A cut comment carried a constraint.** The main risk. The rules cut history, retelling and
  closed references, and keep every contract and every non-obvious reason. Check 6 rereads each
  diff, and each commit is at most 824 comment lines.
- **History or sequence.** A test narrates a scenario in the past tense ("the queue was flushed
  twice") and the scenario is the point. The sentence test above decides, and a doubtful one is
  kept and listed in the step's report for Rob to overrule.
- **An edit changes code** (a stray `*/`, a deleted continuation line, a backslash ending a `//`
  line). Check 1 compares the code and check 2 builds with warnings as errors.
- **A claim is judged true and is not.** Reading is the weakest check. A probe settles what reading
  cannot; a claim that cannot be settled is reworded to what can be shown, or raised with Rob.
- **The comment is right and the code wrong.** Handled by the rule above: an issue, not a fix.
- **The guard is strict on purpose.** An open issue cited in a comment needs its number in the
  allowlist, and closing that issue fails the test until the comment and the allowlist change. That
  is the point: the comment states a limitation that no longer exists.
- **A consumer.** A consumer reads the headers at a tag. Comment-only changes alter nothing it
  builds.
- **A downstream name.** None may appear (`CLAUDE.md`). Each comment is read for one, and Step 12
  runs the leak audit.
- **GCC is unverified locally.** Builds here use Apple Clang, which also has `-Wcomment`. CI's GCC
  leg sees the commits when they are pushed; a build in `ubuntu-dev` is left for later.
- **Assumptions:** ctest is 55 of 55 at `6cf5282` (Step 0 confirms); the dependencies are already in
  `build/relwithdebinfo-macos-arm64/_deps`, so no network is needed; the scratch scripts handle every
  comment form in the tree, which check 1's self-test shows.

## Progress

| Step | Status | Notes |
|---|---|---|
| 0 — Baseline and tools | Done | The compare script passes its own check, and 39 translation units preprocess to the same text with every comment blanked. Strict build prints no warning; ctest 55 of 55. |
| 1 — `dag.*`, `dag_async.*` | Done | Four closed-issue references cut and one tense corrected. The citation of flywheel-dag#31 over ValueSlot's thread rule stays. |
| 2 — `dag_engine.*` and neighbours | Done | 24 closed-issue references cut in `dag_engine.*`, `dag_graph.hpp` and `dag_thread_fence.hpp`; two comparisons with the old `std::system` call rewritten as present facts. `dag_state_store.*` needed no change. Strict build prints no warning; ctest 55 of 55. |
| 3 — Replay, utilities, AAD | Done | All 824 comment lines read. One wording changed ("previously computed result" to "cached result", `dag_memoize.hpp`). The twelve files hold no issue reference and no plan pointer. Claims checked against the code: the coordinator's drain sequence, `bfs_upstream` as the only walk the engine uses, the ring buffer's evict-then-push order, the tape's cost figure (3.9 µs against 0.18 µs, measured in `docs/plans/gradient_node_plan.md`). Strict build prints no warning; ctest 55 of 55. |
| 4 — Ops and time series | Done | All 593 comment lines read; nothing to change. No issue reference, no plan pointer, and every test a comment names exists. Code unchanged, so the Step 3 build covers it. |
| 5 — Core tests | Not started | |
| 6 — Async, wake-state and allocation tests | Not started | |
| 7 — Engine, persistence and utility tests | Not started | |
| 8 — Time-series, ops and AAD tests | Not started | |
| 9 — Root build files, CI, benchmarks, examples | Not started | |
| 10 — Test build files, check scripts, consumer, canary | Not started | |
| 11 — `test_comment_refs` | Not started | |
| 12 — Close out | Not started | |
