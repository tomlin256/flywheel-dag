# Point the README's Two Build Snippets at a Preset

**Status: Approved (2026-11-04).** Approved as written.

Closes [flywheel-dag#64](https://github.com/tomlin256/flywheel-dag/issues/64).

## Problem

Two snippets in `README.md` configure a tree at `build/` itself:

| Where | Today |
|---|---|
| "Or install it and find it with `find_package`" | `cmake -B build`, then `cmake --install build --prefix /opt/flywheel-dag` |
| the quickstart | `cmake -B build && cmake --build build`, then `./build/bin/quickstart` |

Every other tree is `build/<kind>-<os>-<arch>` (flywheel-dag#46), and "Building and testing" and "Benchmark"
use the presets. A reader of either snippet ends up with a tree at `build/` among the named ones. The guard lets
it through, because the name claims no platform, and nothing in the repo uses it.

flywheel-dag#46 kept the snippets on purpose (its D7): the reader "needs no preset and may have CMake older than
3.21", and `build` held a tree of its own at the time. Neither reason stands. Both snippets run from a checkout,
whose `CMakeLists.txt` asks for CMake 4.0 (`test_cmake_floor` holds it), so that reader cannot run them; and the
tree at `build/` is gone. A grep for `-B build`, `--build build`, `--install build`, `--test-dir build` and
`build/bin` finds these four lines and no other command in the checkout outside `docs/plans/`.

## The change

Both snippets use the preset, spelled as the contributor section spells it. Each command keeps its meaning; the
quickstart still builds everything.

```bash
# install snippet, before
cmake -B build
cmake --install build --prefix /opt/flywheel-dag

# install snippet, after
cmake --preset relwithdebinfo-macos-arm64        # relwithdebinfo-linux-arm64 or -linux-x86_64 on Linux
cmake --install build/relwithdebinfo-macos-arm64 --prefix /opt/flywheel-dag

# quickstart, before
cmake -B build && cmake --build build
./build/bin/quickstart

# quickstart, after
cmake --preset relwithdebinfo-macos-arm64        # relwithdebinfo-linux-arm64 or -linux-x86_64 on Linux
cmake --build --preset relwithdebinfo-macos-arm64
./build/relwithdebinfo-macos-arm64/bin/quickstart
```

`test_presets` gains rule R8, which reads `README.md` (from `README_DIR`, default `SOURCE_DIR`, so a hand-made copy
can be passed in as `PRESETS_DIR` does for the presets file). The README names only trees a preset defines:

- each `--preset <name>` is a visible configure preset;
- each `build/<name>` path is one too (`build/<kind>-<os>-<arch>` in prose has no name and is skipped);
- no command is `-B build`, `--build build`, `--install build` or `--test-dir build` followed by anything but a
  path character (a letter, a digit, `_`, `-`, `.` or `/`), which is `build` itself.

## Decisions

- **D1. Presets, not a renamed directory.** The contributor sections already teach `cmake --preset`, whose tree is
  `build/<preset>`. A directory of its own naming (`-B build/quickstart`) would be a third spelling to learn.
- **D2. This supersedes D7 of `platform_build_dirs_plan.md`.** Its two reasons are gone (above). That plan stays as
  written; it is the record of why the snippets were what they were.
- **D3. The rule is R8 in `test_presets`, not a new test.** The file already holds how trees are named and which
  names the presets define, and a README that names a tree is the same property. The ctest count stays 46.

## Steps

### Step 1 — The plan

Commit: `docs: add plan for pointing the README's build snippets at a preset`. **Tests: none.** **Done when:**
the plan is committed and you have approved it.

### Step 2 — The rule, then the README

**Tests modified:** `tests/check_presets.cmake` (`test_presets`): R8 and `README_DIR`, the usage text and the header
comment, and the closing line "R1 to R8 hold". No new test, no C++.

The order is red then green. R8 goes in first and must fail on today's README, reporting the four flat commands
and `build/bin`; then the README changes and R8 holds. **Done when:**

- the two snippets, run as written on the Mac, work: the quickstart exits 0 and prints its output, and the install
  writes the headers and the `find_package` config under a prefix in the scratchpad;
- the build prints no warning and `ctest --preset relwithdebinfo-macos-arm64` passes in full (46 of 46), which
  includes `test_cmake_floor` and `test_cmake_diagnostics`, the two other checks that read the README;
- each hand-made change to a scratch copy of the README, passed as `README_DIR`, fails R8 with the message stated
  and no other rule:

  | Change | R8 reports |
  |---|---|
  | M1. the install line back to `cmake --install build --prefix …` | the command installs `build` itself |
  | M2. the configure line back to `cmake -B build` | it configures `build` itself |
  | M3. the build line back to `cmake --build build` | it builds `build` itself |
  | M4. a line `ctest --test-dir build` added | it tests `build` itself |
  | M5. `./build/bin/quickstart` back | `build/bin` is no configure preset |
  | M6. `--preset relwithdebinfo-macos-arm65` | no such configure preset |
  | M7. `--preset macos-arm64`, a hidden host preset | the preset is hidden |

- CI is green on both legs after the push. `ubuntu-dev` is not run: R8 reads text, nothing in it depends on the
  host, and CI's Linux leg runs `test_presets`.

Commit: `docs: point the README's build snippets at a preset`.

### Step 3 — Close out

Status Done, the Outcome, and the issue closed once the run of Step 2's push is green. A docs commit; later runs of
docs commits are not waited for. No release.

## Outcome

| Step | Status | Commit |
|---|---|---|
| 1 — The plan | Pending | |
| 2 — The rule, then the README | Pending | |
| 3 — Close out | Pending | |

## Self-review — risks and assumptions

- **Assumption: "fix the README" means these two snippets.** They are the only README commands that name `build`
  itself (grep above). If more was meant, Step 2 grows.
- **A host with no preset.** `macos-x86_64` is not in the platform table, so a reader on an Intel Mac meets the
  guard on the preset's name. "Building and testing" has the same limit, and a name that claims no platform
  (`-B build/scratch`) still works there. Unchanged.
- **R8 is a text match.** A future snippet spelled another way (`-S . -B ./build`) passes unseen. R8 pins the
  spellings that existed, and each has a mutant.
- **CMake's regex has no lookahead and no `\b`.** `--build build/relwithdebinfo-macos-arm64` must not read as
  `build` itself, so the flat forms are matched as the flag, spaces, `build`, then anything but a path character:
  a newline, a space or a closing backtick all end it. M3 and the unmutated README cover both sides.
- **Match on the whole text, not `file(STRINGS)` lines.** The README holds `;` and `[`, which list handling would
  split; `check_cmake_floor.cmake` reads it with `file(READ)` for the same reason.
- **Leak audit.** README, plan and commit text name no downstream application; the pre-push audit is dry-run
  before the push.
- **No C++, no header, no hot path.** No benchmark and no clang-tidy run.
