# Build the Engine's Own Targets with -Wall -Wextra -Wpedantic

**Status: Approved (2026-09-28).**

Closes [flywheel-dag#4](https://github.com/tomlin256/flywheel-dag/issues/4).

## Problem

The top-level build adds one warning flag, `-Werror=deprecated-declarations`. Nothing else warns,
so a reordered initialiser, a dead variable or a dangling reference compiles in silence. The issue
asks for three things:

1. turn on `-Wall -Wextra`, and `-Wpedantic` if practical, for the project's own targets;
2. fix what they find;
3. make them errors in CI.

The flags must stay off for consumers. A subproject never sets build-wide options.

The engine is header-only, so its own targets are the translation units that include its headers:
the 19 test suites, the example, the benchmark and `test_bench_report`. The headers are checked
through them.

## What the flags find today

A scratch build of `fc6758f` had `-Wall -Wextra -Wpedantic` added beside the existing flag. It used
Apple Clang 21 on arm64, `RelWithDebInfo`, and every target including `bench_hot_path`. It printed
145 warnings from 10 sites:

| Site | Warning | Fix |
|---|---|---|
| `include/flywheel/dag_timeseries.inl:39` | `-Wreorder-ctor`: `name_` is listed before `upstream_`, but declared after `slot_`. It prints once per `StatefulNodeBase` instantiation, 136 times in all | List the initialisers in declaration order: `upstream_`, `eq_`, `name_` |
| `tests/test_timeseries.cpp:103`, `:208` | `-Wunused-but-set-variable`: a `t` counter that nothing reads | Remove it |
| `tests/test_timeseries.cpp:132`, `:211` | `-Wunused-variable`: `ab` and `em` | Keep the evaluation and drop the name |
| `tests/test_dag_state_store.cpp:498`, `:525`, `:541` | `-Wunused-variable`: an `EvalContext` nothing uses | Remove it |
| `tests/test_dag_graph.cpp:152` | `-Wunused-but-set-variable`: `count` | Remove the loop that computes it. The test asserts on `attrCount` |
| `tests/test_dag_traversal.cpp:60` | `-Wunused-function`: `names_of` | Remove it |

`-Wpedantic` found nothing. No warning came from a dependency's header: spdlog, fmt, nlohmann/json
and googletest were all silent. So `-Wpedantic` is practical, and the fixes are small.

The reorder changes only the text. A class always initialises its members in declaration order,
whatever order the list gives, and none of these three initialisers reads another member. The
other nine fixes remove code that no assertion reads, so no test loses a check.

### GCC is not yet known

No GCC was at hand, so CI's Linux leg (`ubuntu-latest`: GCC 13 and libstdc++) is the first GCC
build with these flags. GCC's `-Wall` and `-Wextra` differ from Clang's. Two findings are likely:

- **`-Wdangling-reference`** (GCC 13 and later, in `-Wall`). It fires when a reference is bound to
  the result of a function that returns a reference and was called with a temporary. The pattern
  is `const T& x = get_value<T>(node->eval(ctx))`, which binds to a value reached through a
  temporary `ValuePtr`. It appears at `dag_timeseries.inl:58` and `tests/test_dag.cpp:999`.
  Neither dangles today, because the evaluated node's own cached value keeps the object alive.
  That is exactly the subtlety the warning is about. The fix is to hold the `ValuePtr` in a local.
  Guaranteed copy elision means that costs no extra reference-count traffic.
- **Optimiser-dependent warnings at `-O2`**: `-Wmaybe-uninitialized`, `-Warray-bounds` and
  `-Wstringop-overflow`. These have known false positives inside libstdc++.

**The rule for anything GCC finds:** fix the code. A confirmed compiler false positive is
suppressed as narrowly as possible: one site, one compiler, and a comment naming the compiler bug.
Turning a warning off for the whole build needs its own approval first.

## Design

### Where the flags go

The existing top-level block gets the flags. It already comes after every `FetchContent`
dependency:

```cmake
if(_flywheel_dag_top_level)
    option(FLYWHEEL_DAG_WARNINGS_AS_ERRORS
           "Make every compiler warning an error in this project's own targets" OFF)

    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        add_compile_options(-Wall -Wextra -Wpedantic -Werror=deprecated-declarations)
        if(FLYWHEEL_DAG_WARNINGS_AS_ERRORS)
            add_compile_options(-Werror)
        endif()
    endif()
    ...
endif()
```

- `add_compile_options` is directory-scoped and applies only to targets created after it. At this
  point that means `tests/`, `examples/` and `benchmarks/`, and nothing in spdlog or googletest.
- The flags are **not** put on `flywheel_dag`'s `INTERFACE`. That would put them on every consumer
  translation unit that links `flywheel::dag`.
- They are set at the top level only. As a subproject, the engine sets nothing. The option is
  declared inside the block, so a consumer's cache never shows a switch that does nothing.
- Only GCC and Clang get the flags, as `-Werror=deprecated-declarations` does today. No CI leg uses
  MSVC.

### Errors in CI only

`FLYWHEEL_DAG_WARNINGS_AS_ERRORS` defaults to `OFF`, and CI configures with it `ON`. A local build
with a newer compiler therefore warns about a new diagnostic instead of refusing to build, and CI
fails on it.

Rejected alternatives:

- **`-DCMAKE_CXX_FLAGS=-Werror` in CI.** It also reaches spdlog's and googletest's own sources. A
  dependency's warning under a new compiler would then fail this project's CI.
- **`CMAKE_COMPILE_WARNING_AS_ERROR` (CMake 3.24).** The cache variable reaches the dependencies
  in the same way. The project's minimum is also 3.18.
- **An `INTERFACE` "warnings" target that each test links.** It has the same effect but needs one
  line per target, and a new target that forgets the line goes unchecked.
- **`-Werror` by default at the top level.** The issue asks for errors in CI. A contributor's newer
  compiler should warn, not stop the build.

### Guards

A warning gate that silently stops applying still passes CI, so the gate itself is tested:

1. **`test_warning_flags`** is a new ctest test that runs `cmake -P tests/check_warning_flags.cmake`.
   It reads `compile_commands.json`, which the root already exports, and fails unless:
   - every translation unit of this project, meaning one under the source directory but outside
     the build directory, compiles with `-Wall`, `-Wextra` and `-Wpedantic`, and also with
     `-Werror` when `FLYWHEEL_DAG_WARNINGS_AS_ERRORS` is `ON`;
   - no dependency's translation unit compiles with `-Werror` or `-Wpedantic`. `-Werror` is the
     flag that would hurt, and no dependency sets `-Wpedantic` for itself, so it stands for the
     rest of the set;
   - at least one translation unit came from each of `tests/`, `examples/` and `benchmarks/`, so
     a wrong path or a new file layout cannot pass by scanning nothing.

   It is registered only where it can run: at the top level, with GCC or Clang, under a Makefiles
   or Ninja generator (the ones that write `compile_commands.json`), and with CMake 3.19 or later
   (for `string(JSON)`).

2. **`test_consumer_subproject`** gains two configure-time checks in `tests/consumer/CMakeLists.txt`.
   `flywheel::dag` must carry no `INTERFACE_COMPILE_OPTIONS`, and `FetchContent_MakeAvailable`
   must leave the consumer's `CMAKE_CXX_FLAGS` as it found them. A failed check fails the
   consumer's configure, and so fails the test.

A scratch prototype of both passed against the probe build. The flag check also failed as it
should against the current tree, which has no flags, and against the probe with
`WARNINGS_AS_ERRORS=ON`, which has no `-Werror`. The consumer check failed as it should once
`target_compile_options(flywheel_dag INTERFACE -Wall)` was added.

### Release

`include/` changes (the reorder, and any header fix GCC calls for), so this is **v0.1.4**.
Consumers who build with `-Wall -Wextra -Wpedantic` stop seeing the engine's warnings. The API does
not change.

## Steps

Every commit subject is scoped to flywheel-dag#4 in the repo's `type(#N): …` form. The subjects
below omit the scope. Every checkpoint runs a full build, including `bench_hot_path`, and a full
ctest run, and both must pass.

### Step 1 — Fix what Clang finds

Make the ten fixes in the table above. The build does not change yet.

No new tests. The step changes only dead test code and the text of one initialiser list, and the
suites that exercise them must stay green. `StatefulNodeBase` is covered by `test_timeseries`,
`test_stateful_node_base` and the benchmark's `chain` row.

**Done when:**

- the probe build (flags added by hand, as above) prints no warnings;
- ctest is green: 22 of 22;
- `bench_hot_path --invariants` matches `benchmarks/expected_invariants.txt`.

Commit: `fix: fix what -Wall -Wextra -Wpedantic find under Clang`.

### Step 2 — Turn the flags on, and guard them

- Root `CMakeLists.txt`: `-Wall -Wextra -Wpedantic` go in the top-level block. There is no
  `-Werror` yet.
- Add `tests/check_warning_flags.cmake` and register `test_warning_flags`, without the `-Werror`
  half.
- Add the consumer checks to `tests/consumer/CMakeLists.txt`.

Tests: `test_warning_flags` is new, and `test_consumer_subproject` gains the checks. Each must be
seen to fail before it is trusted:

| Change made by hand, then reverted | Must fail |
|---|---|
| Delete the `add_compile_options(-Wall …)` line | `test_warning_flags` |
| Add `target_compile_options(flywheel_dag INTERFACE -Wall)` | `test_consumer_subproject` |
| Add `set(CMAKE_CXX_FLAGS "…" CACHE STRING "" FORCE)` in the root | `test_consumer_subproject` |

**Done when:**

- a clean local build, `bench_hot_path` included, prints no warnings;
- ctest is green: 23 of 23;
- each of the three hand-made changes above fails its test;
- CI is green on both legs;
- every warning in the Linux leg's log, and in the macOS leg's (its Apple Clang is older than the
  local one), is recorded in Progress.

Commit: `build: build the project's own targets with -Wall -Wextra -Wpedantic`.

### Step 3 — Fix what CI finds

Fix each warning Step 2 recorded, following the rule above. Header fixes are tested through the
suites that include them. If a fix changes behaviour rather than only text, it gets a test of its
own. If CI found nothing, this step is empty, and Progress says so.

**Done when:** both CI legs build with no warnings, ctest is green, and the benchmark gate passes.

Commit: `fix: fix what GCC finds under -Wall -Wextra -Wpedantic`, named for what was actually
found.

### Step 4 — Warnings are errors in CI

- Root `CMakeLists.txt`: add `FLYWHEEL_DAG_WARNINGS_AS_ERRORS`, which adds `-Werror`.
- `test_warning_flags` gains its `-Werror` half. It gets the option's value on its command line.
- `ci.yml`: the configure step adds `-DFLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON`, and its comment says
  why. Audit the workflow's actions for deprecations while the file is open.

Tests: `test_warning_flags` gains the `-Werror` check and the check that `-Werror` reaches no
dependency. Each must be seen to fail:

| Change made by hand, then reverted, with the option `ON` | Must fail |
|---|---|
| Configure without the `-Werror` line | `test_warning_flags` |
| Move the warnings block above the `FetchContent` calls | `test_warning_flags` (a dependency gets `-Werror`) |
| Add an unused variable to a test | the build |

**Done when:**

- local ctest is green with the option `ON` (23 of 23) and with it `OFF`;
- each of the three hand-made changes above fails as stated;
- CI is green on both legs with the option on, and no run shows a deprecation annotation.

Commit: `ci: make every compiler warning an error in CI`.

### Step 5 — Docs and release

- `README.md`, under Building and testing: the project's own targets build with
  `-Wall -Wextra -Wpedantic` on GCC and Clang, and CI makes every warning an error. None of this
  reaches a consumer.
- `CLAUDE.md`, under Build & Test: the build is warning-free, CI fails on any warning, and the rule
  for suppressing a confirmed false positive.
- Release v0.1.4. Set the project version and the `FetchContent` snippets in `CMakeLists.txt` and
  the README, then tag and push `v0.1.4`.
- Mark this plan done. Close flywheel-dag#4 with a summary comment.

**Done when:** build and ctest are green, CI is green on the release commit, the tag is pushed and
the issue is closed.

Commits: `docs: say what the build warns about`, `build: release v0.1.4` and
`docs: mark the plan done`.

## Self-review — risks and assumptions

- **GCC is unverified until Step 2's CI run.** The predictions above could be wrong in either
  direction. The rule for fixes and suppressions decides what happens. Anything that needs a
  build-wide suppression comes back for approval.
- **`ubuntu-latest` moves to Ubuntu 26 from 2026-10-19**, according to the runner's notice. That
  brings a newer GCC and libstdc++. With `-Werror`, a warning the new compiler adds turns CI red
  with no change to the code. That is the gate working, and the warning is a finding to fix like
  any other. A macOS runner image that updates Apple Clang is the same risk.
- **A dependency bump can bring a warning into this project's own translation units**, through a
  header. The dependencies' include directories are not `SYSTEM`, because `FetchContent` targets
  are not imported. None warns today. If one does, CI fails. The fix would be to mark that
  dependency's includes `SYSTEM`, which is its own change.
- **Errors are for CI only.** A local build can collect warnings until CI rejects them. The local
  build still prints each one.
- **`test_warning_flags` reads `compile_commands.json`.** That format is documented, unlike the
  generators' internal files. Makefiles and Ninja write a `command` string for each entry. The
  coverage check catches a format or layout that yields nothing.
- **The flags do not change code generation.** The benchmark's committed invariants must stay
  byte-identical, and CI checks that they do.
- **MSVC gets no flags.** No CI leg uses it, so any flags for it would be untested.
- **Assumptions:** CI stays on `ubuntu-latest` (GCC) and `macos-latest` (Apple Clang), and the
  default build type stays `RelWithDebInfo`. GCC's optimiser-dependent warnings appear only with
  optimisation, so a `Debug` build could differ from what CI sees.

## Progress

| Step | Status | Notes |
|---|---|---|
| 1 — Fix what Clang finds | Done | All ten sites are fixed. A clean probe build (flags added by hand, Apple Clang 21, `bench_hot_path` included) prints no warnings, down from 145. ctest 22 / 22. `--invariants` matches the committed file |
| 2 — Turn the flags on, and guard them | Done locally | The rebuild recompiled all 24 of this project's translation units and none of the dependencies', and printed no warnings. ctest 23 / 23. `test_warning_flags` counts 24 of this project's translation units and 11 of the dependencies'. The three hand-made changes each failed their test with the expected message: "72 warning flags missing", "flywheel::dag passes compile options to its consumer: -Wall", and "changed its consumer's CMAKE_CXX_FLAGS from '' to '-Wall'". An unchanged copy passed both tests. CI run 36418143590 is green on both legs. The macOS leg (AppleClang 21.0.0) printed no warnings. The Linux leg (GCC 13.3) printed 249 from four sites, none of them in a dependency: `-Wdangling-reference` at `dag_timeseries.inl:58` (136) and `tests/test_dag.cpp:999` (1), as predicted, and `-Wmismatched-new-delete` at the counting `operator delete` in `tests/test_value_slot.cpp:43` (67) and `benchmarks/bench_hot_path.cpp:63` (45) |
| 3 — Fix what CI finds | Done locally | Both `-Wdangling-reference` sites now hold the pulled `ValuePtr` in a local, as `dag::ops` already does. `-Wmismatched-new-delete` is [GCC bug 103993](https://gcc.gnu.org/PR103993): its reproducer is this counting `operator new`/`delete` pair, and a GCC maintainer calls it a false positive. It fires only once GCC inlines the `free()` into a caller. So it is silenced at the two `operator delete` definitions only, for GCC 11 and later only, with a comment naming the bug. The Apple Clang build prints no warnings. ctest 23 / 23. `--invariants` is unchanged. CI run 36418929776 is green. Both legs compiled all 35 translation units, 24 of this project's and 11 of the dependencies', with no warnings, so the pragma holds on GCC 13.3 |
| 4 — Warnings are errors in CI | Done locally | ctest 23 / 23 with the option `ON`, and again with it `OFF`. Both builds recompiled all 24 of this project's translation units, with no warnings. With the option `ON`, `test_warning_flags` finds `-Werror` on all 24, and neither `-Werror` nor `-Wpedantic` on the 11 dependency translation units. The three hand-made changes each failed. Deleting the `-Werror` line gave "24 warning flags missing". Moving the whole block above the `FetchContent` calls gave "22 of this project's warning flags found on dependency translation units". An unused variable in `test_dag.cpp` gave "error: unused variable … [-Werror,-Wunused-variable]". A first attempt at the block move showed that moving the block could unregister the test instead of failing it: step 2 registered the test from a variable that the flags block itself set. The test is now registered wherever the root promises the flags (top level, GCC or Clang), and the move fails it. `actions/checkout@v6` stays: it runs on node24 like v7, v6.1.0 shipped the same day as v7.0.1, and no run shows a deprecation annotation |
| 5 — Docs and release | Not started | |
