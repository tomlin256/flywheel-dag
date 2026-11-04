# Run clang-tidy in CI, at LLVM 23 on the Mac, in Ubuntu and in CI

**Status: Approved (2026-11-03), in progress.** Expanded the same day, from a Linux-only gate on the distro's tool to
one LLVM major, 23, in all three places (D1, D2 and D7 to D9). Approved as written: D1 to D9 stood, with leave to
install LLVM on the Mac and link it (Step 2).

Closes [flywheel-dag#60](https://github.com/tomlin256/flywheel-dag/issues/60), and
[flywheel-dag#40](https://github.com/tomlin256/flywheel-dag/issues/40), which is one of its findings.

## Problem

The issue is its title: *"Make CI run clang tidy"*. Today clang-tidy runs in the editor only. `.clangd` enables
`bugprone-*`, `performance-*` and `cppcoreguidelines-pro-type-member-init` (less two checks), so a finding is a hint on
one machine and CI never sees it. flywheel-dag#40 is such a hint, filed with the words *"CI does not run clang-tidy"*.
And the three places do not agree on a tool: the editor's clangd is Apple's 21.0.0, the clang-tidy you reach by hand is
VS Code's 23.1.0, the `ubuntu-dev` container has 18, and what the two runners give by default is 21 and 20. You asked
for the same LLVM major, 23, on the Mac, in Ubuntu and in CI.

The change is a CI job that holds the project's own sources to those checks and fails on any finding, on code that is
already clean under them, with one LLVM major everywhere and a test that keeps it so. The engine is header-only, so its
headers are checked through the translation units that include them: 35 of them (30 under `tests/`, 2 examples, 3 under
`benchmarks/`), and all 25 of the engine's headers are reached by at least one (measured with `-MM` on the Mac's
compile database).

### What I measured

**The tools today:**

| Where | Tool | LLVM |
|---|---|---|
| Mac, editor (the Claude Code `clangd-lsp` plugin runs `clangd` from `PATH`: `/usr/bin/clangd`) | Apple clangd, clang-tidy built in | 21.0.0 |
| Mac, by hand | clang-tidy bundled in VS Code's cpptools 1.34.4 (cpptools 1.35.6 has 23.1.1) | 23.1.0 |
| `ubuntu-dev` container (Ubuntu 24.04, GCC 13.3) | apt `clang-tidy` | 18.1.3 |
| CI Ubuntu (`ubuntu-latest` is `ubuntu-26.04`, GCC 15.2) | what `apt-get install clang-tidy` gives | 21.1.8 |
| CI macOS (`macos-latest` is `macos-26`, Apple clang 21) | Homebrew's `llvm@20` is on the image | 20.1.8 |

**What 20 to 23 find.** The `.clangd` checks over the 35 units, reporting the project's headers and sources only, on
Ubuntu 26.04 (GCC 15.2, libstdc++):

| clang-tidy | Diagnostics | Sites (file, line, check) | Against the one before |
|---|---|---|---|
| 20.1.8 and 21.1.8 (Ubuntu archive) | 62 | 59 | the same sites |
| 22.1.2 (Ubuntu archive) | 65 | 62 | `main` in `bench_hot_path.cpp`, `std::system` in `toSvg`, `ProbeNode::state`: a changed check and two new ones |
| 23.1.5 (apt.llvm.org) | 78 | 72 | 13 `bugprone-signed-bitwise`, a check new in 23 |
| 23.1.5 with that check's `IgnorePositiveIntegerLiterals` | 65 | 62 | the same as 22 |

**The three places, at 23**, with that option:

| Where | Sites |
|---|---|
| Ubuntu 24.04 as in the `ubuntu-dev` image (GCC 13.3, libstdc++ 13), clang-tidy 23.1.5 | 62 |
| Ubuntu 26.04 (GCC 15.2), clang-tidy 23.1.5 | the same 62 |
| This Mac (Apple SDK), VS Code's 23.1.0 | 67: the same 62 and 5 more |

The 5 are `bugprone-throwing-static-initialization` on `std::acos` and `std::exp` in `const double` initialisers: the
macOS SDK declares them without `noexcept`, glibc with it. Only a macOS run reports them, and no site appears on Linux
and not on the Mac. Without the option the Mac reports 83 diagnostics at 77 sites.

Without a header filter, 23.1 also reports about 300 findings in spdlog's and nlohmann/json's headers, which this
project includes with `-I`, not `-isystem`. They are not ours, so the configuration has to filter them out. Time: 10 s
on the Mac with 8 jobs, 55 s on Ubuntu with 4 jobs (21.1.8; 12 s with 8 jobs at 23.1.5).

### The findings, and the fix for each

| Check | Diagnostics | Where | Fix |
|---|---|---|---|
| `performance-unnecessary-value-param` | 42 | `dag_timeseries.inl` 29, `dag.inl` 6, `dag_aad.inl` 1, `dag_engine.inl` 1; tests 5 | A `NodePtr` taken by value and copied once is moved into place. One taken by value and only read through is a `const NodePtr&`. The message at each site says which |
| `bugprone-signed-bitwise` | 13 | `splitmix64` in the benchmark and the example, `dag_memoize.hpp:26`, `test_timeseries.cpp:550` | All 13 are a positive literal shift count (`x >> 30`) on an unsigned value. Not a bug: set the check's `IgnorePositiveIntegerLiterals`. A signed *variable* operand is still reported |
| `bugprone-unchecked-optional-access` | 7 | `test_timeseries.cpp` 6, `test_dag.cpp` 1 | A check the analysis can see, ahead of the access: `if (!opt.has_value()) FAIL()` in `test_dag.cpp`, and in `test_timeseries.cpp` a helper, `valueOrFail`, that reports an absent value and returns NaN. It cannot see through gtest's `ASSERT_TRUE(opt.has_value())`, it reports `opt.value()` as well as `*opt` (measured on a probe file), and five of the six are `*evalDelay(...)` with no check at all |
| `bugprone-throwing-static-initialization` | 5 (macOS only) | `const double kPi = std::acos(-1.0)` in three tests, `e` and `e2` in a fourth | `constexpr`, from `std::numbers::pi` and `std::numbers::e` (`e2` as a literal) |
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
   `FLYWHEEL_DAG_CLANG_TIDY_ARGS` (a list of extra arguments). Set and not found, or with a generator that writes no
   `compile_commands.json`, the configure fails with a message: a gate that was asked for and silently absent is the
   failure to avoid. On Apple the registration also passes `-isysroot` with the SDK that `xcrun --show-sdk-path`
   names, which is how `/usr/bin/c++` finds it (the Mac's cache has `CMAKE_OSX_SYSROOT` empty, so the compile commands
   carry none). The major is one constant beside the registration: 23.
3. **The tests** (`tests/CMakeLists.txt`, top level only, when the variable is set):

   | Test | Runs | Passes when |
   |---|---|---|
   | `clang_tidy_<stem>` × 35 | `clang-tidy -p <build> --warnings-as-errors=* <file>`, one per `.cpp` under `tests/`, `examples/`, `benchmarks/` (a glob with `CONFIGURE_DEPENDS`) | exit 0. A file no target compiles is analysed with the flags clang-tidy infers from its neighbours in the compile database, so a new file is never skipped |
   | `clang_tidy_version` | `clang-tidy --version` | the output has `LLVM version 23.`: the Mac, the container and CI cannot drift apart unseen (D2) |
   | `clang_tidy_checks` | `clang-tidy --list-checks` with the real `.clang-tidy` | lists a check of each family (`bugprone-use-after-move`, `cppcoreguidelines-pro-type-member-init`, `performance-unnecessary-value-param`); does not list `bugprone-easily-swappable-parameters`, `bugprone-narrowing-conversions`, `readability-` or `modernize-` |
   | `clang_tidy_canary_source` | the same command on a fixture unit that moves a string and then reads it | `canary.cpp:L:C: error: … [bugprone-use-after-move]`: `.clang-tidy` is found, `bugprone` is on, and `--warnings-as-errors` bites |
   | `clang_tidy_canary_engine_header` | the same unit; it includes a fixture header at `…/include/flywheel/canary_engine.hpp` that takes a `std::string` by value | `canary_engine.hpp:L:C: error: … [performance-unnecessary-value-param]`: the filter admits the engine's path, and `performance` is on |
   | `clang_tidy_canary_dependency_header` | the same unit; it includes a fixture header at `…/_deps/canary-src/include/canary_dep.hpp` with the same defect, by `-I` as spdlog is | the source error is reported and `canary_dep.hpp` appears nowhere |

   The fixtures live in `tests/clang_tidy_canary/` (each with the project banner), are copied under the build tree at
   configure time (so their paths are the ones above, not `tests/…`), and are in no compile database. The ctest
   properties `PASS_REGULAR_EXPRESSION` and `FAIL_REGULAR_EXPRESSION` carry the expectations; no script is needed.
   *A lint gate that silently checks nothing passes CI, which is why the gate itself is tested.*

   Every run also passes `--extra-arg=-Wno-error=deprecated-declarations`. The compile commands carry the project's
   `-Werror=deprecated-declarations`, and with it a clang-tidy newer than the SDK's libc++ stops 21 units with an error
   in the SDK's headers, from nlohmann/json 3.12.0's CBOR declarations, which name `std::char_traits<unsigned char>`,
   a specialization libc++ deprecates. Found in Step 4, on Homebrew's 23.1.4 and VS Code's 23.1.0 alike; the real
   build, Apple clang 21 against the same headers, is silent, and Linux has no such error. As a warning the
   deprecation is still an error where it is in this project's own files (`--warnings-as-errors`), and the header
   filter drops it in the SDK's and the dependencies'.
4. **LLVM 23 in each place:**

   | Where | Source | Command |
   |---|---|---|
   | Mac | `brew install llvm` (23.1.4, keg-only, a 457 MB bottle); `clangd` and `clang-tidy` linked into `/opt/homebrew/bin` (D7) | `clang-tidy` |
   | `ubuntu-dev` | the image's repository (D8): apt.llvm.org's `llvm-toolchain-noble-23`, `clang-tidy-23` 23.1.5 (27 packages, 169 MB) | `clang-tidy` and `clang-tidy-23` |
   | CI Ubuntu | apt.llvm.org's `llvm-toolchain-resolute-23`, key fingerprint checked, `apt-get install clang-tidy-23` (D9) | `clang-tidy-23` |
   | CI macOS | `brew install llvm` | `$(brew --prefix llvm)/bin/clang-tidy` |

5. **The CI job** `clang-tidy`, a matrix of two legs (`ubuntu-latest` with `relwithdebinfo-linux-x86_64`,
   `macos-latest` with `relwithdebinfo-macos-arm64`), parallel to the build job and named apart from it. Each leg:
   install the tool as in the table, print its version beside CMake's, `cmake --preset <preset> -Werror=author
   -Werror=deprecated -DFLYWHEEL_DAG_CLANG_TIDY=<tool>` (configure only, nothing is built), then
   `ctest --preset <preset> -R '^clang_tidy_' -j4`. The test preset's `noTestsAction: error` fails a run that selects
   none.
6. **The code fixes** in the table, in four steps.
7. **`CLAUDE.md`** gets a bullet: the gate, the one list, the pin and the one commit that moves it, and that a finding
   is fixed, with `NOLINT(<check>)` only for a tool that is wrong or cannot see an invariant, and a comment saying
   which. **`README.md`** gets the two commands and where to get the tool.

## Decisions to confirm

Each is a place to edit before approving. D1 and D2 changed from the first draft; D7 to D9 are new.

- **D1. The gate runs on Ubuntu and on macOS**, two legs of one job. You asked for parity, and the Mac reports 5 sites
  no Linux run does. The cost is the macOS leg: Homebrew's `llvm` is a 457 MB bottle (measured with a HEAD request;
  the install time is not measured), and the leg waits for a macOS runner, which this repo's CI has been seen to queue
  for eight minutes. Ubuntu alone would be cheaper and would leave those 5 to the Mac.
- **D2. The major is pinned at 23 in every place, and a test holds it.** `clang_tidy_version` passes only if the tool
  says `LLVM version 23.`. A tool at another major (Homebrew's `llvm` becoming 24, an image that moved, a stale
  `PATH`) fails it on the Mac, in the container and in CI alike. Moving to 24 is then one commit: the constant, the
  workflow, the image, and whatever 24 reports. Patch levels are not pinned and will differ: Homebrew ships the tagged
  23.1.4, apt.llvm.org builds the 23.x release branch (23.1.5 today).
- **D3. The check set does not change.** `.clangd`'s list moves into `.clang-tidy` as it is. Adding
  `clang-analyzer-*`, `readability-*` or `misc-include-cleaner` is a separate decision, because each brings its own
  findings. The one option is `bugprone-signed-bitwise.IgnorePositiveIntegerLiterals: true`.
- **D4. Two findings stay as `NOLINT`, each with a comment that says why.** This is the one place the plan does not
  fix a finding in the code, because both fixes are worse than the comment:
  - `~TimeOnExit` (`dag_engine.inl:419`), reported through `RollingCycleWindow::record()` and `RingBuffer::front()`.
    `front()` throws on an empty buffer and `push_back()` on a full one, and `record()` reaches neither: it evicts
    only when the window is full (so the buffer is not empty) and the buffer holds `window + 1`. The tool cannot see
    that. The alternatives are an unchecked accessor on the public `RingBuffer`, or a `catch (...)` in the engine's
    cycle path, each of which would have to be proved against `bench_hot_path` and a disassembly diff, in a path whose
    layout noise is already measured (flywheel-dag#34). A new test pins the invariant the comment claims.
  - `std::system` in `toSvg` has a `NOLINT(cert-env33-c)` today, which is the same check under its CERT name and does
    not silence the `bugprone-command-processor` that 22 and later report. The call is also a real defect: it builds
    a shell command from two unquoted paths. A fix that avoids the shell needs `posix_spawn`, which a consumer on
    Windows could not include, and a test with a stand-in `dot`; it is its own change. Step 8 opens an issue for it
    and the `NOLINT` names that issue.
- **D5. The gate is ctest tests behind an option**, the way the repo holds its other gates. The alternative, a YAML
  step that runs `run-clang-tidy` and nothing else, is shorter and cannot notice a configuration that checks nothing.
- **D6. No release.** Header changes a consumer can see: two enums narrower, seven parameters of five functions
  (`ConditionNode::make`, `ZScoreNode::make`, `OutlierGateNode::make`, the `Tape` constructor, `Engine::install`)
  now `const` references, both source compatible; and the three CRTP bases' constructors private. Only the last can
  break a consumer, one that derives a base with an argument other than itself. Whether that is a v1.0.1 is yours.
- **D7. The Mac: Homebrew's `llvm`, and two links.** `brew install llvm` is keg-only, so nothing lands on `PATH`. Then
  `ln -s "$(brew --prefix llvm)/bin/clangd" /opt/homebrew/bin/clangd` and the same for `clang-tidy`.
  `/opt/homebrew/bin` is ahead of `/usr/bin` on your `PATH`, so the `clangd-lsp` plugin starts clangd 23.1.4 with no
  change to the plugin, the editor's hints and the gate are the same LLVM, and Apple's `c++` stays the compiler. The
  plugin's README suggests putting `/opt/homebrew/opt/llvm/bin` first on `PATH` instead; that would also move `clang`,
  `clang-format` and `lld` ahead of Apple's. VS Code's bundled clang-tidy is then not needed. These change your machine,
  outside the repo, so Step 2 is yours, or mine on your word.
- **D8. Ubuntu local: the `ubuntu-dev` image is fixed in its own repository**, as it was for CMake 4 in
  flywheel-dag#57: its own issue, plan and tests; this plan names the requirement only. `clang-tidy-23` from
  apt.llvm.org's `llvm-toolchain-noble-23`, for arm64 and for the emulated amd64, with `clang-tidy` the command on
  `PATH`, and the image's test asserts the version. The running container keeps clang-tidy 18 until
  `./start.sh --detach` recreates it, which closes any open shell; that is yours. Until then my runs use a throwaway
  container from the new image.
- **D9. CI installs clang-tidy 23 from apt.llvm.org**, a third-party apt source, the LLVM project's own. The workflow
  checks the signing key's fingerprint, `6084F3CF814B57C1CF12EFD515CF4D18AF4F7421` (read from the key apt.llvm.org
  serves today), before trusting it, as the image's CMake download is checked against a SHA-256. The Ubuntu archive
  stops at `clang-tidy-22`, one major behind the Mac. If apt.llvm.org is down the leg fails; there is no fallback to
  another version.

## Steps

Every commit subject is scoped to the issue in the repo's `type(#N): …` form; the subjects below omit the scope. A
checkpoint ends with a full build and a full ctest run on the Mac (CMake 4.4.5, CI's flags), and nothing is pushed
until the last is green. "Remaining" counts are diagnostics from the tool, de-duplicated across units, on the Mac
(Homebrew 23.1.x) and on Ubuntu (23.1.5, in a container). The headers' findings repeat in every unit that includes
them, so the per-file tests go green only when Step 8 is done.

### Step 0 — Approval

The plan was committed as Proposed and linked from flywheel-dag#60; the next commit expanded it to LLVM 23 everywhere,
and a third records the approval. **Done when:** `git status` is clean on that commit and the issue carries the path.
Commits: `docs: add plan for running clang-tidy in CI`, `docs: expand the plan to LLVM 23 on the Mac, in Ubuntu and in
CI`, then `docs: record the approval`.

### Step 1 — Start from green

No commit. Mac: configure with CI's flags, full build, `ctest`. **Done when:** no warning and 45 of 45 pass. Anything
else: stop and report.

### Step 2 — LLVM 23 on the Mac (D7)

No commit. `brew install llvm`, then the two links. **Done when:**

- `clang-tidy --version` and `clangd --version` say 23.1.x (Homebrew), `which -a clangd` lists `/opt/homebrew/bin`
  first and `/usr/bin` after, and `c++ --version` is still Apple clang 21;
- Homebrew's clang-tidy, run by hand on the unchanged tree with the `.clangd` checks and the SDK flag, reports the same
  83 diagnostics at 77 sites as VS Code's 23.1.0. If it differs, or needs more than the SDK flag: stop and report;
- the editor agrees: in a new session a hint that only 23 gives, such as `bugprone-throwing-static-initialization` at
  `tests/test_aad_dual.cpp:26`, appears, and Apple's 21.0.0 never showed it. That is your check; I cannot see the
  plugin's process from here.

### Step 3 — LLVM 23 in the `ubuntu-dev` image (D8)

In the image's repository, under its own issue and plan, with its own tests; no commit here. **Done when:** that
repository's suite is green with an assertion that `clang-tidy --version` says 23 on arm64 and on the emulated amd64
image; its commits are pushed; and a throwaway container from the new image, on this repo's unchanged tree, reports the
62 sites measured above. The live `ubuntu-dev` keeps clang-tidy 18 until you recreate it.

### Step 4 — The configuration and the gate, red (D1, D2, D3, D5)

`.clang-tidy`, `.clangd`, the two variables, the registration, the fixtures. The variable is off by default, so
`ctest` is unchanged.

| Test | Asserts |
|---|---|
| `clang_tidy_<stem>` × 35, `clang_tidy_version`, `clang_tidy_checks`, 3 canaries (new) | as in the table above |
| the other 45 (existing) | unchanged, and pass |

**Done when:**

- with the variable unset: configure and `ctest` are as in Step 1, 45 of 45, and `ctest -N` lists no `clang_tidy_`;
- with it set, on the Mac and in the Ubuntu container: `clang_tidy_version`, `clang_tidy_checks` and the 3 canaries
  pass, and the per-file tests are red on the unchanged code, their output naming the sites above. Remaining: Mac 70
  (the 13 signed-bitwise are gone), Ubuntu 65;
- set and not found: the configure fails with the message; so it does with tests off, and with a generator that writes
  no `compile_commands.json` (Xcode is not installed here, so that branch was run with the check narrowed to Ninja
  under Makefiles); a path in place of a command name is accepted;
- each hand-made change fails exactly the case stated. Each is made on a scratch copy of the checkout and never
  restored:

  | Change | Fails |
  |---|---|
  | M1 `.clang-tidy` renamed | `clang_tidy_checks` and the three canaries, which pass `--config-file` |
  | M2 `HeaderFilterRegex` without `include/flywheel` | the engine-header canary |
  | M3 `HeaderFilterRegex: '.*'` | the dependency-header canary |
  | M4 `performance-*` left out of `Checks` | `clang_tidy_checks` and the engine-header canary |
  | M5 `--warnings-as-errors=*` left off the arguments | all three canaries: no `error:`. The per-file tests turn green, which is the vacuity the canaries exist to catch |
  | M6 a by-value `std::string` parameter seeded in `dag.hpp` | the per-file tests of the units that include it, and no others. Run in Step 8, on the first tree where all 35 are green |
  | M7 a new `tests/test_new.cpp` that no target compiles, with a by-value `std::string` parameter | `clang_tidy_test_new`; the same file without the defect passes |
  | M8 the major constant set to 22 | `clang_tidy_version`, on the Mac and in the container |

Commit: `build: add .clang-tidy and the clang_tidy_ tests, off by default`.

### Step 5 — Tests, examples and the benchmark (flywheel-dag#40)

No engine header is touched: the 24 diagnostics of those 14 files in the table, 19 of them on Ubuntu (the 5
static-initialisation sites are macOS's). **Done when:** remaining 46 on both; `ctest` 45 of 45; both examples and
`bench_hot_path --invariants` run, and the invariants match `benchmarks/expected_invariants.txt`.

| Test | Asserts |
|---|---|
| the 29 suites touched (existing) | pass: `opt.value()` and `constexpr` constants change no assertion; `.value()` on an empty optional now throws and fails the test instead of reading it |
| `test_dag_async` (flywheel-dag#40, existing) | passes with `std::llround`; no behaviour change, as the issue says |

Commits: `test: round the nanosecond conversion with std::llround` (scoped to flywheel-dag#40), then `fix: clear
clang-tidy's findings in the tests, the examples and the benchmark`.

### Step 6 — The engine's headers, mechanically

The value parameters (37), the two switches, the two enums. **Done when:** remaining 5 on both; `ctest` 45 of 45 on the
Mac, and a full build and `ctest` in the `ubuntu:26.04` container (GCC 15.2, CI's compiler) and in the `ubuntu-dev`
image's container (GCC 13.3); the disassembly of `Engine::cycle()` before and after is identical with addresses
stripped (`xcrun llvm-objdump -d --no-show-raw-insn --disassemble-symbols=__ZN3dag5async6Engine5cycleEv` on
`bench_hot_path`, then drop every run of six or more hex digits), and `bench_hot_path --invariants` matches the
committed file.

| Test | Asserts |
|---|---|
| the 45 existing | pass. Every call site that passes a `std::shared_ptr<Derived>` to a `const NodePtr&` converts, as the suites already do |

Commit: `refactor: move or borrow the NodePtr parameters, narrow two enums, merge two cases`.

### Step 7 — The CRTP constructors

New `tests/check_crtp_access.cmake`, registered as `test_crtp_access` for GCC and Clang at the top level (like
`test_standard_guard`): it compiles snippets with `-fsyntax-only`.

| Test | Asserts |
|---|---|
| `test_crtp_access` (new), control | a real op node and a real stateful node, built with `make()`, compile: the flags and include paths work. The suite derives the bases correctly, in the engine's nodes and in `SumNode` of `test_stateful_node_base`, so no case repeats that |
| `test_crtp_access`, misuse | `Bad : OpNodeImpl<RealOp>`, `Bad : NodeImpl<RealTs>` and `Bad : StatefulNodeBase<RealTs, …>`, each with a constructor of its own and a real class that is not the base's `Derived`, are refused with an error that says `private` |
| the 45 existing | pass (46 of 46 with the new test) |

**Done when:** the test is red before the edit (misuse compiles) and green after; remaining 2 on both; M9 (each
constructor back to `protected`, one at a time) fails its own misuse case; the Step 6 container builds and disassembly
diff again.
Commit: `fix: make the CRTP bases' constructors private to their derived class`.

### Step 8 — The two suppressions (D4)

`NOLINT` on `~TimeOnExit` and on the `std::system` call, each with a comment. A new issue for `toSvg`'s unquoted
paths (label `bug`), named in the comment and reported to you.

| Test | Asserts |
|---|---|
| `RollingCycleWindow.RecordNeverThrowsHoweverManyAreRecorded` (new, `test_dag_async`) | `record()` called 3 × window + 2 times does not throw, for windows 1, 2 and 5, and the mean it ends with is that of the last `window` samples |
| the 45 existing + the new one | pass |

**Done when:** remaining 0 on both; all 35 per-file tests, `clang_tidy_version`, `clang_tidy_checks` and the canaries
green on the Mac and on Ubuntu; M10 (`record()` without its eviction) fails the new test (and the existing eviction test); the disassembly diff
is empty. Commit: `fix: name the two findings clang-tidy cannot be satisfied on, and say why`.

### Step 9 — The CI job (D1, D9)

`.github/workflows/ci.yml`. **Done when:** it parses as YAML (`ruby -ryaml`); the Ubuntu leg's install and test
commands, run verbatim in a fresh `ubuntu:26.04` container, pass 40 tests; the macOS leg's, run on the Mac against
Homebrew's `llvm`, pass the same 40; and the build job is untouched. The workflow is read by GitHub, so a mistake shows
as a job that does not start or fails; Step 10 is the test. Commit: `ci: run clang-tidy at LLVM 23`.

### Step 10 — Push, and read CI

The dry run of the pre-push leak audit first (no bare `#N` in a tracked file). One push. **Done when:** both
`clang-tidy` legs are green, each prints a version that says 23, each ran 40 tests, and neither took over ten minutes;
the two build legs are green and named as before. Then flywheel-dag#40 and flywheel-dag#60 are closed with a comment
each. CI is not shown a seeded defect: that is M6, on a scratch copy. If the macOS leg never starts, that is the
runner queue, not the gate: re-run it and say so.

### Step 11 — Docs, and the plan done

`CLAUDE.md`, `README.md`, the plan's status and an Outcome section. Commit: `docs: mark the plan done`. The memory
notes on running clang-tidy by hand and on where the editor's clangd comes from are updated outside the repo.

## Designs I rejected

- **`CMAKE_CXX_CLANG_TIDY`**, tidy during the compile. Every leg compiles anyway, but the ubuntu leg already takes
  nine minutes, an incremental build runs it only on what changed, and it gets GCC's flags. The compile database is
  what clangd uses.
- **A baseline file of known findings.** It parks 83 closable defects.
- **`WarningsAsErrors: '*'` in `.clang-tidy`.** I believe clangd then reports every hint as an error.
- **Disabling `bugprone-signed-bitwise`.** It still catches a signed variable operand.
- **A floor, "23 or later", in place of the exact major.** It lets the Mac run 24 while CI runs 23, which is the
  disagreement this plan removes.
- **VS Code's bundled clang-tidy as the Mac's tool.** It moves with the extension (23.1.0 in 1.34.4, 23.1.1 in
  1.35.6), needs `-resource-dir` as well as the SDK, and ships no `clangd`. Homebrew's is a whole LLVM.
- **`brew link --force llvm`, or `llvm/bin` first on `PATH`.** Both put Homebrew's `clang` ahead of Apple's. The
  two links are enough.
- **Ubuntu's own `clang-tidy-22` for the CI leg.** No third-party source, but one major behind the Mac.
- **Pinning the patch release.** Homebrew keeps only the newest 23.x, and apt.llvm.org's package is a rolling snapshot
  of the release branch.

## Not in this plan

- **Widening the checks** (D3), and **Include Cleaner**. The `unused-includes` hints (flywheel-dag#51,
  flywheel-dag#53, flywheel-dag#54) are clangd's, not a tidy check; `misc-include-cleaner` is its twin, and
  flywheel-dag#61 records a false positive.
- **`tests/consumer/`**, which is a separate project built by a test and in no compile database.
- **`toSvg`'s unquoted paths**, tracked by flywheel-dag#62, which Step 8 opened.
- **Updating VS Code's cpptools**, and the `ubuntu-dev` container's recreation (D8): yours.
- **A release** (D6).

## Self-review — risks and assumptions

- **clangd reads `.clang-tidy`.** It documents that it does, and merges `.clangd`'s `Diagnostics.ClangTidy` on top.
  I cannot show it from here: Apple's `clangd --check` prints no tidy finding even with `-log=verbose` (checked on
  `test_ring_buffer.cpp`, which has one). The check is yours, with clangd 23 after Step 2: after Step 4 a hint you
  see today (for example `test_ring_buffer.cpp:45`) must still appear, and after Step 5 it must go.
- **Same major, not same build.** Homebrew's 23.1.4 is a tagged release and apt.llvm.org's 23.1.5 a snapshot of the
  release branch; `clang_tidy_version` holds the major only. Step 2 compares Homebrew's with VS Code's 23.1.0 on this
  code, and the Mac and the two Ubuntus already agree up to the 5 macOS sites.
- **The tool moves to 24 under the job.** Homebrew's `llvm` becomes 24 when it ships; the macOS leg and your Mac then
  fail `clang_tidy_version`, on purpose. The fix is the bump in D2, or `llvm@23` once Homebrew makes that formula.
- **apt.llvm.org and Homebrew are outside the repo.** An outage fails a leg. The key fingerprint is checked, the
  version is not floating past the major, and a new Ubuntu codename needs apt.llvm.org to carry it: the workflow
  derives the codename with `lsb_release -cs`, so a missing one fails at `apt-get update`, loudly.
- **Time on the macOS leg.** The 457 MB bottle's install is unmeasured; the job has a `timeout-minutes` and the tidy
  run itself is about a minute on four cores. The first run is the measurement.
- **Homebrew's clang-tidy and the SDK.** I assume the SDK flag is all it needs, since it carries its own resource
  directory; Step 2 measures it, and stops if not. Its libc++ headers may be its own rather than the SDK's, which
  matters to a parse only.
- **The image is another repository.** Step 3 depends on it. If it slips, the Ubuntu runs use throwaway containers, as
  the measurements here did, and nothing in this repo's gate depends on `ubuntu-dev`.
- **A clang newer than the SDK's libc++ meets its deprecations.** That is the `-Wno-error=deprecated-declarations`
  above. A deprecation in this project's own files is still an error under `--warnings-as-errors`; one in a dependency
  or the SDK is not this project's to fix, and the build, which uses the SDK's own clang, never sees it.
- **The header filter is a path match** (`/include/flywheel/`, `/tests/`, …). A checkout inside a directory with one
  of those names would admit more than it should. The dependency canary fails if a real `_deps` path were admitted.
- **Only instantiated templates are analysed.** All 25 headers are reached, but a template no unit instantiates is
  not checked. The suites instantiate what they test.
- **`.value()` for `*opt` in tests** turns a read of an empty optional into an exception that gtest reports as a
  failure: stricter, never looser.
- **The canaries read clang-tidy's wording** (`error:`, `[check-name]`, `LLVM version`). A rewording turns them red,
  which is the right failure: the gate's output changed.
- **Assumptions:** the suite stays under `ctest` with no sleeps; the tidy tests need no network beyond the configure's
  `FetchContent`; the runners keep CMake 4.4 or later.

## Progress

| Step | Status | Notes |
|---|---|---|
| 0 Approval | Done | Proposed, expanded to LLVM 23, then approved as written (D1 to D9) with leave to install LLVM on the Mac. |
| 1 Start from green | Done | No commit. Mac, CMake 4.4.5, CI's flags: no CMake or compiler warning, ctest 45 of 45. |
| 2 LLVM 23 on the Mac | Done | No commit. `brew install llvm`: 23.1.4, 1.8 GB on disk with z3 and zstd. `clangd` and `clang-tidy` are linked into `/opt/homebrew/bin`; `which -a clangd` lists them before `/usr/bin/clangd`, and `c++` is still Apple clang 21. Homebrew's clang-tidy with the SDK flag alone reports the same 83 diagnostics at 77 sites as VS Code's 23.1.0. The editor check (a 23-only hint in a new session) is yours. |
| 3 LLVM 23 in the `ubuntu-dev` image | Done | In the image's repository: its issue is closed and its plan (005) is committed; `clang-tidy-23` 23.1.5 from apt.llvm.org, key fingerprint checked, in place of apt's 18; the suite is green twice (`test_image.sh` 53 assertions, `test_start_e2e.sh` 17, six of them new); the three hand-made changes failed as stated; commits pushed. A container from the new image reports the same 62 sites. The live `ubuntu-dev` keeps 18 until you recreate it. |
| 4 The configuration and the gate, red | Done | `.clang-tidy`, `.clangd` trimmed, the two variables, 40 tests, the fixtures. Variable unset: configure and build clean, ctest 45 of 45, no `clang_tidy_` test. Set, on the Mac (Homebrew 23.1.4), on Ubuntu 26.04 (23.1.5) and in a container from the `ubuntu-dev` image: 9 pass (version, checks, three canaries, four files with no finding) and 31 fail on the findings. M1 to M5, M7 and M8 failed the cases stated; M1 also takes the other two canaries, and M5 turns the 31 green, which only the canaries notice. M6 waits for Step 8. **Found:** clang 23 against the macOS SDK's libc++ stops 21 units on nlohmann/json's `char_traits<unsigned char>` under the project's `-Werror=deprecated-declarations`; the tests pass `-Wno-error=deprecated-declarations` (see The change). |
| 5 Tests, examples and the benchmark | Done | `test: round the nanosecond conversion with std::llround` (flywheel-dag#40), then `fix: clear clang-tidy's findings in the tests, the examples and the benchmark`. The 24 diagnostics are gone: remaining 46 at 43 sites on the Mac (Homebrew 23.1.4) and on Ubuntu 26.04 (23.1.5), the same kinds and counts, and no error from the SDK or a dependency. Build clean with CI's flags, ctest 45 of 45, both examples exit 0, `bench_hot_path --invariants` matches the committed file. **Found:** the plan's `opt.value()` was wrong; the check reports it as it does `*opt` (probe file), so the optional sites use a check the analysis can see. |
| 6 The engine's headers, mechanically | Done | `refactor: move or borrow the NodePtr parameters, narrow two enums, merge two cases`. The 41 diagnostics are gone: 30 `NodePtr` parameters are moved at their one copy, 7 parameters of 5 functions are `const` references, `NodeKind` and `Direction` are `std::uint8_t`, and `shapeFor` merges its labels. Remaining 5 at 5 sites on the Mac and on Ubuntu 26.04 (3 CRTP, `~TimeOnExit`, `std::system`). Mac: build clean with CI's flags, ctest 45 of 45. Ubuntu 26.04 (GCC 15.2) and the `ubuntu-dev` image (GCC 13.3): configure and build clean, ctest 45 of 45. The disassembly of `Engine::cycle()` is identical before and after (190 lines, addresses stripped), and `bench_hot_path --invariants` matches the committed file. |
| 7 The CRTP constructors | Done | `fix: make the CRTP bases' constructors private to their derived class`. `test_crtp_access` was red on the unchanged headers, for the three misuse cases and nothing else (the control compiled), and is green after. M9a to M9c, each constructor back to `protected`, fail their own case and no other. Remaining 2 at 2 sites on the Mac and on Ubuntu (`~TimeOnExit`, `std::system`). Mac: build clean, ctest 46 of 46. Ubuntu 26.04 (GCC 15.2) and the `ubuntu-dev` image (GCC 13.3): configure and build clean, ctest 46 of 46. The disassembly of `Engine::cycle()` is identical to the pre-Step-6 binary, and the invariants match. |
| 8 The two suppressions | Done | `fix: name the two findings clang-tidy cannot be satisfied on, and say why`. `NOLINTNEXTLINE(bugprone-exception-escape)` on `~TimeOnExit`, with the invariant stated, and `NOLINT(cert-env33-c, bugprone-command-processor)` on the `std::system` call, with a comment that points at **flywheel-dag#62** (opened: `toSvg` builds a shell command from two unquoted paths; label `bug`). Remaining 0 on the Mac (Homebrew 23.1.4), on Ubuntu 26.04 (23.1.5) and in a container from the `ubuntu-dev` image (23.1.5): all 40 `clang_tidy_` tests pass on each. Mac: build clean, ctest 46 of 46; the disassembly of `Engine::cycle()` is identical to the pre-Step-6 binary and the invariants match. M10 (no eviction in `record()`) fails the new test. **M6** (a by-value `std::string` parameter in `dag.hpp`): of 35 units 30 include it, and exactly those 30 per-file tests failed; the 5 that do not (`allocation_counter`, `bench_report`, `test_allocation_counter`, `test_bench_report`, `test_ring_buffer`) passed. |
| 9 The CI job | Done | `ci: run clang-tidy at LLVM 23`. The workflow parses as YAML (Ruby) with the jobs `build`, untouched, and `clang-tidy`. The Ubuntu leg's steps, taken out of the YAML and run as separate `bash -e` steps in a fresh `ubuntu:26.04` container (arm64 here, with the arm64 preset; the runner is x86_64): the key fingerprint check passes, apt installs clang-tidy 23.1.6 (apt.llvm.org's snapshot had moved on from 23.1.5 within hours), and 40 of 40 tests pass in 22 s. The macOS leg's steps on the Mac, against Homebrew's 23.1.4: 40 of 40 in 15 s. **Not proved here:** the x86_64 packages, and a first `brew install llvm` on a runner. Step 10 is that test. |
| 10 Push, and read CI | | |
| 11 Docs, and the plan done | | |
