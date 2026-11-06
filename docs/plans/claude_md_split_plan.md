# Split CLAUDE.md: Engine Rules for Importers, Maintainer Rules for This Repo

**Status: In progress (approved 2026-11-06).** Approved as written. The rules for building nodes and driving an engine
move to `docs/engine_rules.md`; `CLAUDE.md` keeps the maintainer rules and imports it; `docs/releasing.md` writes the
release down; a test holds the sizes. The Outcome is filled in when the push has gone.

Closes [flywheel-dag#70](https://github.com/tomlin256/flywheel-dag/issues/70).

## Problem

`CLAUDE.md` is 14,045 bytes (217 lines). Claude Code loads it whole at the start of every session in this repo, and whole
into any project that imports it: an import is expanded with its importer, so it costs what the file costs. Thirty-two
commits have touched it, and each plan's docs step has been free to add to it.

It has two readers, and they need different halves:

| Reader | Sections | Bytes |
|---|---|---|
| Code that builds nodes and drives an engine | Writing nodes, Threads and the engine, State persistence, and three compiler traps from the warnings bullet | about 5,700 |
| A maintainer of this repo | build, CI, warnings, clang-tidy, benchmark, install, conventions, and the ops and AAD rules | about 8,300 |

A project that imports the file pays for both and uses the first. Of the second, about 4 KB repeats `README.md`
("Building and testing", "clang-tidy", "Benchmark") or a comment that already carries the rule: the ops partials at
`dag_ops.inl` and `tests/test_aad_partials.cpp`, the tape rules at the top of `dag_aad.hpp`, the canary fixtures at the
registration in `tests/CMakeLists.txt`.

Two facts about Claude Code decide the shape (code.claude.com/docs/en/memory, read 2026-11-06). A `CLAUDE.md` in a
subdirectory loads when Claude first reads or edits a file there, and Claude Code keeps only its first 40,000
characters on a 200K context window, leaving the rest to a Read call Claude has to think of. And it advises under 200
lines per file.

## The change

| File | Change |
|---|---|
| `docs/engine_rules.md` | New. Writing nodes, Threads and the engine, State persistence, and the three compiler traps, for any project that builds on the engine. `CLAUDE.md` imports it. |
| `CLAUDE.md` | The maintainer rules only: commands, the rules `README.md` does not state, conventions. Imports `docs/engine_rules.md`. Says how to keep itself short. The line `-Werror=author -Werror=deprecated` stays, because `test_cmake_diagnostics` reads it. |
| `docs/releasing.md` | New. The release procedure: scope, number, version in three places, the gate on both platforms, commit and tag, CI read afterwards, record. |
| `README.md` | One sentence in "Documentation" pointing at the engine rules. |
| `tests/check_claude_md_budget.cmake`, `tests/claude_md_budget/`, `tests/CMakeLists.txt` | `test_claude_md_budget` and its fixtures. |

The budgets, written once in the registration in `tests/CMakeLists.txt`:

| What | Budget | Unit | Why |
|---|---|---|---|
| `CLAUDE.md` | 6,500 | bytes | in every session of this repo |
| `docs/engine_rules.md` | 6,000 | bytes | in every session of every project that imports it |
| any `CLAUDE.md` in the checkout | 38,000 | characters | Claude Code keeps only the start of a nested file past 40,000 |

Bytes for the first two, because the context pays bytes. Characters for the cap, because Claude Code counts characters:
a dash is one character and three bytes, and CMake's `string(LENGTH)` counts bytes, so the script subtracts the UTF-8
continuation bytes.

Where the old text went:

| Old section | Now |
|---|---|
| Writing nodes, Threads and the engine, State persistence | `docs/engine_rules.md`, compressed; the `FeedRegistry` details stay in the `dag_async.hpp` block |
| The three compiler traps in the warnings bullet | `docs/engine_rules.md`, "Traps GCC rejects and Apple Clang accepts" |
| Ops and derivatives | The headers: `dag_ops.hpp` (concrete types, unguarded), `dag_ops.inl` and `tests/test_aad_partials.cpp` (the cancellation-free partials), the top of `dag_aad.hpp` (tape rules). `CLAUDE.md` keeps the pointer and the local `-ffp-contract=off` check. |
| Build trees, clang-tidy and benchmark bullets | Shortened to the rules `README.md` does not state; `README.md` and `cmake/flywheel_dag_platform.cmake` keep the rest |
| The install, warnings, conventions bullets | Kept, tightened |

## Decisions

- **D1. Split, not trim in place.** A project that imports the file should pay for what it uses. Trimming the one file to
  the same total would leave an importer with the maintainer half.
- **D2. The ops and AAD rules stay in the headers.** Every one of them is in a comment next to the code it governs
  (checked before the section was dropped); a second copy in a loaded file only goes stale.
- **D3. The budgets are arguments.** The script takes the three numbers, so the fixtures can use 20, 20 and 30 and stay
  a few bytes long, while the real numbers are written once, where the real test is registered.
- **D4. The strict-flag line stays in `CLAUDE.md`.** `test_cmake_diagnostics` holds `CLAUDE.md` to the spelling.
- **D5. `docs/releasing.md` reads CI afterwards.** The gate is local, on macOS and in the Linux container, on the tree as
  committed. The release is tagged without waiting for CI, CI is read once it finishes, and a failure the release
  carries gets a patch release. A published tag is not moved.

## Steps

### Step 1: The plan

Commit: `docs: add plan for splitting CLAUDE.md`. **Tests: none.** **Done when:** the plan is committed and you have
approved it.

### Step 2: The budget test, red, then green

**Tests added:** `test_claude_md_budget`, which holds the real tree, and five fixtures in `tests/claude_md_budget/` run
with budgets of 20 bytes, 20 bytes and 30 characters:

| Fixture | Holds | The script must |
|---|---|---|
| `accepts` | `CLAUDE.md` of exactly 20 bytes (14 characters), rules of exactly 20 bytes, `sub/CLAUDE.md` of exactly 30 characters (70 bytes) | accept: the limit is allowed, and the cap is characters, not bytes |
| `rejects_claude_md` | `CLAUDE.md` of 22 bytes (14 characters) | refuse on bytes, though the characters are under every limit |
| `rejects_rules` | rules of 21 bytes | refuse the rules file |
| `rejects_nested` | `sub/CLAUDE.md` of 31 characters (73 bytes) | refuse on characters |
| `rejects_no_claude_md`, `rejects_no_rules` | the other file only | refuse: a file that is missing is a problem, not a pass |

A refusal passes on the text of the script's own message and fails on the message of any other rule, because a
`PASS_REGULAR_EXPRESSION` ignores the exit status.

The order is red then green:

1. The registration and the fixtures go in first, with no script: all seven tests fail on the missing file.
2. The script goes in. The six fixture tests pass; `test_claude_md_budget` fails on the tree as it is, for the right
   reasons: `CLAUDE.md` is 14,045 bytes against 6,500, and `docs/engine_rules.md` is missing.
3. Eight hand-made changes to a copy of the script, each run against the six fixtures, must each fail the fixture named:

   | Change | Caught by |
   |---|---|
   | M1. characters counted as bytes | `accepts`, `rejects_nested` |
   | M2. `CLAUDE.md` measured in characters | `rejects_claude_md` |
   | M3. `CLAUDE.md` at its limit is over | `accepts` |
   | M4. the cap at its limit is over | `accepts` |
   | M5. subdirectories not scanned | `rejects_nested` |
   | M6. a missing `CLAUDE.md` is not a problem | `rejects_no_claude_md` |
   | M7. a missing rules file is not a problem | `rejects_no_rules` |
   | M8. the rules file at its limit is over | `accepts` |

### Step 3: The files, green

`docs/engine_rules.md`, `CLAUDE.md`, `docs/releasing.md` and the README sentence. **Done when:**

- `test_claude_md_budget` passes, with `CLAUDE.md` and the rules under their budgets;
- `test_cmake_diagnostics` passes, which reads `CLAUDE.md` for the strict-flag spelling;
- `check_public_leaks.py --tree` is clean over the changed text;
- `ctest --preset relwithdebinfo-macos-arm64` passes in full, with `test_cmake_floor` and `test_presets`, which read the
  tree, among them;
- the pre-push hook accepts the push. One push for the plan: CI has no path filter, and the macOS leg can queue.

Commit: `docs: split CLAUDE.md into engine rules and maintainer rules`.

### Step 4: Close out

Status Done, the Outcome, and the issue closed. A docs commit. No release: nothing under `include/` changes.

## Outcome

Steps 2 and 3, on the tree before the commit. Step 3's push, CI and Step 4 are still to do.

- The order held. With the registration and the fixtures and no script, all seven tests failed on the missing file.
  With the script, the six fixture tests passed and `test_claude_md_budget` failed on the tree as it was:
  `CLAUDE.md is 14045 bytes, over its budget of 6500` and `docs/engine_rules.md is missing`.
- The unchanged script passed all six fixtures; M1 to M8 each failed the fixtures in the table above (M3 and M8 also
  disturbed the refusals, which is why a refusal fails on the message of any other rule).
- Sizes: `CLAUDE.md` 14,045 to 5,973 bytes (217 to 37 lines), `docs/engine_rules.md` 5,377 bytes (31 lines),
  `docs/releasing.md` 2,502 bytes. The largest `CLAUDE.md` is 5,954 characters.
- `ctest --preset relwithdebinfo-macos-arm64` passes 55 of 55 (the 48 that were there and the seven new ones), with
  `test_cmake_diagnostics`, `test_cmake_floor` and `test_presets` among them.
- `check_public_leaks.py --tree`, over a copy of the changed text, is clean.

## Self-review: risks and assumptions

- **Assumption: an import costs what the file costs.** Claude Code's documentation says imported files load with the
  file that imports them. The split saves an importer the maintainer half only because the importer imports the new
  file and not `CLAUDE.md`.
- **A moved rule is lost if the move drops it.** The old sections were compared against the new ones rule by rule; the
  rules that were compressed keep every sentence that states a rule, and what was cut sits in a header comment or in
  `README.md` (the table above).
- **`test_cmake_diagnostics` reads `CLAUDE.md`.** The line it needs stays, and the test is part of the full run.
- **The import line is plain text to anything but Claude Code.** The sentence above it names the file.
- **The budget is a text size, not a quality bar.** It stops growth; it does not tell what to cut. The rule in
  `CLAUDE.md` ("Keeping this file short") says what belongs: an invariant, a trap or a contract the code cannot carry.
- **Not covered:** that every `@path` import in `CLAUDE.md` names a file that exists. The one import there is,
  `docs/engine_rules.md`, is covered, because the budget test fails when it is missing; a second import would not be.
- **No C++, no header, no hot path.** No benchmark and no clang-tidy run.
- **Leak audit.** The text names no downstream application; the pre-push audit is dry-run before the push.
