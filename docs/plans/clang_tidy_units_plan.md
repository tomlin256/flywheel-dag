# clang-tidy units from the targets, held to the compile database

**Status: In progress (2026-11-07).** Approved as written, so D1 to D4 stand at their defaults. Rob asked for flywheel-dag#65 to be
fixed if it still applies. It does: the gate's list still comes from a glob, and it differs from what the tree compiles whenever
the examples or the benchmark are switched off (measured below).

Closes [flywheel-dag#65](https://github.com/tomlin256/flywheel-dag/issues/65).
Follows [`clang_tidy_ci_plan.md`](clang_tidy_ci_plan.md) (the gate in CI) and
[`clang_tidy_workflow_plan.md`](clang_tidy_workflow_plan.md) (the workflow of its own).
Design source: cryptocogs plan 035, D9 ([tomlin256/cryptocogs#34](https://github.com/tomlin256/cryptocogs/issues/34)): its
[module](https://github.com/tomlin256/cryptocogs/blob/a94b0e1/cmake/cryptocogs_clang_tidy.cmake) (the filter and the walk) and
[`check_clang_tidy_units.cmake`](https://github.com/tomlin256/cryptocogs/blob/a94b0e1/tests/clang_tidy_checks/check_clang_tidy_units.cmake).

**Not here:** any change under `include/`, so no tag and no version (`docs/releasing.md`: a test or CI change needs no release);
the checks in `.clang-tidy`, the LLVM major, the workflow's triggers and legs; a baseline file; the root's refusals (a tool that
cannot run, tests off, a generator that writes no database), which stay as they are.

## Problem

The gate registers one `clang_tidy_<stem>` test for each `.cpp` that a glob finds in `tests/`, `examples/` and `benchmarks/`
(`tests/CMakeLists.txt`, `CONFIGURE_DEPENDS`). Nothing holds that list to the units the tree compiles, so the two can differ, and a
unit that is not in the tree's compile database is analysed with flags that clang-tidy guesses from its neighbours' entries, not the
flags a build uses.

### What I checked (2026-11-07, read-only)

On `git archive` copies of `3aa98fd` in the session scratchpad, configured offline (`FETCHCONTENT_SOURCE_DIR_*` pointing at the
Mac tree's fetched sources) with the gate on; the repo and its trees were not touched. CMake 4.4.5, Homebrew clang-tidy 23.1.4.

| Option set | Units in `compile_commands.json` | Per-unit tests registered | Registered and not built |
|---|---|---|---|
| defaults | 36 | 36 | none |
| `-DFLYWHEEL_DAG_BUILD_EXAMPLES=OFF` | 34 | 36 | `aad`, `quickstart` |
| `-DFLYWHEEL_DAG_BUILD_BENCHMARKS=OFF` | 33 | 36 | `bench_hot_path`, `bench_report`, `test_bench_report` |
| both off | 31 | 36 | those five |

(A unit is a `.cpp` under the checkout, less the build tree and anything under `_deps`. `allocation_counter.cpp` is in three
database entries and `bench_report.cpp` in two: one unit each.)

- **An unbuilt unit passes.** In the examples-off tree `clang_tidy_aad` and `clang_tidy_quickstart` ran and passed, on inferred
  flags.
- **The comment over the glob is false.** It says a file no target compiles "has no compile command, and fails". A clean
  `tests/stray_unit.cpp` that no target compiles passed (exit 0), and one with a `bugprone-use-after-move` finding failed: it is
  linted on inferred flags, and nothing says it is not built. Step 3 rewrites the comment.
- **A list taken from the targets is the database's list.** A walk over `BUILDSYSTEM_TARGETS` and `SUBDIRECTORIES` from the root,
  run at the end of the root `CMakeLists.txt`, less the same exclusions, was identical to the database's units in all four option
  sets with Ninja and with Unix Makefiles (the Mac's default generator): 36, 34, 33 and 31.
- **Configure-based tests are cheap.** A scratch configure with the gate on takes 0.8 s offline and compiles nothing.
- **Baseline.** Mac tree, gate off: 77 tests. Gate on: 41 `clang_tidy_` tests (36 units, `version`, `checks`, three canaries).
- **Where the gate is named.** Its registration is `tests/CMakeLists.txt` lines 437 to 527. `check_ci_clang_tidy.cmake` reads the
  major from there; the root, `.clang-tidy`, `clang-tidy.yml` and `CLAUDE.md` name the file; the README says "one test per source
  file". `CLAUDE.md` is 6,478 of its 6,500 bytes.

## The change

### 1. `cmake/flywheel_dag_clang_tidy.cmake` (new): the filter, the walk, the registration

The gate's registration moves here whole, so that it can be called after the last `add_subdirectory` (its units are read from the
targets), and so that the registration and the check call one filter.

| Function | Does |
|---|---|
| `flywheel_dag_clang_tidy_select(<root> <build> <candidates> <units> <problem>)` | The units among absolute candidate paths: `.cpp`, `.cc` or `.cxx`; under `<root>`; not under `<build>`; no path component called `_deps`. Normalised, unique, sorted. `<problem>` is empty, or says why: no unit at all, or two units that would be one test name (`tests/a.cpp` and `examples/a.cpp` are both `clang_tidy_a`). It reads no disk, so `cmake -P` runs it. |
| `_flywheel_dag_clang_tidy_walk(<directory> <out>)` | The absolute sources of the executable and library targets made in `<directory>` and every directory below it. Interface and custom targets are skipped, and so is a source inside a generator expression, which cannot be read at configure time (`clang_tidy_units` is what notices one). Needs a project. |
| `flywheel_dag_clang_tidy_add_tests(<tool> <root> <extra>)` | Registers `clang_tidy_<stem>` for each unit, `clang_tidy_version`, `clang_tidy_checks`, the three canaries as they are today, and the new `clang_tidy_units`. The arguments (`--warnings-as-errors=*`, the deprecation override, the macOS SDK, the caller's list) are assembled as they are today. |

The root's tool checks stay where they are. Its last lines call the module:

```cmake
if(_flywheel_dag_clang_tidy)
    include(${CMAKE_CURRENT_LIST_DIR}/cmake/flywheel_dag_clang_tidy.cmake)
    flywheel_dag_clang_tidy_add_tests("${_flywheel_dag_clang_tidy}" "${CMAKE_CURRENT_LIST_DIR}"
                                      "${FLYWHEEL_DAG_CLANG_TIDY_ARGS}")
endif()
```

`tests/CMakeLists.txt` loses the block (lines 437 to 527). The major (`set(_flywheel_dag_clang_tidy_major 23)`) moves with it, and
`check_ci_clang_tidy.cmake` reads it there.

### 2. `clang_tidy_units` and `tests/check_clang_tidy_units.cmake` (new)

A gate test, run as `cmake -DSOURCE_DIR=<repo> -DBUILD_DIR=<tree> "-DREGISTERED=<unit>|<unit>|…" -P check_clang_tidy_units.cmake`;
the registration hands it its list, bar-separated. It reads the tree's `compile_commands.json` at test time, runs the module's
`select` on the entries' `file` paths with the same root and build tree, and fails in either direction, naming the units:

- in the database and not registered: a target made after the registration call, or a source the walk could not read;
- registered and not in the database: a unit the tree does not build, which clang-tidy would give guessed flags.

A missing database, a missing argument and a gate over no unit at all are errors that say so. `clang_tidy_` is the gate's prefix,
so the lint workflow's `-R '^clang_tidy_'` runs it with no change to the workflow.

### 3. Tests (always on, no clang-tidy needed)

**`test_clang_tidy_module`** (`tests/check_clang_tidy_module.cmake`, registered at the top level; no configure, about a second):

- *The filter's table.* Extensions (`.cpp`, `.cc`, `.cxx` in; `.hpp`, `.inl`, `.txt` out); under the root as a path component
  (`/repo2/x.cpp` is not under `/repo`) and a trailing slash on the root; under the build tree; `_deps` as a component anywhere
  below the root and not as a substring (`my_deps/a.cpp` and `_deps_a.cpp` are units); a `..` segment; duplicates; the order;
  no unit; a name collision, naming both files.
- *The units script, run as the gate runs it* against compile databases the driver writes: accepted (including an entry under the
  build tree, one under `_deps`, one outside the root, and one file in three entries); a unit in the database and not registered;
  a unit registered and not in the database; both at once; no database; each missing argument; an empty database.

**`test_clang_tidy_wiring`** (`tests/check_clang_tidy_wiring.cmake`, registered at the top level when the generator is Makefiles
or Ninja, as `test_warning_flags` is; configures and runs `ctest -N`, compiles nothing, needs no network):

- *A stub project* the driver writes, with the tool `sh` (never run) and copies of the real fixtures. Its units: an executable, a
  static, a shared, a module and an object library, one source compiled by two targets, and a target in a subdirectory. Its
  sources that are not units: one generated into the build tree, one that only a custom target lists, one under a `_deps` in the
  stub's root and one outside the root. `ctest -N` must list exactly the units and the six fixed names; `clang_tidy_units` must pass
  in that tree. Two variants of the stub must make `clang_tidy_units` fail, naming the file: a target made after the registration
  call, and a source inside a generator expression.
- *The real root*, with the tool `sh`, in the four option sets of the table above. The expected set comes from the checkout, not
  the module: every `.cpp` directly in `tests/`, and in `examples/` and `benchmarks/` when their option is on. `ctest -N` must list
  those and the six fixed names, nothing else; `clang_tidy_units` must pass in each tree; and one unit's command, `clang_tidy_version`,
  `clang_tidy_checks` and the canaries must carry what the registration gives them today (read from the tree's
  `CTestTestfile.cmake`), so that the move is pinned.

A `.cpp` in those directories that no target compiles is in the oracle and not in the registration, so it fails
`test_clang_tidy_wiring`, naming it (D4).

### 4. Docs

The README's "clang-tidy" says what a unit is, that `clang_tidy_units` holds the list to the database, and where a local clone of
a dependency goes (outside the checkout or under a directory called `_deps`). The comments that name `tests/CMakeLists.txt` as the
gate's home (root, `.clang-tidy`, `clang-tidy.yml`, `check_ci_clang_tidy.cmake`, the comment over `test_ci_clang_tidy`) and the
`CLAUDE.md` bullet that says where the major moves name the module. `CLAUDE.md` stays inside its budget.

## Decisions to confirm

- **D1 Both halves of the issue, in one change.** The check cannot be green over the glob when an option is off, and the targets
  alone leave the list unheld. *Alternative:* none that closes the gap.
- **D2 The registration moves whole into a module, called at the end of the root.** The issue asks for the walk to run there, and
  one filter must be callable from the registration and from a `cmake -P` check. *Alternatives:* `cmake_language(DEFER)` from
  `tests/CMakeLists.txt`; or `tests/` last in the root's `add_subdirectory` order (see the rejected designs).
- **D3 Tests as in section 3**, with the stub project, and a hand-made change for each rule (Steps 2 and 3). *Alternative:* hand-made
  changes alone, as flywheel-dag#72 had for a YAML check: not enough here, because the generated-source, generator-expression and
  `_deps` rules are reached only by a project that has them.
- **D4 A `.cpp` that no target compiles fails `test_clang_tidy_wiring`, in every tree, naming the file.** Today such a file is linted
  on guessed flags and is neither built nor run, silently (measured above); a forgotten entry in `FLYWHEEL_DAG_TESTS` is the likely
  case. Dropping the glob would otherwise drop even that accidental lint. *Alternative:* not reported (outside the gate and outside
  the build), one oracle row less.

## Steps

Commits go straight to `main`, one per step, each subject in the repo's `type(#N): …` form scoped to flywheel-dag#65, and each is
pushed once its checkpoint is green (the push is not held for CI; runs are read afterwards, and the Outcome says what finished). No
message carries a closing keyword: flywheel-dag#65 is closed by hand in Step 4. A checkpoint is a full build and
`ctest --preset relwithdebinfo-macos-arm64` on the Mac with CI's flags (`-Werror=author -Werror=deprecated
-DFLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON`), no warning. Counts are expected ones, replaced by measured ones in the Outcome. Before each
push the pre-push leak audit is dry-run on the tip, and every reference in a document is `flywheel-dag#N`, never a bare number.
Files that another session left uncommitted are left alone: commits are by explicit path.

### Step 0 — Approval

*Tests:* none (documentation). Commit this plan (`docs: add plan for clang-tidy units from the targets`, scoped to the issue),
push, and comment on flywheel-dag#65 with the path. *Done when:* the plan and the issue link each other.

### Step 1 — Start from green

No commit. *Done when:* the Mac tree builds and `ctest` passes 77 of 77 with no warning, and the last run on `main` is green on
every job. Anything else: stop and report.

### Step 2 — The filter and the units script (test first), one commit

*Tests:* `test_clang_tidy_module` and its `add_test` (new). *Red first:* the driver with no module and no script, expected red
("does not exist"). Then `cmake/flywheel_dag_clang_tidy.cmake` with `select` only, and `tests/check_clang_tidy_units.cmake`, and
green. The gate is untouched in this step (still the glob), so nothing user-visible changes.

*Hand-made changes* (a scratch copy made with `git archive`, each through the real test; a change that does not apply stops the
harness):

| | Change | Expected |
|---|---|---|
| a | `select` also takes headers | red, table |
| b | `select` without the under-root rule | red, table |
| c | `select` without the build-tree rule | red, table |
| d | `_deps` matched as a substring | red, table (`my_deps`, `_deps_a.cpp`) |
| e | `select` without the `_deps` rule | red, table |
| f | no de-duplication | red, table |
| g | no sort | red, table |
| h | a name collision not reported | red, table |
| i | no unit not reported | red, table |
| j | the script ignores a unit that is in the database and not registered | red, script cases |
| k | the script ignores a unit that is registered and not in the database | red, script cases |
| l | the script counts entries under the build tree | red, script cases |
| m | the script passes with no database | red, script cases |
| n | the control, nothing changed | green |

*Checkpoint.* Expected 78 of 78. *Done when:* red then green as stated; a to m red and n green; pushed.

### Step 3 — The walk, the registration moves, `clang_tidy_units` (test first), one commit

*Tests:* `test_clang_tidy_wiring` and its `add_test` (new); `test_ci_clang_tidy` (its path to the major changes). *Red first:* the
wiring driver against the tree as Step 2 left it, expected red (examples off lists `clang_tidy_aad`; no `clang_tidy_units`). Then the
walk, `add_tests`, the root's call, the block gone from `tests/CMakeLists.txt` with a comment that names the module, and green.

*Hand-made changes* (as in Step 2):

| | Change | Expected |
|---|---|---|
| a | the registration back to the glob | red, wiring (examples off) |
| b | the walk does not recurse into subdirectories | red, wiring (stub and real root) |
| c | the walk keeps a source under the build tree | red, wiring (stub) |
| d | the walk takes a custom target's listed source | red, wiring (stub) |
| e | the walk reads a generator-expression source as a path | red, wiring (stub) |
| f | relative sources not made absolute | red, wiring |
| g | the call moved above `add_subdirectory(benchmarks)` | red, wiring (real root: the benchmark's units missing) |
| h | a target made after the call | red, `clang_tidy_units` in the stub, naming the file |
| i | `REGISTERED` left empty | red, `clang_tidy_units` in every tree |
| j | `clang_tidy_units` not registered | red, wiring |
| k | `PASS_REGULAR_EXPRESSION` removed from `clang_tidy_version` | red, wiring (pins the move) |
| l | a `.cpp` added to `tests/` that no target compiles | red, wiring, naming it (D4) |
| m | the major moved to 24 in the module only | red, `test_ci_clang_tidy` |
| n | the control, nothing changed | green |

*Gate on, Mac:* `cmake --preset relwithdebinfo-macos-arm64 -DFLYWHEEL_DAG_CLANG_TIDY=clang-tidy`, then `ctest -R '^clang_tidy_' -j8`:
42 tests, all green, as the 41 were; and `clang_tidy_units` green in the other three option sets (counts 34, 33 and 31).

*Checkpoint.* Expected 79 of 79 gate off. *Done when:* red then green as stated; a to m red and n green; the gate-on run as stated;
pushed; the push's `clang-tidy` run, when it finishes, shows 42 tests per leg and `CI` is read.

*After the push, optional (left for later in the Outcome if skipped):* the same checkpoint and a gate-on run in `ubuntu-dev`.

### Step 4 — Docs, close

*Tests:* none new; `test_claude_md_budget`, `test_comment_refs`, `test_ci_paths` and `test_cmake_floor` read the files edited. The
README, the comments in section 4, `CLAUDE.md`, and this plan's Outcome; the memory note on the gate, if it names the old home.
Editing `clang-tidy.yml` and the README starts both workflows (neither paths list covers them). flywheel-dag#65 is closed by hand
with a comment that lists the commits, the runs, the counts and the red-then-green record. *Done when:* pushed, and the issue is
closed.

## Designs I rejected

- **`clang_tidy_units` over the glob.** The glob has the examples when they are off, so the check would be red by design in those
  trees, and a check that first drops the unbuilt ones from the glob compares the database with itself.
- **The glob, filtered by the options** (`if(FLYWHEEL_DAG_BUILD_EXAMPLES)` around the examples). It writes the root's conditions a
  second time, which is the drift the issue is about.
- **`cmake_language(DEFER)` from `tests/CMakeLists.txt`.** The block could stay, but the arguments it needs cross the deferred call
  as strings, the check still needs a file it can include for the filter, and a reader has to know that the call runs at the end of
  the root.
- **`tests/` last in the root's `add_subdirectory` order.** It moves `include(CTest)` (the examples register tests) and changes what
  every directory sees.
- **One test that reads a generated list** (`file(GENERATE)`) and lints every unit in a loop. It loses the per-unit names and the
  parallelism of `ctest -R` and `-j`.
- **A fixed list of directories for the walk.** The check exists to notice a directory nobody listed.

## Self-review — risks and assumptions

- **A move of about 90 lines is the larger risk than the new logic.** The wiring test pins what the move must keep (the commands
  and the properties of the units, the version, the checks and the canaries), and the gate-on run on the Mac runs all of them. The
  first lint run on each platform is the real test of the Linux half.
- **`add_test` is now called from the root**, so ctest runs the gate's tests in the tree's top directory. Every command in them
  names absolute paths.
- **The walk misses a source inside a generator expression.** `clang_tidy_units` is red and names the file, so it is not silent; the
  fix is to list the source plainly. There is none in the tree.
- **A dependency checked out inside the checkout under a name without `_deps`** is a unit on both sides: extra `clang_tidy_` tests,
  loudly, and the check stays green. The README says where a clone goes.
- **Absolute paths are compared as strings.** Both sides come from CMake's own path of the checkout, so a symlinked checkout
  agrees with itself; the wiring test runs from whatever path ctest gives it.
- **`test_clang_tidy_wiring` configures the real root four times and a stub three times** (expected under 10 s, offline, scratch
  under the build tree) and runs on the `build` job's two platforms. It is registered for Makefiles or Ninja only, because it reads
  a database.
- **D4 makes a stray `.cpp` a red test in every tree**, including a contributor's scratch file in `tests/` until it has a target. That
  is the point; the file can live in a subdirectory (the glob and the oracle are not recursive).
- **Behaviour change:** one new module and two scripts, two new always-on tests and one gate test, a changed registration (units
  from the targets), no header and no engine file.
- **I assume** you want D1 to D4 as written; each is a few lines to change.

## Progress

| Step | Status | Notes |
|---|---|---|
| 0 Approval | Done | Approved as written on 2026-11-07, with the defaults of D1 to D4. |
| 1 Start from green | Done | The last runs on `3aa98fd` (`CI` and `clang-tidy`) were green. The Mac tree at `aed3ff7` built with no warning and passed 77 of 77. |
| 2 The filter and the units script | Done | Red first ("does not exist"), then green: 78 of 78, no warning, 32 cases. Hand-made changes: a to t, 20 runs as expected (n green); o to t are rows the plan did not have. |
| 3 The walk, the registration moves, `clang_tidy_units` | Done | Red first (the stubs fail to configure on the missing `flywheel_dag_clang_tidy_add_tests`; the real roots lack `clang_tidy_units`; the examples-off tree lists `clang_tidy_aad` and `clang_tidy_quickstart`), then green: 79 of 79, no warning, 31 cases. Gate on, Mac: 42, 40, 39 and 37 `clang_tidy_` tests green, with 36, 34, 33 and 31 units that `clang_tidy_units` finds exactly the tree's. Hand-made changes: 27 red and the control green, for the plan's a to m and 14 more (the Outcome lists them). |
| 4 Docs, close | Not started | |
