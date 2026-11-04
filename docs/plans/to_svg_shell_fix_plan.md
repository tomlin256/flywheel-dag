# Run `dot` Without a Shell in `GraphExporter::toSvg`

**Status: Proposed (2026-11-04).** Awaiting approval. One decision is the maintainer's: D3, what `toSvg()` does
off POSIX.

Closes [flywheel-dag#62](https://github.com/tomlin256/flywheel-dag/issues/62).

## Problem

`GraphExporter::toSvg()` (`include/flywheel/dag_graph.hpp`) renders through the shell:

```cpp
const std::string cmd = "dot -Tsvg " + dotPath + " -o " + outPath;
const int rc = std::system(cmd.c_str());  // NOLINT(cert-env33-c, bugprone-command-processor)
```

Both paths go into the command line unquoted, and `toSvg()` is public API, so they come from wherever its
caller got them. A throwaway driver (Apple Clang 21 on the Mac, repository untouched) calls `toSvg()` on 14
output paths, 13 that can be written and one in a directory that does not exist, once against the unchanged
header and once against a scratch copy with the change below. It runs twice: with a stand-in `dot` on `PATH`
that records its arguments, beside a stand-in `pwn` command that leaves a marker file, and with the real
Graphviz 14.1.4 in place of the stand-in.

| Output path holds | Unchanged code | `posix_spawnp` |
|---|---|---|
| `;`, `$(pwn)`, a backtick, `\|`, a newline | the shell ran `pwn` (with real `dot` too: the marker exists and there is no SVG) | argv is exactly `-Tsvg`, the `.dot` path, `-o`, the output path; `pwn` did not run |
| a space (in the file or a directory name), `'` or `"` | the stand-in sees split or merged arguments, and `toSvg()` returns `true` with the SVG not at the path asked for; real `dot` cannot open the pieces, and `toSvg()` returns `false` | one argument, intact |
| a leading `-` (`-graph.svg`) | the stand-in sees `-graph.dot` as given; real `dot` rejects `-d` as an option. For `-x.dot` it takes `-x` for an option, reads standard input and exits 0, having written nothing | `./-graph.dot` and `./-graph.svg` |
| the 13 writable paths, real `dot` | 2 produce an SVG (the plain path and one with `*?[`) | 13 of 13, each named exactly |
| the missing directory | `false`; `dot` never ran | the same |

The new code is also run on Linux in `ubuntu-dev` (GCC 13.3.0, glibc), with the same argv for every path, and
compiles clean with `-Wall -Wextra -Wpedantic -Werror` under Apple Clang 21 and GCC 13.3.0. The second `NOLINT` name on the
`std::system` line is flywheel-dag#60's: clang-tidy 22 and later report the call as `bugprone-command-processor`,
and the finding is suppressed there with a pointer to this issue. Without the shell there is nothing to
suppress: clang-tidy 23.1.4 with the repo's `.clang-tidy` reports nothing in the new `dag_graph.hpp`, and the
same run reports that check at once on a copy with one `std::system` call appended, so the silence is not the
tool failing to look.

## The change

`toSvg()` writes the `.dot` file as before and then calls a private `runDot(dotPath, outPath)`:

```cpp
std::vector<std::string> args{ "dot", "-Tsvg", asArgument(dotPath), "-o", asArgument(outPath) };
// ... a char* vector over args, ending in nullptr ...
pid_t pid = 0;
if (posix_spawnp(&pid, "dot", nullptr, nullptr, argv.data(), env) != 0) return false;
// waitpid, retried on EINTR; true only for WIFEXITED && WEXITSTATUS == 0
```

`asArgument()` turns a leading `-` into `./-`, which names the same file and cannot be an option. The
contract in the header's comment is unchanged: `true` removes the `.dot` file; `false` (no `dot` on `PATH`, a
non-zero exit, an unwritable directory) keeps it. The child gets the caller's environment and stdio, as it did
under `std::system`; `env` is `environ`, or `*_NSGetEnviron()` on Apple, where a shared library cannot name
`environ`. The header comment, the `NOLINT` line in `CLAUDE.md` (`toSvg` until flywheel-dag#62) and the README's
row for `dag_graph.hpp` change with it.

## Decisions

- **D1. Spawn; do not quote.** Quoting each path keeps `std::system`, so the shell and the `NOLINT` stay, and the
  quoting is the shell's: `sh` and `cmd.exe` quote differently, so one quoting function is portable to one of
  them. An argument vector has nothing to get wrong.
- **D2. `posix_spawnp`, not `fork` and `execvp`.** A process with feed threads (an engine's) may only call
  async-signal-safe functions between `fork` and `exec`, and `execvp` is not on that list. `posix_spawnp` is
  built for it.
- **D3. Off POSIX, `toSvg()` is not declared.** The issue asks for this decision. `<spawn.h>` is POSIX, and this
  header is the only one in the engine that would need it. The header chooses by `__has_include(<spawn.h>)` and
  `__has_include(<sys/wait.h>)` and defines `FLYWHEEL_DAG_HAS_TO_SVG` when `toSvg()` exists; without them the
  class has `toDot()` and the header still compiles, so a Windows consumer keeps the DOT export. Not taken:
  - A `CreateProcessW` branch. CI builds on Linux (GCC) and macOS only (README), so the branch and its own
    command-line quoting rules could be neither built nor run here, which breaks "every change is unit-tested".
    Supporting Windows is a plan with a Windows leg.
  - `toSvg()` returning `false` off POSIX. A caller would read it as "Graphviz is absent" for ever. A compile
    error says what is true.

  The cost: a consumer that calls `toSvg()` on Windows today stops compiling, and writes `toDot()`'s text and
  runs `dot` itself. The release notes say so.
- **D4. A leading `-` becomes `./-`.** With the shell gone, an argument that `dot` takes for an option is the
  remaining way a path is read as something other than a path, and the table's `-x.dot` row shows it happens,
  silently. `./` names the same file and needs no `--`, which Graphviz 14.1.4 rejects ("option -- unrecognized").
- **D5. Out of scope: the `.dot` path's derivation.** `rfind('.')` over the whole path overwrites and deletes an
  unrelated `keep.dot` for `.../keep.d/graph`, and an `outPath` ending in `.dot` is its own input. Found while
  measuring this issue, and unrelated to the shell: [flywheel-dag#63](https://github.com/tomlin256/flywheel-dag/issues/63).

## Steps

### Step 1 — The plan

Commit: `docs(#62): add plan for running dot without a shell in toSvg`. **Tests: none.** **Done when:** the plan
is committed and a comment on the issue gives its path.

### Step 2 — The change and its tests

**Tests written** in `tests/test_dag_graph.cpp`, inside `#ifdef FLYWHEEL_DAG_HAS_TO_SVG`, in a fixture that puts a
stand-in `dot` (a `/bin/sh` script that records its arguments NUL-separated and the variable it was handed,
writes `<svg/>` to the `-o` file, and exits with the code in a file beside it, so that no behaviour of the
stand-in rides on the environment) and a stand-in `pwn` on `PATH`, runs in a scratch directory under the system
temp directory, and puts `PATH` and the working directory back. No sleep, no real Graphviz. The four success
tests share one check: `true`; argv is exactly `-Tsvg`, the `.dot` path, `-o`, the output path; the SVG exists;
the `.dot` file is gone; `pwn` did not run.

| Test | Asserts |
|---|---|
| `RunsDotWithThePathsAsArguments` | the shared check, for a plain path |
| `PassesAnyNameAsOneArgument` | the shared check for a file named with a space, `;`, `$(pwn)`, a backtick, `\|`, `&`, `'`, `"`, a newline, `*?[` |
| `PassesAnyDirectoryNameAsOneArgument` | the shared check for a directory named `dir with space;pwn` |
| `HandsDotAPathThatStartsWithADashAsAPath` | from the scratch directory, `-graph.svg`: the shared check with argv `./-graph.dot`, `./-graph.svg` |
| `KeepsTheDotFileWhenDotFails` | the stand-in exits 3: `false`, the `.dot` file kept |
| `KeepsTheDotFileWhenThereIsNoDot` | `PATH` without `dot`: `false`, the `.dot` file kept |
| `ReturnsFalseWhenTheExitStatusCannotBeRead` | the test ignores `SIGCHLD`, so `waitpid` fails with `ECHILD`: `false`, the `.dot` file kept, though the stand-in succeeded |
| `RunsNothingWhenTheDirectoryIsMissing` | `false`; `dot` never ran; no `.dot` file anywhere |
| `HandsDotTheCallersEnvironment` | a variable set by the test reaches the stand-in |

The tests and the header go in one commit, because a red commit fails the checkpoint. The red run is mutant M1
below, which is the unchanged code. **Done when:**

- the build prints no warning and `ctest --preset relwithdebinfo-macos-arm64` passes in full (46 of 46 today;
  the tests join `test_dag_graph`, so the count holds);
- each hand-made change below, on a scratch copy of `include/` (flywheel-dag's usual way: never the tree), fails
  the tests stated and no others, run test by test under a watchdog:

  | Change | Fails |
  |---|---|
  | M1. `runDot()` is the unchanged `std::system` line | `PassesAnyNameAsOneArgument`, `PassesAnyDirectoryNameAsOneArgument`, `HandsDotAPathThatStartsWithADashAsAPath` |
  | M2. `asArgument()` returns its argument unchanged | `HandsDotAPathThatStartsWithADashAsAPath` |
  | M3. any exit status is `true` | `KeepsTheDotFileWhenDotFails` |
  | M4. the `.dot` file is removed on failure too | `KeepsTheDotFileWhenDotFails`, `KeepsTheDotFileWhenThereIsNoDot` |
  | M5. the `.dot` file is never removed | the four success tests |
  | M6. `posix_spawnp` gets an empty environment | `HandsDotTheCallersEnvironment` |
  | M7. `toSvg()` ignores a `.dot` file that would not open | `RunsNothingWhenTheDirectoryIsMissing` |
  | M8. `runDot()` does not check that `waitpid` returned the child | `ReturnsFalseWhenTheExitStatusCannotBeRead` |

- the `clang_tidy_` tests pass (40 of 40) on the Mac and in `ubuntu-dev`, and `dag_graph.hpp` has no `NOLINT`;
- `CLAUDE.md` no longer lists `toSvg` among the `NOLINT` sites, and the README's row for `dag_graph.hpp` says
  `toSvg()` needs a POSIX host and Graphviz's `dot`;
- `ctest` passes in full in `ubuntu-dev` (GCC 13.3.0, glibc), and `tests/test_dag_graph.cpp` compiles clean with
  the Linux tree's flags under GCC 15.2.0 (a throwaway `ubuntu:26.04`, CI's compiler);
- CI is green on both legs after the push.

Commit: `fix(#62): run dot without a shell in GraphExporter::toSvg`.

### Step 3 — Close out

Status Done, the Outcome, and the issue closed once the run of Step 2's push is green. A docs commit; later runs
of docs commits are not waited for. The release is not part of this plan.

## Self-review — risks and assumptions

- **Shell expansion of `outPath` goes.** `toSvg(roots, "~/g.svg")` and `"$HOME/g.svg"` used to expand; they now
  name a directory called `~` or `$HOME`. That is the bug, not a regression to keep, and it is in the release
  notes with D3.
- **`PATH` lookup is measured on two libcs.** The stand-in is found through the test's own `setenv("PATH")` on
  macOS (Apple libc) and in `ubuntu-dev` (glibc). musl and the BSDs are not measured; CI runs glibc and macOS.
- **`waitpid` can fail.** A process that ignores `SIGCHLD` gets `ECHILD` and no status: `false`, and the
  `.dot` file is kept, though `dot` succeeded and wrote the SVG. That is what `std::system` did, measured on the
  Mac and in `ubuntu-dev` (three rounds each, old code and new), and the status is initialised to 0, which reads
  as success, so the child's pid check is what stands between an unknown status and `true`: M8 pins it. The
  `EINTR` retry has no test. A deterministic one needs a signal to land inside `waitpid`, and the ways to aim
  one are timers and sleeps, which the suite does not use; it is the standard three-line loop.
- **`extern "C" char** environ;`** redeclares glibc's own declaration. It is compatible, and the prototype
  compiles under Apple Clang 21 and GCC 13.3.0; GCC 15.2.0 is checked at Step 2.
- **The tests change process state** (`PATH`, the working directory, one environment variable) and put it
  back in `TearDown`, which gtest runs after a failed `ASSERT_` and after a fatal failure in `SetUp()`. Tests in
  a gtest executable run one at a time.
- **The stand-in is a shell script**, so the tests are POSIX; they compile only where `toSvg()` does.
- **No benchmark.** The engine and `Engine::cycle()` do not include `dag_graph.hpp`, so the invariants and the
  disassembly cannot move, and I do not run them.
- **A downstream caller on POSIX** sees the same signature and the same contract.
