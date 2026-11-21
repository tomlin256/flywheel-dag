# Options Go to Targets: CMake 4.5 Makes Each Directory-Wide Option an Error Under `-Werror=author`

**Status: Proposed (2026-11-21).**

Closes [flywheel-dag#84](https://github.com/tomlin256/flywheel-dag/issues/84).

## Problem

Every CI job fails at Configure on CMake 4.5.0 (released 2026-11-12). The runners have it on every leg now: CI and
clang-tidy at `d289cdf` (runs 44925848253 and 44925848269) fail on macOS and on Ubuntu, and the nightly Sanitizers run
of 2026-11-21 (44898771459) fails in both modes. The errors are one kind, at six places:

| Where | Command | Fails |
|---|---|---|
| `CMakeLists.txt:148`, `:149` | `add_compile_options`, `add_link_options` (the sanitizer options) | Sanitizers |
| `CMakeLists.txt:293`, `:295` | `add_compile_options` (the warning flags) | CI, clang-tidy, Sanitizers |
| googletest v1.18.0: `googletest/CMakeLists.txt:113`, `googlemock/CMakeLists.txt:75` | `include_directories` | CI, clang-tidy, Sanitizers |

flywheel-dag#84 lists the last four. The Sanitizers log adds the first two.

**Why.** CMake 4.5 added the diagnostic category `CMD_NON_TARGET_DIRECTIVE`. Seven commands report it when a target
command could do the job: `add_compile_definitions`, `add_compile_options`, `add_definitions`, `add_link_options`,
`include_directories`, `link_directories` and `link_libraries` (the commands whose sources name it in Kitware's v4.5.0).
It is off by default. Its parent is the new `CMD_STRICT` ("allowed but discouraged usage", also off), whose parent is
`CMD_AUTHOR`. `-Werror=author` promotes a category and every child of it (cmake-diagnostics(7)), so the workflows'
`-Werror=author -Werror=deprecated` turned a default-off advice into an error at each of the six places.

**What the fix cannot be.** CMake 4.4.5, which this Mac and `ubuntu-dev` have and some runner legs still get
(flywheel-dag#84), rejects 4.5's names. Measured on 4.4.5: `-Wno-error=non-target-directive` exits 1 with `The warning
category "non-target-directive" is not known.`, and `cmake_diagnostic(DEMOTE CMD_NON_TARGET_DIRECTIVE WARN)` in a file
stops with `unrecognized diagnostic category`. So no flag in a workflow answers it, and no call in a CMake file does
without a version test. A local run on 4.4.5 cannot show the problem either.

**googletest is not ours to fix.** v1.18.0 is the latest release (2026-08-10) and its `main` has the same two lines at
the same places, so a bump changes nothing.

## The change

One new module, three edits to the root `CMakeLists.txt`, three tests. Nothing in a workflow, a preset, a header, an
example or the install changes, and the strict flags stay spelled as they are.

**1. Options go to targets.** `cmake/flywheel_dag_target_options.cmake` holds one function:

```cmake
flywheel_dag_add_target_options([IN <directory>...] [COMPILE <option>...] [LINK <option>...])
```

It visits every target that the current directory and its subdirectories made (`SUBDIRECTORIES`, then
`BUILDSYSTEM_TARGETS`), and skips aliases, imported targets, INTERFACE libraries and custom targets. `COMPILE` goes to
executables and to static, shared, module and object libraries as `PRIVATE` compile options. `LINK` goes to
executables, shared libraries and modules as `PRIVATE` link options. With `IN`, only the named subdirectories of the
current one, and what they added, are visited, and a directory it never added contributes nothing, which is what a
tree with the tests off needs. A call with neither `COMPILE` nor `LINK`, an unknown keyword or an empty option list is a
`FATAL_ERROR` that names it. Nothing in it is a non-target directive.

**2. The root calls it twice, after its last `add_subdirectory`** (top level only, as today):

- the warning flags, `IN tests examples benchmarks`: this project's own targets, which is what `test_warning_flags` and
  `test_warning_option` define as "ours";
- the sanitizer options, over the whole tree: the fetched dependencies' targets too, which `test_sanitizer_coverage`
  requires.

The paragraph that says the sanitizer options must sit before the first `FetchContent_MakeAvailable`, and the one that
says the warning flags must sit after the last, go: neither order matters to a function that runs on a finished tree.
Nothing goes on `flywheel_dag`'s INTERFACE and nothing in `CMAKE_CXX_FLAGS` or the cache, as now.

**3. googletest's two lines stay visible and stop failing.** On CMake 4.5 or later the category is demoted to a warning
(`cmake_diagnostic(DEMOTE CMD_NON_TARGET_DIRECTIVE WARN)`, never `IGNORE`) for the dependencies this project fetches, and
not for its own files. The demotion is a no-op where the category is already off, which is every configure without
`-Werror=author`. The placement is the one thing Step 1 measures. Two candidates:

- **(a) Around `include(FetchContent)`.** A macro runs with the diagnostics recorded where it was defined
  (`Source/cmMacroCommand.cxx`, v4.5.0), and `FetchContent_MakeAvailable` adds the subdirectory through macros defined
  at include time (`Modules/FetchContent.cmake`). So the demotion has to be in force when the module is included, and it
  then covers every dependency fetched afterwards. Three lines and a comment.
- **(b) Around a hand-made `add_subdirectory` of googletest.** `SOURCE_SUBDIR` naming no directory makes
  `FetchContent_MakeAvailable` populate without adding (its documented populate-only use), and a
  `block(SCOPE_FOR DIAGNOSTICS)` holds the demotion around our own call. It covers googletest alone, and costs the extra
  call in both branches of the version test.

Default (a). It is (b) if Step 1 shows that (a) does not reach googletest. Either way the test below fails when the
demotion goes.

**Cost.** None at run time: no header changes. The compile and link lines of every unit keep the same arguments, only
the position of the options within a line may differ, and Step 3 shows that per unit for the default, `asan` and `tsan`
trees.

## Decisions to confirm

Each is a default to edit before approving.

- **D1. A function visits the finished tree** (above). One mechanism for the warnings, which must stay off the
  dependencies, and for the sanitizers, which must reach them. A new target of this project's directories is covered
  with no line of its own, which is what `compiler_warnings_plan.md` (flywheel-dag#4) wanted when it rejected an INTERFACE
  target for needing one line per target.
- **D2. The one `CMAKE_VERSION` test.** Item 3 needs `NOT CMAKE_VERSION VERSION_LESS 4.5`, because 4.0 to 4.4 have no
  such category. Your rule makes that your call, and the other way is to raise the floor to 4.5 and drop the test. I do
  not recommend it: the floor would refuse 4.4.5, which is this Mac, `ubuntu-dev` and, today, some runner legs.
- **D3. One push, after Step 6.** It starts the four jobs of CI and clang-tidy. The plan pushes once, never forced,
  looks at the runs when they have finished, and never waits for them. Say "no push" and the work stays local: the
  Outcome then says CI was not consulted, and the nightly Sanitizers run stays red.

## Steps

Every commit subject is scoped to flywheel-dag#84 in the repo's `type(#N): …` form. The subjects below omit the scope.
Every checkpoint ends with a full build and a full `ctest` run on the Mac (CMake 4.4.5, CI's flags), and nothing is
pushed until Step 6.

### Step 0 — Approval

This file is committed as Proposed and linked from flywheel-dag#84 (the issue gets a comment with the path and the two
Sanitizers lines). Approval and the decisions are recorded in a second commit.

**Done when:** `git status` is clean and flywheel-dag#84 carries the link. Commits: `docs: add plan for options that go
to targets`, then `docs: record the approval in the plan`.

### Step 1 — Measure on CMake 4.5.0 (no repo change)

Kitware's `cmake-4.5.0-macos-universal.tar.gz` and `cmake-4.5.0-linux-aarch64.tar.gz` are downloaded and their SHA-256
sums match `cmake-4.5.0-SHA-256.txt` (both are in the scratchpad, in a directory of their own). Unpacking and running
the macOS one was refused by the permission classifier once, so this step waits for a "try again".

- **1a. The red, reproduced.** The unchanged tree on 4.5.0 with CI's flags, in trees whose names claim no platform:
  default, `asan` and `tsan`. Expected: four errors, and six in the sanitizer trees, at the lines in the table.
- **1b. The baseline.** On the unchanged tree and 4.4.5 (offline, nothing compiled), the `compile_commands.json` and every
  `link.txt` of the default, `asan` and `tsan` trees, reduced to a file and its sorted arguments. Step 3 compares
  against them.
- **1c. The probes**, in a stub project, on 4.5.0 and on 4.4.5: the function's walk (nested directories, a dependency
  in a sibling directory); and, for item 3, which of (a) and (b) makes a stub dependency that calls
  `include_directories()` a warning under `-Werror=author`, with the same stub on 4.4.5 untouched.

**Done when:** each reading is in the plan's Outcome, and (a) or (b) is chosen. Anything that contradicts the Problem
section above: stop and report.

### Step 2 — The module and its test

`cmake/flywheel_dag_target_options.cmake`, `tests/check_target_options.cmake`, and `test_target_options` in
`tests/CMakeLists.txt` beside the other top-level checks. The test writes a stub project under `SCRATCH_DIR` (own
directories two deep, a dependency directory, one target of every type), includes the module from the checkout, calls the
function, and reads the targets' `COMPILE_OPTIONS` and `LINK_OPTIONS`. It needs no network and compiles nothing.

| Test | Asserts |
|---|---|
| T1 `test_target_options` (new) | with `IN`, own targets at every depth have the compile options and a dependency's have none |
| | with no `IN`, every real target has them |
| | `LINK` reaches executables, shared libraries and modules, and not static or object libraries |
| | an INTERFACE library, an imported target, an alias and a custom target are left alone |
| | a directory named in `IN` that was never added contributes nothing and is no error |
| | a target made after the call has no option (the rule the root's comment states) |
| | no `COMPILE` or `LINK`, an unknown keyword and an empty list each stop, naming themselves |
| | on CMake 4.5 or later the stub configures with `-Werror=author -Werror=deprecated`, which proves the module uses no non-target directive; before 4.5 that case is skipped and the status line says so |
| the other tests (existing) | unchanged, and pass |

**Done when:** T1 is red before the module exists, for the right reason; it passes on 4.4.5 and on 4.5.0; and each
hand-made change below, made on a scratch copy of the module and never restored, fails exactly the case it pins:

| Change | Fails |
|---|---|
| M1 `IN` ignored, so every target is visited | a dependency's target got an option |
| M2 `LINK` also given to static libraries | static libraries have no link option |
| M3 INTERFACE libraries not skipped | the stub does not configure |
| M4 no descent into subdirectories | the target two deep |
| M5 an unadded directory in `IN` is an error | an unadded directory contributes nothing |
| M6 the module calls `add_compile_options` | the strict configure of the stub (4.5.0) |

Commit: `feat: add flywheel_dag_add_target_options, which gives options to targets`.

### Step 3 — The root uses it, for the warnings and the sanitizers

The four lines (`148`, `149`, `293`, `295`) become the two calls of item 2, and the comments that described the old
order are rewritten to what is true now: in the root, in `cmake/flywheel_dag_sanitize.cmake`, and in
`tests/check_sanitize_option.cmake` ("and before it makes the dependencies").

**Done when:**

- `test_warning_flags`, `test_warning_option`, `test_sanitize_option`, `test_sanitize_flags` and
  `test_consumer_subproject` pass unchanged, and in an `asan` tree `test_sanitizer_coverage` and the canaries do;
- the reduced compile and link lines of the default, `asan` and `tsan` trees equal Step 1b's, unit for unit;
- a configure on 4.5.0 with CI's flags has only googletest's two errors left;
- the configure's wall time before and after, on the same tree, is in the Outcome.

Commit: `fix: give the warning and sanitizer options to targets, not the directory`.

### Step 4 — googletest, the strict-configure test, and the gate probe

The demotion of item 3 at the placement Step 1 chose, behind the version test. Two tests:

| Test | Asserts |
|---|---|
| T2 `test_non_target_directives` (new), scan | no CMake file of the checkout outside a build tree (the code, comments blanked, as `check_fetch_declarations.cmake` reads it) calls one of the seven commands; a hit names `file:line`; fewer than three files read fails, so a moved layout cannot pass by reading nothing |
| T2, behaviour (CMake 4.5 or later) | the real project, offline, configured with `-Werror=author -Werror=deprecated` in the default, `asan` and `tsan` configurations, exits 0 and prints no `CMake Error`; googletest's two lines are `CMake Warning (non-target-directive)` |
| T2, before 4.5 | the behaviour cases are skipped, and the status line says so; the scan still runs |
| T3 `test_cmake_diagnostics` (existing, three cases added), CMake 4.5 or later | a stub project that calls `add_compile_options` exits non-zero with `CMake Error (non-target-directive)` under the strict flags, exits 0 and silent without them, and a stub that uses `target_compile_options` exits 0 under them; before 4.5 these cases are skipped |

T3 holds the gate, not the repo, as the file's other cases do: without it a CMake that stops reporting the category
would leave T2's behaviour cases green and proving nothing.

**Done when:** T2 is red on the unchanged tree, on 4.5.0, for the right reason (the scan names the four lines of the
root; the behaviour cases fail with the CMake errors); the strict configure of the final tree exits 0 on 4.5.0 in all
three configurations; the same configure on 4.4.5 prints what it printed before; T3 passes on 4.5.0 and skips on 4.4.5;
and each hand-made change below, on a scratch copy of the checkout, fails exactly the case it pins:

| Change | Fails |
|---|---|
| M7 `include_directories(.)` added to `examples/CMakeLists.txt` | T2 scan, naming `examples/CMakeLists.txt` and the line |
| M8 the demotion removed | T2 behaviour, naming googletest's two lines (4.5.0) |
| M9 `add_compile_options(-Wall)` back in the root | T2 scan, and T2 behaviour (4.5.0) |
| M10 the strict flags in T3 spelled `-Werror=autor` | T3 (4.5.0): CMake rejects the category |

Commit: `fix: keep googletest's non-target directives warnings, and test the strict configure`.

### Step 5 — Docs

One line in `CLAUDE.md` (an invariant the code cannot carry: options go to targets, and `test_non_target_directives`
holds it), within `test_claude_md_budget`; the README's strict-configure paragraph says that 4.5's `strict` group is part
of `-Werror=author`; this plan's Status, Progress and Outcome. Comments cite no issue and no plan.

**Done when:** the budget, `test_comment_refs` and the rest of the suite pass. Commit: `docs: say that options go to
targets, and mark the plan done`.

### Step 6 — Verify before the push

No commit. Everything below runs on the final tree, and a failure stops the plan and goes to you.

- **Mac, 4.4.5:** the preset tree, full build and `ctest`, as at every checkpoint, plus the benchmark invariants diff.
- **Mac, 4.5.0:** a scratch tree (a name that claims no platform) configured by the 4.5.0 binary with CI's flags: full
  build, `ctest` (so T2's behaviour cases run), and the benchmark invariants diff.
- **`ubuntu-dev` (GCC 13.3, CMake 4.4.5):** the preset tree, full build and `ctest`; an `asan` tree configured, with the
  canaries built and `ctest -R "sanitiz"` run. A full run of the sanitized suite is left for later.
- **CI's Ubuntu leg, as nearly as a laptop can:** a throwaway `ubuntu:26.04` container (GCC 15.2) with the 4.5.0 Linux
  tarball, CI's two configure flags, build, `ctest` and the benchmark diff. Flywheel-dag#80's commits have had no GCC verdict yet, so
  this is where one would show. A finding there is its own issue, and the plan stops to say so.

**Done when:** all four are green, and the Outcome records each tool version.

### Step 7 — Push once, and look at the runs

`git fetch`, then one push of Steps 0 to 5 (never forced). The runs are read in passing when they have finished. The
issue is closed by hand, after a closing comment, only when I have seen both CI and clang-tidy finish green; otherwise it
stays open and the Outcome says what I saw and did not see. The nightly Sanitizers run is read the next time it runs.

## Designs I rejected

- **`-Wno-error=non-target-directive` (or `=strict`) in the workflows.** CMake 4.4 exits 1 on a category it does not
  know (measured), and the runners' CMake varies. It would also make this project's own uses non-fatal.
- **`set_property(DIRECTORY … COMPILE_OPTIONS …)` or `set_directory_properties`.** The same directory-wide effect, which
  CMake does not report today: it dodges the diagnostic and keeps the smell it names.
- **A normal `CMAKE_CXX_FLAGS` (and linker flags) in the root.** Reaches every `try_compile`, so `-Werror` would break a
  configure check, and it reverses `sanitizer_ci_plan.md`'s decision (flywheel-dag#45) to keep the options out of
  `CMAKE_CXX_FLAGS`.
- **An INTERFACE `warnings` target that each target links.** One line per target, which was the objection when the flags
  were first added, and it cannot reach a dependency's targets, which the sanitizers need.
- **`IGNORE` in place of `WARN` for googletest.** It would hide the report. A warning in the log is the honest state of
  a dependency we cannot change.
- **Re-implementing googletest's targets**, **pinning the runners' CMake** and **bumping googletest**. The first
  re-implements a dependency, the second adds a workflow leg for a CMake version and hides the next category, and the
  third changes nothing (above).

## Not in this plan

- **A release.** No header, example or install rule changes.
- **The sanitized suite in `ubuntu-dev`**, beyond the canaries and the coverage test.
- **Moving the floor** (D2).
- **A GCC 15 finding in flywheel-dag#80's code**, if Step 6 shows one: its own issue.

## Self-review — risks and assumptions

- **Step 1 needs a binary that was refused once.** Without it nothing here is measured on 4.5, and the plan stops at
  Step 1. The Kitware tarballs are checked against the release's SHA-256 file; the Homebrew 4.5.1 bottle is a different
  patch release and not what the runners have.
- **The function's reading of CMake's own lists.** It relies on `SUBDIRECTORIES`, `BUILDSYSTEM_TARGETS` and a target's
  `TYPE`, all of which the stub project in T1 pins on both CMake versions. A directory that a dependency adds through a
  macro of its own is still a subdirectory of its parent, so it is visited.
- **Where the options sit on a line changes.** A target's own options now come before ours, where the directory's came
  before them. Step 3 compares the sorted arguments, which proves the set, not the order. No flag here depends on
  order (no `-Wno-…` follows a `-W…`), and `bench_hot_path`'s `-ffp-contract=off` is a target option either way.
- **A target made after the call has no options.** The root says so where it calls, beside the existing rule that no
  target is made below the clang-tidy block, and `test_warning_flags` and `test_sanitizer_coverage` fail on a unit
  that lacks them.
- **The demotion in (a) covers every dependency fetched after the include**: spdlog and nlohmann/json too. Their reports
  stay visible as warnings. Under (b) only googletest's is demoted. Both are `WARN`, never `IGNORE`.
- **The strict group is a moving target.** A later CMake that adds a child of `CMD_STRICT` or `CMD_AUTHOR` can turn CI
  red again, which is what the gate is for. T2's scan knows the seven commands of 4.5.0, and T2's behaviour cases are
  what finds an eighth.
- **Before 4.5 T2's behaviour cases and T1's strict case are skipped.** On this Mac and in `ubuntu-dev` (4.4.5) only the
  scan and the effects run. Step 6 runs them on 4.5.0 by hand, and CI runs them wherever a runner has 4.5.
- **A runner can still change CMake between two jobs of one push.** Both paths are exercised: 4.4.x skips the demotion,
  4.5.x applies it.
- **Assumptions:** the runners' CMake is 4.5.0 or a patch of it; googletest keeps its two lines; the sanitizer
  nightly's configure is what `check_ci_sanitizers.cmake` reads; ctest runs under the CMake that configured the tree,
  which is why Step 6's 4.5.0 tree is a tree of its own.

## Progress

| Step | Status | Notes |
|---|---|---|
| 0 Approval | Proposed | |
| 1 Measure on CMake 4.5.0 | Not started | blocked on a "try again" |
| 2 The module and its test | Not started | |
| 3 The root uses it | Not started | |
| 4 googletest, the strict-configure test, the gate probe | Not started | |
| 5 Docs | Not started | |
| 6 Verify before the push | Not started | |
| 7 Push once, and look at the runs | Not started | |
