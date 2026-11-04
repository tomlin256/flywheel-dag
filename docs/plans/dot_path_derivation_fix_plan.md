# Name `toSvg()`'s `.dot` File From the File Name Alone

**Status: Done (2026-11-04).** Approved as written. `toSvg()` names its `.dot` file from the file name alone,
and refuses a path with a NUL.

Closes [flywheel-dag#63](https://github.com/tomlin256/flywheel-dag/issues/63).

## Problem

`GraphExporter::toSvg()` (`include/flywheel/dag_graph.hpp`) names the intermediate `.dot` file with
`outPath.rfind('.')` over the whole path, then writes it, runs `dot`, and removes it on success. The last `.`
can be in a directory name, in a file name that is only an extension, or hidden by a NUL. A probe on `main`
(a stand-in `dot` that writes `<svg/>` to its `-o` file; the Mac, whose file system ignores case, and
`ubuntu-dev`, GCC 13.3.0) shows four ways to lose a file, each with `toSvg()` returning `true`:

| Output path | What happens today |
|---|---|
| `<w>/keep.d/graph`, with `<w>/keep.dot` beside the directory | the DOT is written to `<w>/keep.dot`, replacing it, and removed on success: `keep.dot` is gone (Mac and Linux) |
| `<w>/graph.dot` | input and output are one file, and removing the `.dot` file removes the output. Real Graphviz 14.1.4 exits 1 and leaves SVG in the file (flywheel-dag#63) |
| `<w>/up.DOT` | the same on the Mac, where `up.dot` and `up.DOT` are one file; fine on Linux |
| `x.svg` then a NUL then `y.z` | the OS sees `x.svg`, which is also the output: the output is gone |

`std::filesystem::path::replace_extension` looks at the last component only. On the 10 names probed
(`graph.svg`, `keep.d/graph`, `graph.v2.svg`, `.hidden`, `a/.hidden`, `graph.`, `graph.dot`, `graph.DOT`, `dir/`,
`a.b/c`) libc++ and libstdc++ give the same answer, and it equals today's for a file name with or without one
extension in a directory path that has no dot.

## The change

```cpp
// beside outPath, named from the file name alone: its extension replaced by ".dot", or ".dot" appended
// when it has none; if the extension already is .dot (any case), ".dot" is appended to the whole path
static std::string dotPathFor(const std::string& outPath);
```

and `toSvg()` returns `false` first thing for an `outPath` with a NUL, before it writes anything. The header
comment of `toSvg()` states the rule. The stem of the derived name is the stem of `outPath`, byte for byte, so
the two names can only meet when the extension is `.dot` in some case: that is the whole collision test.

## Decisions

- **D1. Keep `graph.dot` for `graph.svg`.** The caller learns the kept file's name from this rule alone
  (`toSvg()` returns a `bool`), so renaming every kept file to `graph.svg.dot` would break callers to avoid a
  collision that needs a `.dot` output. Only the names that were wrong change.
- **D2. A `.dot` extension in any case falls back to `outPath + ".dot"`.** On the Mac `up.DOT` is the file
  `up.dot`; on Linux the fallback is merely unneeded, and the file is gone on success either way.
- **D3. A NUL is refused, not truncated.** Truncation is what turns it into the same file as the output.

## Steps

### Step 1 — The plan

Commit: `docs: add plan for naming toSvg's .dot file from the file name alone`. **Tests: none.** **Done when:**
the plan is committed and a comment on the issue gives its path.

### Step 2 — The change and its tests

**Tests written** in `tests/test_dag_graph.cpp`, in flywheel-dag#62's `DagGraphToSvg` fixture (stand-in `dot`,
scratch directory, no sleep). The success checks of that fixture are reused; they gain the `.dot` path as a
parameter, with today's callers passing the path they derive now:

| Test | Asserts |
|---|---|
| `NamesTheDotFileAfterTheLastExtensionOfTheFileName` | `graph.v2.svg` gives `graph.v2.dot`, `graph` gives `graph.dot`, `.hidden` gives `.hidden.dot`: dot is given that path, and it is gone afterwards |
| `LeavesAFileBesideADottedDirectoryAlone` | `<w>/keep.d/graph` with `<w>/keep.dot` holding `precious`: dot is given `<w>/keep.d/graph.dot`, and `keep.dot` still holds `precious` |
| `DoesNotReadTheFileItWrites` | `graph.dot` and `graph.DOT`: dot is given `graph.dot.dot` and `graph.DOT.dot`, the SVG is `graph.dot` and `graph.DOT`, and the `.dot.dot` files are gone |
| `RefusesAPathWithANul` | `false`; `dot` never ran; the scratch directory is empty |

The existing success tests keep the plain rule (`graph.svg` gives `graph.dot`). **Done when:**

- the build prints no warning and `ctest --preset relwithdebinfo-macos-arm64` passes in full (46 of 46);
- each hand-made change, on a scratch copy of `include/`, run test by test under a watchdog, fails the tests
  stated and no others:

  | Change | Fails |
  |---|---|
  | M1. the derivation is `rfind('.')` over the whole path again, with no fallback | `LeavesAFileBesideADottedDirectoryAlone`, `DoesNotReadTheFileItWrites`, and `NamesTheDotFileAfterTheLastExtensionOfTheFileName` (the `.hidden` name) |
  | M2. no fallback for a `.dot` extension | `DoesNotReadTheFileItWrites` |
  | M3. the extension is compared with case | `DoesNotReadTheFileItWrites` |
  | M4. a NUL is not refused | `RefusesAPathWithANul` |
  | M5. the name is always `outPath + ".dot"` | every test that expects `graph.dot` for `graph.svg`: the four success tests, the three that keep the `.dot` file, and `NamesTheDotFileAfterTheLastExtensionOfTheFileName` |

- the `clang_tidy_` tests pass (40 of 40) on the Mac and in `ubuntu-dev`, and `ctest` passes in full there, built
  at `--parallel 4`;
- CI is green on both legs after the push. CI's Ubuntu leg is GCC 15.2.0; no throwaway container is run for it
  here, because the change is `<filesystem>` calls and test code of the kind that passed there in flywheel-dag#62.

Commit: `fix: name toSvg's .dot file from the file name alone`.

### Step 3 — Close out

Status Done, the Outcome, and the issue closed once the run of Step 2's push is green. A docs commit; later runs
of docs commits are not waited for. The release is not part of this plan.

## Outcome

| Step | Status | Commit |
|---|---|---|
| 1 — The plan | Done | `e8e2db5`; the approval is recorded in `1e9e07f` |
| 2 — The change and its tests | Done | `90e6c4d` |
| 3 — Close out | Done | the commit that adds this section |

Every item of Step 2's "done when" held, on the files as committed:

- The build prints no warning, and `ctest` passes 46 of 46 on the Mac (the four tests are in `test_dag_graph`).
- The hand-made changes, run test by test under a watchdog on scratch copies of `include/`, with a control copy
  that has no change (nothing fails in it; 30 tests). Each fails exactly the tests the plan names:

  | Change | Fails |
  |---|---|
  | M1 | `NamesTheDotFileAfterTheLastExtensionOfTheFileName`, `LeavesAFileBesideADottedDirectoryAlone`, `DoesNotReadTheFileItWrites` |
  | M2 | `DoesNotReadTheFileItWrites` |
  | M3 | `DoesNotReadTheFileItWrites` |
  | M4 | `RefusesAPathWithANul` |
  | M5 | the four success tests, `KeepsTheDotFileWhenDotFails`, `KeepsTheDotFileWhenThereIsNoDot`, `ReturnsFalseWhenTheExitStatusCannotBeRead`, `NamesTheDotFileAfterTheLastExtensionOfTheFileName` |

- The `clang_tidy_` tests pass 40 of 40 on the Mac and in `ubuntu-dev`, and neither file has a `NOLINT`.
- In `ubuntu-dev` (GCC 13.3.0, glibc) the Linux tree builds at `--parallel 4` without a warning, `ctest` passes 46
  of 46, and the 13 `DagGraphToSvg` tests pass. Only `test_dag_graph` includes `dag_graph.hpp`, so it is the one
  binary that needed relinking.
- CI run 41843319933 on `90e6c4d` is green: `build` and `clang-tidy`, each on macOS and on Ubuntu (x86_64).

Where the work differs from the plan: it does not. No throwaway `ubuntu:26.04` container was run, as the plan
said; CI's Ubuntu leg is the GCC 15.2.0 check.

## Self-review — risks and assumptions

- **A name that was wrong changes, and nothing else does.** A caller that looked for the `.dot` file of a dotted
  directory or a hidden file looked in the wrong place. Ordinary names keep today's rule, which the existing
  tests pin (M5).
- **`dir/`** (a trailing slash) still gives `dir/.dot`, `dot` still cannot write a directory, and `toSvg()`
  still returns `false` with the `.dot` file kept. Unchanged, and not a loss of anything.
- **The case rule is ASCII.** The extension `.dot` is ASCII and the stems are identical, so no Unicode folding
  can make the two names meet.
- **The NUL check is on `outPath` only.** `graphName` goes into the DOT text, quoted, and a NUL there is data.
- **No benchmark.** `dag_graph.hpp` is not in the engine's hot path.
