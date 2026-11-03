# Require C++23

**Status: Proposed (2026-11-03), awaiting approval.** Nothing but this plan has changed.

Closes [flywheel-dag#56](https://github.com/tomlin256/flywheel-dag/issues/56).

## Problem

The issue reads: *"Upgrade to c++ 23. It will be a major revision and all consumers will have to upgrade. We are
abandoning c++17."*

Today the repo asks for C++17 in code in two places, sets a CMake floor that predates C++23, and says "C++17" in
words in 68 files:

- `CMakeLists.txt:88` sets `CMAKE_CXX_STANDARD 17` for this project's own targets.
- `CMakeLists.txt:150` gives `flywheel_dag` the usage requirement `cxx_std_17`. `flywheel::dag` hands it to every
  consumer, whether it adds the engine as a subproject or finds the installed package.
- `CMakeLists.txt:19` and `tests/consumer/CMakeLists.txt:14` say `cmake_minimum_required(VERSION 3.18)`. `cxx_std_23`
  needs 3.20.
- 65 banners, `README.md` (twice), `CLAUDE.md` (twice) and the top of `CMakeLists.txt` say "C++17".

Nothing stops a C++17 build from including the headers, and nothing in the engine needs a C++23 feature yet. This
plan therefore moves the floor and makes it real: a consumer's CMake raises its target to `-std=c++23` through
`flywheel::dag`, and any other build that includes a header below C++23 gets one error line. It adopts no C++23
feature, and the only change under `include/` besides the banners is that guard.

### What I measured

Scratch copies of `342fcc1` (the repo was not touched), with the standard flipped by three substitutions in
`CMakeLists.txt` (`CMAKE_CXX_STANDARD 23`, `cxx_std_23`, `cmake_minimum_required` 3.20) and nothing else edited:
no header, test or example. Configured and built with CI's flags, `-Werror=dev -Werror=deprecated
-DFLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON`, so a warning is an error.

| Compiler, library, CMake | At `-std=gnu++23` |
|---|---|
| Apple Clang 21.0.0, libc++, CMake 4.2.3 (the macOS leg's compiler) | builds; ctest 41 of 41. One warning, from googletest (below) |
| GCC 15.2.0, libstdc++ 15, CMake 4.2.3 (the `ubuntu:26.04` image, aarch64; CI's `ubuntu-latest` is this image on x86_64) | builds clean; 41 of 41 |
| GCC 13.3.0, libstdc++ 13, CMake 3.28.3 (`ubuntu-dev`, aarch64) | builds clean; 41 of 41 |
| GCC 13.3.0 with CMake 3.20.6 (Kitware's binary, checksum verified) | builds clean; 40 of 40 (`test_presets` registers from CMake 3.21) |
| Clang 18.1.3, libstdc++ 13 (`ubuntu-dev`) | 40 of 41: `test_allocation_counter` does not compile, and does not at C++17 either |

- **The engine needs no edit to compile as C++23.** Every row built the unmodified headers, tests, examples and
  benchmark without a warning of ours (the Apple Clang row's one warning is googletest's, next). I also grepped
  `include/`, `tests/`, `examples/` and `benchmarks/` for what C++20 and C++23 remove or change: no `u8` literal or
  `char8_t`, no `std::result_of`, `std::iterator`, `std::is_pod`, `std::uncaught_exception` or atomic free function
  on a `shared_ptr`, and no `<=>`. The two `volatile`s and the ten `[=]` lambdas are in `tests/`, and compile clean.
- **One diagnostic, and it is googletest's.** Compiling `gtest-all.cc` as C++20 or later on Apple Clang 21 prints
  `gtest-printers.h:528:35: warning: implicit conversion from 'char8_t' to 'char32_t' [-Wcharacter-conversion]`.
  v1.16.0 (ours) and v1.17.0 have the line; v1.18.0 (2026-08-10) does not. With v1.18.0 the same Apple Clang build
  prints nothing and passes 41 of 41.
- **Clang 18 fails the same way before the change.** Clang before 19 does not enable sized deallocation, so
  libstdc++'s `<new>` does not declare `operator delete(void*, std::size_t)`, which `test_allocation_counter.cpp`
  calls by hand. I ran the unmodified C++17 source as a control and got the same eight errors. It is
  [flywheel-dag#58](https://github.com/tomlin256/flywheel-dag/issues/58), and this plan does not fix it. Until it is
  fixed, Clang 18 is not a compiler this plan claims to support.
- **The hot path does not move on Apple Clang.** `bench_hot_path --invariants` built as C++23 matches
  `benchmarks/expected_invariants.txt` exactly, and `Engine::cycle()`'s disassembly (190 lines, addresses stripped)
  is identical to the C++17 build of the same commit.
- **`__cplusplus` is not 202302L everywhere.** `-std=gnu++23` gives 202302L on Clang 18, Apple Clang 21 and GCC 15.2,
  and 202100L on GCC 13.3. A guard written as `>= 202302L` would reject GCC 13, and neither CI leg would notice.

## The change

```cmake
cmake_minimum_required(VERSION 3.20)                         # the first CMake with cxx_std_23
...
    set(CMAKE_CXX_STANDARD 23)                               # top level only, as now
...
target_compile_features(flywheel_dag INTERFACE cxx_std_23)   # reaches every consumer
```

Every public header refuses to compile below C++23. `dag.hpp` carries the guard, and every other header reaches
`dag.hpp` except `dag_ring_buffer.hpp`, which carries the guard too. The test is "newer than C++20", for the reason
in the comment:

```cpp
// flywheel-dag is C++23 (flywheel-dag#56). Linking flywheel::dag from CMake asks for it
// (cxx_std_23); a build that does not use CMake must pass -std=c++23 itself. This tests for
// newer than C++20, not for C++23's own value: GCC 13 reports 202100L for -std=c++23. A header
// that includes no other engine header needs this guard too; test_standard_guard fails until
// it has one.
#if __cplusplus <= 202002L
#error "flywheel-dag requires C++23: compile with -std=c++23, or link flywheel::dag from CMake, which asks for it"
#endif
```

The guard is also what gives the consumer tests teeth. `tests/consumer` sets no `CMAKE_CXX_STANDARD` on purpose, so
that the usage requirement is what raises it, but `main.cpp` compiles at any standard today, so no consumer test can
tell whether the requirement arrived. With the guard, the subproject consumer and both package consumers fail to
compile unless `cxx_std_23` reaches them.

`test_standard_guard` (`tests/check_standard_guard.cmake`, a `cmake -P` script like the other `check_*` scripts)
globs `include/flywheel/*.hpp`, writes a one-line translation unit per header, and compiles each with
`-std=c++17` and `-std=c++20` and `-fsyntax-only`. Each compile must fail with "requires C++23" on stderr. It fails
if it finds no header, so a wrong path cannot pass by scanning nothing, and it names every header that compiled or
failed without the message. A header added later is covered by the glob. It is registered at the top level for GCC
and Clang, as `test_warning_flags` is.

The rest is words: the 65 banners, the README, `CLAUDE.md`, the top of `CMakeLists.txt`, and the comment in
`tests/allocation_counter.hpp` that cites C++17 for the twenty replaceable allocation functions (the list is the
same in C++23). The README gains a short paragraph for a consumer: link `flywheel::dag` and your target is compiled as
C++23, and a build outside CMake must pass `-std=c++23`.

## Decisions to confirm

My defaults, each one a place to edit before approving.

- **D1. The release is v1.0.0.** I read "a major revision" as the major version. Consequences: the package version
  policy becomes `SameMajorVersion`, which the comment at `CMakeLists.txt:178` already says happens at 1.0;
  `find_package(flywheel_dag 1 …)`; the README's "before 1.0" sentence is rewritten; and from here a change that
  breaks consumers needs 2.0. The alternative is **v0.4.0**: `SameMinorVersion` stays, `find_package(flywheel_dag 0.4)`
  refuses a 0.3 request just as well, and 1.0 stays free for the day you call the API stable. A tag cannot be taken
  back once a consumer pins it, so say which you mean.
- **D2. One release with flywheel-dag#57.** "Upgrade to cmake 4" was opened a minute after this issue, with the same
  framing ("we are abandoning cmake 3"), and it also breaks consumers. Under D1 a second breaking release would be
  2.0. I recommend one release that carries both, so consumers upgrade once. Then Steps 1 to 6 land now and Step 7
  waits for the plan of flywheel-dag#57 to finish. Strike that wait to release this change alone.
- **D3. The header guard.** Without it, only CMake enforces the floor. A consumer on another build system finds out
  when the engine first uses a C++23 feature; today it uses none, so a C++17 build still compiles. With it, any build
  gets one line, and the consumer tests pin the usage requirement. I recommend it. Strike Step 3 for the CMake change
  alone.
- **D4. googletest v1.16.0 to v1.18.0.** It removes the one warning the C++23 build prints on the macOS leg, and the
  repo's rule is a warning-free build. The alternative is a flag that silences it, which the rule forbids. It is a
  test-only dependency and nothing is installed or exported. Strike Step 4 and the macOS build prints that warning.
- **D5. `cmake_minimum_required` 3.20, not 4.** 3.20 is the first CMake that has `cxx_std_23`, and I ran the build on
  3.20.6. flywheel-dag#57 raises it again in its own plan.

## Steps

Every commit subject is scoped to flywheel-dag#56 in the repo's `type(#N): …` form. The subjects below omit the
scope. Each step ends with a full build and a full ctest run on the Mac and in the `ubuntu-dev` container, and
nothing is pushed until its step is green. CI is the arbiter for GCC 15.2 on x86_64, so the push that carries
Step 3 goes on its own and the next waits for CI.

### Step 0 — Approval

The plan is committed as Proposed so it can be read and edited. Nothing else lands until it is approved. A comment on
flywheel-dag#56 links the plan, and a commit records the approval here, with D1 to D5 as you decide them.

**Done when:** `git status` is clean on that commit and flywheel-dag#56 carries the link.

Commit: `docs: add plan for requiring C++23`.

### Step 1 — Start from green, and record the baseline

No commit. `cmake --preset relwithdebinfo-macos-arm64`, build, `ctest --preset …`. Then build with
`-Werror=dev -Werror=deprecated -DFLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON`, build `bench_hot_path`, run `--invariants`
against `benchmarks/expected_invariants.txt`, and save the disassembly of `Engine::cycle()` from the benchmark binary
(`xcrun llvm-objdump -d --no-show-raw-insn`, one symbol, addresses stripped) as the reference for Step 6. Repeat the
build and ctest in the container's `relwithdebinfo-linux-arm64` tree.

**Done when:** no configure or build warning, ctest passes in full on both (41 of 41 at the last commit), the
invariants match, and the reference disassembly is saved. Anything else: stop and ask.

### Step 2 — Raise the standard

`CMakeLists.txt`: `cmake_minimum_required(VERSION 3.20)` with the comment above it rewritten (it explains why a plain
minimum and not a range), `CMAKE_CXX_STANDARD 23`, `cxx_std_23`. `tests/consumer/CMakeLists.txt`:
`cmake_minimum_required(VERSION 3.20)`, because the consumer stands for a project with the engine's own floor, and
the comment over `add_executable` names `cxx_std_23`.

No new test: this step changes only what the existing 41 prove, which is that the engine builds and passes as C++23.
Step 3 adds the tests that fail if the requirement stops reaching a consumer.

**Done when:** each tree reconfigures and rebuilds everything, ctest is green in full on both, the build prints no
warning with CI's flags on GCC 13.3 and prints only googletest's on Apple Clang, and
`grep -nE "CXX_STANDARD|cxx_std" CMakeLists.txt` shows 23 and nothing else.

Commit: `build: require C++23`.

### Step 3 — Refuse to compile below C++23

The guard in `dag.hpp` and `dag_ring_buffer.hpp` (above), `tests/check_standard_guard.cmake`, its registration in
`tests/CMakeLists.txt`, and the consumer comment.

| Test | Asserts |
|---|---|
| T1 `test_standard_guard` | every header in `include/flywheel/*.hpp` fails to compile at `-std=c++17` and at `-std=c++20`, with "requires C++23" on stderr; the glob found headers |
| T2 `test_consumer_subproject`, `test_consumer_package`, `test_consumer_package_own_dependencies` (existing) | now compile only because `cxx_std_23` reaches the consumer |

**Done when:** T1 fails on a scratch copy without the guard and passes with it, T2 fails on a scratch copy without
`cxx_std_23` and passes with it, ctest is green in full on both, the build prints no warning, and each hand-made
change below fails exactly the tests stated.

Each change is made on a scratch copy and never restored (Make 3.81 compares mtimes in whole seconds), and each test
runs alone under a watchdog:

| Change | Fails |
|---|---|
| M1 `target_compile_features` without `cxx_std_23`, `CMAKE_CXX_STANDARD 23` left in place | the three consumer tests; everything else passes |
| M2 the guard compares `<= 201703L` | T1, naming every header at `-std=c++20` |
| M3 the guard compares `< 202302L` | not T1, which compiles below C++23 only; every test on GCC 13.3, which reports 202100L. CI cannot see this one (GCC 15.2 and Apple Clang 21 report 202302L), so the comment names the value and the container run is the check |
| M4 the guard is missing from `dag_ring_buffer.hpp` | T1, naming that header |
| M5 the script's glob points at a directory with no header | T1, "found no header" |

Commit: `feat: refuse to compile below C++23`.

### Step 4 — googletest v1.18.0 (D4)

`GIT_TAG v1.18.0` in the root `CMakeLists.txt`. No new test: the suite is the test.

**Done when:** the Apple Clang build prints no warning at all (Step 2's printed one), ctest is green in full on both,
and the GCC 13.3 build prints none. Anything new in a v1.18.0 header: stop and report it.

Commit: `build: update googletest to v1.18.0`.

### Step 5 — Words

A `perl -pi` over the 65 banners from `header-only C++17 reactive` to `header-only C++23 reactive`; `README.md` (the
first line, the requirement sentence "Requires a C++23 compiler and CMake 3.20 or later", and the paragraph for
consumers); `CLAUDE.md` (the first line and the banner example); the first line of `CMakeLists.txt`; the allocation
comment. Older plans are records and stay as written.

No test: prose and comments only.

**Done when:** `grep -rn "C++17" . --exclude-dir=build --exclude-dir=.git --exclude-dir=plans` prints nothing;
`git diff --numstat` shows one line changed in each of the 65 banner files, and the other files are the ones named;
the `pre-push` hook's audit of the tree is clean; ctest is green in full on both.

Commit: `docs: say C++23 in the banners, the README and CLAUDE.md`.

### Step 6 — Measure

No commit. On the Mac: `--invariants` against the committed file; `Engine::cycle()`'s disassembly against Step 1's;
`bench_hot_path` from Step 1's tree and this one, three runs each, interleaved, after a 300 ms spin, rows compared
against the repo's documented noise (about 1%; up to 6% for a one-loop micro-benchmark, flywheel-dag#34). A row beyond
the noise gets the pad sweep before it is called real. In the container: `--invariants` against the committed file,
ctest in `relwithdebinfo-linux-arm64`, and ctest in the AddressSanitizer and UndefinedBehaviorSanitizer tree
(`build/asan-linux-arm64`, `--parallel 4`; LeakSanitizer runs only there).

**Done when:** both invariants match, `cycle()` is identical after addresses are stripped, no benchmark row moves
beyond the noise, and both container trees pass in full with no sanitizer report. Any other result: stop and report
it, with the numbers.

### Step 7 — Release (D1, D2)

The version, as v0.3.2 was released: `project(… VERSION 1.0.0 …)`, the two FetchContent snippets (`CMakeLists.txt`,
`README.md`), `COMPATIBILITY SameMajorVersion` with its comment, `find_package(flywheel_dag 1 …)` in both places, and
the README's compatibility sentence. A new `tests/check_package_version.cmake` and `test_package_version` read the
generated `flywheel_dagConfigVersion.cmake` and check which requests it accepts:

| Request | 1.0.0 accepts |
|---|---|
| `1`, `1.0`, `1.0.0` | yes |
| `0.3`, `0.3.2`, `0.9` | no: an old consumer must not silently get a C++23-only engine |
| `1.1`, `2` | no |

The test's table is written first and fails against `SameMinorVersion` (M6), then the policy flips. Before the notes
say what changed, run `git log --format='%h %s' v0.3.2..HEAD` and `git diff v0.3.2..HEAD --stat -- include`: the
include diff should be the banner line in each of `include/`'s 25 files and the two guards, and the notes list every
commit since the tag. Then the tag, a GitHub release whose notes lead with the break (C++23 and CMake 3.20 required; the
`find_package` request that works), the plan marked done with an Outcome, and flywheel-dag#56 closed with a summary
comment. Under D1 = v0.4.0 the policy line stays and the table is written for 0.4.

**Done when:** ctest is green in full on both, CI is green on the release commit on both legs, the installed version
file reports the new version, M6 fails `test_package_version`, the release is published, and flywheel-dag#56 is closed.

Commits: `build: release v1.0.0` and `docs: mark the plan done`.

## Designs I rejected

- **Keep supporting C++17 and add a C++23 leg.** The issue says the opposite, and it would keep every later change
  from using a C++23 feature.
- **Only `CMAKE_CXX_STANDARD 23`, without the usage requirement.** It sets this project's own targets and reaches no
  consumer.
- **Stop at C++20.** The issue says 23, and the probes show nothing in the engine that needs either.
- **Guard on `__cplusplus >= 202302L`.** GCC 13 reports 202100L for C++23.
- **Guard with `static_assert` or a feature-test macro.** `#error` stops the translation unit with one line before
  any other diagnostic, and no feature macro is common to the compilers in the table.
- **Silence googletest's warning with `-Wno-character-conversion`.** The rule is to fix a warning, and v1.18.0 does.
- **Jump to CMake 4 here.** That is flywheel-dag#57, and it would put two breaking changes in one commit history with
  one plan.

## Not in this plan

- **Adopting a C++23 feature in the engine.** `std::move_only_function` for the wake and output hooks,
  `std::expected`, `std::unreachable`, `if consteval` and deducing `this` in place of the CRTP bases are all
  candidates. Each is a design change with its own risk and its own library availability on GCC 13.3, so each gets its
  own issue after this lands.
- **CMake 4:** [flywheel-dag#57](https://github.com/tomlin256/flywheel-dag/issues/57).
- **Clang before 19 on libstdc++:** [flywheel-dag#58](https://github.com/tomlin256/flywheel-dag/issues/58).
- **A CI leg for the oldest compiler.** The README promises a C++23 compiler, CI shows GCC 15.2 and Apple Clang 21, and
  the container shows GCC 13.3. A leg for GCC 13 is worth adding when a C++23 library feature is adopted and the floor
  becomes a question.
- **MSVC and Windows.** Not built or tested. The guard reads `__cplusplus`, which MSVC reports as 199711L without
  `/Zc:__cplusplus`, so it would reject a Windows build that passes `/std:c++latest` alone.
- **The older plans.** "The engine stays header-only C++17" in their assumptions was true when written.

## Self-review — risks and assumptions

- **The probes show that unmodified code compiles and passes, not that no C++17 to C++23 semantic change matters.**
  What I could grep for is absent (above). The suite passes on three compiler and library pairs with Clang 18 failing
  only the test that fails at C++17, the benchmark's checksum is identical, and `cycle()` is identical on Apple Clang.
  GCC's `cycle()` was not compared, and C++20's comparison rewriting is covered only by `Dual<N>`'s tests, which pass.
- **A consumer's own code may not survive C++23.** That is the issue's premise, and this repo cannot test it. The
  release notes say so.
- **The guard rejects a build that passes `-std=c++20` and compiled before.** That is the intent. It also gives the
  mixed-standard case, one consumer target at C++17 including headers another has at C++23, a compile error instead of
  a silent ODR hazard.
- **CMake older than 3.20 on the `find_package` path.** The exported target names `cxx_std_23`, which CMake 3.19 and
  older do not know, so they fail inside the targets file with CMake's own message about the feature. I add no
  version check to the package config for that.
- **x86_64.** The GCC 15.2 probe was aarch64. CI's ubuntu leg is x86_64, and its compiler is the one that has found
  warnings the container could not (flywheel-dag#44). CI is the arbiter, which is why Step 3 is pushed alone.
- **Step 2 alone leaves banners saying C++17** for three commits. Nothing is pushed between.
- **googletest.** v1.18.0 replaces the framework for 33 test files. It built clean and passed 41 of 41 on Apple Clang;
  Step 4 repeats that on GCC 13.3, and CI does on GCC 15.2.
- **The version.** A tag is published once a consumer pins it. D1 and D2 are the two things in this plan that cannot
  be undone by a revert, which is why they are listed first.
- **Assumptions:** the engine stays header-only; no C++23 feature is adopted; the suite keeps running under `ctest`
  with no sleeps; CI stays on GCC 15.2 and Apple Clang; CMake 3.20 or later is available to every consumer.

## Progress

| Step | Status | Notes |
|---|---|---|
| 0 — Approval | Not started | |
| 1 — Start from green | Not started | |
| 2 — Raise the standard | Not started | |
| 3 — Refuse to compile below C++23 | Not started | |
| 4 — googletest v1.18.0 | Not started | |
| 5 — Words | Not started | |
| 6 — Measure | Not started | |
| 7 — Release | Not started | |
