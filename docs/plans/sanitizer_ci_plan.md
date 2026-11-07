# Sanitizer builds of the engine's own tests, in CI

**Status: Approved (2026-11-07) as written; D1 to D7 stand at their defaults.**

Closes [flywheel-dag#45](https://github.com/tomlin256/flywheel-dag/issues/45). It follows
[`thread_fence_wtsan_plan.md`](thread_fence_wtsan_plan.md) (flywheel-dag#49: the suite builds under GCC's ThreadSanitizer with
`-Werror`) and the two fixes the issue waited for, flywheel-dag#47 (the allocation counter aborted under AddressSanitizer) and
flywheel-dag#48 (a leak in `test_dag_traversal`). Design source: cryptocogs's sanitizer setup, which has run there since
2026-11-04: [`cmake/cryptocogs_sanitize.cmake`](https://github.com/tomlin256/cryptocogs/blob/main/cmake/cryptocogs_sanitize.cmake),
[`tests/sanitize_checks/`](https://github.com/tomlin256/cryptocogs/tree/main/tests/sanitize_checks) and
[`sanitizers.yml`](https://github.com/tomlin256/cryptocogs/blob/main/.github/workflows/sanitizers.yml).

**Not here:** MemorySanitizer (it needs an instrumented standard library); Clang on Linux; fuzzing and Valgrind; the benchmark
under a sanitizer (it would time the instrumentation); a ThreadSanitizer suppression file (D5); a sanitizer run on every push; a
sanitizer kind with `assert()` on. No file under `include/` changes unless a sanitizer shows a defect there (D5), so there is no
tag and no version (`docs/releasing.md`: a CI or test change needs no release, and the new option is the top level's alone).

## Problem

CI builds and runs the suite uninstrumented. A bug of the class flywheel-dag#42 was (a hook that locks a destroyed mutex)
survives an ordinary run, and the test written for it fills the dead `Engine` with `0xFF` so that it fails without a sanitizer.
That is one bug already found. A sanitizer job finds the next of its kind without anyone having to think of it first.

### What I checked (2026-11-07, read-only; scratch trees outside the checkout)

- **Nothing in the repository switches a sanitizer on.** `-fsanitize` is named in `tests/check_tsan_fences.cmake` (a probe that
  compiles and never links), in comments and in earlier plans, and not in `CMakeLists.txt`, `cmake/`, `CMakePresets.json` or
  `.github/`. Three hand-made trees sit in `build/`: `asan-linux-arm64` (`CMAKE_CXX_FLAGS=-fsanitize=address,undefined
  -fno-omit-frame-pointer`, so UBSan prints and carries on, and `-Werror` is off), `tsan-linux-arm64` and `tsan-macos-arm64`
  (`-fsanitize=thread -fno-omit-frame-pointer`). They cannot be rebuilt from the repository, and their names claim a platform
  and say nothing of the flags.
- **The suite is clean wherever it has been run.** The comments on flywheel-dag#45 record `ctest` passing in full under
  AddressSanitizer + UBSan in the container (GCC 13.3, aarch64) after flywheel-dag#47 and flywheel-dag#48. The flywheel-dag#49 plan records the
  whole suite under ThreadSanitizer there with no report. Today's runs on `f11501f`, with the flags the option will set
  (`-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined`, or `-fsanitize=thread
  -fno-omit-frame-pointer`), `RelWithDebInfo`, CI's strict flags (`-Werror=author -Werror=deprecated
  -DFLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON`), the dependencies instrumented too, and the runtime options CI will use:

  | Tree | Toolchain | Build | `ctest` | Result |
  |---|---|---|---|---|
  | asan, Mac | Apple clang 21.0.0, libc++, CMake 4.4.5, 12 cores | 26 s (`--parallel 8`) | 56 s | 78 of 79, no report |
  | tsan, Mac | the same | 17 s | 56 s | 78 of 79, no report |
  | asan, `ubuntu-dev` | GCC 13.3.0, libstdc++, aarch64, `detect_leaks=1` | 155 s (`-j4`) | 63 s | 79 of 79, no report |
  | tsan, `ubuntu-dev` | the same, `halt_on_error=1` | 75 s | 58 s | 79 of 79, no report |
  | asan, CI's compiler | GCC 15.2.0, libstdc++, `ubuntu:26.04` throwaway, aarch64, CMake 4.2.3 | 147 s (`-j4`), no warning | 57 s | 79 of 79, no report |
  | tsan, CI's compiler | the same | 74 s, no warning | 56 s | 77 of 79, no report; see below |

  The two that fail under GCC 15.2's ThreadSanitizer, `test_dag_async` and `test_wake_state`, die with `SIGILL` and print nothing. Under
  gdb the signal is at the `bti c` landing pad of `sigsetjmp`, reached from `__tsan::call_pthread_cancel_with_cleanup` in
  libtsan's `pthread_cond_wait` interceptor, which is what a branch-target-identification fault looks like on aarch64, and no test
  code is in the frames that matter. x86_64 has no such instruction and the same two tests pass under GCC 13.3's runtime in
  `ubuntu-dev`, so I read it as a property of this container's runtime and kernel. The runner is the first place GCC 15.2's
  ThreadSanitizer runs these tests.

  The one failure on the Mac, `test_warning_flags`, is the measurement's own: the dependency sources were borrowed from another
  tree inside the checkout, so the test counted their 44 files as the project's. It is not a finding, and the container trees
  read their copies from outside the checkout and pass it.
- **The canaries work as ported.** cryptocogs's three programs, compiled unchanged with `-O2 -g -Wall -Wextra -Wpedantic -Werror`
  and the mode's flags, build without a diagnostic and stop non-zero with the sanitizer's own report: Apple clang 21 (exit 134 three
  times), GCC 13.3 and GCC 15.2 (exit 1, 1 and 66 on each).
- **What it costs.** The tree is 365 MB (asan) and 288 MB (tsan) on the Mac. Of the 56 s of `ctest` there, 46 s are the nine
  tests that run a compiler or a sub-build and no instrumented engine code (`test_standard_guard`, the three consumer builds,
  `install_dependencies`, `test_clang_tidy_wiring`, `test_crtp_access`, `test_tree_name`, `test_tsan_fences`); the other 70 tests
  take 10 s together. The runner is unmeasured, and the first dispatch answers it (Step 6). For scale, cryptocogs's cold runs on
  the same runner label took 4 to 10 minutes for a larger build.
- **What the option has to reach.** `compile_commands.json` of the asan tree lists 50 translation units: 33 tests, 2 examples, 4
  benchmark units, and 11 in fetched dependencies (spdlog 7, googletest 4; nlohmann/json is header-only). Partial
  instrumentation is possible: a dependency compiled without the flags links into instrumented tests with no error to say so, and
  for ThreadSanitizer that reads as a race in our code, because it only sees synchronisation it can intercept or instrument.
- **Who reads what this changes** (each is kept green or extended below): `test_presets` (the table of kinds; R8 reads the
  README), `test_platform_name` and `test_tree_name` (the guard), `test_cmake_diagnostics` (the files that spell the strict
  flags), `test_ci_paths` (a workflow with neither a `push` nor a `pull_request` trigger is skipped, by design),
  `test_clang_tidy_wiring` (a `.cpp` directly in `tests/` that no target compiles fails it, so the canaries sit in a
  subdirectory, which its expected list does not read), `test_comment_refs` (no closed issue and no plan in a comment),
  `test_warning_flags`, `test_cmake_floor` (every minimum is 4.0) and `test_claude_md_budget` (`CLAUDE.md` is 6,493 of 6,500
  bytes).

### The issue's questions, answered

| Question | Answer |
|---|---|
| Which presets | Two kinds, `asan` (AddressSanitizer + UBSan) and `tsan`, each its own configure, on each of the three hosts: six presets. One option, `FLYWHEEL_DAG_SANITIZE`, `OFF`, `asan` or `tsan`; the preset sets it and the build type. |
| When it runs | Nightly and on demand, in a workflow of its own, so that a red sanitizer run cannot mask whether the build in `ci.yml` is green. The price is a finding reported up to a day after the push. |
| Are the dependencies instrumented | Yes. The options are set before the first `FetchContent_MakeAvailable`, and two checks hold it: `test_sanitize_option` (a configure of the real root) and `test_sanitizer_coverage` (every unit and every link line of a sanitizer tree). |
| The runner's `mmap` randomisation | A step logs `vm.mmap_rnd_bits` before and after, and lowers it to 28 for the `tsan` job only when it is higher. The first dispatch says whether the image needs it. |
| A canary per preset | `sanitizer_canary_asan`, `sanitizer_canary_ubsan` and `sanitizer_canary_tsan`: each commits one deliberate violation, and the test needs a non-zero exit and the sanitizer's own report. A tree whose flags never reached the compiler goes red. |
| What a finding is | D5: a bug, one issue each; a small fix with a regression test that is red under the sanitizer first may be made here, otherwise I stop and ask. No test is skipped, excluded or weakened, and nothing is suppressed without your OK. |

## The change

### 1. The option (`cmake/flywheel_dag_sanitize.cmake`, the root `CMakeLists.txt`)

One pure function, so that `cmake -P` tests the mapping in milliseconds with no configure and the root calls the very same one:
`flywheel_dag_sanitizer_flags(<mode> <compile-out> <link-out>)`.

| Mode | Compile options | Link options |
|---|---|---|
| `OFF` or empty (default) | none | none |
| `asan` | `-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined` | `-fsanitize=address,undefined` |
| `tsan` | `-fsanitize=thread -fno-omit-frame-pointer` | `-fsanitize=thread` |

Anything else is a `FATAL_ERROR` that names the value and lists the modes. The match is exact and case-sensitive, because a mistyped
`adress` that quietly meant "no sanitizer" would give a tree that is green because it sanitised nothing.
`-fno-sanitize-recover=undefined` is there because UBSan's default is to print a report and carry on, which leaves a green run over a
real finding; AddressSanitizer aborts by itself and ThreadSanitizer exits non-zero once it has reported.

At the top level only, the root declares `FLYWHEEL_DAG_SANITIZE` (a cache string, `OFF` by default) and calls
`add_compile_options` and `add_link_options` **before the first `FetchContent_MakeAvailable`**, because a directory's options reach
only the targets made after they are set. The warning flags sit after every dependency on purpose, so that they reach none of
them; these sit before, so that they reach all. A subproject ignores the option: the engine is header-only, a consumer
sanitises its own targets, and an option that instrumented the engine's fetched spdlog alone would be partial instrumentation.
Nothing goes on `flywheel_dag`'s `INTERFACE` or in `CMAKE_CXX_FLAGS`. A status line names the mode.

### 2. Canaries and the coverage check (sanitizer trees only)

Three programs in `tests/sanitizer_canary/` (each with the project banner, written so that the optimiser cannot delete the
violation: `volatile`, `noinline`, and for the overflow a store of the sum to a volatile, because GCC drops an unused sum's UBSan
check together with the sum): a heap use-after-free, a signed overflow, a data race between two joined threads. They are targets,
and tests, only in the tree whose mode must catch them. `tests/run_sanitizer_canary.cmake` runs one and requires **a non-zero exit
and the sanitizer's own report header**: exit 0 with no report means the flags never applied; exit 0 *with* a report means live
but not stopping (UBSan's default); non-zero with no report means it died of something else.

| Test | Registered when | Requires |
|---|---|---|
| `sanitizer_canary_asan` | mode `asan` | non-zero exit and `AddressSanitizer: heap-use-after-free` |
| `sanitizer_canary_ubsan` | mode `asan` | non-zero exit and `runtime error: signed integer overflow` |
| `sanitizer_canary_tsan` | mode `tsan` | non-zero exit and `ThreadSanitizer: data race` |
| `test_sanitizer_coverage` | any mode, Makefiles | every unit in `compile_commands.json` carries every compile option of the mode and no `-fno-sanitize=` (the mode's own `-fno-sanitize-recover=` is another option), and every executable's `link.txt` every link option; it refuses to pass having scanned fewer than a floor, so a changed layout fails loudly |

### 3. The guard knows the sanitizer in a tree's name

`flywheel_dag_tree_problem` gets a fifth argument, the value of `FLYWHEEL_DAG_SANITIZE`, and a rule: a platform tree whose kind
is `asan` or `tsan` must be configured for that mode. A tree called `asan-linux-x86_64` that is really uninstrumented is a green
sanitizer run over nothing, and no canary exists in it to say so, because canaries are registered only where the mode is on. Nothing
else is required of a sanitizer tree: `release-linux-arm64` with `asan` stays legal, as does any name that claims no platform.
The platform table itself is untouched.

### 4. Presets

Hidden kind presets `asan` and `tsan` set `CMAKE_BUILD_TYPE` to `RelWithDebInfo` (what CI builds, with the line numbers a report
needs) and `FLYWHEEL_DAG_SANITIZE` to their own mode; six visible configure presets (`asan-macos-arm64`, `asan-linux-arm64`,
`asan-linux-x86_64`, and the same for `tsan`) each have a build and a test preset of the same name, built as the existing nine.
`test_presets` learns the new kinds (R4: a sanitizer kind sets its mode and no other kind sets one). The three hand-made trees
that carry the same names are removed first (D6).

### 5. The workflow (`.github/workflows/sanitizers.yml`) and its check

`schedule` (nightly) and `workflow_dispatch` with a `which` input (`both`, `asan`, `tsan`); `permissions: contents: read`; a
`concurrency` group per `which` that cancels a run in flight; one matrix job per mode on `ubuntu-latest` with `fail-fast: false`, so
that one mode red does not hide the other, and no cache of any kind (a dependency tree restored from the other mode is silent
partial instrumentation). The steps are the preset's, as in `ci.yml`:

```yaml
- name: Configure
  run: cmake --preset ${{ env.PRESET }} -Werror=author -Werror=deprecated -DFLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON
- name: Build
  run: cmake --build --preset ${{ env.PRESET }} --parallel 4
- name: Test
  env:
    ASAN_OPTIONS: detect_stack_use_after_return=1:strict_string_checks=1:detect_leaks=1
    UBSAN_OPTIONS: print_stacktrace=1
    TSAN_OPTIONS: halt_on_error=1:second_deadlock_stack=1
  run: ctest --preset ${{ env.PRESET }}
```

with `PRESET: ${{ matrix.kind }}-linux-x86_64`. The whole suite runs, canaries and coverage test included, with no `-R` and no `-E`.
`detect_leaks=1` is the part macOS cannot do (Apple's runtime answers `detect_leaks is not supported on this platform` and exits
134), which is why the CI legs are Linux (D1). The schedule runs `asan` alone until `tsan` has five consecutive green dispatch
runs, because ThreadSanitizer reports a race only when it sees both accesses and one green run proves little (Step 7).
`test_ci_sanitizers` (D3) reads the workflow's code, not its comments, and fails unless: every matrix mode is one the module
accepts and `<mode>-linux-x86_64` is a configure, build and test preset; the configure line carries the strict flags and names no
`FLYWHEEL_DAG_SANITIZE`, `CMAKE_CXX_FLAGS` or `CMAKE_BUILD_TYPE` (the preset is the one place); a `ctest` command runs the whole
suite, unfiltered and unpiped; there is no `continue-on-error`, no `|| true`, no cache and no `suppressions=`;
`ASAN_OPTIONS` has `detect_leaks=1` and `TSAN_OPTIONS` has `halt_on_error=1`; the job has a `timeout-minutes`; the `vm.mmap_rnd_bits`
step comes before the configure; and (from Step 6) the schedule trigger exists.

### 6. Docs

README: the two kinds in the trees table and a "Sanitizers" section (the commands, the runtime options, that macOS has no
LeakSanitizer, that a finding is fixed or filed and never skipped), and a badge. `CLAUDE.md`: one sentence changes ("the presets set
the tree and the build type, and a sanitizer kind its mode, and nothing else") and stays inside the 6,500-byte budget by
tightening that sentence. `docs/releasing.md` step 6 also reads the last `Sanitizers` run. Comments at the declarations carry the
rest.

## Decisions to confirm

Defaults stand if you approve the plan as written.

- **D1 Linux x86_64 only in CI.** The macOS and `linux-arm64` presets exist for local runs. LeakSanitizer exists only on Linux, and
  GCC 15.2 is the compiler that rejects what the others accept. *Alternative:* a `macos-latest` leg for each mode (Apple clang,
  libc++, Apple's runtime): two more jobs (the Mac builds in 17 to 26 s and tests in 56 s; a runner is slower), a second source of
  toolchain-drift noise, an `os` key in the matrix and in `test_ci_sanitizers`, and `ASAN_OPTIONS` without `detect_leaks`.
- **D2 The guard learns the sanitizer in a tree's name** (section 3). *Alternative:* leave the guard alone; the presets and the
  workflow are right, and only a hand-made tree can lie.
- **D3 `test_ci_sanitizers`** holds the workflow, as `test_ci_clang_tidy` holds the lint workflow. *Alternative:* the dispatch run
  is its test, as in cryptocogs.
- **D4 The runtime options live in the workflow's `env`** and are repeated in the README for local runs (a local macOS run
  omits `detect_leaks`). *Alternative:* the test presets carry an `environment`, one place for CI and local, but a preset then
  sets more than the tree, the build type and the mode.
- **D5 What a finding is.** A sanitizer finding is a bug outside this plan: one `bug`-labelled issue each, and I tell you. If the
  fix is small and gets a regression test that is red under the sanitizer first, it may be made here; otherwise I stop and ask. No
  test is skipped, excluded or weakened to get a green job, and no suppression lands without your OK and a comment naming its cause
  (a `race:` on a qualified function, with the issue).
- **D6 The three hand-made trees are deleted** (`build/asan-linux-arm64`, `build/tsan-linux-arm64`, `build/tsan-macos-arm64`,
  ignored by git) in Step 4: the presets reuse their names, a preset configure over a cache that already holds the flags would
  double them, and nothing in them is needed once the preset trees exist. I look at each first and keep any that another session is
  using.
- **D7 Pushes, comments and dispatches.** Each step is one commit on `main`, pushed once its checkpoint is green; approving the
  plan approves those pushes, the comment on flywheel-dag#45 that links this plan, the dispatch runs of Steps 6 and 7, and one
  issue in cryptocogs (its platform module's header says flywheel-dag's copy has no sanitizer rule, which stops being true in Step 3).

## Steps

Commits are `type(#N): …` with this issue's number, and carry no closing keyword: flywheel-dag#45 is closed by hand in Step 7, after its comment. A
checkpoint is a full build and `ctest --preset relwithdebinfo-macos-arm64` on the Mac with CI's flags (`-Werror=author
-Werror=deprecated -DFLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON`), no warning, and the same in `ubuntu-dev`
(`relwithdebinfo-linux-arm64`) for a step that changes CMake. The default tree is 79 tests today; counts below are expected ones,
replaced by measured ones in the Outcome. Before each push the leak audit passes on a `git archive` of the tree, every reference in
a document is `flywheel-dag#N` and never a bare number, and the push is not held for CI (each run is read afterwards).

### Step 0 — Approval

*Tests:* none (documentation). Commit this plan (`docs: add plan for sanitizer builds of the engine's tests in CI`, scoped to the
issue), push, and comment on flywheel-dag#45 with the path. *Done when:* the issue and the plan link each other.

### Step 1 — The option (test first), one commit

*Tests (new):* `test_sanitize_flags` and `test_sanitize_option`, each a plain `cmake -P` registered at the top level in every tree.

1. *Red:* `tests/check_sanitize_flags.cmake` and its `add_test`; the module is missing. *Green:* `cmake/flywheel_dag_sanitize.cmake`.
   The test: `OFF` and the empty string give no options; `asan` and `tsan` give exactly the lists above, compared as strings so
   that the order counts; each of `address ASAN Asan thread TSAN msan adress ON off asan,tsan -fsanitize=address ubsan lsan` is
   refused, each in a child `cmake -P` run whose status is non-zero and whose message names `FLYWHEEL_DAG_SANITIZE='<value>'` and
   lists `OFF, asan, tsan` (compared by words, since CMake word-wraps).
2. *Red:* `tests/check_sanitize_option.cmake` and its `add_test`; there is no option. *Green:* the root block. The test
   configures the real root offline in scratch trees whose names claim no platform (examples and benchmarks on, tests off, the two
   dependencies taken from the running build, so no network and no compile): with no option there is no status line, the cache holds
   `OFF` and no unit has a `-fsanitize`; with `asan` and `tsan` the status line names the mode and every unit in
   `compile_commands.json`, the fetched spdlog's included, carries each option of the mode (the expected list is read from the
   module, so the flags test is the one place that pins the text; it refuses to pass having scanned fewer than 5 units); `adress`
   fails the configure naming the value and the modes; and a stub parent that `add_subdirectory`s the engine with
   `-DFLYWHEEL_DAG_SANITIZE=asan` has no `-fsanitize` anywhere in its database.

*Hand-made changes* (a scratch copy made with `git archive`, each through the real check): in the module, `-fno-sanitize-recover=undefined`
dropped; `tsan` given `address`; the modes missing from the message; the `FATAL_ERROR` made a warning; tsan's options reordered; `off`
accepted. In the root, the two `add_*_options` calls removed; the status line removed; the default `OFF` made `asan`; the mode ignored;
the block moved below the spdlog `FetchContent_MakeAvailable`; the block made to run in a subproject too. Each must fail the test that
names it, and the control passes.

*Checkpoint.* Expected 81 of 81. *Done when:* red then green for both, every change as stated, pushed.

### Step 2 — Canaries, the coverage check, and the first pass (test first), one commit

*Tests (new):* `sanitizer_canary_asan`, `sanitizer_canary_ubsan`, `sanitizer_canary_tsan` and `test_sanitizer_coverage`, registered
in `tests/CMakeLists.txt` only in the tree they apply to; the scripts `tests/run_sanitizer_canary.cmake` and
`tests/check_sanitizer_coverage.cmake`; the sources in `tests/sanitizer_canary/`.

*By hand,* trees named `scratch-asan` and `scratch-tsan` under `build/` (the presets come in Step 4): the canaries and the coverage test
pass in the tree that registers them, and the default tree's `ctest -N` lists none. *Mutations,* each in a scratch copy configured
and built for real from the fetched sources, must fail the tests named:

| # | Mutation | Must go red |
|---|---|---|
| A | the root stops applying the options (asan, then tsan) | the canaries, `test_sanitizer_coverage`, `test_sanitize_option`; not `test_sanitize_flags` |
| B | `-fno-sanitize-recover=undefined` removed from the module | `sanitizer_canary_ubsan` (exit 0 with a report), `test_sanitize_flags` |
| C | the block moved below the spdlog `FetchContent_MakeAvailable` | `test_sanitize_option` and `test_sanitizer_coverage`, naming the spdlog units; the canaries stay green, which is why the coverage check exists |
| D | `-fsanitize` removed from the link options only | the build fails at link, before a test can run |
| E | a test target given `-fno-sanitize=all` after the block | `test_sanitizer_coverage`, naming the target |

*The first pass.* The whole suite in both trees on the Mac (Apple clang 21) and in `ubuntu-dev` (GCC 13.3), with the options of
section 5 (on the Mac without `detect_leaks`, with `symbolize=0`). Every report goes in a table in the Outcome: sanitizer, test,
class, location, ours or a dependency's, disposition (D5). The expected table is empty, as today's runs say. The canaries also pass the
clang-tidy gate: a sanitizer tree configured with `-DFLYWHEEL_DAG_CLANG_TIDY=clang-tidy` passes `ctest -R '^clang_tidy_'`.

*Checkpoint.* Default tree 81 of 81; the asan tree 84 of 84 and the tsan tree 83 of 83. *Done when:* red then green, A to E as stated,
the first pass recorded, pushed.

### Step 3 — The guard (test first), one commit

*Tests (modified):* `test_platform_name` (`tests/check_platform.cmake`): its helpers take the fifth argument and every existing case
gets an empty one; accepted: `asan-linux-arm64` with `asan`, `asan-linux-arm64-gcc13` with `asan`, `tsan-macos-arm64` with `tsan`,
`release-linux-arm64` with `asan`, `asanx-linux-arm64` with nothing; refused, naming the tree and the mode: `asan-linux-arm64` with
nothing, with `tsan`, and `tsan-macos-arm64` with `asan`. The case `asan-macos-arm64` with nothing, accepted today, becomes a refusal.
`test_tree_name` (`tests/check_tree_name.cmake`), real configures: `asan-<host>-gcc13` with `-DFLYWHEEL_DAG_SANITIZE=asan`
configures; `asan-<host>` with nothing and with `tsan` are refused naming the tree and both modes, and leave no compiler detection
behind; `release-<host>` with `asan` configures.

*Red first,* then `cmake/flywheel_dag_platform.cmake` (the argument, the rule, the header comment) and the root's guard call, which
passes `"${FLYWHEEL_DAG_SANITIZE}"` as it reads the cache before the option is declared. *Mutations,* in a scratch copy, must fail both
tests: the rule deleted; the kind matched without its dash (so that `asanx-…` is refused); the message hiding the value. A comment on
a new cryptocogs issue gives the one sentence of its header that stops being true (D7).

*Checkpoint.* Expected 81 of 81. *Done when:* red then green, the mutations red, pushed.

### Step 4 — The presets (test first), one commit

*Tests (modified):* `test_presets` (`tests/check_presets.cmake`): the table of kinds carries `<name>:<build type>:<mode or none>` (`asan` and
`tsan` set `RelWithDebInfo` and their own mode, which the module accepts; no other kind sets one); R5 and R6 already loop over the kinds
and the hosts; R7 expects five presets per section on Darwin and ten on Linux; R8 holds the README to them.

*Red first,* then the two hidden kind presets and six visible ones with their build and test presets in `CMakePresets.json`, and the
README's trees table and count ("the fifteen in use"). The hand-made trees are removed first (D6). *Mutations,* in a scratch copy, each
must fail `test_presets` naming the rule: `tsan` setting `asan`; `asan` setting `Release`; `release` setting a mode; `asan-linux-arm64`
inheriting `tsan`; a build preset missing; a test preset that sets `noTestsAction` to `ignore`.

*Checkpoint.* Expected 81 of 81; `cmake --list-presets` on the Mac lists `asan-macos-arm64` and `tsan-macos-arm64`; the preset trees
`cmake --preset asan-macos-arm64`, build and `ctest --preset` pass 84 of 84, and `tsan` 83 of 83, and in `ubuntu-dev`
`asan-linux-arm64` and `tsan-linux-arm64` do the same with CI's runtime options; the scratch trees of Step 2 are deleted. *Done when:*
as stated, mutations red, pushed.

### Step 5 — The workflow and its check (test first), one commit

*Tests (new and modified):* `test_ci_sanitizers` (`tests/check_ci_sanitizers.cmake`, new); `test_cmake_diagnostics` holds the new
workflow to the strict flags' spelling (its list of files grows to five, and its comment says "CI's three workflows").

*Red first:* `test_ci_sanitizers` with no workflow ("does not exist"). Then `.github/workflows/sanitizers.yml` as section 5 describes,
without a `schedule` yet (Step 6), and a `timeout-minutes` of 45 until a cold run is measured. *Hand-made changes* (scratch copy, each
through the real check, the harness in the scratchpad): a misspelt `PRESET`; a matrix mode `adress`; `continue-on-error: true`; `|| true`
on the ctest line; `ctest … -R '^test_dag'`; `-E`; an `actions/cache` step; `ASAN_OPTIONS` without `detect_leaks=1`;
`TSAN_OPTIONS` with `suppressions=`; `-DFLYWHEEL_DAG_SANITIZE=asan` on the configure line; the strict flags taken off it; no
`timeout-minutes`; the `vm.mmap_rnd_bits` step after the configure; the control (green); a rule mentioned only in a comment (green).

*Also:* all three workflows pass actionlint (its Docker image, the files mounted read-only; if Docker is down that is said in the
Outcome); every `uses:` in every workflow is re-audited (`actions/checkout@v6` is `using: node24`); the workflow's commands are
rehearsed by hand: the presets on the Mac and in a fresh `ubuntu:26.04` container (GCC 15.2, CI's compiler, with `-Werror`).

*Checkpoint.* Expected 82 of 82. *Done when:* red then green, every change as stated, actionlint clean, the GCC 15.2 builds
warning-free, pushed. The push starts `CI` (two jobs) and `clang-tidy` (two legs) and no sanitizer run.

### Step 6 — The first dispatches, the cost, the schedule, the docs

1. `gh workflow run sanitizers.yml --ref main -f which=both`. Read the legs' steps, durations and annotations (not only the
   conclusion), and the runner's kernel, GCC, CMake, cores and `vm.mmap_rnd_bits`. Set `timeout-minutes` to about twice the slower
   cold run.
2. *Test first:* `test_ci_sanitizers` also requires the `schedule` trigger (red), then the trigger is added (green). It goes in only
   if the `asan` job is green; if not, the findings are issues (D5), the trigger waits, and I stop and tell you.
3. Docs: the README (trees table done in Step 4; the Sanitizers section, the badge), `CLAUDE.md` (the one sentence, inside the
   budget), `docs/releasing.md` step 6, the comment over `test_cmake_diagnostics`.

*Done when:* the `asan` job is green with the canaries and `test_sanitizer_coverage` in its log, the `tsan` job's result is recorded
(green, or exactly the findings D5 treats), `timeout-minutes` is set from the runs, no deprecation annotation, and the docs are
pushed. The runner numbers go in the Outcome.

### Step 7 — `tsan` joins the nightly; the issue closes

Five consecutive green `tsan` dispatch runs, one at a time (a newer dispatch cancels the one in flight), then the matrix fallthrough
becomes `["asan","tsan"]` and `test_ci_sanitizers` requires both modes to be reachable from the schedule (red first). A scheduled
run is read with `gh run list --workflow sanitizers.yml --event schedule`, both jobs green. The plan's Outcome is written and
flywheel-dag#45 gets a closing comment (the commits, the runs, the red-then-green record, the counts) before it is closed.
*Done when:* a `schedule` run on `main` shows both jobs green and the issue is closed. If a scheduled tick has not come, or the five
runs are not in, that is said in the Outcome, the issue stays open, and the rest is pushed.

## Designs I rejected

- **Free-form flags** (`FLYWHEEL_DAG_SANITIZE=address,undefined`): ASan and TSan cannot share a binary, a mistyped list must not
  quietly mean "no sanitizer", and the guard needs a closed set to compare a tree's name with.
- **`CMAKE_CXX_FLAGS` in the preset,** as the hand-made trees did: the flags would live in JSON where no pure-function test reaches
  them, a `-DCMAKE_CXX_FLAGS` on a command line would replace them, and the guard would have no variable that names the mode.
- **Sanitizer legs in `ci.yml`:** a red sanitizer leg would hide whether the build is green, and the lint workflow was split out
  for the same reason.
- **A sanitizer run on every push:** a cost on every commit for a finding that a nightly reports within a day; a trigger edit
  later if you want it.
- **A cache for the dependencies:** a tree restored from the other mode is partial instrumentation with no error to say so, and the
  fetch and build of spdlog and googletest is a minute.
- **Running only the engine's gtest suites (`-R`),** which would cut the nine sub-build tests: the plan's bar is that no test is
  skipped, and those nine are 46 s on the Mac.
- **Instrumenting only the project's own targets:** partial instrumentation, and for ThreadSanitizer a source of false races.

## Self-review — risks and assumptions

- **The first run on the runner is the real measurement.** Everything above ran on aarch64. The runner is x86_64 with GCC 15.2 and a
  kernel I have not seen, and the x86_64 build is emulated and slow here, so CI owns it. What I could close without the runner is
  closed: both modes build under GCC 15.2 with `-Werror` and no warning, `asan` passes there in full, and `tsan` passes everywhere
  it can run. The two tests that cannot run under GCC 15.2's ThreadSanitizer in the container are the ones the first dispatch
  decides; if they fail there with a report, that is a finding (D5), and with `SIGILL` it is a runtime problem of the image.
- **A clean ThreadSanitizer run proves less than a clean AddressSanitizer run.** It reports a race only when it sees both accesses, and
  `CycleSeqLock` is out of its reach by construction (it guards atomics, and ThreadSanitizer neither reports on an atomic access nor
  models the fences: `dag_engine.hpp`). Hence five dispatch runs before it is scheduled. The suite has no sleeps, so a loaded 4-core
  runner should not change what it sees, but that is an assumption the five runs test.
- **Toolchain drift is the point.** `ubuntu-latest` moves GCC and its runtime, so a nightly that goes red with no code change is the
  early warning this plan exists to give. The response is to fix the cause, never to silence it.
- **The size of a first runner pass is unknown.** If it shows more than a handful of independent bugs, I stop after Step 6 and come back
  with the list, not with a growing pile of fixes (D5).
- **The nine sub-build tests run uninstrumented compiles** in a sanitizer tree (a consumer builds the way a downstream project does,
  by design). They add 46 s on the Mac and nothing to the sanitizer's coverage; they stay, because no test is skipped.
- **Canary fragility.** An optimiser can delete a deliberate violation (GCC 15.2 drops an unused UBSan sum with its check). The canaries
  carry the fixes, and a compiler that defeats one turns the job red, which is the right failure. They run as ported on all three
  compilers above.
- **`compile_commands.json` and `link.txt` are CMake's layout, not a contract.** The coverage check refuses to pass having scanned fewer
  than a floor (the tree has 50 units and 34 executable link lines), so a layout change fails loudly instead of passing on nothing. It reads
  `link.txt`, so it is registered for the Makefiles generator only, which is the presets' default.
- **The guard change touches every configure.** The new rule examines only `asan-…` and `tsan-…` names in the platform shape, and the
  tests include the near misses (`asanx-…`, `release-…` with `asan`).
- **A scheduled workflow may not run.** GitHub disables a schedule after 60 days without repository activity, and cryptocogs's first
  tick made no run (its plan 032, Outcome). Step 7 reads a scheduled run; if none comes, the question is the schedule and not the
  build, since a dispatch of both takes the matrix branch the schedule takes.
- **flywheel-dag#67 (`-Werror` on by default) would make the explicit `=ON` on the workflow's configure line redundant.** The workflow
  passes it today, as the other two do, and `test_ci_sanitizers` requires it; the issue's own change drops that one rule with the
  flag.
- **`CLAUDE.md` has 7 bytes to spare.** The edit trades bytes inside one sentence; `test_claude_md_budget` is the judge, and the budget
  is not raised.
- **Public repository.** Nothing here names a private repository or how many projects use the design; cryptocogs is public. The leak
  audit runs before every push.
- **Disk.** A sanitizer tree is about 0.3 to 0.4 GB here. `df -h /` before building each, and delete scratch copies as soon as their
  result is recorded.
- **Behaviour change:** one option, six presets (and two hidden kinds), one workflow, seven tests added (three in every tree, four in
  the sanitizer trees) and four modified (`test_platform_name`, `test_tree_name`, `test_presets`, `test_cmake_diagnostics`). No
  header, no engine file, no default configure changes.

## Progress

| Step | Status | Notes |
|---|---|---|
| 0 Approval | In progress | Approved 2026-11-07; the plan is committed and flywheel-dag#45 gets a comment that links it |
| 1 The option | Not started | |
| 2 Canaries, coverage, first pass | Not started | |
| 3 The guard | Not started | |
| 4 The presets | Not started | |
| 5 The workflow and its check | Not started | |
| 6 First dispatches, schedule, docs | Not started | |
| 7 `tsan` joins the nightly; close | Not started | |
