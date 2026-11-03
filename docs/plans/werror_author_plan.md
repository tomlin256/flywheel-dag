# Spell the strict CMake flag `-Werror=author`, and drop the floor job

**Status: Approved (2026-11-03), with one change: the CI floor job is removed (D2).** D1, D3 and D4 stood.

Closes [flywheel-dag#59](https://github.com/tomlin256/flywheel-dag/issues/59).

## Problem

The issue reads: *"cmake --preset relwithdebinfo-linux-x86_64 -Werror=dev -Werror=deprecated
-DFLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON — The error=dev option is deprecated. Use -Werror=author instead."* That is CI's
Configure step, and the line is in its log today: run 41695678712 (`3725a1a`) prints it on `ubuntu-latest` and on
`macos-latest`, both CMake 4.4.4. The floor job (CMake 4.0.7) prints nothing.

CMake 4.4 replaced the `dev` switches with diagnostic categories: `-W<category>`, `-Werror=<category>`
(cmake-diagnostics(7), "Added in version 4.4"). The category is `author`, and `deprecated` is its child.
`-Wdev`, `-Wno-dev` and `-Werror=dev` are "compatibility synonyms", deprecated since 4.4, and each prints the notice.

The old spelling is written in `.github/workflows/ci.yml` (the Configure step and the comment over it), `README.md:269`
and `:273`, and `CLAUDE.md:43`. The plans in `docs/plans/` quote it as the command that was run and stay as they are.

### What I measured

A probe project (`project(probe NONE)`) that says `message(AUTHOR_WARNING)` and `message(DEPRECATION)`, configured by
Kitware's 4.0.7 and 4.2.3 macOS tarballs (SHA-256 checked against each release's file, in a scratch directory) and by
this Mac's Homebrew 4.4.5:

| CMake | `-Werror=dev` | `-Werror=author` | `-Werror=bogus` |
|---|---|---|---|
| 4.0.7 (the declared floor) | author warning fatal | **ignored**: both warn, exit 0 | ignored |
| 4.2.3 | author warning fatal | **ignored** | ignored |
| 4.4.5 | author warning and deprecation fatal, and the notice | the same, no notice | error: `The warning category "bogus" is not known.` |

With `-Werror=deprecated` added, 4.0.7 and 4.2.3 still make the deprecation fatal and leave the author warning a
warning. On 4.4.5 `-Werror=author` alone already makes the deprecation fatal.

The real project, preset `relwithdebinfo-macos-arm64`, CMake 4.4.5: with `-Werror=dev -Werror=deprecated
-DFLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON` the first line of output is the notice, exit 0. With `-Werror=author` in its
place there is no notice, exit 0. `ctest -N` lists 44 tests.

So the rename is safe where CMake is 4.4 and **silent** where it is not: a CMake before 4.4 takes `-Werror=author` and
does nothing with it, and so does it with a typo. CMake 4.4 itself rejects an unknown category. The one CI job on such a
CMake is the floor job added by flywheel-dag#57, which is why it goes (D2).

## The change

`-Werror=dev` becomes `-Werror=author` in the three places, `-Werror=deprecated` stays, and the floor job goes:

```yaml
# .github/workflows/ci.yml, the Configure step
run: cmake --preset ${{ matrix.preset }} -Werror=author -Werror=deprecated -DFLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON
```

- **The comment over the step** says what `author` is, that CMake before 4.4 ignores it without a word (measured on
  4.0.7 and 4.2.3), and that the runners' CMake (4.4.4) is what gates. The `Tool versions` step prints it.
- **The floor job goes** (D2): its matrix entry and the comment over it, the `Install the CMake floor` step, and the
  `CMake {0}` part of the job name, which only that entry set. The other two job names do not change.
- **`README.md`** gets the new spelling in the sentence and in the command, and one sentence: a strict configure wants
  CMake 4.4, because an older one ignores `-Werror=author` (`-Werror=deprecated` still bites there).
- **`CLAUDE.md:43`** gets the new spelling and a clause pointing at `test_cmake_diagnostics`.
- **`docs/plans/require_cmake4_plan.md`** gets one line under its status: its D3 job was removed by flywheel-dag#59.
- None of the three writes the old flag out, so the test below can forbid it everywhere it checks.
- **The declared floor, 4.0, stays** (`CMakeLists.txt`, the check scripts, the README, `test_cmake_floor`). No CI job
  runs it any more.

A new test, `tests/check_cmake_diagnostics.cmake`, registered as `test_cmake_diagnostics` beside `test_cmake_floor`
(top level only), in the style of `check_warning_flags.cmake`: *a warning gate that silently stops applying still passes
CI, which is why the gate itself is tested.*

1. **Spelling.** `ci.yml`, `README.md` and `CLAUDE.md` each contain `-Werror=author -Werror=deprecated` and none
   contains `-Werror=dev`. A file that cannot be read fails; it does not pass empty.
2. **Behaviour, on CMake 4.4 or later.** The script writes a probe project under `SCRATCH_DIR`, no network and nothing
   compiled, and configures it with the test's own copy of the flags:

   | Case | Expected |
   |---|---|
   | `message(STATUS)`, strict flags | exit 0, no "option is deprecated" line (the issue's symptom) |
   | `message(AUTHOR_WARNING)`, strict flags | exit non-zero, `CMake Error (author)` |
   | `message(DEPRECATION)`, strict flags | exit non-zero, `CMake Error (deprecated)` |
   | `message(AUTHOR_WARNING)`, no flags | exit 0, `CMake Warning (author)`: the flags are what bites |
   | `message(STATUS)`, `-Werror=dev` | the notice, or a non-zero exit: the first case can fail |

3. **Before 4.4.** A CMake the README still allows (4.0 to 4.3) and no CI job runs. The behaviour cases are skipped,
   with a status line that names the CMake and says it ignores `-Werror=author`. The spelling is still checked, so
   ctest does not go red for a developer the README welcomes.

## Decisions to confirm

Each was a place to edit before approving. Where you changed one, it says so.

- **D1. The spelling is `-Werror=author -Werror=deprecated` everywhere, with no version test.** You said to use
  modern CMake always. `-Werror=deprecated` stays because it costs nothing, and on a CMake before 4.4 it is the only
  one of the two that bites.
- **D2. The floor job is removed. (Changed at approval.)** I proposed leaving it, and it would have stopped failing on
  author warnings, since its CMake ignores `-Werror=author`. Your words: *"Remove the floor job, i dont care"*. CI
  then runs two jobs, both on CMake 4.4.4, so `-Werror=author` bites on every leg. The declared floor stays 4.0 and
  nothing in CI runs it. Raising it to 4.4 would refuse consumers on 4.0 to 4.3 and drop T1's "before 4.4" case; that
  is a separate issue if you want it.
- **D3. `test_cmake_diagnostics`.** I recommend it. Without it, `-Werror=dev` can creep back into a doc unnoticed, and a
  gate that a CMake ignores passes without a word, which is what the table above shows. Strike it and Step 2 is the
  text edits and the workflow.
- **D4. Linux is proved by CI.** Nothing here touches C++. The Mac gets a full build and ctest, and the push is read on
  both CI jobs (CMake 4.4.4). No container run.

## Steps

Every commit subject is scoped to flywheel-dag#59 in the repo's `type(#N): …` form. A checkpoint ends with a full
build and a full ctest run on the Mac (CMake 4.4.5, CI's flags), and nothing is pushed until it is green.

### Step 0 — Approval

The plan was committed as Proposed (`9a6dc46`) and linked from flywheel-dag#59. It was approved with one change, the
floor job's removal (D2), and this commit records both.

**Done when:** `git status` is clean on that commit and flywheel-dag#59 carries the link and the change.

Commits: `docs(#59): add plan for the CMake author diagnostic flag`, then `docs(#59): record the approval, and remove
the floor job in the plan`.

### Step 1 — Start from green

No commit. The Mac's tree was reconfigured by 4.4.5 above, so: full build with CI's flags (new spelling), `ctest`.

**Done when:** the build has no CMake or compiler warning and ctest passes 44 of 44. Anything else: stop and report.

### Step 2 — The spelling, the floor job, and a test that holds the spelling (D1, D2, D3)

The three text edits, the workflow, the one line in `require_cmake4_plan.md`, `tests/check_cmake_diagnostics.cmake`,
and its registration in `tests/CMakeLists.txt`.

| Test | Asserts |
|---|---|
| T1 `test_cmake_diagnostics` (new), spelling | the three files say `-Werror=author -Werror=deprecated` and never `-Werror=dev`; an unreadable file fails |
| T1, behaviour (CMake 4.4+) | the five cases above |
| T1, CMake before 4.4 | the behaviour cases are skipped with the status line; the spelling still holds |
| the other 44 (existing) | unchanged, and pass |

The workflow is read by GitHub, so a mistake in it shows as a run that does not start or a job that fails. No unit test
can stand in for that run (Step 3).

**Done when:**

- T1 is red on the unchanged tree for the right reason: it names `ci.yml`, `README.md` and `CLAUDE.md`;
- after the edits, ctest passes 45 of 45 on the Mac, and the configure prints no "option is deprecated" line;
- T1 passes under 4.0.7 and under 4.4.5 when the script is run directly on a scratch copy (4.0.7 says it skipped the
  behaviour cases);
- `ci.yml` has two matrix entries and no `matrix.cmake`, `Install the CMake floor` or `4.0.7` left in it, and parses as
  YAML;
- each hand-made change below fails exactly the case stated.

Each change is made on a scratch copy of the checkout and never restored:

| Change | Fails |
|---|---|
| M1 `ci.yml`'s Configure step back at `-Werror=dev` | spelling, naming `ci.yml` |
| M2 the README back at `-Werror=dev` | spelling, naming `README.md` |
| M3 `CLAUDE.md` back at `-Werror=dev` | spelling, naming `CLAUDE.md` |
| M4 the test's own flags as `-Werror=autor` | behaviour: CMake rejects the category, so the first case does not exit 0 |
| M5 the test's own flags as `-Werror=deprecated` alone | behaviour: the author-warning case configured, and was expected to fail |
| M6 `ci.yml` renamed | spelling: cannot read `.github/workflows/ci.yml` |

Commit: `fix(#59): spell the strict CMake flag -Werror=author, and drop the floor job`.

### Step 3 — Push, and read CI

The commit is pushed on its own. **Done when** both jobs are green on it and are named as before
(`build (ubuntu-latest, relwithdebinfo-linux-x86_64)` and `build (macos-latest, relwithdebinfo-macos-arm64)`); no third
job ran; the Configure log of each has no "option is deprecated" line; `Tool versions` prints CMake 4.4.4 or later in
each. Then flywheel-dag#59 is closed with a comment.

### Step 4 — Mark the plan done

The plan's status, the progress table and an Outcome section. Commit: `docs(#59): mark the plan done`.

## Designs I rejected

- **Choose the spelling by CMake version** (a matrix key, or a shell test). With the floor job gone no CI leg needs it.
  A developer on 4.0 to 4.3 loses only the author half of a strict configure, which the README says.
- **Drop `-Werror=deprecated`**, since `author` covers it on 4.4. It costs nothing, and a strict configure on a CMake
  before 4.4 still gets its deprecations fatal.
- **Keep the floor job and pass it `-Werror=dev`.** That was the cost of D2's first default; you chose to drop the job.
- **Presets `errors` fields.** CLAUDE.md keeps strictness out of the presets: they set the tree and the build type and
  nothing else.

## Not in this plan

- **Raising the declared floor to 4.4** (D2).
- **The older plans**, except the one line in `require_cmake4_plan.md`. They quote what was run.
- **A release.** No header, example or install rule changes; v1.0.0 stands.

## Self-review — risks and assumptions

- **The probe stands in for the project.** T1 proves the flags bite on the running CMake. That the project itself
  configures clean under them is Step 1 and 2's real configure on the Mac, and CI's Configure step.
- **T1 reads CMake's wording**: `CMake Error (author)`, `CMake Error (deprecated)`, `CMake Warning (author)` and
  "option is deprecated". A newer CMake that rewords them turns T1 red, and the strings are edited. That is the right
  failure: the gate's output changed.
- **On a CMake before 4.4 the gate is off and T1 passes.** That is a developer's CMake, which the README still allows.
  CI has none now. If a runner image ever fell back below 4.4, nothing would fail: only the status line and the
  `Tool versions` step would say so. Raising the declared floor to 4.4 is what closes that, and it is yours to decide.
- **The declared floor, 4.0, is now only what someone runs by hand.** That is the state flywheel-dag#57 added the floor
  job to avoid. It follows from D2.
- **4.1 and 4.3 are assumed to ignore `-Werror=author` as 4.0.7 and 4.2.3 do.** No CI job runs either.
- **A later CMake may drop `-Werror=author`.** From 4.4 an unknown category is an error, so the Configure step fails
  loudly and T1's first case fails with it.
- **Assumptions:** the runners keep CMake 4.4.4 or later; the suite keeps running under `ctest` with no sleeps; the
  probe needs no network and compiles nothing.

## Progress

| Step | Status | Notes |
|---|---|---|
| 0 Approval | Done | Proposed in `9a6dc46`; approved with the floor job's removal (D2). |
| 1 Start from green | | |
| 2 The spelling, the floor job and the test | | |
| 3 Push, and read CI | | |
| 4 Mark the plan done | | |
