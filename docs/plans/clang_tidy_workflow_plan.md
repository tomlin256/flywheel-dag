# clang-tidy as a workflow of its own

**Status: Done (2026-11-07).** Approved as written, so D1 to D6 stood at their defaults; the Outcome at the end records what the steps found. Rob asked for the clang-tidy job to become a
workflow of its own, modelled on cryptocogs's.

Closes [flywheel-dag#72](https://github.com/tomlin256/flywheel-dag/issues/72). It takes items 1 and 2 of
[flywheel-dag#69](https://github.com/tomlin256/flywheel-dag/issues/69); item 3 there (shallow clones) is not here.
Follows [`clang_tidy_ci_plan.md`](clang_tidy_ci_plan.md), which put the gate in CI as a job of `ci.yml`.
Design source: cryptocogs's [`clang-tidy.yml`](https://github.com/tomlin256/cryptocogs/blob/main/.github/workflows/clang-tidy.yml)
and its two checks, [`check_clang_tidy_workflow.cmake`](https://github.com/tomlin256/cryptocogs/blob/main/tests/ci_checks/check_clang_tidy_workflow.cmake)
and [`check_paths.cmake`](https://github.com/tomlin256/cryptocogs/blob/main/tests/ci_checks/check_paths.cmake)
(cryptocogs plan 040, tomlin256/cryptocogs#49).

**Not here:** shallow clones (flywheel-dag#69, item 3: it edits the root `CMakeLists.txt` for every build); a `paths-ignore` on
`ci.yml` (D3); a dependency cache (nothing is built, and the configure is 26 to 32 s); a nightly run; pinning clang-tidy to a
point release; a rule that makes the job required (`main` has no branch protection and no ruleset); any change under
`include/`, so no tag and no version (`docs/releasing.md`: a CI change needs no release).

## Problem

The gate is green and CI holds it, as a job of `ci.yml` beside `build`. That placement has three costs:

- **One status for two questions.** The workflow's badge and conclusion are red when the build is, or the lint is, and nothing
  says which.
- **The major is written five times.** `23` is in two step names, the apt source, the package and `CLANG_TIDY=clang-tidy-23`.
  `clang_tidy_version` catches a drift between them and the major the gate registers, but only as a red run after the push.
- **Every change runs it.** A commit that changes only `docs/` or `CLAUDE.md` runs both lint legs for something the lint never
  reads.

### What I checked (2026-11-07, read-only)

- **The job today.** Two legs (`ubuntu-latest` with `relwithdebinfo-linux-x86_64`, `macos-latest` with
  `relwithdebinfo-macos-arm64`), `timeout-minutes: 30`, and the workflow's `concurrency` group (a newer run cancels the one in
  flight). Run 42366473696 (`f62f349`): Ubuntu 1 min 59 s (install 11 s, configure 26 s, tests 77 s), macOS 2 min 12 s (install
  37 s, configure 32 s, tests 53 s), 41 tests on each; the build legs took 6 min 29 s and 3 min 29 s. Ubuntu is image
  `ubuntu-26.04` with LLVM 23.1.6, macOS is `macos-26-arm64` with Homebrew LLVM 23.1.4, CMake 4.4.5 on both. The lint is not the
  longest leg of anything, so moving it changes no time to a last green.
- **Who reads `ci.yml`.** `test_cmake_diagnostics` looks for `-Werror=author -Werror=deprecated` in the file; it finds the string
  in `build`'s Configure line, so the lint job's copy is held by nothing. `test_comment_refs` scans `.github/` for closed-issue
  references and plan pointers. The README (badge, "clang-tidy" section), `CLAUDE.md` (one bullet) and `docs/releasing.md`
  (steps 6 and 7) name it.
- **What the lint reads, and what it does not.** The `clang_tidy_` tests read the sources and headers, `.clang-tidy`, the canary
  fixtures and what the configure reads. They read no document. Checks that do: `test_claude_md_budget` reads `CLAUDE.md` and
  `docs/engine_rules.md`; `test_cmake_floor`, `test_cmake_diagnostics` and `test_presets` read `README.md`. So `build` cannot
  ignore `docs/**` or `CLAUDE.md` (an over-budget file in a documentation-only commit would start no run that measures it), and
  the lint can.
- **Budget.** `CLAUDE.md` is 6,475 of 6,500 bytes. Its one bullet that names `ci.yml` grows by 9 bytes when it names
  `clang-tidy.yml`.
- **Baseline.** Mac tree `relwithdebinfo-macos-arm64`, gate off: 75 tests. CMake 4.4.5, Homebrew clang-tidy 23.1.4.
- **Nothing keys on the check names.** `main` has no branch protection and no ruleset, so `CI / clang-tidy (…)` becoming
  `clang-tidy / clang-tidy (…)` breaks nothing.
- **Actions.** `actions/checkout@v6` is the only one in the workflows; its `action.yml` says `using: node24`.
- **Tools.** actionlint is not installed; Docker is, and `ubuntu-dev` is up.

## The change

### 1. `.github/workflows/clang-tidy.yml` (new)

A workflow of its own, as in cryptocogs, so that a red lint run cannot mask whether the build is green. The real file carries the
comments, written in the present tense with no issue and no plan in them (`test_comment_refs`); this is its shape:

```yaml
name: clang-tidy

on:
  push:
    branches: [ "main" ]
    paths-ignore:              # Step 3; a commit that changes only these starts no run
      - 'docs/**'
      - 'CLAUDE.md'
  pull_request:
    branches: [ "main" ]
    paths-ignore:
      - 'docs/**'
      - 'CLAUDE.md'
  workflow_dispatch:

permissions:
  contents: read

env:
  LLVM_MAJOR: "23"             # written once; test_ci_clang_tidy holds it to the gate's registration

jobs:
  clang-tidy:
    strategy:
      fail-fast: false
      matrix:
        include:
          - os: ubuntu-latest
            preset: relwithdebinfo-linux-x86_64
          - os: macos-latest
            preset: relwithdebinfo-macos-arm64
    runs-on: ${{ matrix.os }}
    timeout-minutes: 30
    steps:
    - uses: actions/checkout@v6

    - name: Install clang-tidy ${{ env.LLVM_MAJOR }} (Ubuntu)
      if: runner.os == 'Linux'
      run: |                    # the key is checked against LLVM's published fingerprint before apt trusts it
        . /etc/os-release
        curl -fsSL -o "$RUNNER_TEMP/llvm.asc" https://apt.llvm.org/llvm-snapshot.gpg.key
        found=$(gpg --show-keys --with-colons "$RUNNER_TEMP/llvm.asc" | awk -F: '/^fpr/ { print $10; exit }')
        test "$found" = 6084F3CF814B57C1CF12EFD515CF4D18AF4F7421 || { echo "apt.llvm.org's key is $found" >&2; exit 1; }
        sudo install -D -m 0644 "$RUNNER_TEMP/llvm.asc" /etc/apt/keyrings/apt.llvm.org.asc
        echo "deb [signed-by=/etc/apt/keyrings/apt.llvm.org.asc] https://apt.llvm.org/$VERSION_CODENAME/ llvm-toolchain-$VERSION_CODENAME-$LLVM_MAJOR main" | sudo tee /etc/apt/sources.list.d/llvm.list
        sudo apt-get update
        sudo apt-get install -y --no-install-recommends "clang-tidy-$LLVM_MAJOR"
        echo "CLANG_TIDY=clang-tidy-$LLVM_MAJOR" >> "$GITHUB_ENV"

    - name: Install clang-tidy (macOS)   # Homebrew's llvm is the latest major, keg-only; clang_tidy_version fails the leg past LLVM_MAJOR
      if: runner.os == 'macOS'
      run: |
        brew install llvm
        echo "CLANG_TIDY=$(brew --prefix llvm)/bin/clang-tidy" >> "$GITHUB_ENV"

    - name: Tool versions
      run: |
        cmake --version | head -1
        "$CLANG_TIDY" --version | head -1

    - name: Configure
      run: cmake --preset ${{ matrix.preset }} -Werror=author -Werror=deprecated -DFLYWHEEL_DAG_CLANG_TIDY="$CLANG_TIDY"

    - name: clang-tidy
      run: |
        cores=$(getconf _NPROCESSORS_ONLN)
        echo "runner cores: $cores"
        ctest --preset ${{ matrix.preset }} -N -R '^clang_tidy_' | tail -1
        ctest --preset ${{ matrix.preset }} -R '^clang_tidy_' -j "$cores"
```

`ci.yml` keeps `build` and the workflow's `concurrency` group. The `clang-tidy` job and its comment block leave it.

### 2. `test_ci_clang_tidy` (new)

`tests/check_ci_clang_tidy.cmake`, run as `cmake -DSOURCE_DIR=<repo> -P …` and registered at the top level beside
`test_cmake_diagnostics`, so it runs in every tree. A port of cryptocogs's check. It reads the workflow, `tests/CMakeLists.txt`
(the major beside the registration) and `CMakePresets.json`, reads the workflow's code and not its comments, and fails, naming
what is wrong, unless:

| Rule | Why it is a rule |
|---|---|
| `LLVM_MAJOR` is set on one line, in the env, and equals the number in the one `set(_flywheel_dag_clang_tidy_major N)` | A major moved in one place only shows after the push, as a red `clang_tidy_version`. |
| no line has a literal major (`clang-tidy-<n>`, `clang-tidy <n>`, `llvm-toolchain-…-<n>`), and the apt install, the apt source and `CLANG_TIDY=` on the Ubuntu leg name `$LLVM_MAJOR` | One place to move. |
| the configure passes `-DFLYWHEEL_DAG_CLANG_TIDY=`, on a line that also carries `-Werror=author -Werror=deprecated` | Without the first the leg configures, selects no `clang_tidy_` test, and `noTestsAction` is the only thing left to notice. Without the second a CMake author warning or deprecation passes: `test_cmake_diagnostics` reads the whole file, so the comment that explains the flags meets it (row s of Step 2). |
| every `ctest` command selects `-R '^clang_tidy_'` | The prefix is the gate's. Without it the job would run a suite nothing built. |
| no `continue-on-error`, no line ending in `\|\| true` or `\|\| :` | A lint step that cannot fail gates nothing. |
| every `preset:` of the matrix names a configure and a test preset of `CMakePresets.json`, every `--preset` argument is `${{ matrix.preset }}`, and the matrix has a Linux leg and a macOS leg (D1) | A misspelt preset is a red run after the push; a literal one tests a tree the matrix did not configure. |

It reads each file or fails; it never passes by reading nothing.

### 3. `test_cmake_diagnostics` (modified)

`tests/check_cmake_diagnostics.cmake` holds the strict flags' spelling in `.github/workflows/ci.yml`, `README.md` and
`CLAUDE.md`. `.github/workflows/clang-tidy.yml` joins that list (four files), so a lint workflow that drops the spelling is red,
and the comment over the test in `tests/CMakeLists.txt` says "CI's workflows". The check reads comments as well as code, so
section 2 holds that the Configure line itself carries the flags.

### 4. `paths-ignore` and `test_ci_paths` (new; Step 3)

The lint workflow carries the two lists above under both triggers. `tests/check_ci_paths.cmake` (`test_ci_paths`) is a port of
cryptocogs's `check_paths.cmake`: it reads every workflow with a `push` or `pull_request` trigger, in the block layout it
expects (anything else fails and says so), and fails unless

- a workflow that has a `paths-ignore` list has one under both triggers, and the two are equal;
- each entry is a file name or `<dir>/**`, and matches at least one file in the checkout;
- no entry covers a CMake or C++ file (`CMakeLists.txt`, `*.cmake`, `*.cmake.in`, `CMakePresets.json`, `*.cpp`, `*.hpp`, `*.inl`),
  `.clang-tidy`, a file under `tests/clang_tidy_canary/`, or a file under `.github/` (a workflow that ignored its own file would
  not run for a change to it) — what the build and the lint read;
- for a workflow that runs the whole suite (a `ctest` command that does not select `^clang_tidy_`), no entry covers a file that a
  check reads as `${SOURCE_DIR}/<name>` in `tests/*.cmake` either (`CLAUDE.md`, `docs/engine_rules.md` and `README.md` today);
- at least one workflow has a list, and a workflow with neither trigger is skipped and named in the status line.

It differs from cryptocogs's in two places. A workflow may have no list: `ci.yml` has none, deliberately (D3), where cryptocogs
holds every list equal to the main workflow's. And what an entry may cover depends on what the workflow runs, because the lint
reads no document and the full suite reads three.

### 5. Docs

README: a badge for the new workflow beside `CI`'s (a lint failure no longer reddens `CI`'s), and "clang-tidy" says the
workflow's name where it now says `ci.yml`. `CLAUDE.md`: its bullet names `clang-tidy.yml`. `docs/releasing.md`: step 6 reads
both workflows' runs, step 7's cancel note stays true of `ci.yml` only. `clang_tidy_ci_plan.md` gets one line pointing here.
The memory note that names `ci.yml` for the install steps is updated outside the repo.

## Decisions to confirm

- **D1 Both legs stay.** The macOS leg is the only gate on what the macOS SDK adds, and it costs a 37 s install and 2 minutes
  of a runner that is free here. *Alternative:* Ubuntu only, as cryptocogs has it; the matrix and one rule of the check go.
- **D2 No `concurrency` group on the new workflow.** As cryptocogs: every push keeps its own result, so a red one names its
  commit. The lint now runs to the end for a burst of pushes where `ci.yml`'s group cancelled it, which costs free runner time.
  *Alternative:* the same group as `ci.yml` (`${{ github.workflow }}-${{ github.ref }}`, cancel in progress), one block.
- **D3 `paths-ignore: docs/**, CLAUDE.md` on the new workflow only.** `ci.yml` stays unfiltered because `test_claude_md_budget`
  reads `CLAUDE.md` and `docs/engine_rules.md`, and three checks read `README.md`: flywheel-dag#69's item 2 asked for the lists on
  every workflow, and that part would let a budget overrun through. `README.md` stays out of the lint's list too, as in
  cryptocogs. *Alternative:* no filter and no `test_ci_paths` (drop Step 3 and section 4).
- **D4 A move, with two small changes.** The Configure line keeps `-Werror=author -Werror=deprecated`; `test_cmake_diagnostics`
  holds the spelling in the file (section 3) and `test_ci_clang_tidy` that the Configure line carries it (section 2). `-j` is
  the runner's core count, read with `getconf` because macOS has no `nproc`, and printed, as
  cryptocogs does; the old job said `-j4`.
- **D5 The checks are scripts proven by hand-made changes on a scratch copy**, recorded in the Outcome, as cryptocogs did.
  *Alternative:* committed fixture trees with a test per refusal, as `test_comment_refs` has; stronger and about three times the
  files.
- **D6 A second README badge.** Without it the README shows nothing of the lint.

## Steps

Commits go straight to `main`, one per step, each subject scoped to the issue in the repo's `type(#N): …` form, and are pushed
once the checkpoint is green (the push is not held for CI; each run is read afterwards and the Outcome records what finished).
No message carries a closing keyword: flywheel-dag#72 is closed by hand in Step 4. Counts are expected ones, replaced by
measured ones in the Outcome. A checkpoint is a full build and
`ctest --preset relwithdebinfo-macos-arm64` on the Mac with CI's flags (`-Werror=author -Werror=deprecated
-DFLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON`), no warning. Before each push the leak audit passes on a `git archive` of the tree, and
every reference in a document is `flywheel-dag#N`, never a bare number.

### Step 0 — Approval

*Tests:* none (documentation). Commit this plan (`docs: add plan for a clang-tidy workflow of its own`, scoped to the issue),
push, and comment on flywheel-dag#72 with the path. *Done when:* the issue and the plan link each other (the plan does; the
issue names the path).

### Step 1 — Start from green

No commit. *Done when:* the Mac tree builds and `ctest` passes 75 of 75 with no warning, and the last run on `main` is green on
all four jobs (run 42366473696 is). Anything else: stop and report.

### Step 2 — `test_ci_clang_tidy`, the workflow, and the job leaves `ci.yml` (test first), one commit

*Tests:* `test_ci_clang_tidy` and its `add_test` (new); `test_cmake_diagnostics` with the workflow in its list (modified).
*Red first:* both with no workflow, expected red ("does not exist", "cannot be read"). Then the workflow, the job removed from
`ci.yml`, and green.

*Hand-made changes* (a scratch copy made with `git archive`, not `rsync`, so no `build*` is copied; each through the real
check, the harness in the scratchpad):

| | Change | Expected |
|---|---|---|
| a | `LLVM_MAJOR: "22"` | red |
| b | the major beside the registration, 23 → 24 | red |
| c | a literal `clang-tidy-23` in the apt install line | red |
| d | a literal `-23 main` in the apt source | red |
| e | a literal 23 in the Ubuntu step's name | red |
| f | `CLANG_TIDY=clang-tidy-23` | red |
| g | delete `-DFLYWHEEL_DAG_CLANG_TIDY=…` | red |
| h | `-R 'clang_tidy_'` (no `^`) | red |
| i | add `continue-on-error: true`; then end the ctest line in `\|\| true` | red, each |
| j | `preset: relwithdebinfo-linux-x86-64` in the matrix | red |
| k | a literal `--preset relwithdebinfo-linux-x86_64` | red |
| l | delete the macOS matrix entry | red |
| m | `LLVM_MAJOR` set twice | red |
| n | `-Werror=dev` in the Configure line | red (`test_cmake_diagnostics`) |
| o | delete the workflow | red |
| p | the control, nothing changed | green |
| q | the major written `${LLVM_MAJOR}` and `${{ env.LLVM_MAJOR }}` | green |
| r | a rule mentioned only in a comment | green |
| s | the strict flags taken off the Configure line, the header comment still saying them (added while the rows were run) | red in `test_ci_clang_tidy`; `test_cmake_diagnostics` alone stays green |

*Also:* both workflows pass actionlint (its Docker image, the files mounted read-only; if Docker is down, that is said in the
Outcome and the first run is the lint). `actions/checkout@v6` is re-audited (`using: node24`).

*Checkpoint.* Expected 76 of 76. *Done when:* red then green as stated; a to o and s red and p to r green; actionlint clean; pushed;
the push starts `CI` (two jobs) and `clang-tidy` (two legs), and each lint leg runs the same 41 tests as before.

*After the push, optional (left for later in the Outcome if skipped):* the Ubuntu leg's steps, taken out of the YAML and run as
`bash -e` steps in a fresh `ubuntu:26.04` container (arm64 here, so the arm64 preset; the runner is x86_64), and the macOS leg's
on the Mac less the `brew install`. The commands are the old job's, so the first run is the real test.

### Step 3 — `paths-ignore` and `test_ci_paths` (test first), one commit

*Tests:* `test_ci_paths` and its `add_test` (new). *Red first:* the check against the tree as Step 2 left it, expected red
("found no workflow with a paths-ignore list"). Then the lists in the workflow, and green.

*Hand-made changes* (scratch copy, each through the real check):

| | Change | Expected |
|---|---|---|
| a | the lint workflow with the real triggers and lists | green |
| b | its `push` list without `CLAUDE.md` | red, the lists differ |
| c | its `pull_request` list deleted | red, names the workflow and the trigger |
| d | `include/**` added to both | red, covers engine headers |
| e | `.clang-tidy` added to both | red |
| f | `tests/clang_tidy_canary/**` added to both | red |
| g | `notes/**` added to both | red, matches no file |
| h | `README.md` added to both | green (the lint does not read it) |
| i | its `push` list deleted, the trigger kept | red, names the workflow and the trigger |
| j | `on: [push, pull_request]` in the flow layout | red, says it needs the block layout |
| k | `CLAUDE.md` added to `ci.yml`'s lists, both triggers | red, covers a file `test_claude_md_budget` reads (D3, shown) |
| l | a third workflow with only `schedule` | green, skipped |
| m | no workflow with a list | red |
| n | no workflow files | red |
| o | comments after `on:`, entries unquoted, double-quoted and reordered, a comment inside a list | green |
| p | `.github/**` added to both (added while the rows were written: a workflow that ignored its own file would not run for a change to it) | red, covers the workflows |
| q | the lint workflow's ctest line without `-R '^clang_tidy_'` (added) | red: it now runs more than the lint, and `docs/**` covers files a check reads |
| r | `docs/**` added to `ci.yml`'s lists, both triggers (added) | red, covers `docs/engine_rules.md` |
| s | a `.yaml` workflow with both triggers and a list of `notes/**` (added) | red, matches no file: the `.yaml` file was read |
| t | the lint workflow's `on` key quoted, `"on":` (added) | red, no top-level `on:` key |

*Checkpoint.* Expected 77 of 77. *Done when:* a to t as stated (h, l, o and a green, the rest red); pushed; the push's own lint runs are green.

### Step 4 — Docs, close

*Tests:* none new; `test_cmake_floor`, `test_cmake_diagnostics`, `test_presets` and `test_claude_md_budget` read the files edited.
Two commits, so that the second is documentation only and **must start no lint run** (the check of D3 on the real thing,
`gh run list --workflow clang-tidy.yml`): (4a) `README.md` and the comment over `test_cmake_diagnostics`, which start both
workflows; (4b) `CLAUDE.md` (6,475 of 6,500 bytes now, and the edit stays inside the budget), `docs/releasing.md`, the pointer
in `clang_tidy_ci_plan.md`, and this plan's Outcome, which start `CI` only. A comment on flywheel-dag#69: items 1 and 2 are
done, and `ci.yml` cannot take item 2 because `test_claude_md_budget` and three more checks read the documents; item 3 stays.
The memory note is updated. flywheel-dag#72 is closed by hand with a comment that lists the commits, the runs, the
red-then-green record and the counts. *Done when:* pushed; 4b started no lint run; the issue is closed.

## Designs I rejected

- **A second job in `ci.yml` that carries the lint's own `paths-ignore`.** A filter applies to the workflow, not the job.
- **`paths-ignore` on `ci.yml` too** (flywheel-dag#69, item 2). It would skip `test_claude_md_budget` on exactly the commits that
  change the file it measures (D3).
- **A reusable workflow** (`workflow_call`) shared by repositories. Each repository's job differs in its legs, its gate option and
  its presets, and each has its own check; a shared file would need a version of its own.
- **LLVM's `llvm.sh`, or a third-party action to install LLVM.** `llvm.sh` installs more than clang-tidy and cannot check the key
  against a fingerprint first; an action adds a dependency for eight lines.
- **Pinning the package version** (`clang-tidy-23=23.1.6-…`). The project holds one major (`clang_tidy_version`), not one build.
- **A retry loop on the apt steps.** No failure has been seen; added when one is.

## Self-review — risks and assumptions

- **The first run is a move, not a new toolchain.** The commands are the old job's, so Step 2's risk is a typo in the
  `$LLVM_MAJOR` substitution, which the check is for. The one thing it does not see is GitHub's own parser of the YAML (an
  expression in a step name): actionlint and the first run do.
- **The CI badge no longer covers the lint.** D6 puts a second badge beside it.
- **The key fingerprint lives in this workflow alone now.** A rotation fails at that step with the new value in the message, and
  the fix is one line.
- **No cancellation.** A burst of pushes runs the lint for each (D2). Free runner time, and a red one names its commit.
- **`test_ci_paths` is a parser of YAML written in CMake.** It reads one layout and fails on another, as cryptocogs's does; the
  hand-made changes j and o pin both sides.
- **The check reads the major from `tests/CMakeLists.txt` by pattern.** A restructure of that line turns the check red, saying
  that it found the number on zero or several lines; the alternative is a module for one number.
- **The drift the lint cannot see.** A change to `README.md`'s or `CLAUDE.md`'s clang-tidy text starts `CI` and not the lint, which
  reads neither. That is the point of D3.
- **Behaviour change:** one new workflow, one job gone from another, two new tests and one extended. No header, no engine file.
- **I assume** you want both legs, no cancellation and the filter on the lint only, as above; each is a few lines to change.

## Progress

| Step | Status | Notes |
|---|---|---|
| 0 Approval | Done | Approved as written on 2026-11-07, with the defaults of D1 to D6. |
| 1 Start from green | Done | The last run on `main` before this work (42366473696) was green on all four jobs. The unmodified tree was not built on its own: Step 2's checkpoint runs all 75 of its tests. |
| 2 `test_ci_clang_tidy`, the workflow, the job leaves `ci.yml` | Done | Red first (both tests), then green: 76 of 76, no warning. Hand-made changes: a to o and s red, p to r green, 23 runs as expected. actionlint: 0 errors in both workflows. |
| 3 `paths-ignore` and `test_ci_paths` | Done | Red first ("found no workflow with a paths-ignore list"), then green: 77 of 77, no warning. Hand-made changes: a to t, 20 runs as expected (h, l, o and a green). |
| 4 Docs, close | Done | 4a (README) and 4b (CLAUDE.md, releasing.md, the older plan's pointer, this Outcome); 77 of 77 at each. The runs and the check that 4b started no lint run are in the closing comment on flywheel-dag#72. |

## Outcome

Done on 2026-11-07, as approved, with the additions below. Times are from the runs' job records; counts are from the Mac tree
`relwithdebinfo-macos-arm64`, configured with CI's flags (`-Werror=author -Werror=deprecated
-DFLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON`), and no step built with a warning.

| Step | Count | The push's runs |
|---|---|---|
| 2 `test_ci_clang_tidy`, the workflow, the job leaves `ci.yml` | **76 of 76** | `CI` [run 42370887095](https://github.com/tomlin256/flywheel-dag/actions/runs/42370887095) green: the two build jobs took 4 min 28 s (Ubuntu) and 5 min 25 s (macOS). `clang-tidy` [run 42370887104](https://github.com/tomlin256/flywheel-dag/actions/runs/42370887104) green: 41 tests on each leg, as before the move; Ubuntu 1 min 55 s (LLVM 23.1.6, 4 cores), macOS 2 min 22 s (Homebrew LLVM 23.1.4, 3 cores). |
| 3 `paths-ignore` and `test_ci_paths` | **77 of 77** | `clang-tidy` [run 42371390863](https://github.com/tomlin256/flywheel-dag/actions/runs/42371390863) green: 41 tests per leg; Ubuntu 1 min 47 s, macOS 2 min 0 s. Its `CI` run, 42371390685, was cancelled by the next push (`cancel-in-progress`), and the next push's tree contains it. |
| 4a README | 77 of 77 | Starts both workflows: the README is in neither list. |
| 4b `CLAUDE.md` (6,478 of 6,500 bytes), `docs/releasing.md`, the older plan's pointer, this Outcome | 77 of 77 | Starts `CI` only. That the lint workflow started nothing for it is checked with `gh run list --workflow clang-tidy.yml` after the push, and the result is in the closing comment on flywheel-dag#72. |

**Red first, as planned.** Step 2: `test_ci_clang_tidy` ("does not exist") and `test_cmake_diagnostics` ("cannot be read")
failed with no workflow; both passed with it. Step 3: `test_ci_paths` failed with "found no workflow with a paths-ignore list"
until the workflow had its lists.

**Hand-made changes**, on a scratch copy made from git's file list (so no `build*` was copied), each through the real check, with
a change that did not apply stopping the harness. Step 2: 23 runs, all as expected. Rows a to o and s are red and p, q and r are green; i ran in its two forms
(`continue-on-error`, then `|| true`) and o through both checks, and n and the second run of o are red in
`test_cmake_diagnostics`, the rest in `test_ci_clang_tidy`. Step 3: 20 runs, all as expected; a, h, l and o are green and b to g,
i, j, k, m, n and p to t are red, each with the message the row names.

**Where it differs from the plan.**

- **Row s, a rule the plan did not have.** Taking `-Werror=author -Werror=deprecated` off the lint's Configure line left `test_cmake_diagnostics`
  and `test_ci_clang_tidy` green, because `test_cmake_diagnostics` reads the whole file and the comment above the line says the same words.
  `test_ci_clang_tidy` now requires the configure line that turns the gate on to carry them (red first: the row was green until
  the rule was added). This closes a hole the old job had too.
- **Rows p to t of Step 3.** `.github/` is refused as an ignored path (a workflow that ignored its own file would not run for a
  change to it), a lint workflow whose ctest line loses `-R '^clang_tidy_'` is treated as running the whole suite (so the
  document rule applies to it), `ci.yml` with a list is checked (q, r), and a `.yaml` workflow and a quoted `"on":` are read or
  refused (s, t). Row i is the `push` list deleted with the trigger kept, the mirror of row c.
- **The status line of `test_ci_paths`** names each workflow's list and the files it covers, the workflows with no list, and the
  skipped ones, so that what it read is in the log.
- **The comment over `test_cmake_diagnostics`** was brought to "CI's two workflows" in Step 2, with its list, and not in 4a.

**actionlint** (Docker image `rhysd/actionlint`, the files mounted read-only): 0 errors in both workflows after Step 2 and again
after Step 3. `actions/checkout@v6` is `using: node24`, as audited before the work began.

**What the move showed.** The two lint legs cost what they did in `ci.yml` (Ubuntu 1 min 59 s then, 1 min 47 s to 1 min 55 s now;
macOS 2 min 12 s then, 2 min to 2 min 22 s now), so a last green is no later. With no concurrency group the lint for the Step 3
commit finished and said green while its `CI` run was cancelled by the push after it: each commit keeps its own lint result, as
D2 intended.

**Left for later.** The optional rehearsal of the Ubuntu steps in a fresh `ubuntu:26.04` container was not run: the first run on
the runner was the real test, and it passed. flywheel-dag#69 item 3 (shallow clones) is still open.
