# Run clang-tidy in CI

**Status: Proposed (2026-11-03).**

Closes [flywheel-dag#60](https://github.com/tomlin256/flywheel-dag/issues/60), and
[flywheel-dag#40](https://github.com/tomlin256/flywheel-dag/issues/40), which is one of its findings.

## Problem

The issue is its title: *"Make CI run clang tidy"*. Today clang-tidy runs in the editor only. `.clangd` enables
`bugprone-*`, `performance-*` and `cppcoreguidelines-pro-type-member-init` (less two checks), so a finding is a hint on
one machine and CI never sees it. flywheel-dag#40 is such a hint, filed with the words *"CI does not run clang-tidy"*.

The change is a CI job that holds the project's own sources to those checks and fails on any finding, on code that is
already clean under them. The engine is header-only, so its headers are checked through the translation units that
include them: 35 of them (30 under `tests/`, 2 examples, 3 under `benchmarks/`), and all 25 of the engine's headers are
reached by at least one (measured with `-MM` on the Mac's compile database).

### What I measured

The `.clangd` checks over the 35 units, reporting the project's headers and sources only:

| Toolchain | Diagnostics | Sites (file, line, check) | Time |
|---|---|---|---|
| This Mac: VS Code's clang-tidy 23.1, Apple SDK | 83 | 77 | 10 s, 8 jobs |
| `ubuntu:26.04` container: clang-tidy 21.1.8, GCC 15.2, libstdc++, CMake 4.2.3 | 62 | 59 | 55 s, 4 jobs |

The container is the Ubuntu release `ubuntu-latest` runs today (run 41705700562: `ubuntu-26.04`, GCC 15.2). The runner
image preinstalls more, so the job's own version line is the authority. Linux's 59 sites are a subset of the Mac's 77.
The other 18 come from four checks that clang-tidy 21 does not have (`bugprone-signed-bitwise`,
`bugprone-throwing-static-initialization`, `bugprone-derived-method-shadowing-base-method`,
`bugprone-command-processor`: confirmed with `--list-checks`) and one `main` that 21 does not flag.

Without a header filter, 23.1 also reports about 300 findings in spdlog's and nlohmann/json's headers, which this
project includes with `-I`, not `-isystem`. They are not ours, so the configuration has to filter them out.

### The findings, and the fix for each

| Check | Diagnostics | Where | Fix |
|---|---|---|---|
| `performance-unnecessary-value-param` | 42 | `dag_timeseries.inl` 29, `dag.inl` 6, `dag_aad.inl` 1, `dag_engine.inl` 1; tests 5 | A `NodePtr` taken by value and copied once is moved into place. One taken by value and only read through is a `const NodePtr&`. The message at each site says which |
| `bugprone-signed-bitwise` | 13 | `splitmix64` in the benchmark and the example, `dag_memoize.hpp:26`, `test_timeseries.cpp:550` | All 13 are a positive literal shift count (`x >> 30`) on an unsigned value. Not a bug: set the check's `IgnorePositiveIntegerLiterals`. A signed *variable* operand is still reported |
| `bugprone-unchecked-optional-access` | 7 | `test_timeseries.cpp` 6, `test_dag.cpp` 1 | `*opt` becomes `opt.value()`. The analysis cannot see through gtest's `ASSERT_TRUE(opt.has_value())`, and two of the six are `*evalDelay(...)` with no check at all |
| `bugprone-throwing-static-initialization` | 5 | `const double kPi = std::acos(-1.0)` in three tests, `e` and `e2` in a fourth | `constexpr`, from `std::numbers::pi` and `std::numbers::e` (`e2` as a literal) |
| `bugprone-exception-escape` | 4 | `main` in `aad.cpp`, `quickstart.cpp` and `bench_hot_path.cpp`; `~TimeOnExit` in `dag_engine.inl` | The three `main`s catch `std::exception`, print it and return `EXIT_FAILURE`. `~TimeOnExit`: D4 |
| `bugprone-crtp-constructor-accessibility` | 3 | `OpNodeImpl`, `NodeImpl`, `StatefulNodeBase` | Constructor private, `friend Derived` (and `NodeImpl` also befriends `StatefulNodeBase`, its only subclass). A wrong CRTP argument stops compiling |
| `performance-enum-size` | 2 | `NodeKind`, `ThresholdNode::Direction` | `: std::uint8_t` |
| `bugprone-branch-clone` | 2 | `GraphExporter::shapeFor` | Merge the case labels that return the same shape |
| `performance-inefficient-vector-operation` | 2 | `test_dag.cpp:1031`, `test_ring_buffer.cpp:45` | `reserve()` before the loop |
| `bugprone-incorrect-roundings` | 1 | `test_dag_async.cpp:1660` | `std::llround`. This is flywheel-dag#40 |
| `bugprone-derived-method-shadowing-base-method` | 1 | `ProbeNode::state` in `test_lazy_invalidation.cpp` | `using NodeBase::state;` in place of a forwarding method |
| `bugprone-command-processor` | 1 | `std::system` in `GraphExporter::toSvg` | D4 |

`toSvg` has no caller and no test in the repo.

## The change

1. **`.clang-tidy`** (new) holds the checks: today's `.clangd` list, a `HeaderFilterRegex` that admits
   `include/flywheel`, `tests`, `examples` and `benchmarks` paths and nothing under `_deps`, and the one check option
   (D3). `.clangd` drops its `Diagnostics` block and keeps the compilation database and the inlay hints. There is then
   one list, which clangd reads too, and nothing for the two to drift on. `WarningsAsErrors` is not in the file:
   clangd would, I believe, report every hint as an error. The gate passes `--warnings-as-errors=*`.
2. **Two cache variables** in the root `CMakeLists.txt`, in the same place as the other options:
   `FLYWHEEL_DAG_CLANG_TIDY` (the command; empty, the default, leaves the gate out) and
   `FLYWHEEL_DAG_CLANG_TIDY_ARGS` (a list of extra arguments, for a tool that cannot find its own headers, which is
   VS Code's on this Mac). Set and not found, or with a generator that writes no `compile_commands.json`, the
   configure fails with a message: a gate that was asked for and silently absent is the failure to avoid.
3. **The tests** (`tests/CMakeLists.txt`, top level only, when the variable is set):

   | Test | Runs | Passes when |
   |---|---|---|
   | `clang_tidy_<stem>` × 35 | `clang-tidy -p <build> --warnings-as-errors=* <file>`, one per `.cpp` under `tests/`, `examples/`, `benchmarks/` (a glob with `CONFIGURE_DEPENDS`) | exit 0. A file the build does not compile has no compile command and fails, so a new file is never skipped |
   | `clang_tidy_checks` | `clang-tidy --list-checks` with the real `.clang-tidy` | lists `cppcoreguidelines-pro-type-member-init`; does not list `bugprone-easily-swappable-parameters`, `bugprone-narrowing-conversions`, `readability-` or `modernize-` |
   | `clang_tidy_canary_source` | the same command on a fixture unit that moves a string and then reads it | `canary.cpp:L:C: error: … [bugprone-use-after-move]`: `.clang-tidy` is found, `bugprone` is on, and `--warnings-as-errors` bites |
   | `clang_tidy_canary_engine_header` | the same unit; it includes a fixture header at `…/include/flywheel/canary_engine.hpp` that takes a `std::string` by value | `canary_engine.hpp:L:C: error: … [performance-unnecessary-value-param]`: the filter admits the engine's path, and `performance` is on |
   | `clang_tidy_canary_dependency_header` | the same unit; it includes a fixture header at `…/_deps/canary-src/include/canary_dep.hpp` with the same defect, by `-I` as spdlog is | the source error is reported and `canary_dep.hpp` appears nowhere |

   The fixtures live in `tests/clang_tidy_canary/` (each with the project banner), are copied under the build tree at
   configure time (so their paths are the ones above, not `tests/…`), and are in no compile database. The ctest properties
   `PASS_REGULAR_EXPRESSION` and `FAIL_REGULAR_EXPRESSION` carry the expectations; no script is needed.
   *A lint gate that silently checks nothing passes CI, which is why the gate itself is tested.*
4. **The CI job** `clang-tidy` (`ubuntu-latest`, parallel to the two build legs): install `clang-tidy`, print its
   version beside CMake's, `cmake --preset relwithdebinfo-linux-x86_64 -Werror=author -Werror=deprecated
   -DFLYWHEEL_DAG_CLANG_TIDY=clang-tidy` (configure only, nothing is built), then
   `ctest --preset relwithdebinfo-linux-x86_64 -R '^clang_tidy_' -j4`. The test preset's `noTestsAction: error` fails
   a run that selects none.
5. **The code fixes** in the table, in four steps.
6. **`CLAUDE.md`** gets a bullet (the gate, the one list, and that a finding is fixed, with `NOLINT(<check>)` only for a
   tool that is wrong or cannot see an invariant, and a comment saying which); **`README.md`** gets the two commands.

## Decisions to confirm

Each is a place to edit before approving.

- **D1. The gate runs on Linux only**, as a job of its own. Apple ships no clang-tidy, and the macOS runner's Homebrew
  `llvm@20` is older than Linux's 21 and needs the SDK flags. Every check in the set is AST-based, so the library
  does not change the verdict. The Mac keeps the editor, and can run the same tests by hand (D2).
- **D2. The tool floats with the image.** `apt-get install clang-tidy` gives 21.1.8 on `ubuntu-26.04`, and the job
  prints the version. A new image with a newer LLVM may add checks and turn CI red with no code change; that is how a
  newer compiler's new warning is already treated here (flywheel-dag#44), and the fix is in the code. Pinning
  `clang-tidy-21` would hide the drift instead. By hand on the Mac, with VS Code's binary:
  `-DFLYWHEEL_DAG_CLANG_TIDY=<path> -DFLYWHEEL_DAG_CLANG_TIDY_ARGS="--extra-arg=-resource-dir=…;--extra-arg=-isysroot;--extra-arg=<sdk>"`.
  The 17 sites that only 23.1 reports (the four checks above) are cleaned too, since `ubuntu-latest` will reach that
  LLVM; but until it does, CI cannot gate them.
- **D3. The check set does not change.** `.clangd`'s list moves into `.clang-tidy` as it is. Adding
  `clang-analyzer-*`, `readability-*` or `misc-include-cleaner` is a separate decision, because each brings its own
  findings. The one option is `bugprone-signed-bitwise.IgnorePositiveIntegerLiterals: true`. Older tools ignore the
  key, and 21 has no such check.
- **D4. Two findings stay as `NOLINT`, each with a comment that says why.** This is the one place the plan does not
  fix a finding in the code, because both fixes are worse than the comment:
  - `~TimeOnExit` (`dag_engine.inl:419`), reported through `RollingCycleWindow::record()` and `RingBuffer::front()`.
    `front()` throws on an empty buffer and `push_back()` on a full one, and `record()` reaches neither: it evicts
    only when the window is full (so the buffer is not empty) and the buffer holds `window + 1`. The tool cannot see
    that. The alternatives are an unchecked accessor on the public `RingBuffer`, or a `catch (...)` in the engine's
    cycle path, each of which would have to be proved against `bench_hot_path` and a disassembly diff, in a path whose
    layout noise is already measured (flywheel-dag#34). A new test pins the invariant the comment claims.
  - `std::system` in `toSvg` has a `NOLINT(cert-env33-c)` today, which is the same check under its CERT name and does
    not silence the bugprone name that 23.1 reports. The call is also a real defect: it builds a shell command from
    two unquoted paths. A fix that avoids the shell needs `posix_spawn`, which a consumer on Windows could not
    include, and a test with a stand-in `dot`; it is its own change. Step 6 opens an issue for it and the `NOLINT`
    names that issue.
- **D5. The gate is ctest tests behind an option**, the way the repo holds its other gates. The alternative, a YAML
  step that runs `run-clang-tidy` and nothing else, is shorter and cannot notice a configuration that checks nothing.
- **D6. No release.** Header changes a consumer can see: two enums narrower, seven parameters of five functions
  (`ConditionNode::make`, `ZScoreNode::make`, `OutlierGateNode::make`, the `Tape` constructor, `Engine::install`)
  now `const` references, both source compatible; and the three CRTP bases' constructors private. Only the last can
  break a consumer, one that derives a base with an argument other than itself. Whether that is a v1.0.1 is yours.

## Steps

Every commit subject is scoped to the issue in the repo's `type(#N): …` form; the subjects below omit the scope. A
checkpoint ends with a full build and a full ctest run on the Mac (CMake 4.4.5, CI's flags), and nothing is pushed
until the last is green. "Remaining" counts are diagnostics from the tool, de-duplicated across units, on the Mac
(23.1) and in the `ubuntu:26.04` container (21.1.8). The headers' findings repeat in every unit that includes them, so
the per-file tests go green only when Step 6 is done.

### Step 0 — Approval

The plan is committed as Proposed and linked from flywheel-dag#60. **Done when:** `git status` is clean on that commit
and the issue carries the path. Commit: `docs: add plan for running clang-tidy in CI`.

### Step 1 — Start from green

No commit. Mac: configure with CI's flags, full build, `ctest`. **Done when:** no warning and 45 of 45 pass. Anything
else: stop and report.

### Step 2 — The configuration and the gate, red (D1, D3, D5)

`.clang-tidy`, `.clangd`, the two variables, the registration, the fixtures. The variable is off by default, so
`ctest` is unchanged.

| Test | Asserts |
|---|---|
| `clang_tidy_<stem>` × 35, `clang_tidy_checks`, 3 canaries (new) | as in the table above |
| the other 45 (existing) | unchanged, and pass |

**Done when:**

- with the variable unset: configure and `ctest` are as in Step 1, 45 of 45, and `ctest -N` lists no `clang_tidy_`;
- with it set, on the Mac: `clang_tidy_checks` and the 3 canaries pass, and the per-file tests are red on the
  unchanged code, their output naming the sites above. Remaining: Mac 70 (the 13 signed-bitwise are gone), Linux 62;
- set and not found: the configure fails with the message; set with a generator that writes no
  `compile_commands.json` (Xcode): the same;
- each hand-made change fails exactly the case stated. Each is made on a scratch copy of the checkout and never
  restored:

  | Change | Fails |
  |---|---|
  | M1 `.clang-tidy` renamed | `clang_tidy_checks` and the source canary |
  | M2 `HeaderFilterRegex` without `include/flywheel` | the engine-header canary |
  | M3 `HeaderFilterRegex: '.*'` | the dependency-header canary |
  | M4 `performance-*` left out of `Checks` | `clang_tidy_checks` and the engine-header canary |
  | M5 `--warnings-as-errors=*` left off the canary command | all three canaries: no `error:` |
  | M6 a by-value `std::string` parameter seeded in `dag.hpp` | the per-file tests of the units that include it, and no others |
  | M7 a new `tests/test_new.cpp` that no target compiles | `clang_tidy_test_new`, for want of a compile command |

Commit: `build: add .clang-tidy and the clang_tidy_ tests, off by default`.

### Step 3 — Tests, examples and the benchmark (flywheel-dag#40)

No engine header is touched: the 24 diagnostics of those 14 files in the table. **Done when:** remaining Mac 46,
Linux 45; `ctest` 45 of 45; both examples and `bench_hot_path --invariants` run, and the invariants match
`benchmarks/expected_invariants.txt`.

| Test | Asserts |
|---|---|
| the 29 suites touched (existing) | pass: `opt.value()` and `constexpr` constants change no assertion; `.value()` on an empty optional now throws and fails the test instead of reading it |
| `test_dag_async` (flywheel-dag#40, existing) | passes with `std::llround`; no behaviour change, as the issue says |

Commits: `test: round the nanosecond conversion with std::llround` (scoped to flywheel-dag#40), then `fix: clear
clang-tidy's findings in the tests, the examples and the benchmark`.

### Step 4 — The engine's headers, mechanically

The value parameters (37), the two switches, the two enums. **Done when:** remaining Mac 5, Linux 4; `ctest` 45 of 45
on the Mac, and a full build and `ctest` in the `ubuntu:26.04` container (GCC 15.2, CI's compiler); the disassembly of
`Engine::cycle()` before and after is identical with addresses stripped (`xcrun llvm-objdump -d --no-show-raw-insn
--disassemble-symbols=__ZN3dag5async6Engine5cycleEv` on `bench_hot_path`, then drop every run of six or more hex
digits), and `bench_hot_path --invariants` matches the committed file.

| Test | Asserts |
|---|---|
| the 45 existing | pass. Every call site that passes a `std::shared_ptr<Derived>` to a `const NodePtr&` converts, as the suites already do |

Commit: `refactor: move or borrow the NodePtr parameters, narrow two enums, merge two cases`.

### Step 5 — The CRTP constructors

New `tests/check_crtp_access.cmake`, registered as `test_crtp_access` for GCC and Clang at the top level (like
`test_standard_guard`): it compiles snippets with `-fsyntax-only`.

| Test | Asserts |
|---|---|
| `test_crtp_access` (new), control | a class deriving `OpNodeImpl<Self>` and a class deriving `StatefulNodeBase<Self, …>` (which derives `NodeImpl<Self>`) compile |
| `test_crtp_access`, misuse | `Bad : OpNodeImpl<Other>`, `Bad : NodeImpl<Other>` and `Bad : StatefulNodeBase<Other, …>`, each with a constructor of its own, are refused with an access error |
| the 45 existing | pass |

**Done when:** the test is red before the edit (misuse compiles) and green after; remaining Mac 2, Linux 1; M8 (a
constructor back to `protected`) fails its misuse case; the Step 4 container build and disassembly diff again.
Commit: `fix: make the CRTP bases' constructors private to their derived class`.

### Step 6 — The two suppressions (D4)

`NOLINT` on `~TimeOnExit` and on the `std::system` call, each with a comment. A new issue for `toSvg`'s unquoted
paths (label `bug`), named in the comment and reported to you.

| Test | Asserts |
|---|---|
| `RollingCycleWindow.RecordNeverThrows…` (new, `test_dag_async`) | `record()` called 3 × window times does not throw, for windows 1, 2 and 5 |
| the 45 existing + the new one | pass |

**Done when:** remaining 0 on both toolchains; all 35 per-file tests, `clang_tidy_checks` and the canaries green on
the Mac and in the container; M9 (`buf_(window)` in place of `window + 1`) fails the new test; the disassembly diff is
empty. Commit: `fix: name the two findings clang-tidy cannot be satisfied on, and say why`.

### Step 7 — The CI job

`.github/workflows/ci.yml`. **Done when:** it parses as YAML (`ruby -ryaml`); the same four commands, run in a fresh
`ubuntu:26.04` container with `apt-get install clang-tidy`, pass (35 + 4 tests); and the two existing jobs are
untouched. The workflow is read by GitHub, so a mistake shows as a job that does not start or fails; Step 8 is the
test. Commit: `ci: run clang-tidy`.

### Step 8 — Push, and read CI

The dry run of the pre-push leak audit first (no bare `#N` in a tracked file). One push. **Done when:** the
`clang-tidy` job is green and prints a version, ran 39 tests (35 + 4), and took under five minutes; the two build
legs are green and named as before. Then flywheel-dag#40 and flywheel-dag#60 are closed with a comment each. CI is not
shown a seeded defect: that is M6, on a scratch copy.

### Step 9 — Docs, and the plan done

`CLAUDE.md`, `README.md`, the plan's status and an Outcome section. Commit: `docs: mark the plan done`. The memory
note on running clang-tidy by hand is updated outside the repo.

## Designs I rejected

- **`CMAKE_CXX_CLANG_TIDY`**, tidy during the compile. Every leg compiles anyway, but the ubuntu leg already takes
  nine minutes, an incremental build runs it only on what changed, and it gets GCC's flags. The compile database is
  what clangd uses.
- **A baseline file of known findings.** It parks 83 closable defects.
- **`WarningsAsErrors: '*'` in `.clang-tidy`.** I believe clangd then reports every hint as an error.
- **Disabling `bugprone-signed-bitwise`.** It still catches a signed variable operand.
- **A macOS leg.** D1.

## Not in this plan

- **Widening the checks** (D3), and **Include Cleaner**. The `unused-includes` hints (flywheel-dag#51, flywheel-dag#53,
  flywheel-dag#54) are clangd's, not a tidy check; `misc-include-cleaner` is its twin, and flywheel-dag#61 records a false positive.
- **`tests/consumer/`**, which is a separate project built by a test and in no compile database.
- **`toSvg`'s unquoted paths**, tracked by the issue Step 6 opens.
- **A release** (D6).

## Self-review — risks and assumptions

- **clangd reads `.clang-tidy`.** It documents that it does, and merges `.clangd`'s `Diagnostics.ClangTidy` on top.
  I cannot show it from here: `clangd --check` prints no tidy finding even with `-log=verbose` (checked on
  `test_ring_buffer.cpp`, which has one). The check is yours: after Step 2, a hint you see today (for example
  `test_ring_buffer.cpp:45`) must still appear, and after Step 3 it must go.
- **Three toolchains disagree.** Apple's clangd (21.0.0), the container's clang-tidy (21.1.8) and VS Code's (23.1) see
  different sets. CI gates 21.1.8. The plan cleans the union, but a 23-only check can regress with no signal until
  the image reaches it, or you run the gate by hand.
- **The image can move under the job** (D2). If it ships a clang-tidy that `apt-get install clang-tidy` does not
  replace, the version line in the log is what shows it.
- **The header filter is a path match** (`/include/flywheel/`, `/tests/`, …). A checkout inside a directory with one
  of those names would admit more than it should. The dependency canary fails if a real `_deps` path were admitted.
- **Only instantiated templates are analysed.** All 25 headers are reached, but a template no unit instantiates is
  not checked. The suites instantiate what they test.
- **`.value()` for `*opt` in tests** turns a read of an empty optional into an exception that gtest reports as a
  failure: stricter, never looser.
- **The canaries read clang-tidy's wording** (`error:`, `[check-name]`). A rewording turns them red, which is the
  right failure: the gate's output changed.
- **Time:** about a minute of tidy on four cores plus the install and configure, in parallel with the build legs.
- **Assumptions:** the suite stays under `ctest` with no sleeps; the tidy tests need no network beyond the configure's
  `FetchContent`; the runners keep CMake 4.4 or later.

## Progress

| Step | Status | Notes |
|---|---|---|
| 0 Approval | Proposed | |
| 1 Start from green | | |
| 2 The configuration and the gate, red | | |
| 3 Tests, examples and the benchmark | | |
| 4 The engine's headers, mechanically | | |
| 5 The CRTP constructors | | |
| 6 The two suppressions | | |
| 7 The CI job | | |
| 8 Push, and read CI | | |
| 9 Docs, and the plan done | | |
