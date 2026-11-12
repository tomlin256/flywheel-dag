# Close Out the Hardening Milestone

**Status: Done (2026-11-12).** Approved as written, so D1 to D8 stood at their defaults. The five issues are closed,
v1.3.1 is released and the milestone is closed. The Outcome at the end records what each step found and where the work
differs from this text: the two includes that Step 4 puts in `dag.hpp` went into `dag.inl`. Everything under "Measured"
was taken read-only at `0b21b87`, before the approval, in scratch copies and scratch trees outside the checkout.

Closes [flywheel-dag#32](https://github.com/tomlin256/flywheel-dag/issues/32),
[flywheel-dag#66](https://github.com/tomlin256/flywheel-dag/issues/66),
[flywheel-dag#67](https://github.com/tomlin256/flywheel-dag/issues/67),
[flywheel-dag#69](https://github.com/tomlin256/flywheel-dag/issues/69) and
[flywheel-dag#73](https://github.com/tomlin256/flywheel-dag/issues/73), the five issues of the
[Hardening milestone](https://github.com/tomlin256/flywheel-dag/milestone/2): "small, specified changes that make an
existing guarantee hold by default or by a check". One plan for the five, because each is one checkpoint, they share
the gates, and the one that touches `include/` wants one release.

| Issue | The change | What holds it afterwards |
|---|---|---|
| flywheel-dag#73 | 16 rows in `tests/check_platform.cmake` | the rows themselves (four hand-made changes) |
| flywheel-dag#32 | `RollingSumNode` joins the cast test's list | `test_stateful_node_list`, new |
| flywheel-dag#66 | `<cmath>` and `<typeinfo>` in `dag.hpp`, `<ctime>` in `dag_state_store.inl` | `test_headers_alone`, new, and `clang_tidy_headers_alone`, new, in the gate |
| flywheel-dag#69 | nlohmann/json from its release archive; spdlog and googletest cloned shallow | `test_fetch_declarations`, new |
| flywheel-dag#67 | `-Werror` on by default at the top level | `test_warning_option`, new |

Two of the issues turned out differently from how they read, and both are under "Measured": the engine's headers do
not compile today under LLVM 23 with its own libc++ (flywheel-dag#66 is a build failure there, not a risk), and
`GIT_SHALLOW TRUE` saves half of what flywheel-dag#69 expected of it.

## Measured

### flywheel-dag#73 — the platform table

`test_platform_name` has 56 cases. With the issue's 16 rows it has 72, and all hold against the module as it is. Four
hand-made changes to a copy of `cmake/flywheel_dag_platform.cmake`, each at the kind pattern's three sites or at the
`Darwin` test, each against the table as it is and with the rows:

| Change to the module | 56 cases today | 72 cases with the rows |
|---|---|---|
| M1. A kind may start with a digit (`[a-z0-9]+`) | all hold | 9 fail: eight digit-led names are refused, and `2025-macos-arm64` that remembers `macos-arm64` is refused for its claim, not its record |
| M2. `darwin` is matched after the lower-casing | all hold | 2 fail: `darwin` and `DARWIN` give `macos-arm64` |
| M3. A kind is letters only (`[a-z]+`) | all hold | 1 fails: `gcc13-linux-x86_64` on `linux-arm64` is accepted |
| M4. A kind has two characters or more | all hold | 1 fails: `x-macos-arm64` on `linux-arm64` is accepted |

The issue measured M1 and M2; M3 and M4 are what its last three rows are for. Three of the 16 rows fail under none of
the four and are the truthful half of a pair: `2025-macos-arm64` on `macos-arm64`, `9lives-linux-arm64` on
`linux-arm64` (accepted either way), and `gcc13-linux-arm64` on `linux-arm64`.

### flywheel-dag#32 — the stateful node list

Fourteen classes derive `StatefulNodeBase`, all in `dag_timeseries.hpp`, and no other class in `include/` implements
`IStatefulNode`. `CastWorksOnEveryStatefulNodeType` builds thirteen. Its comment cites the issue, and
`tests/CMakeLists.txt` lists 32 in `OPEN_ISSUES`, so the citation and the number leave together. Nothing compares the
list with the header, which is how a fourteenth class went unnoticed.

### flywheel-dag#66 — headers and the standard headers they use

`dag.hpp` has had `<string>` since `eea43b9`. The rest of the issue stands, and one part of it is worse than it reads.
Each of the 16 public headers, as the only include of a unit, `-std=c++23 -fsyntax-only`:

| Compiler and standard library | Headers that compile alone |
|---|---|
| Apple clang 21, the SDK's libc++ (22.1) | 16 of 16, and 16 of 16 with `-D_LIBCPP_REMOVE_TRANSITIVE_INCLUDES` |
| GCC 13.3, libstdc++ (`ubuntu-dev`) | 16 of 16 |
| GCC 15.2, libstdc++ (`ubuntu:26.04`) | 16 of 16 |
| clang 18 and clang 23, libstdc++ (`ubuntu-dev`) | 16 of 16 |
| LLVM 23 `clang++` (Homebrew), its own libc++ (23.1) | **2 of 16**: fourteen stop at `dag.inl:81`, "no member named 'abs' in namespace 'std'" |
| the same, with `<cmath>` added to a copy of `dag.hpp` | 16 of 16 |

So a program built with LLVM 23 and its libc++ cannot include the engine unless something includes `<cmath>` first. The
call is a qualified name in a template, looked up where the template is defined, so it fails in every unit, whether or
not `EpsilonPolicy` is used. The SDK's libc++ is one major behind LLVM's and still brings `std::abs` in through
another header.

clang-tidy 23, given a one-include unit with its flags after `--`, compiles it against LLVM's own libc++ on the Mac
(`_LIBCPP_VERSION` 230104 there, 220106 under Apple clang): 2 of 16 at `HEAD`, each failure a
`[clang-diagnostic-error]` at `dag.inl:81`, and 16 of 16 with no finding on the copy that has `<cmath>`; the 16 take 3
seconds, six at a time. The gate's unit tests do not see it: a unit run through a compile database whose command
names Apple's compiler meets the SDK's library (a probe unit that reports its `_LIBCPP_VERSION` says 22 that way and
23 with its flags after `--`).

`clang-include-cleaner` 23 (`--disable-remove --print=changes`), on the copy with `<cmath>`, with each `.hpp` as the
main file and each `.inl` made the main file of a unit that includes its `.hpp`:

- **Two more standard headers that no engine header on the way names.** `<typeinfo>`: `dag.inl:22` and `:39` say
  `typeid` and `std::bad_cast`, and `dag.hpp` names `<typeindex>`. `<ctime>`: `dag_state_store.inl:273` says
  `std::strftime` and `std::gmtime`, and the file names `<chrono>`. Every library above brings both in, so no compile
  shows them.
- **Two findings that are the tool's.** `<cstdlib>` for `std::abs` in `dag_timeseries.inl`, which `<cmath>` declares
  too; and `<sys/_types/_pid_t.h>` for `pid_t` in `dag_graph.hpp`, which `<spawn.h>` and `<sys/wait.h>` define.
- **The tool is not an oracle either.** It did not list `<cmath>` for `dag.inl:81`: the call's argument depends on
  the template's parameter.
- **32 lines in 10 headers** would make each header and its `.inl` name every standard header their own code uses.
  Two are the two above. The other 30 name a header that an engine header on the way already names:
  `dag_async.hpp`'s `<string>`, which the issue asks for, is one, and reaches it through `dag.hpp`. Those cannot
  break a build, since the engine's own headers are the engine's to keep.

clang-tidy's `misc-include-cleaner` would not hold any of this: it reads a unit's main file only. Over the 38 units
it reports 243 findings, every one in `tests/`, `examples/` or `benchmarks/`, and none in `include/`.

### flywheel-dag#69 — what a cold configure fetches

One fresh configure each on the Mac, CMake 4.4.5, with `-Werror=author -Werror=deprecated`:

| Declarations | `_deps` | Configure |
|---|---|---|
| as they are | 425 MB: nlohmann/json 360, spdlog 46, googletest 20 | 24 s |
| `GIT_SHALLOW TRUE` on the three | 208 MB: nlohmann/json 196, spdlog 5, googletest 8 | 8 s |
| nlohmann/json from its release archive, the other two shallow | 15 MB: nlohmann/json 2 | 3 s |

All three exit 0 with no warning line, check out the commits the tags name (`git describe` says `v1.17.0`, `v3.12.0`,
`v1.18.0`) and leave the same working files; the archive's `include/` is byte for byte the tag's.

The issue expected 37 MB from shallow clones, measured with `git clone --depth 1 --branch <tag>`. CMake's shallow
clone is `--depth 1 --no-single-branch` (`ExternalProject/shared_internal_commands.cmake`), which fetches the tip of
every branch and tag, and nlohmann/json's are 196 MB. The archive is what its release publishes for this purpose,
`json.tar.xz`: 114,576 bytes, SHA-256 `42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa`, holding the
headers, `CMakeLists.txt` and `cmake/`. A scratch tree that fetched it builds with no warning and passes the suite,
85 of 85, the tests that build and install json from the fetched tree among them (`install_dependencies`,
`test_consumer_package`, `test_consumer_package_own_dependencies`).

A tree made before the change is not fetched again: a full-depth tree, reconfigured after its declarations gained
`GIT_SHALLOW`, took 0.2 s and kept its clones.

### flywheel-dag#67 — `-Werror` by default

CI is green with the option on: `CI` run 43182800843 on `0b21b87`, both legs, and the scheduled `Sanitizers` run
43209434927, `asan` and `tsan`. Those are RelWithDebInfo trees. With the default on, the `release` and `debug` presets
become strict too, so each was built in a scratch tree with `-DFLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON`, every target and
`bench_hot_path`:

| Compiler | RelWithDebInfo | Release | Debug |
|---|---|---|---|
| Apple clang 21 (the Mac) | CI's macOS leg | 51 objects, no warning | 51 objects, no warning |
| GCC 13.3 (`ubuntu-dev`) | 51 objects, no warning | 51 objects, no warning | 51 objects, no warning |
| GCC 15.2 (`ubuntu:26.04`) | 51 objects, no warning | 51 objects, no warning | 51 objects, no warning |

The option is named in eight files besides `CMakeLists.txt`: `ci.yml` (a comment and the configure line),
`sanitizers.yml` (two comments and the configure line), `README.md` (three places), `CLAUDE.md`, `docs/releasing.md`,
`tests/CMakeLists.txt`, `tests/check_presets.cmake`, and `tests/check_ci_sanitizers.cmake`, which today *requires*
`=ON` on the sanitizer configure line. Two comments say the default build is not `-Werror`
(`tests/check_tsan_fences.cmake`, `tests/CMakeLists.txt`). `CLAUDE.md` is 6,499 bytes against a budget of 6,500.
`actions/checkout@v6`, the one action the three workflows use, runs on `node24`.

## Decisions to confirm

Each has a default, which is what the steps below do.

- **D1. One plan, and how its commits are scoped.** A commit that serves one issue cites that issue. The plan's own
  commits and the release commit cite flywheel-dag#73, the last issue of the batch, as `docs/releasing.md` has a
  release commit do.
- **D2. flywheel-dag#66 fixes what another library's header was trusted for: three lines.** `<cmath>` and `<typeinfo>`
  in `dag.hpp`, `<ctime>` in `dag_state_store.inl`. It does not add the 30 lines that an engine header on the way
  already names, `dag_async.hpp`'s `<string>` among them: the issue's reason is that the standard does not promise
  what a *standard* header includes, and since `eea43b9` that header reaches `<string>` through `dag.hpp`. Naming all
  32 is include-what-you-use proper, and it stays true only with `clang-include-cleaner` as a gate: a tool the Ubuntu
  lint leg does not install, a wrapper unit for each `.inl`, and a list for the tool's two false findings. That is a
  design of its own and not a small, specified change, so it becomes an issue (D8). *Alternative:* add the 30 lines
  here, held by nothing.
- **D3. The headers are held twice.** `test_headers_alone` compiles each header alone with the tree's compiler, in
  every tree. It is green today under Apple clang and GCC, so it would not have caught `std::abs`; it catches a
  header that needs another to come first. `clang_tidy_headers_alone` is the one that is red today: the gate's tool
  over the same units, which on a Mac means LLVM's own libc++. It joins the gate, so the lint workflow's macOS leg and
  a release's local gate run it with no new option and no new install. On Linux the tool compiles against libstdc++,
  which shows nothing more; that leg is kept because it costs seconds. *Alternative:* `test_headers_alone` only, and
  `<cmath>` is then held by no test until Apple's or GNU's library drops the include.
- **D4. nlohmann/json comes from its release archive, with its SHA-256; spdlog and googletest are cloned shallow.**
  That is 15 MB and 3 s against 425 MB and 24 s, and the hash pins the bytes where a tag pins a name. Its cost: the
  three pins are no longer spelled alike, a pin bump changes a URL and a hash together, and the fetched json tree has
  no `.git` to ask `git describe` of. *Alternative:* `GIT_SHALLOW TRUE` on all three, the issue as written, for 208 MB
  and 8 s.
- **D5. No workflow names `FLYWHEEL_DAG_WARNINGS_AS_ERRORS`, and a relaxed tree says so.** The default is the one
  place that sets it, CI tests the default, and `test_warning_option` refuses a workflow that names the option, which
  replaces `test_ci_sanitizers`'s rule that the line must carry `=ON`. `option()` caches, so a tree made before the
  change keeps `OFF` until `cmake -UFLYWHEEL_DAG_WARNINGS_AS_ERRORS`; a top-level configure with the option off
  therefore prints one status line, which makes a relaxed tree visible, a stale one included.
- **D6. flywheel-dag#32 gets a check, not only the row.** The row closes the instance. `test_stateful_node_list`
  closes the cause: it reads the classes that derive `StatefulNodeBase` from the headers and fails on one that the
  test does not build. Its cost is one `cmake -P` script that runs in milliseconds.
- **D7. Release v1.3.1 when the five are in.** `dag.hpp` changes, and the change is a fix: a patch. The number is
  yours to confirm. The release also carries the docs and test commits since v1.3.0.
- **D8. One issue to open on approval**, with no milestone, since it would reopen this one: hold
  include-what-you-use on the engine's headers with `clang-include-cleaner` (the 30 lines, the wrapper for an `.inl`,
  the two false findings, and the `<typeinfo>` and `<ctime>` kind of gap that no compile shows). The measurements
  above go in it.

## Steps

Every checkpoint is a full build and `ctest --preset relwithdebinfo-macos-arm64` on the Mac, passing in full, and a
push; CI is read in passing and not waited for. A step that changes what is compiled or how (4, 5 and 6) also builds
and tests in `ubuntu-dev` (`relwithdebinfo-linux-arm64`) and runs the clang-tidy gate on both hosts. Hand-made changes
are made to a scratch copy, never the tree, each after an assertion that its old text occurs once, beside an unchanged
control. The ctest count goes from 85 to 89, and the gate, when it is on, gains one test.

### Step 1 — The plan

Commit: `docs: add plan for closing out the Hardening milestone`. Then a comment on each of the five issues that
names this file. **Tests: none.** **Done when:** the plan is committed and pushed, you have approved it, and each
issue names it.

### Step 2 — flywheel-dag#73: the rows

**Tests modified:** `tests/check_platform.cmake` (`test_platform_name`): the 16 rows, each in the section it belongs
to, and a line in the header comment for what they hold. The module does not change.

**Done when:** `test_platform_name` reports that all 72 cases hold; M1 to M4, repeated on the committed table, fail
9, 2, 1 and 1 cases as measured, and a control fails none. `ubuntu-dev` is not run: the table hands the functions a
host, and CI runs it on both legs.

Commit: `test: hold a kind's first letter and the exact Darwin spelling in the platform table`.

### Step 3 — flywheel-dag#32: the list, and a check that it is whole

**Tests new:** `tests/check_stateful_node_list.cmake` (`test_stateful_node_list`, registered at the top level). It
reads every header under `include/flywheel/` for a class that derives `StatefulNodeBase<Name, …>`, reads the body of
`TEST(StatefulDiscovery, CastWorksOnEveryStatefulNodeType)`, and fails on a name that the body does not `make()`. It
fails too if it finds no class or no such test, so that a moved file cannot pass by reading nothing.
**Tests modified:** `tests/test_engine_stateful_discovery.cpp` (the row `RollingSumNode::make("sum", inp_d, 4)`, and
the comment, which says what the test does and cites nothing); `tests/CMakeLists.txt` (`OPEN_ISSUES` loses 32).

The order is red then green: the check goes in first and must fail naming `RollingSumNode` and no other class.

**Done when:** the check holds for 14 classes; `test_comment_refs` passes with `OPEN_ISSUES=31`; and each hand-made
change fails the check with the text stated:

| Change | The check reports |
|---|---|
| N1. the `EWMANode` row removed from a copy of the test | `EWMANode` is not built |
| N2. a fifteenth class, `ProbeNode`, added to a copy of the header | `ProbeNode` is not built |
| N3. the test renamed in a copy | no such test |
| N4. the header's directory emptied | no class found |

Commit: `test: build every stateful node type in the discovery cast test, held by test_stateful_node_list`.

### Step 4 — flywheel-dag#66: three includes, and headers that compile alone

**Tests new:** `tests/check_headers_alone.cmake`, one script with two callers.

- `test_headers_alone`, at the top level for GCC and Clang, like `test_standard_guard`: each header under
  `include/flywheel/`, found by a glob at run time so that a new header is covered, is the only include of a unit
  compiled with the tree's compiler at `-std=c++23 -fsyntax-only`, with `_LIBCPP_REMOVE_TRANSITIVE_INCLUDES` defined
  (libc++ reads it, libstdc++ ignores it). Seven seconds for the 16 on the Mac.
- `clang_tidy_headers_alone`, registered by `cmake/flywheel_dag_clang_tidy.cmake` with the rest of the gate: the same
  units through the gate's tool, with `.clang-tidy` and the flags after `--`.

Both run a control that must *not* compile (a unit that names `std::string` and includes nothing), so that a compiler
whose exit status is not read cannot pass. On a Mac the gate's caller runs one more, which must compile: a unit that
stops unless `_LIBCPP_VERSION` is the tool's major. That is what says the units met LLVM's own libc++ and not the
SDK's, the difference between a test that sees `std::abs` and one that does not.

**Tests modified:** `tests/check_clang_tidy_wiring.cmake` (the gate's fixed tests gain the new name, and its
command is checked); `tests/CMakeLists.txt` (the registration and its comment).

**The change:** `#include <cmath>` and `#include <typeinfo>` in `dag.hpp`, `#include <ctime>` in
`dag_state_store.inl`. `README.md` says under "Headers" that each compiles alone, and under "clang-tidy" what the
gate adds. `CLAUDE.md` does not change.

The order is red then green: the tests first, with `clang_tidy_headers_alone` failing on the Mac for fourteen
headers at `dag.inl:81`, then the includes.

**Done when:**

- both tests pass on the Mac and in `ubuntu-dev`, and the whole gate (`-R '^clang_tidy_'`) passes on both;
- `bench_hot_path --invariants` matches `benchmarks/expected_invariants.txt` on both. It is not timed: an include
  line changes no code;
- each hand-made change to a copy of `include/`, passed as `INCLUDE_DIR`, gives the result stated:

  | Change | `test_headers_alone` | `clang_tidy_headers_alone` on the Mac |
  |---|---|---|
  | H1. `<cmath>` removed from `dag.hpp` | passes, under Apple clang and under GCC: the reason for the second test | 14 headers fail at `dag.inl:81` |
  | H2. `"flywheel/dag.hpp"` removed from `dag_window_status.hpp` | names `dag_window_status.hpp` | names it |
  | H3. a new header that names `std::string` and includes nothing | names the new header | names it |
  | H4. the must-not-compile control given `<string>`, in a copy of the script | "the control compiled" | "the control compiled" |
  | H5. the gate's caller told the tool's major is 22 | not run | "the units were not compiled against the tool's libc++" |

- `clang-include-cleaner`, run as under "Measured" on the committed headers, lists no standard header that no
  engine header on the way names, other than the two that are the tool's.

Commit: `fix: dag.hpp names <cmath> and <typeinfo>, dag_state_store.inl names <ctime>, and every header compiles alone`.

### Step 5 — flywheel-dag#69: what a cold configure fetches

**Tests new:** `tests/check_fetch_declarations.cmake` (`test_fetch_declarations`, at the top level). It reads the
code, comments dropped, of every `CMakeLists.txt` and `*.cmake` in the checkout outside a build tree, itself
excepted, and each `FetchContent_Declare` must be one of three things: a `SOURCE_DIR` that fetches nothing; a `GIT_REPOSITORY` with
`GIT_SHALLOW TRUE` and a `GIT_TAG` that is not a 40-digit commit, which a shallow clone cannot name; or a `URL` with a
`URL_HASH SHA256=` of 64 hex digits. It fails if the root declares fewer than three.

**The change:** in `CMakeLists.txt`, nlohmann/json is `URL …/releases/download/v3.12.0/json.tar.xz` with its
`URL_HASH`, and spdlog and googletest gain `GIT_SHALLOW TRUE`; the comment over the block says why the three are not
spelled alike. `tests/consumer/CMakeLists.txt`'s two declarations, which model a consumer's and are never fetched,
gain `GIT_SHALLOW TRUE`.

The order is red then green: the check first, failing on five declarations.

**Done when:**

- a fresh configure of the real root on the Mac, with the strict flags, exits 0 with no warning line and a `_deps` of
  about 15 MB, and the same in `ubuntu-dev`;
- the full suite passes in the fresh tree on the Mac, the three tests that build or install from the fetched json
  among them (`install_dependencies`, `test_consumer_package`, `test_consumer_package_own_dependencies`), and in
  `ubuntu-dev`'s existing tree, which is not fetched again;
- each hand-made change fails the check with the text stated:

  | Change | The check reports |
  |---|---|
  | F1. `GIT_SHALLOW TRUE` removed from spdlog | spdlog is cloned at full depth |
  | F2. `URL_HASH` removed from nlohmann/json | the archive has no hash |
  | F3. a hash of 63 digits | the hash is not a SHA-256 |
  | F4. googletest's `GIT_TAG` made a 40-digit commit | a shallow clone cannot name a commit |
  | F5. a fourth declaration with a `GIT_REPOSITORY` alone, in `tests/consumer` | names it |

  and the declaration in the root's header comment, which is a comment, is not read (the control).

Commit: `build: fetch nlohmann/json as its release archive and clone the other two shallow, held by test_fetch_declarations`.

### Step 6 — flywheel-dag#67: `-Werror` by default

**Tests new:** `tests/check_warning_option.cmake` (`test_warning_option`, registered as `test_warning_flags` is: top
level, GCC or Clang, Makefiles or Ninja). Like `test_sanitize_option`, it configures the real root offline into
scratch trees whose names claim no platform, compiles nothing, and reads each tree's `compile_commands.json` and
cache:

- **no option:** every unit of this project, at least one each under `tests/`, `examples/` and `benchmarks/`, has
  `-Wall -Wextra -Wpedantic -Werror=deprecated-declarations` and `-Werror`; no dependency's unit has `-Werror` or
  `-Wpedantic`, at least one having been seen; the cache holds the option as `ON` and an empty `CMAKE_CXX_FLAGS`; and
  the relaxed tree's status line is absent. The default the script expects is one line in it, so the root's default
  and the test cannot part unseen.
- **`=OFF`:** no unit of this project has `-Werror`, the other three flags stay, and the status line is there.
- **a parent project** (`tests/consumer`): with no option, its cache has no entry of the option's name; with the
  option on its command line, no unit has any of the flags.
- **the workflows:** no line of code in `.github/workflows/` names the option.

**Tests modified:** `tests/check_ci_sanitizers.cmake` (the rule that the configure line carries `=ON` goes; the
workflows rule above replaces it for all three); `tests/CMakeLists.txt`.

**The change:** `option(FLYWHEEL_DAG_WARNINGS_AS_ERRORS … ON)`, its comment, and the status line when it is off;
`ci.yml` and `sanitizers.yml` drop the `-D` and say why in the comment; `README.md` says that the build is strict by
default, that `=OFF` relaxes it for a compiler whose new warning would otherwise stop you, and that a tree made
earlier wants `-U`; `docs/releasing.md`'s gate line drops the option; `CLAUDE.md`'s bullet is rewritten in no more
bytes than it has; and the comments in `tests/check_tsan_fences.cmake`, `tests/check_presets.cmake` and
`tests/CMakeLists.txt` say what is true of a strict default.

The order is red then green: the test first, failing on the no-option tree, then the default.

**Done when:**

- each of the five kinds builds from its preset with no `-D`, every target and `bench_hot_path`, on the Mac and in
  `ubuntu-dev`, after `cmake --preset <kind> -UFLYWHEEL_DAG_WARNINGS_AS_ERRORS` has cleared what its cache held, and
  the full suite passes in the `relwithdebinfo` tree of each;
- each hand-made change to a copy of the root or of a workflow fails `test_warning_option` in the case stated:

  | Change | Fails |
  |---|---|
  | W1. the default back to `OFF` | no option: every unit lacks `-Werror`, and the cache holds `OFF` |
  | W2. the flags set before the dependencies are made | no option: spdlog's units carry them |
  | W3. the option declared outside the top-level block | the parent project: its cache holds the option |
  | W4. `-Werror` put on `flywheel_dag`'s `INTERFACE` | the parent project: its units carry the flag |
  | W5. `-DFLYWHEEL_DAG_WARNINGS_AS_ERRORS=OFF` on `ci.yml`'s configure line | the workflows, naming `ci.yml` |
  | W6. the status line removed | `=OFF` |

- `test_ci_sanitizers`, `test_cmake_diagnostics`, `test_ci_paths`, `test_presets` and `test_claude_md_budget`, the
  checks that read the files this step edits, pass;
- `sanitizers.yml` runs on a schedule or by hand only, so after the push it is started once
  (`gh workflow run sanitizers.yml --ref main -f which=both`) and both jobs are read.

Commit: `build: make -Werror the default at the top level, held by test_warning_option`.

### Step 7 — Release v1.3.1

As `docs/releasing.md` has it: the scope from `git log` and `git diff --stat -- include` since v1.3.0, the version in
its three places, the gate on the Mac and in `ubuntu-dev` on the tree as committed (strict configure, full build,
full suite, the clang-tidy gate, `bench_hot_path --invariants`), the release commit, the annotated tag, the GitHub
release with its notes, and CI read afterwards. **Tests: none new.** **Done when:** `gh release list` shows v1.3.1 as
Latest and the release commit's `CI` and `clang-tidy` runs have been read.

### Step 8 — Close out

The Outcome below, Status Done, a closing comment on each of the five issues and then each closed by hand, and the
milestone closed. A docs commit, pushed once the release commit's `CI` run has finished, since a push cancels a run
in flight.

## Not in this plan

- **A `paths-ignore` for `ci.yml`**, the rest of flywheel-dag#69's second item. flywheel-dag#72 decided against it,
  for a reason that `test_ci_paths` holds: the suite reads `CLAUDE.md`, `docs/engine_rules.md` and `README.md`.
  flywheel-dag#69 closes with items 1 and 2 as flywheel-dag#72 left them.
- **Include-what-you-use as a gate** (D8), and `misc-include-cleaner` over the tests' 243 findings.
- **CMake's own `-Werror=author -Werror=deprecated`**, which stay CI's and a strict local configure's.

## Self-review — risks and assumptions

- **The lint workflow's macOS leg is assumed to resolve the tool's own libc++ as the Mac does.** It installs the same
  Homebrew formula, but no run has shown it. If it does not, the control of Step 4 fails the leg by name and says
  which library the units met; the fix is then to name the library's directory to the tool, not to drop the control.
- **Homebrew moves `llvm` to a new major on its own schedule.** `clang_tidy_version` already fails the macOS lint leg
  on purpose when it does. A newer libc++ that keeps more headers apart then fails `clang_tidy_headers_alone` as
  well, which is the early warning this is for, and is fixed in the commit that moves the major.
- **`test_headers_alone` cannot see what its library hides.** H1 says so in the table. It is in the plan for the
  failure it does see, in every tree, with no tool.
- **`<typeinfo>` and `<ctime>` are shown by one tool, once.** No compile at hand fails without them, so nothing holds
  them after Step 4 until D8's issue is done.
- **The archive is a URL.** If the release asset moved, a cold configure would stop with a download error that names
  the URL, and a changed file would stop it at the hash. A consumer that provides `nlohmann_json::nlohmann_json`, or
  declares the name first, is not affected, as now.
- **A consumer that lets the engine fetch its dependencies sees Step 5**: a smaller fetch of the same files. The
  release notes say so.
- **`option()` caches.** A tree made before Step 6 keeps `OFF` without a word from CMake; the status line is what
  says so, and the README says how to clear it.
- **A strict default stops a build on a compiler newer than any measured here.** That is the issue's stated cost.
  `=OFF` is the way out and the README says so.
- **`check_clang_tidy_wiring.cmake` is 570 lines and holds the gate's test names exactly.** Step 4's change to it is
  the largest edit to an existing check in the plan.
- **A push cancels the `CI` run in flight**, and a macOS job can queue for minutes, so a mid-plan checkpoint may get
  no macOS result; the next commit's run covers it, and the Outcome will say which runs were read.
- **This repo is public.** The plan, the commits, the issue comments and the release notes name the engine only, and
  the leak audit runs over each before its push.

## Outcome

**Done 2026-11-12.** The five issues are closed, each with a closing comment, v1.3.1 is tagged on the release commit and
published as Latest, and the milestone is closed.

| Step | Status | Commit |
|---|---|---|
| 1 — The plan | Done | `2f84ba2`; each issue names the plan in a comment |
| 2 — flywheel-dag#73: the rows | Done | `bb6b1db` |
| 3 — flywheel-dag#32: the list, and its check | Done | `1584cf4` |
| 4 — flywheel-dag#66: the includes, and headers alone | Done | `04d1547` |
| 5 — flywheel-dag#69: what a cold configure fetches | Done | `b9635d5` |
| 6 — flywheel-dag#67: `-Werror` by default | Done | `34628d4` |
| 7 — Release v1.3.1 | Done | `12555db`, tagged `v1.3.1` |
| 8 — Close out | Done | the commit that adds this section |

What each step's "done when" found, on the files as committed:

- **Step 2.** `test_platform_name` reports that all 72 cases hold. M1 to M4 fail 9, 2, 1 and 1, and a control none.
- **Step 3.** The check went in first and failed naming `RollingSumNode` and no other of the 14 classes. With the row it
  holds, and `test_comment_refs` passes with `OPEN_ISSUES=31`. N1 to N4 are reported as stated; so is the `EWMANode` row
  made a comment, which shows that a commented-out row is not a row.
- **Step 4.** `clang_tidy_headers_alone` went in first and failed on the Mac for 14 of the 16 headers at `dag.inl:81`;
  with the includes it passes. Both tests pass on the Mac and in `ubuntu-dev`, and on both legs of CI's two workflows.
  On the lint workflow's macOS leg the control passed, so the units there met LLVM 23's own libc++, as on the Mac: the
  assumption of the Self-review holds. The first takes 6 s on the Mac, 8 in the container and about 20 on a runner; the
  second 9, 16 and about 35. `clang-include-cleaner`, run again on the committed headers, lists only its own two
  findings.

  | Change | `test_headers_alone` | `clang_tidy_headers_alone` on the Mac |
  |---|---|---|
  | H1. `<cmath>` removed from `dag.inl` | passes | 14 headers fail at `std::abs` |
  | `<typeinfo>` removed from `dag.inl`, or `<ctime>` from `dag_state_store.inl` | passes | passes |
  | H2. `"flywheel/dag.hpp"` removed from `dag_window_status.hpp` | names the header | names the header |
  | H3. a new header that names `std::string` and includes nothing | names it, 1 of 17 | names it, 1 of 17 |
  | H4. the must-not-compile control given `<string>` | "the control compiled" | "the control compiled" |
  | H5. the gate's caller told that the major is 22 | not run | "the units were not compiled against the tool's libc++" |

- **Step 5.** The check went in first and failed on five declarations. A fresh configure with the strict flags exits 0
  with no warning line and a `_deps` of 15 MB in 2 seconds, on the Mac and in `ubuntu-dev`; on the release commit CI's
  cold configure took 3.6 s on Ubuntu and 8.5 s on macOS. F1 to F5 are reported by name, and so are two more: an
  archive with no hash, and a root that declares nothing.
- **Step 6.** The test went in first and failed: all 42 units of the project lacked `-Werror`, the cache held `OFF`,
  and two workflows named the option. With the default it passes in 2 seconds. Each of the five kinds builds from its
  preset with no `-D` and no warning, every target and `bench_hot_path`, under Apple clang 21 and under GCC 13.3. W1 to
  W6 fail in the case stated, and so does a seventh, the script told that the default is `OFF`. `CLAUDE.md` is 6,492
  bytes. The dispatched `Sanitizers` run 43318553418 passed `asan` and `tsan` with its `-D` gone.
- **Step 7.** The gate, on the tree as committed, with `-Werror=author -Werror=deprecated` and the clang-tidy gate on:
  the Mac (Apple clang 21.0.0, CMake 4.4.5, LLVM 23.1.4) and `ubuntu-dev` (GCC 13.3.0, CMake 4.4.5, LLVM 23.1.6)
  configure with no warning line, build with no warning, pass the suite, 89 of 89, and the `clang_tidy_` tests, 45 of
  45, and `bench_hot_path --invariants` matches. `gh release list` shows v1.3.1 as Latest. On the release commit,
  `CI` run 43319730148 passed 89 of 89 on both legs (GCC 15.2.0 and Apple clang 21.0.0, CMake 4.4.5), with no
  `warning:` line in either log, and `clang-tidy` run 43319730242 passed on both.
- **30 hand-made changes** in all, each on a scratch copy beside a control. 28 fail the check written to hold them. Two
  fail nothing, the second row of the table above.

Where the work differs from the plan:

- **`<cmath>` and `<typeinfo>` are in `dag.inl`, not `dag.hpp`.** `dag.inl` is the file that says `std::abs`, `typeid`
  and `std::bad_cast`, `dag_state_store.inl` already names what it alone uses, and an editor reports an include that
  `dag.hpp` does not use itself. The header and its `.inl` name them either way, which is what D2 asked.
- **`flywheel_dag_clang_tidy_add_tests()` takes a fourth argument,** the include directories of the libraries that the
  headers include, which the headers-alone units need. `test_clang_tidy_wiring` holds it.
- **The lint workflow's header comment changed,** to say what the gate now runs. That is an edit to a workflow, so its
  one action was looked up again: `actions/checkout@v6` runs on `node24`.
- **A tree made earlier does fetch once.** Step 5 said that `ubuntu-dev`'s existing tree would not be fetched again,
  which had been measured for a clone that only gains `GIT_SHALLOW`. nlohmann/json changes from a clone to an archive,
  so an existing tree downloads the archive, 115 KB, and keeps its two clones at full depth.
- **W4 fails as a refused configure.** `-Werror` on `flywheel_dag`'s `INTERFACE` stops the parent project's configure,
  because `tests/consumer` itself refuses compile options on `flywheel::dag`; the option-off case reports it too, on 40
  of 42 units.
- **The cache's `CMAKE_CXX_FLAGS` is held to carry no warning flag,** not to be empty: an environment's `CXXFLAGS`
  would fail an emptiness check for no fault of the project's.
- **Every tree already held `ON`.** The five trees on each host had been configured with the option on, so clearing
  it changed none of them. The stale case showed itself anyway: a scratch tree made before Step 6 printed "warnings
  are not errors in this tree" on its next configure.
- **CI did not run every checkpoint on macOS.** A push cancels the `CI` run in flight. The runs on `2f84ba2` and
  `bb6b1db` were cancelled. The run on `1584cf4` ended `failure` with its Ubuntu leg green and a macOS job that has no
  steps: a runner that never came, cancelled by the next push. `04d1547`, `b9635d5`, `34628d4` and `12555db` each
  have a `CI` run that passed on both legs and a `clang-tidy` run that passed on both.

What this does not settle:

- **[flywheel-dag#77](https://github.com/tomlin256/flywheel-dag/issues/77)**, opened for D8 with no milestone:
  include-what-you-use on the engine's headers as a gate. `<typeinfo>` and `<ctime>` are held by no test until then.
- **The shallow-clone measurement** is also a comment on tomlin256/cryptocogs#54, whose table it corrects for
  nlohmann/json.
