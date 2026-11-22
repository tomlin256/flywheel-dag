# Options Go to Targets: CMake 4.5 Makes Each Directory-Wide Option an Error Under `-Werror=author`

**Status: Done and pushed (2026-11-22). Approved (2026-11-21) as written; D1 to D3 stood at their defaults.** Steps 0 to 6
are done, and Step 7's one push carries them and this commit. The CMake 4.5.0 binary, refused once by the permission
classifier, ran after a "try again". What the CI runs did is on flywheel-dag#84, not here. The Progress table says what
each reading was, and the Outcome sums them up.

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

**Step 4 is two commits.** The scan needs no CMake 4.5, and the rest does, so the scan is done first (4a) and
the demotion, T2's behaviour cases and T3 wait for the binary of Step 1 (4b). 4a differs from the table above in
four ways, each found while writing it: the scan reads `*.cmake.in` too (the package config template is a CMake
file a consumer's configure runs); it takes `-DMIN_FILES` (3 for the checkout) so that its own fixtures can use small
floors; it does not read two files, itself and `tests/check_non_target_directives_fixtures.cmake`, which hold the seven
names as text (a script run by `cmake -P` cannot call them); and its own behaviour is held by a second test,
`test_non_target_directives_fixtures`, which writes trees at run time and compares the scan's report with the calls
they hold, exactly. 4b adds `tests/check_cmake_diagnostics.cmake` to the files the scan does not read if T3's stubs
hold the names as text.

**4b differs from the table in four ways.** The behaviour cases are their own test, `test_strict_configure`
(`tests/check_strict_configure.cmake`), since they need the dependencies' sources and a compiler, where the scan needs
nothing. They hold three cases more than the table lists: with no flags the configure says nothing about the category;
a copy of the root with a directive of its own at the end still fails with exactly that one error, which pins the
demotion's scope; and a parent project that pulls the engine in and then fetches a dependency that makes a directive
still gets that error, which pins the top-level condition (Step 1c measured the leak that the condition closes). The
demotion is `(a)` at the top level only. And the scan does not read four files, not two: each of the two new scripts
holds a directive's name as text that it writes into a project.

### Step 5 — Docs

One line in `CLAUDE.md` (an invariant the code cannot carry: options go to targets, and `test_non_target_directives`
holds it), within `test_claude_md_budget`; the README's strict-configure paragraph says that 4.5's `strict` group is part
of `-Werror=author`; this plan's Status, Progress and Outcome. Comments cite no issue and no plan.

**Done when:** the budget, `test_comment_refs` and the rest of the suite pass. Commit: `docs: say that options go to
targets, and mark the plan done`.

**Step 5 differs in two ways.** `CLAUDE.md` gets no line. It stands at 6469 of the 6500 bytes that `test_claude_md_budget`
allows, and the rule is carried where the code is: the root's comment at the call, the module's header, and
`test_non_target_directives`, whose refusal names the target commands and the function that replace the seven. Room
for a line would mean trimming lines this plan does not touch, or raising the budget, which is your call; say so and
it is one more commit. And the README's paragraph is written in two parts, so that it is true at each commit: the
options now, and the sentence on googletest's warnings in 4b's commit, which is where the demotion it describes
appears. The plan's Status and Outcome are written last.

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
- **(a) redefines the parent's macros when this project is a subproject.** `FetchContent.cmake` has no include guard, so
  an include in a subproject defines `FetchContent_MakeAvailable` again, and every later call of it, the parent's too, runs
  with the diagnostics recorded at that include (read in the v4.5.0 sources, not measured). (a) is therefore applied at the
  top level only, where there is no parent and where the tests, and so googletest, are on by default. Step 1c checks that
  a subproject's configure leaves the parent's strictness as it was.
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
| 0 Approval | Done | Proposed in `0aaa610` and linked from flywheel-dag#84. Approved as written; D1 to D3 at their defaults (one push after Step 6). |
| 1 Measure on CMake 4.5.0 | Done | The tarballs match `cmake-4.5.0-SHA-256.txt`, and after the "try again" the macOS one was unpacked into a directory of its own and runs (`cmake version 4.5.0`). **1a, the red:** `origin/main` (`d289cdf`) configured offline on 4.5.0 with CI's two flags, in three trees that claim no platform. The default tree prints four `CMake Error (non-target-directive)`: googletest's `googletest/CMakeLists.txt:113` and `googlemock/CMakeLists.txt:75`, and the root's `CMakeLists.txt:293` and `:295`. The `asan` and `tsan` trees print six, with the root's `:148` and `:149` too. That is the Problem section's table, line for line. **1c, the probes** on 4.5.0, in a stub project whose dependency calls `include_directories`, under CI's two flags: with no demotion the dependency is an error. With (a), a `block(SCOPE_FOR DIAGNOSTICS)` around `include(FetchContent)` that demotes the category, it is a warning and the configure exits 0; with (b), populate only and our own `add_subdirectory` in such a block, the same. In both, the parent's own `add_compile_options` stays an error, and with no strict flags nothing is reported. On 4.4.5 every probe exits 0 and says nothing. **The leak is real:** a subproject that applies (a) makes the parent's later `FetchContent_MakeAvailable` report a warning where the parent had an error. **Chosen: (a), at the top level only.** 1b: three offline trees on the unchanged root, CMake 4.4.5, CI's flags: 68, 70 and 69 compile units and 48, 50 and 49 link files for the default, `asan` and `tsan` trees; the configure takes 0.7 s in each. Earlier, on 4.4.5 only: the probe of the walk gave the options to the targets of one directory tree and to no other, nested directories included, and 4.4.5 rejects 4.5's category name in a file and on the command line (the Problem section). Read in Kitware's v4.5.0 sources and manuals (kept in the scratchpad), and then measured by 1c: `endmacro()` records every category's action and the macro runs with that record, whoever calls it (`cmMacroCommand.cxx`, `cmMakefile::RecordDiagnostics`, `PushMacroScope`); `FetchContent_MakeAvailable` adds a subdirectory through `__FetchContent_MakeAvailable_add_subdirectory`, a macro defined when the module is included; the module has no include guard; the manual says the command line sets the initial state of the stack and `cmake_diagnostic` alters its top. So (a) should reach googletest, which 1c confirmed. |
| 2 The module and its test | Done | T1 was red before the module existed, with "cmake/flywheel_dag_target_options.cmake does not exist". It passes on 4.4.5 with 110 cases (the strict case is skipped there) and on 4.5.0 with 111. It grew two cases the plan did not list: a target's interface options stay empty (the options are PRIVATE), and `IN` naming a directory and one below it gives a target its options once. Hand-made changes on a scratch copy of the module, each failing the case it pins: `IN` ignored (M1), `LINK` on static libraries (M2), INTERFACE libraries not skipped (M3), no descent (M4), an unadded directory not skipped (M5), `PUBLIC` for `PRIVATE` (M7), no de-duplication (M8), a mistyped directory accepted (M9), an unknown word accepted (M10), no option at all accepted (M11), an empty keyword accepted (M12), and an `add_compile_options` call in the function (M13: on 4.4.5 the `late` case sees the option reach a target made after the call, and on 4.5.0 the strict configure sees it too, so both cases fail there, which is the plan's M6). Full build and `ctest` on the Mac: 151 of 151. |
| 3 The root uses it | Done | The root reads and checks the sanitizer mode where it did, before anything is fetched, and gives the options to targets after its last `add_subdirectory`: the sanitizer options over the whole tree, then the warning flags to `tests`, `examples` and `benchmarks`, in the order the directory options had. Reduced compile and link lines of the default, `asan` and `tsan` trees (68, 70 and 69 units; 48, 50 and 49 link files) are identical to Step 1b's, unit for unit, and so is the text of each configure. The units of the `asan` tree: 59 of this project's and 11 of dependencies, all 70 with the sanitizer options and only the 59 with `-Wall` and `-Werror`. Argument order is the same too, but for `bench_hot_path`, whose own `-ffp-contract=off` now comes before the options and conflicts with none. Configure wall time, before and after, default / `asan` / `tsan`: 0.699, 0.705, 0.699 s and 0.704, 0.703, 0.703 s. `test_warning_flags`, `test_warning_option`, `test_sanitize_option`, `test_sanitize_flags`, `test_consumer_subproject` pass unchanged; in `asan` and `tsan` trees (Mac, Apple Clang) the canaries, `test_sanitizer_coverage` and `test_sanitizer_scripts` pass. Hand-made changes to a scratch copy of the root, each failing the checks that pin it, and the unchanged copy passing all five: sanitizer options for own targets only (R1), warning flags for the whole tree (R2), `tests` added below the call (R3), no sanitizer link options (R4), no warning call (R5), no `-Werror` (R6), no sanitizer call (R7). Full build (nothing rebuilt, which is the same fact again) and `ctest` on the Mac: 151 of 151. On 4.5.0, with CI's flags, the configure of the changed tree in the three trees leaves only googletest's two errors: the root's four, and six in the sanitizer trees, are gone. |
| 4 googletest, the strict-configure test, the gate probe | Done | 4a: `test_non_target_directives` (the scan) and `test_non_target_directives_fixtures` (its behaviour over trees written at run time: 8 cases). Red first: the fixtures test failed with "check_non_target_directives.cmake does not exist", and the scan over a copy of the checkout with the root as it was before Step 3 named exactly `CMakeLists.txt:148`, `:149`, `:293` and `:295`. The scan over the checkout reads 48 files and finds none. Hand-made changes on a scratch copy, each failing what pins it: of the scan, case-blind (S1), a name that is the tail of a longer one (S2), a build tree, `.git` and `cmake-build-*` read (S3 to S5), comments read (S6), trailing comments read (S7), no floor (S8), a name at the end of its line missed (S9), `*.cmake.in` not read (S10), line numbers off by one (S11), an unchecked `MIN_FILES` or `SOURCE_DIR` (S12, S13), `;` and `[` left in the code (S14, S15), no hint in the refusal (S16); of the checkout, `include_directories(.)` in `examples/CMakeLists.txt` (M7), `add_compile_options(-Wall)` back in the root (M9), the scan reading itself (M11), `ADD_DEFINITIONS` in a module (M12) and `link_libraries` in the package config template (M13). Full build and `ctest` on the Mac: 153 of 153. 4b: the demotion is in the root, around `include(FetchContent)`, for the top level on CMake 4.5 or later. On 4.5.0 the strict configure of the real project exits 0 in the default, `asan` and `tsan` trees, printing googletest's two reports as `CMake Warning (non-target-directive)` and no error. On 4.4.5 the three trees' configure logs, compile lines and link lines are identical to Step 3's, and a configure takes the same time with and without the change (1.28 s and 1.29 s, alternating, three rounds). `test_strict_configure` runs 7 configures on 4.5.0 and is skipped, with its status line, on 4.4.5; `test_cmake_diagnostics` runs 8 cases on 4.5.0 and 5 on 4.4.5. Hand-made changes to scratch copies, run by CMake 4.5.0, each failing what pins it, and the unchanged copy passing all three checks: no demotion (M8: the three strict configures), the demotion after the include (M15: the same, which pins the placement), no block (M16: only the case with the root's own directive), no top-level condition (M14: only the parent's), the version test inverted (M17: the strict configures), `add_compile_options(-Wall)` back in the root (M9: the scan and the strict configures), the gate's flags mistyped (M10: `The warning category "autor" is not known`), and the scan reading either of the new scripts (M19, M20). The first run of the suite failed one test, `test_fetch_declarations`, which read the parent stub's `URL` with no hash in the new script's text; the stub declares its dependency with `SOURCE_DIR`, a form that check allows, and the second run passed. Full build and `ctest` on the Mac (4.4.5): 154 of 154. |
| 5 Docs | Done but for the plan's Status and Outcome | The README's strict-configure section says that 4.5's `non-target-directive` sits in the `strict` group under `author` and so is made an error by `-Werror=author`, names the seven commands, and says that the project's options go to targets through `flywheel_dag_add_target_options`, which `test_non_target_directives` holds. `CLAUDE.md` gets no line (the Step 5 section says why). It also says that googletest's two calls are warnings at the top level on CMake 4.5 or later and this project's own files errors, which `test_strict_configure` holds. Owed: this plan's Status and Outcome, written last. |
| 6 Verify before the push | Done | On the final tree (`3886b76`). **Mac, CMake 4.4.5**, Apple clang 21.0.0, the preset tree: full build and `ctest` 154 of 154, benchmark invariants identical. **Mac, CMake 4.5.0**, a scratch tree `build/scratch45`, CI's two flags: configure exit 0, printing googletest's two `CMake Warning (non-target-directive)` and nothing else; full build exit 0 in 23 s, 65 objects, no warning; `ctest` 154 of 154 in 156 s, `test_strict_configure` among them (7 configures, 4.7 s); benchmark invariants identical. The first run of that tree failed `test_warning_flags`: it took its dependencies' sources from the preset tree's `_deps`, which is under the source directory and outside the scratch tree, and that test counts such a unit as the project's own. With the sources copied into the scratch tree's own `_deps`, where a tree that fetched them has them, the rerun passed. **`ubuntu-dev`** (GCC 13.3.0, CMake 4.4.5, aarch64): configure exit 0, the build up to date (no C++ and no compile line changed), `ctest` 154 of 154 in 193 s; an `asan` tree configured, the three canaries built, and the 13 sanitizer, warning, target-option and scan tests pass. **Throwaway `ubuntu:26.04`** (GCC 15.2.0, CMake 4.5.0 from Kitware's linux-aarch64 tarball, aarch64): configure exit 0 with googletest's two warnings and nothing else; full build exit 0 in 105 s, 65 objects, no warning or error line; `ctest` 102 of 102 in 76 s, `test_strict_configure` among them; benchmark invariants identical. Interim readings at `aafe471`, from before the 4.5.0 binary ran: **`ubuntu-dev`** (GCC 13.3.0, CMake 4.4.5, aarch64): configured with CI's two flags (exit 0), built (exit 0, 62 s, an incremental build of 20 objects, no warning), `ctest` 153 of 153 in 179 s; an `asan` tree configured, the three canaries built, and the 13 sanitizer, warning, target-option and scan tests pass. **Throwaway `ubuntu:26.04`** (GCC 15.2.0, the distribution's CMake 4.2.3, aarch64): configured with CI's two flags (exit 0), built (exit 0, 92 s, 65 objects, no warning or error line), `ctest` 101 of 101 in 69 s (the 52 `clang_tidy_` tests are registered only where `FLYWHEEL_DAG_CLANG_TIDY` names a tool, and there is none), and the benchmark invariants are identical to `benchmarks/expected_invariants.txt`. That is a first GCC 15.2 verdict on flywheel-dag#80's commits, and it is green. It is not CI's leg: the container is aarch64 and CMake 4.2.3, and CI's runner is x86_64 and CMake 4.5.0. Docker was not running at the start: `open -a Docker`, then `docker start ubuntu-dev`, which had exited with 143 46 hours before. |
| 7 Push once, and look at the runs | Pushed with this commit | One push of Steps 0 to 6, never forced, after `git fetch` found nothing new on `origin/main`. The runs are read in passing, once, and what was seen is a comment on flywheel-dag#84. |

## Outcome

**What changed.** Options go to targets. `flywheel_dag_add_target_options` (`cmake/flywheel_dag_target_options.cmake`) gives the
sanitizer options to every target of the finished tree and the warning flags to the targets of `tests`, `examples` and
`benchmarks`, so the root calls none of the seven non-target directives that CMake 4.5 reports. googletest's two
`include_directories` calls, which are not ours to change, are warnings at the top level on CMake 4.5 or later: the root
demotes the category around `include(FetchContent)`, and not for a project that pulls the engine in. No header, example,
workflow or install rule changed, and every unit has the same compile and link arguments as before (Step 3 compared them
unit by unit).

**What it costs.** Nothing at run time. A configure on 4.4.5 takes the same time with and without the change (1.28 s and
1.29 s).

**What was measured.** Step 6 and the Progress table give each reading with its tool versions. In short: CMake 4.5.0
reproduces the red on the unchanged tree, with four errors in the default tree and six in `asan` and `tsan`, at the lines of
the Problem section; the final tree configures with CI's two flags on 4.5.0 and prints googletest's two reports as
warnings; and the Mac (4.4.5 and 4.5.0), `ubuntu-dev` (GCC 13.3, 4.4.5) and a GCC 15.2 container (4.5.0) build with no
warning and pass the suite and the benchmark invariants. Step 6 showed no GCC 15 finding in flywheel-dag#80's code: both
GCC 15.2 builds, the interim one and this one, have no warning, so no issue was opened for one.

**What differed from the plan.** `CLAUDE.md` got no line (Step 5). The behaviour cases are a test of their own and hold
three cases more than the table lists, and the demotion is applied at the top level only, because Step 1c measured that a
subproject's include of `FetchContent` redefines the macros that its parent runs next (Step 4). The scan does not read four
files, not two.

**What was not done.** A run on x86_64, which is what the runners are. The sanitized suite in `ubuntu-dev`, beyond the
canaries and the coverage test, as the plan said. A line in `CLAUDE.md`: say so and it is one more commit.

**What went wrong on the way.** The permission classifier refused the 4.5.0 binary once, and the steps that needed it
waited for a "try again" and nothing was done to get round it. Three shell calls ran a bare `python3` where the guidelines
say `pipenv run`: a check that it runs, a read of a scratch probe tree's `compile_commands.json`, and a one-line
placeholder. None changed a file.

**Environment.** Docker was not running when the work began: `open -a Docker`, then `docker start ubuntu-dev`, which had
exited with 143 46 hours earlier.
