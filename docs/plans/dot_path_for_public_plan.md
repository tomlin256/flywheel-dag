# Make `GraphExporter::dotPathFor` Public

**Status: Approved (2026-11-06).** The release, v1.2.0, is part of it (Step 3).

Closes [flywheel-dag#68](https://github.com/tomlin256/flywheel-dag/issues/68).

## Problem

`GraphExporter::toSvg()` (`include/flywheel/dag_graph.hpp`) writes the DOT text to a `.dot` file beside `outPath`, runs
`dot`, and removes the file when `dot` succeeds. When `dot` is missing or fails it keeps the file and returns `false`.
The file is named by `dotPathFor` (flywheel-dag#63), which is private, so a caller that wants to say which file was kept,
or to convert it later, writes its own copy of the rule from `toSvg()`'s comment. The rule has changed once already,
between v1.0.1 and v1.0.2, and a copy of the old one names a file `toSvg()` does not write for an `outPath` with a dot
in a directory name and none in the file name, and for one that ends in `.dot` in any case (the measured table is in
flywheel-dag#68).

## The change

`dotPathFor` moves from the private section to the public one, beside `toSvg()`, with its comment and its body as they
are; `asciiLower`, which it calls, stays private. `toSvg()`'s comment names it, and says when the file is not there after
a `false`: the `.dot` file could not be written (its directory is missing), or `outPath` was refused (a NUL). The
README's table of headers names it. Nothing else in `include/` changes.

## Decisions

- **D1. Move the function; add nothing else.** `toSvg()` returns a `bool` and keeps doing so (flywheel-dag#63, D1: the
  caller learns the kept file's name from the rule alone). A richer return value changes every caller, and an overload
  that reports the path would be a second place the rule is written in code. The existing function is the one `toSvg()`
  already calls, so the name it reports and the name `toSvg()` writes cannot disagree.
- **D2. The rule is API from now on.** Changing it later is a breaking change, where flywheel-dag#63 changed it in a
  patch release. The new public name makes the next release 1.2.0, as the new header of flywheel-dag#49 made v1.1.0.

## Steps

### Step 1 — The plan

Commit: `docs: add plan for making dotPathFor public`. **Tests: none.** **Done when:** the plan is committed and a
comment on the issue gives its path.

### Step 2 — The test and the change

**Tests written** in `tests/test_dag_graph.cpp`, in flywheel-dag#62's `DagGraphToSvg` fixture (stand-in `dot`, scratch
directory, no sleep):

| Test | Asserts |
|---|---|
| `DotPathForNamesTheFileKeptWhenThereIsNoDot` | with no `dot` on `PATH`, for `graph.svg`, `graph.v2.svg`, `graph`, `.hidden`, `keep.d/graph`, `graph.dot` and `graph.DOT`, each in a directory of its own: `dotPathFor(out)` is the string the documented rule gives, and `toSvg()` returns `false` leaving that one file and nothing else |
| `DotPathForNamesTheFileKeptWhenDotFails` | the same with a `dot` that exits 3: the files left are `dotPathFor(out)` and the SVG the stand-in wrote to `out`, and nothing else |
| `DotPathForKeepsARelativePathRelative` | `graph.svg` gives `graph.dot` and `out/graph.svg` gives `out/graph.dot`, with no file system involved |

The tests do not build while `dotPathFor` is private: that is the red. **Done when:**

- the build prints no warning and `ctest --preset relwithdebinfo-macos-arm64` passes in full;
- each hand-made change, on a scratch copy of `include/`, run test by test under a watchdog, fails the tests stated and
  no others:

  | Change | Fails |
  |---|---|
  | M1. `dotPathFor` is v1.0.1's rule again: everything before the last `.` of the whole path, `.dot` appended when there is none | the first two tests above, and flywheel-dag#63's `NamesTheDotFileAfterTheLastExtensionOfTheFileName`, `LeavesAFileBesideADottedDirectoryAlone` and `DoesNotReadTheFileItWrites` |
  | M2. `toSvg()` names its file `outPath + ".dot"` and no longer calls `dotPathFor` | the first two tests above, and the tests that expect `graph.dot` for `graph.svg` (flywheel-dag#63's M5) |

- the clang-tidy gate passes on the Mac and in `ubuntu-dev`, and `ctest` passes in full there;
- the pre-push leak check passes.

Commit: `feat: make GraphExporter::dotPathFor public`.

### Step 3 — The release

`build: release v1.2.0` moves the project version and the FetchContent snippets in `CMakeLists.txt` and the README
to 1.2.0, as v1.1.0's release commit did, and records `git diff v1.1.0..HEAD --stat -- include`. The checkpoint passes
on it before anything is pushed (the generated version file reports 1.2.0). Then `main` and an annotated tag `v1.2.0`
are pushed and a GitHub release is published, with notes in the form of v1.1.0's. **Tests: none written.** **Done
when:** `git ls-remote --tags origin v1.2.0` shows the tag at the release commit and `gh release view v1.2.0` shows
the notes.

### Step 4 — Close out

Status Done, the Outcome, and the issue closed. A docs commit; runs of docs commits are not waited for.

## Self-review

- **A public name is API once released.** Removing or changing `dotPathFor` is then a breaking change. That is the
  point of D2, and the cost of the accessor.
- **No behaviour changes.** `toSvg()` calls the same function with the same argument, so no file is named differently
  and no object code of another header changes. `dotPathFor` is pure: `std::filesystem::path` and ASCII lower-casing,
  no file touched.
- **Where it exists.** It is declared where `toSvg()` is (`FLYWHEEL_DAG_HAS_TO_SVG`), so a build without `<spawn.h>`
  sees neither.
- **A public function calling a private helper** (`asciiLower`) is fine: access is checked at the definition.
- **No benchmark.** `dag_graph.hpp` is not on the engine's hot path.
