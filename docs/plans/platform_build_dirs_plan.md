# One Build Tree Per Platform

**Status: Done (2026-11-02), released as v0.3.0.** The plan was approved as written: D1 to D9 stood, so the trees
are `build/<kind>-<os>-<arch>` with the kinds `relwithdebinfo`, `release` and `debug`, the guard refuses a name
that claims another platform and nothing else, and the change is in
[v0.3.0](https://github.com/tomlin256/flywheel-dag/releases/tag/v0.3.0). The Outcome section at the end records
what the steps found and where they differ from this text.

Closes [flywheel-dag#46](https://github.com/tomlin256/flywheel-dag/issues/46).

## Problem

One checkout is built on more than one platform on the same host: natively on macOS (arm64), and on Linux in a
container that mounts the checkout, either as arm64 (native speed here) or as x86_64 (emulated, and CI's
architecture). A CMake build tree belongs to one platform. It records the absolute path of its source, the
compiler and the target it builds for, and the flags. The Mac and the container see the checkout at different
paths (`/Users/rob/src/flywheel-dag`, `/work/flywheel-dag`). Everything in this repo names the one tree
`build/`, so a second platform has nowhere to build. The container against the Mac's tree (2026-11-01; the
tree was byte-identical afterwards):

```text
$ cd /work/flywheel-dag && cmake -B build
CMake Error: The current CMakeCache.txt directory /work/flywheel-dag/build/CMakeCache.txt is different than the directory /Users/rob/src/flywheel-dag/build where CMakeCache.txt was created. This may result in binaries being created in the wrong place. If you are not sure, reedit the CMakeCache.txt
CMake Error: The source "/work/flywheel-dag/CMakeLists.txt" does not match the source "/Users/rob/src/flywheel-dag/CMakeLists.txt" used to generate cache.  Re-run cmake with a different source directory.
```

So whatever only Linux shows waits for a push and a GitHub runner: a GCC warning that Apple Clang does not give
(flywheel-dag#44), libstdc++'s behaviour, and the AddressSanitizer and ThreadSanitizer runs that
flywheel-dag#45 asks CI to make. The fix is a tree of its own for each platform and for each way of building,
all under `build/` and named for what they are: `build/<kind>-<os>-<arch>`. The kind says how the tree is built
(a CMake build type in lower case, `relwithdebinfo`, `release` or `debug`; the sanitizers are flywheel-dag#45's),
and the operating system and architecture say where: `build/relwithdebinfo-macos-arm64`,
`build/release-linux-arm64`, `build/debug-linux-x86_64`. The tree that is in `build/` today stays where it is, and
the new trees nest inside it (Not in this plan). Every place that names the tree moves onto the new names, so
that a Linux build can be made, and proven, on the machine the change is written on.

Nothing in `CMakeLists.txt` or `tests/` names `build` (they use `CMAKE_BINARY_DIR`), and `.gitignore` already
ignores `build*/`, which covers `build/` and everything under it. What names it is `.clangd`,
`.github/workflows/ci.yml`, `README.md`, `CLAUDE.md` and the header comments of `benchmarks/CMakeLists.txt` and
`benchmarks/bench_hot_path.cpp`. A consumer that pulls the engine in with FetchContent or `find_package` builds
in its own tree and is not affected.

## The change

Four parts.

1. **A platform tag**, `cmake/flywheel_dag_platform.cmake` (new): `<os>-<arch>` from CMake's host system name
   and machine. `Darwin` is `macos` and `Linux` is `linux`; the machine is lower-cased, with `aarch64` spelled
   `arm64` and `amd64` spelled `x86_64`, so the Mac and a Linux container on the same chip both say `arm64`.

2. **A guard**, in the top-level `CMakeLists.txt` before `project()`. It has two halves, and either one stops the
   configure with an error that names the tree to use:
   - A directory whose name has the shape of a platform tree, an optional kind and then `macos` or `linux`
     (`release-linux-arm64`, `linux-x86_64`, `asan-macos`), must be `[<kind>-]<os>-<arch>` for the host the
     configure runs on, with anything after the architecture free (`relwithdebinfo-linux-arm64-gcc13`). Names are
     compared in lower case, since the bind mount is case-insensitive. Other names (`build`, `scratch`,
     `cmake-build-debug`, `release-linuxbrew`) are not examined. Only the last component of the path is read, so
     the parent is `build/` by convention and not by rule.
   - Every tree remembers the platform of its first configure, in an internal cache entry, and refuses a
     configure from another. That covers a free-form name, and two platforms that mount the checkout at the same
     path, which CMake's own path check (the error above) cannot see. A tree made before this change has no
     entry, and its next configure records one.

   It checks the top-level project only, so a consumer's tree is never examined. The error reads like this:

   ```text
   CMake Error at CMakeLists.txt:NN (message):
     flywheel-dag: the build directory "release-linux-x86_64" is for linux-x86_64, but this host is
     linux-arm64. A build tree belongs to one OS and one architecture: use "release-linux-arm64"
     under build/ (cmake --list-presets shows the presets for this host).
   ```

3. **Presets**, `CMakePresets.json` (new), the one file that spells out the trees. A hidden `base` preset sets
   the directory from the preset's own name, a hidden preset per kind sets the build type, a hidden preset per
   known host says which OS it is for, and each visible preset is one kind on one host:

   ```json
   {
     "version": 3,
     "cmakeMinimumRequired": { "major": 3, "minor": 21, "patch": 0 },
     "configurePresets": [
       { "name": "base", "hidden": true, "binaryDir": "${sourceDir}/build/${presetName}" },

       { "name": "relwithdebinfo", "hidden": true, "cacheVariables": { "CMAKE_BUILD_TYPE": "RelWithDebInfo" } },
       { "name": "release",        "hidden": true, "cacheVariables": { "CMAKE_BUILD_TYPE": "Release" } },
       { "name": "debug",          "hidden": true, "cacheVariables": { "CMAKE_BUILD_TYPE": "Debug" } },

       { "name": "macos-arm64",  "hidden": true,
         "condition": { "type": "equals", "lhs": "${hostSystemName}", "rhs": "Darwin" } },
       { "name": "linux-arm64",  "hidden": true,
         "condition": { "type": "equals", "lhs": "${hostSystemName}", "rhs": "Linux" } },
       { "name": "linux-x86_64", "hidden": true,
         "condition": { "type": "equals", "lhs": "${hostSystemName}", "rhs": "Linux" } },

       { "name": "relwithdebinfo-macos-arm64",  "inherits": ["base", "relwithdebinfo", "macos-arm64"] },
       { "name": "release-macos-arm64",         "inherits": ["base", "release",        "macos-arm64"] },
       { "name": "debug-macos-arm64",           "inherits": ["base", "debug",          "macos-arm64"] },
       { "name": "relwithdebinfo-linux-arm64",  "inherits": ["base", "relwithdebinfo", "linux-arm64"] },
       { "name": "release-linux-arm64",         "inherits": ["base", "release",        "linux-arm64"] },
       { "name": "debug-linux-arm64",           "inherits": ["base", "debug",          "linux-arm64"] },
       { "name": "relwithdebinfo-linux-x86_64", "inherits": ["base", "relwithdebinfo", "linux-x86_64"] },
       { "name": "release-linux-x86_64",        "inherits": ["base", "release",        "linux-x86_64"] },
       { "name": "debug-linux-x86_64",          "inherits": ["base", "debug",          "linux-x86_64"] }
     ]
   }
   ```

   Each visible preset also has a build preset and a test preset of the same name, which inherit a hidden build
   (test) preset per host that carries the same condition. The test presets set `output.outputOnFailure`,
   `execution.timeout` 120 (what CI passes today) and `execution.noTestsAction` `error`. The commands are
   `cmake --preset relwithdebinfo-macos-arm64`, `cmake --build --preset relwithdebinfo-macos-arm64` and
   `ctest --preset relwithdebinfo-macos-arm64`, and the same with `release-…`, `debug-…`, `linux-arm64` or
   `linux-x86_64`. `cmake --list-presets` shows the ones for the host's OS.

4. **Everything that names the tree** moves onto the presets: `.clangd`, the CI workflow, the build and
   benchmark sections of `README.md`, `CLAUDE.md`, and the two benchmark header comments. Then a release.

What I checked, in scratch projects on CMake 4.2.3 (macOS), 3.28.3 (the container) and 3.18.4 (Debian 11, the
project's floor):

- A preset cannot see the CPU. `${hostSystemName}` is the only host fact a condition can read, so every Linux
  host lists the presets of both Linux hosts, and the guard is what makes the wrong one an error.
- The host's architecture is not known at the top of `CMakeLists.txt`: `CMAKE_HOST_SYSTEM_PROCESSOR` is empty
  until `project()`. `cmake_host_system_information(QUERY OS_PLATFORM)` answers before it (`arm64` on the Mac,
  `aarch64` in the container), so the guard can refuse before any compiler is detected. A configure that stops
  there leaves a cache with no compiler entries; a completed one has three.
- A refused configure into an existing tree changes no value in its `CMakeCache.txt`. In a scratch project of one
  directory the file is byte-identical, mtime included, as long as the command line adds no new `-D`
  (2026-11-02, 4.2.3). **In this project it is not**, which Step 4 found: CMake rewrites the file after a refusal,
  resets the type and the help text of each variable the command line gives (`CMAKE_BUILD_TYPE` from a preset,
  `FLYWHEEL_DAG_WARNINGS_AS_ERRORS` when passed), and changes two of the generator's own entries. The next `cmake --build`
  of that tree regenerates it, restores the file byte for byte, and rebuilds nothing (Outcome).
- `${presetName}` in an inherited `binaryDir` expands to the inheriting preset's name, so a preset's tree is
  `build/<its name>` and two presets cannot share one. A preset that inherits `base`, a kind and a host gets that
  tree, the kind's `CMAKE_BUILD_TYPE` and the host's condition, and the hidden presets are not listed (2026-11-02,
  4.2.3 and 3.28.3).
- With a `condition` on every preset, `--list-presets=all` lists only the host OS's presets, and
  `cmake --preset release-linux-arm64` on the Mac stops with `Could not use disabled preset`. A build or test
  preset for the other OS stops with `Cannot use disabled build preset` or `Cannot use disabled test preset`.
- `ctest` that finds no tests exits 0. `execution.noTestsAction: error` makes it exit 8, so a test preset that
  points at a tree with no tests cannot report a green run.
- CMake rejects a field it does not know (`"$comment"` is an error), so the JSON cannot carry its own
  rationale. It lives in this plan, in `CLAUDE.md` and in the header of `tests/check_presets.cmake`.
- CMake 3.18.4 configures, builds and runs ctest on a project that has a version 3 `CMakePresets.json`, and
  treats `--preset` as an unknown option. The presets are an addition; the project's floor stays 3.18.
- A tree nested in an older, configured tree configures, builds and tests on its own. Neither cache changes when
  the other tree is configured, built, cleaned or reconfigured, and the outer tree's `ctest` lists only its own
  tests (2026-11-02, a scratch project, 4.2.3 and 3.28.3).
- The suite built as `Debug` and as `Release` on the Mac (2026-11-02; Apple Clang; `-Werror=dev
  -Werror=deprecated -DFLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON`; trees outside the checkout): no CMake warning and no
  compiler warning in either, 36 of 36 tests pass in 20 s and in 24 s, and the `Release` build's
  `bench_hot_path --invariants` is identical to `benchmarks/expected_invariants.txt`. No CI leg and no default
  build runs these tests with `assert()` on, or at `-O3`.
- In the container (2026-11-02): GCC 13.3's ThreadSanitizer, and its AddressSanitizer with
  UndefinedBehaviorSanitizer, link and run a threaded program. Clang 18.1.3's sanitizer runtimes are not
  installed: both links fail for want of `libclang_rt.tsan_cxx-aarch64.a` and `libclang_rt.asan_cxx-aarch64.a`.
  `/proc/sys/vm/mmap_rnd_bits` is not readable as the container's user. It has 12 CPUs and 7.7 GiB of RAM.

## Decisions to confirm

My defaults, each one a place to edit before approving.

- **D1. Presets for the names, a guard for what a preset cannot say.** The tree's name is then written once, and
  the CLI, VS Code's CMake Tools and CI all read it. The alternative is to document
  `cmake -B build/relwithdebinfo-macos-arm64` and rely on the guard alone: less to add, but the name would sit in
  five files and `--list-presets` would tell no one what exists. A wrapper script is code that needs its own
  tests and tells VS Code nothing.
- **D2. `build/<kind>-<os>-<arch>`, always.** One directory, `build/`, holds every tree, and each tree's name
  says how it is built and where. The kind is one lowercase word. The three that get presets are CMake's build
  types (D9), and any other word, such as flywheel-dag#45's `asan` and `tsan`, is a hand-made tree that the guard
  accepts. The OS is `macos` or `linux`, the architecture is `arm64` or `x86_64` (`aarch64` is spelled `arm64`,
  `amd64` is spelled `x86_64`), so a tree reads the same on every host. Presets exist for each kind on the three
  hosts in use: the Mac, the arm64 container, and CI's and an emulated container's x86_64. Another host, or
  another kind, gets a preset when someone has one.
- **D3. The guard in `CMakeLists.txt`, with both halves, and it refuses a false claim, not a style.** Without it
  the architecture in a name is a label that one wrong `--preset` turns into a lie: an arm64 tree called
  `release-linux-x86_64`, found out by a link error or a test that dies with `Exec format error`. A label nothing
  checks is a gap I would be leaving, so CMake checks it. The remembered platform is the second half because a
  name the first does not examine, such as `build`, can still be configured from the wrong platform. The first
  half is deliberately loose about what is not a claim: the kind is optional and anything may follow the
  architecture, so a hand-made `relwithdebinfo-linux-arm64-clang18` (the container has both compilers) is a
  truthful name and passes. The stricter rule, exactly `<kind>-<os>-<arch>`, would refuse it and police style
  for no gain. The cost is two regular expressions on a directory name at configure time. The alternative is a
  convention and no guard.
- **D4. The presets name the tree and set the build type the kind names, and nothing else.** No warning flags.
  `FLYWHEEL_DAG_WARNINGS_AS_ERRORS` is off by default so that a newer compiler's new warning does not stop a
  contributor's build (`CMakeLists.txt` says so), and CI turns it on. A preset that set it would end that, and
  would reach anyone configuring from a checkout to install it. CI keeps its flags on its own command line, as
  now, and a strict local configure adds them:
  `cmake --preset relwithdebinfo-macos-arm64 -DFLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON`.
- **D5. `.clangd` points at `build/relwithdebinfo-macos-arm64`.** clangd runs on the editing host, which is the
  Mac here, and the compile commands differ between the kinds only in the optimisation flags. Another host edits
  the one line; the file says so.
- **D6. v0.3.0.** Before 1.0 this repo's rule is that a minor release may break: `find_package(flywheel_dag 0.2)`
  accepts any 0.2.z and nothing newer. This change moves every build command the repo documents, and refuses a
  directory name that used to be free (one that claims a platform that is not the host's), so it is a minor
  release and not v0.2.8. It is not v1.0.0, which would end the pre-1.0 rule and promise an API this change says
  nothing about. No header changes, and no signature.
- **D7. The README's two consumer-facing snippets keep `cmake -B build`.** The install snippet and the
  quickstart are for someone with one platform, who needs no preset and may have CMake older than 3.21. The
  guard does not examine `build`. It is also the directory that holds the named trees and, today, a tree of its
  own (Not in this plan); they are separate trees, and nothing reads across them (checked above). The
  contributor sections, "Building and testing" and "Benchmark", use the presets and explain the tree per
  platform.
- **D8. Pushes.** Each step's commit is pushed to `main` once it passes locally, as the earlier plans did, and
  CI's result on the pushed commit is part of that step's "done". The release step also pushes the tag and
  publishes the GitHub release. Approving the plan approves those pushes. Every push follows a dry run of the
  leak audit.
- **D9. The kinds are CMake's build types, in lower case.** `relwithdebinfo` is `RelWithDebInfo` (`-O2 -g
  -DNDEBUG`), `release` is `Release` (`-O3 -DNDEBUG`) and `debug` is `Debug` (`-g` and no optimisation flag, `assert()` on).
  `relwithdebinfo` is the project's own default and the build CI makes, which `ci.yml` calls "the configuration
  consumers get", so CI moves onto `relwithdebinfo-…` and compiles exactly what it does now. It is also the tree
  `.clangd` reads and the benchmark commands run. A word that is a CMake build type keeps its meaning wherever a
  tree is named, so `release` is never `RelWithDebInfo`. The Mac is green as `Release` and as `Debug` (above),
  and Step 1 proves Linux for all three before anything is built on it. The alternative is fewer kinds,
  `relwithdebinfo` and `debug`, because nothing here builds `Release`; nine presets become six.

## Steps

Every commit subject is scoped to flywheel-dag#46 in the repo's `type(#N): …` form. The subjects below omit the
scope.

### Step 0 — Approval

The plan is committed as Proposed so it can be read and edited. Nothing else lands until it is approved. A
comment on flywheel-dag#46 links the plan.

**Done when:** `git status` is clean on that commit and flywheel-dag#46 carries the link.

Commit: `docs: add plan for per-platform build trees`, and one commit for each revision of this file
(`git log -- docs/plans/platform_build_dirs_plan.md`).

### Step 1 — Start from green, on both systems

No commit, and nothing is deleted: the tree in `build/` today stays, and the named trees nest inside it. The
trees are named by hand, since the presets do not exist yet, with the names the presets will give them. The
commands are the ones `CLAUDE.md` gives. On the Mac, then in the container from `/work/flywheel-dag`
(`docker exec ubuntu-dev`; Ubuntu 24.04 on aarch64, GCC 13.3.0, Clang 18.1.3, CMake 3.28.3, ccache, 12 CPUs,
7.7 GiB of RAM), one after the other and never at once:

```bash
cmake -B build/relwithdebinfo-macos-arm64 -DCMAKE_BUILD_TYPE=RelWithDebInfo -Werror=dev -Werror=deprecated -DFLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON   # relwithdebinfo-linux-arm64 in the container
cmake --build build/relwithdebinfo-macos-arm64 --parallel 8                                                                                         # --parallel 8 in the container too
ctest --test-dir build/relwithdebinfo-macos-arm64 --output-on-failure --timeout 120
cmake --build build/relwithdebinfo-macos-arm64 --target bench_hot_path
./build/relwithdebinfo-macos-arm64/bin/bench_hot_path --invariants | diff - benchmarks/expected_invariants.txt
```

In the container only, the first three lines again for `release-linux-arm64` (`-DCMAKE_BUILD_TYPE=Release`) and
`debug-linux-arm64` (`-DCMAKE_BUILD_TYPE=Debug`). The Mac's runs of those two kinds are in "What I checked".
`--parallel 8` and not 12 in the container: twelve compilers on 7.7 GiB can have one OOM-killed, which reads as a
code failure. The number used and the wall time of each cold configure, build and ctest run are recorded.

**Done when:** on each system the configure prints no CMake warning, the build prints no compiler warning and
ctest passes in full for `relwithdebinfo` (36 of 36 on the Mac; the Linux count is recorded), with an empty
invariants diff; the old tree in `build/` is untouched, and the nested tree configures and builds on its own; in
the container `release` and `debug` pass in full too. This is the suite's first run on Linux/aarch64 with GCC 13,
and the first of `Release` and `Debug` on Linux. Anything else: stop and ask. A failure here is not this plan's to
work around.

### Step 2 — The platform tag and the guard

New: `cmake/flywheel_dag_platform.cmake`, `tests/check_platform.cmake` and `tests/check_tree_name.cmake`; edited:
`CMakeLists.txt` and `tests/CMakeLists.txt`. ctest goes from 36 tests to 38.

The module has two functions, both pure, so that a test can feed them any host:

- `flywheel_dag_platform(<out> <system-name> <machine>)` sets `<out>` to the tag.
- `flywheel_dag_tree_problem(<out> <name> <platform> <recorded>)` sets `<out>` to empty, or to the error text for
  the first problem it finds. `<name>` is the tree's own name, the last component of its path, which the caller
  takes with `get_filename_component`. `<recorded>` is the platform the tree remembers, or empty on a first
  configure.

`CMakeLists.txt` calls them before `project()`, for the top-level project only, with `CMAKE_HOST_SYSTEM_NAME` and
`cmake_host_system_information(QUERY OS_PLATFORM)`, stops with `FATAL_ERROR` on a problem, and otherwise records
the platform in the cache (`FLYWHEEL_DAG_TREE_PLATFORM`, `INTERNAL`).

**`test_platform_name`** (`check_platform.cmake`, run with `cmake -DSOURCE_DIR=… -P`) holds a fixed table, so it
does not depend on the host it runs on. Each name is a tree's own name, not a path:

| Group | Cases |
|---|---|
| tag | `Darwin`/`arm64` is `macos-arm64`; `Linux`/`aarch64` is `linux-arm64`; `Linux`/`x86_64` and `Linux`/`AMD64` are `linux-x86_64`; `Darwin`/`x86_64` is `macos-x86_64`; `Windows`/`AMD64` is `windows-x86_64` |
| accepted | `relwithdebinfo-linux-arm64`, `release-linux-arm64-gcc13` and `linux-arm64` on `linux-arm64`; `asan-macos-arm64` on `macos-arm64`; `build`, `scratch`, `vscode`, `build-vscode`, `cmake-build-debug`, `release-linuxbrew` on any host; a tree that remembers its own platform; a first configure |
| refused, and the text names both platforms | on `linux-arm64`: `release-linux-x86_64`; `linux-x86_64` (no kind); `debug-macos-arm64`; `release-linux-aarch64` (not the canonical spelling); `release-linux-arm64x` (the architecture is a prefix of a longer word); `Release-Linux-X86_64` (case); a tree that remembers `macos-arm64` |
| refused | on `linux-arm64`: `release-linux` (a kind and an OS, no architecture); `release-linux-asan` (no architecture); `linux` |

**`test_tree_name`** (`check_tree_name.cmake`) configures the real top-level project, with the tests, examples,
benchmarks and install rules off and the dependencies taken from the sources this build fetched, so it needs no
network and no compile. Under a scratch directory, in `<scratch>/build/<name>`, with the host's own platform as
the oracle and the other architecture as the lie:

| Case | Expected |
|---|---|
| `relwithdebinfo-<host platform>` | configures; the cache holds `FLYWHEEL_DAG_TREE_PLATFORM` set to the host's platform; configuring it again also succeeds |
| `build` | configures, and records the platform |
| `<host platform>` (no kind) | configures |
| a free-form tree seeded with `-DFLYWHEEL_DAG_TREE_PLATFORM=<the other platform>` | refused, and the output names both |
| `release-<host OS>-<other architecture>` | refused, the output names the tree, and the cache holds no `CMAKE_CXX_COMPILER` entry |
| `<host OS>-<other architecture>` (no kind) | refused |
| `release-<host OS>` | refused |
| `tests/consumer` configured as a top-level project in a tree named for the other architecture, taking the engine as a subproject | configures |

Both tests are written first and seen red: the table test fails with the module missing, and `test_tree_name`
fails because the lying configure succeeds. Then the module and the wiring are added. Each hand-made change below
is made to a scratch copy of the repo, which the tests reach through `-DSOURCE_DIR`, and must fail the test and
the case named:

| Change | Test: case |
|---|---|
| `aarch64` is not spelled `arm64` | table: `Linux`/`aarch64` |
| `amd64` is not spelled `x86_64` | table: `Linux`/`AMD64` |
| `Darwin` is not `macos` | table: both `Darwin` rows |
| a suffix after the architecture is refused | table: `release-linux-arm64-gcc13` |
| a kind in front is refused | table: `relwithdebinfo-linux-arm64` |
| the kind is required for a name to be examined | table: `linux-x86_64`; configure: the kind-less lie |
| the architecture is matched as a prefix, not a whole word | table: `release-linux-arm64x` |
| names are compared case-sensitively | table: `Release-Linux-X86_64` |
| a name for the wrong architecture is accepted | table: `release-linux-x86_64` |
| a name with no architecture is accepted | table: `release-linux` |
| the remembered platform is ignored | table: the tree that remembers `macos-arm64`; configure: the seeded tree |
| the wiring is removed from `CMakeLists.txt` | configure: the lying tree |
| the platform is never recorded | configure: the cache entry |
| the guard runs after `project()` | configure: the refused tree's compiler entry |
| the guard runs on a subproject too | configure: the consumer |
| the guard passes `CMAKE_HOST_SYSTEM_PROCESSOR`, which is empty before `project()` | configure: the truthful tree is refused |

**Done when:** both tests fail before the module exists and each change above fails as stated; ctest passes in
full on the Mac (38 of 38); `cmake --build build/relwithdebinfo-macos-arm64` from Step 1 reconfigures through the
guard and rebuilds nothing; the CI run on the pushed commit is green on both legs, where the new tests run on
x86_64 Linux and on arm64 macOS.

Commit: `build: refuse a build tree named for another platform`.

### Step 3 — The presets, and the test that pins them

New: `CMakePresets.json`, `tests/check_presets.cmake`, and a `test_presets` entry in `tests/CMakeLists.txt`
(guarded like `test_warning_flags`, for the top level and CMake 3.21 or later). ctest goes from 38 tests to 39.

`check_presets.cmake` runs as `cmake -DSOURCE_DIR=<repo> -DHOST_SYSTEM_NAME=<name> [-DPRESETS_DIR=<dir>] -P`.
It reads `<dir>/CMakePresets.json` with `string(JSON)` and fails, naming the rule, unless the following hold. Its
header holds the table of kinds, so a new kind is a deliberate edit to the test as well as to the file:

| Rule | Holds |
|---|---|
| R1 | `version` is at least 3 (the first with `condition`) |
| R2 | `base` is hidden and its `binaryDir` is `${sourceDir}/build/${presetName}` |
| R3 | there is one hidden host preset per known host, named `<os>-<arch>` in the module's own spelling (`flywheel_dag_platform` returns the name for the OS's CMake name and that architecture), whose `condition` is `${hostSystemName}` equal to that OS's CMake name (`Darwin`, `Linux`) |
| R4 | there is one hidden kind preset per kind, and it sets `CMAKE_BUILD_TYPE`: `relwithdebinfo` to `RelWithDebInfo`, `release` to `Release`, `debug` to `Debug` |
| R5 | the visible configure presets are exactly the pairs of a kind and a host, each named `<kind>-<os>-<arch>`, inheriting `base`, that kind and that host, and setting no `binaryDir` of its own; none appears twice |
| R6 | each has a build preset and a test preset of the same name, which name it as `configurePreset` and carry its host's condition; each test preset sets `execution.noTestsAction` to `error`, `execution.timeout` to 120 and `output.outputOnFailure` to true |
| R7 | `cmake -S <dir> --list-presets=all` lists, for the configure, build and test presets, exactly the presets in the file that are for `HOST_SYSTEM_NAME`'s OS: one per kind on Darwin, two per kind on Linux, none elsewhere |

R7 is the end-to-end check that CMake agrees with the file's text, and it is where a visible preset's inheritance
of its host's `condition` is tested in the repo, on every CMake the suite runs under.

The test is written first and shown red: run against a tree with no `CMakePresets.json`, it fails. Then the
presets are added. Each hand-made change below is made to a scratch copy and passed with `-DPRESETS_DIR`, and
must fail on the rule named:

| Change | Fails |
|---|---|
| no `CMakePresets.json` | the file is missing |
| `"version": 2` | R1 |
| `base`'s `binaryDir` is `${sourceDir}/build` | R2 |
| `base` loses `"hidden": true` | R2 |
| the `linux-arm64` host preset's `rhs` is `Darwin` | R3 |
| the `linux-arm64` host preset is renamed `linux-aarch64` | R3 |
| the `release` kind preset sets `Debug` | R4 |
| the `relwithdebinfo` kind preset sets `Release` | R4 |
| the `debug` kind preset loses its `CMAKE_BUILD_TYPE` | R4 |
| `release-linux-arm64` is renamed `release-linux-aarch64` | R5 |
| `debug-macos-arm64` loses its kind from `inherits` | R5 |
| `release-linux-arm64` sets a `binaryDir` of its own | R5 |
| the `debug-linux-x86_64` configure preset is deleted | R5 |
| the `release-linux-x86_64` build preset is deleted | R6 |
| the `release-macos-arm64` test preset's `rhs` is `Linux` | R6 |
| a test preset loses `noTestsAction` | R6 |
| a test preset's `timeout` is 60 | R6 |
| the unmodified file, with `-DHOST_SYSTEM_NAME=Linux` on the Mac | R7 |

**Done when:** the test fails without the file and each change above fails as stated; ctest passes in full on
the Mac (39 of 39); `cmake --preset relwithdebinfo-macos-arm64 -DFLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON` and then
`cmake --build --preset relwithdebinfo-macos-arm64` reuse the Step 1 tree and rebuild nothing; for each of
`release-macos-arm64` and `debug-macos-arm64`, the same two commands configure and build a tree whose cache holds
the kind's `CMAKE_BUILD_TYPE`, and `ctest --preset` passes in full on all three; CI is green on both legs, which
runs `test_presets` against the runners' CMake.

Commit: `build: presets name one build tree per kind and platform`.

### Step 4 — The real test: Linux, through the presets

No commit. In the container, from `/work/flywheel-dag`, with the Step 1 trees `build/relwithdebinfo-linux-arm64`,
`build/release-linux-arm64` and `build/debug-linux-arm64` removed first so the whole path runs, for each kind:
`cmake --preset <kind>-linux-arm64 -Werror=dev -Werror=deprecated -DFLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON`,
`cmake --build --preset <kind>-linux-arm64 --parallel 8` and `ctest --preset <kind>-linux-arm64`, then the
benchmark invariants for `relwithdebinfo`. Then:

1. **The trees side by side.** `build/` now holds the Mac's three trees and the container's three, beside the old
   tree. `shasum` of the Mac's and the container's `relwithdebinfo` `CMakeCache.txt` files before and after the
   other platform's run is unchanged; a build of each tree after the other's run rebuilds nothing; `git status`
   is clean, so no tree is tracked.
2. **The architecture in the name, from an x86_64 host.** In a throwaway `--platform linux/amd64` container (an
   `ubuntu:24.04` image with `build-essential`, `cmake` and `git`, removed afterwards; `ubuntu-dev` is not
   touched), on the same checkout, which it sees at the same path the arm64 container does:
   - `cmake --preset relwithdebinfo-linux-arm64` is refused by the guard, naming both platforms, and no value in
     the arm64 tree's `CMakeCache.txt` changes (the file is rewritten, and one `cmake --build` restores it byte
     for byte and rebuilds nothing; see the Outcome). CMake's own path check cannot refuse it, because the path
     is the same;
   - `cmake --preset relwithdebinfo-linux-x86_64` configures (it fetches its dependencies; the throwaway
     container has network) and writes `build/relwithdebinfo-linux-x86_64` beside
     `build/relwithdebinfo-linux-arm64`;
   - `test_ring_buffer` builds in it and passes under
     `ctest --preset relwithdebinfo-linux-x86_64 -R test_ring_buffer`, and `readelf -h` reports x86-64 for its
     binary and AArch64 for the arm64 tree's.
   The whole x86_64 suite stays CI's: here it would run emulated.
3. **A probe, by hand and not committed, of the reason for the issue.** AddressSanitizer with
   UndefinedBehaviorSanitizer over the full suite in `build/asan-linux-arm64`
   (`-DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"`,
   so the fetched dependencies are instrumented too), and ThreadSanitizer in `build/tsan-linux-arm64`
   (`-fsanitize=thread`). Both use GCC's runtimes, which start here (above) and are CI's compiler on Linux;
   Clang's are not installed. The kinds are `asan` and `tsan` because a kind is any lowercase word, and those are
   the names flywheel-dag#45's presets would give. A sanitizer finding is a bug outside this plan: one labelled
   issue per finding, reported, and this plan carries on. If a run cannot start, the reason is recorded.

**Done when:** `ctest --preset <kind>-linux-arm64` passes in full for all three kinds (Step 1's Linux count plus
3), the invariants diff is empty, the three checks in 1 hold, every line of 2 holds, and the ASan/UBSan and TSan
outcomes in 3 are recorded as a pass, as issues, or as "cannot start, and why".

### Step 5 — CI

`.github/workflows/ci.yml`: the matrix becomes `include` pairs
`{ os: ubuntu-latest, preset: relwithdebinfo-linux-x86_64 }` and `{ os: macos-latest, preset: relwithdebinfo-macos-arm64 }`.
Configure is `cmake --preset ${{ matrix.preset }}` followed by the flags it has now
(`-Werror=dev -Werror=deprecated -DFLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON`) under the same comment, whose last
sentence now says that the build type is the preset's, `RelWithDebInfo`, the project's own default and the
configuration consumers get (D9); Build is `cmake --build --preset ${{ matrix.preset }} --parallel 4`; Test is
`ctest --preset ${{ matrix.preset }}` (the test preset carries `--output-on-failure --timeout 120`); Benchmark
builds the target through the preset and runs `./build/${{ matrix.preset }}/bin/bench_hot_path`. The rule that the
tree is `build/<preset>` is R2's, and the guard makes a runner of another architecture fail at the Configure step,
naming the preset to use.

It is its own commit because the workflow can be proven only by running it, and a red run should be the
workflow's and not the docs'.

Tests: none new; `test_presets` (R2, R4, R5) holds the name CI builds the path from and that it is
`RelWithDebInfo`. The workflow is parsed as YAML before the push.

**Done when:** ctest passes in full locally (39 of 39) and CI is green on both legs, with the same Build, Test and
Benchmark steps as before (39 of 39 tests, invariants unchanged, the same build type).

Commit: `ci: configure, build and test through the presets`.

### Step 6 — Documentation, `.clangd` and the benchmark comments

- `.clangd`: `CompilationDatabase: build/relwithdebinfo-macos-arm64`, with a comment that it is the editing host's
  tree (D5).
- `README.md`, "Building and testing": the three preset commands, the rule (a tree per kind, OS and
  architecture, `build/<kind>-<os>-<arch>`, and what `relwithdebinfo`, `release` and `debug` build), why a tree
  cannot be shared (the error above) and what the guard refuses, how to build on a second platform from the same
  checkout, CI's flags and the strict local configure (D4), and that the presets need CMake 3.21. "Benchmark":
  the commands with `build/relwithdebinfo-<os>-<arch>`.
- `CLAUDE.md`, "Build and test": the same commands, a bullet for the rule, the guard and their reason, the
  benchmark commands, and a sentence added to the `NDEBUG` bullet that `debug-<os>-<arch>` is the build where
  `assert()` runs. The container's own commands stay out of the repo: they describe one machine.
- `benchmarks/CMakeLists.txt` and `benchmarks/bench_hot_path.cpp`: the usage comments name
  `build/relwithdebinfo-<os>-<arch>`.

Tests: none new; prose, a config line and comments. The leftover check is
`git grep -n -E 'build/(bin|compile_commands|CMake|_deps)|(-B|--test-dir|--build|cd) build( |$)|Database: build$' -- . ':!docs/plans'`,
which finds the old forms (it lists 24 lines today) and must return only the README's install and quickstart
snippets (D7) and nothing else. `clangd --check=include/flywheel/dag.hpp` must report the compile command from
`build/relwithdebinfo-macos-arm64/compile_commands.json`.

**Done when:** the leftover check returns only what D7 keeps, clangd resolves the Mac's database, ctest passes in
full (39 of 39) and CI is green on both legs.

Commit: `docs: name the per-platform build trees`.

### Step 7 — Release v0.3.0

As v0.2.7 was released: the project version and the FetchContent tag in `CMakeLists.txt` and the README, and
`find_package(flywheel_dag 0.3 …)` and the "any 0.3.z" sentences in both, the tag, and a GitHub release with
notes. The notes say what changed in one paragraph: the trees are per platform and kind, named
`build/<kind>-<os>-<arch>` by the presets, and a top-level configure refuses a directory named for another
platform. No header, signature or installed file changed. Before writing that, I run `git log v0.2.7..HEAD` and
`git diff v0.2.7..HEAD --stat -- include`, because a release carries every commit since the last tag.

Tests: the install and package tests read `PROJECT_VERSION`, so they check the new number; no new test.

**Done when:** ctest passes in full with no warning (39 of 39), the installed version file reports 0.3.0, CI is
green on the release commit on both legs, the tag and release are published, and flywheel-dag#46 is closed with a
summary comment.

Commit: `build: release v0.3.0`.

### Step 8 — Close the plan

Mark the plan done and fill in the Outcome.

Commit: `docs: mark the plan done`.

## Not in this plan

- **Sanitizer presets and CI jobs** are flywheel-dag#45. This plan makes `build/asan-linux-arm64` and
  `build/tsan-linux-arm64` possible and probes them once by hand; it adds no sanitizer preset, job or test.
- **Other kinds**: `MinSizeRel`, and the sanitizers. Each gets a preset, a line in the table in
  `check_presets.cmake` and a sentence in the README when someone needs it.
- **The older plans in `docs/plans/`** keep `build/`. They record what was run.
- **The tree in `build/` on this machine.** It is the contents of `build/` that are not a `<kind>-<os>-<arch>`
  directory: the Mac's `RelWithDebInfo` tree from before this change, with its `CMakeCache.txt` directly in
  `build/` (611 MB). It is ignored by git and cannot be moved, because its cache holds the Mac's absolute path,
  but it keeps working until it is deleted, since the guard does not examine `build`. The new trees nest inside it
  and share no file with it (checked above). I leave it; delete its files without the new directories, when
  convenient. Until then `cmake --build build` still builds the old tree, which is stale, and no document names it.
- **Hosts without a preset**: an Intel Mac, a Windows machine, another Linux architecture. The guard accepts a
  truthful `release-macos-x86_64` or `release-linux-riscv64`, and a free-form name needs nothing. A preset is
  three JSON objects when someone has the host.
- **Cross-compiling.** The guard names the host. A cross build uses a free-form name.
- **Anything outside this repo that names `build/`**, such as a global `.clangd` example, an editor's local
  settings, a consumer's own tree or its pinned tag.

## Self-review — risks and assumptions

- **Step 1 may find that the suite does not pass on Linux.** It has never run here on GCC 13 with libstdc++ 13 on
  aarch64, and CI runs GCC 15.2 on x86-64. A failure is a finding, not a snag: stop and ask. A pass proves a
  Linux build passes; it does not prove CI's exact compiler, so the CI run after each push stays the check.
- **`debug` runs `assert()` over these tests on Linux for the first time, and `release` runs them at `-O3`.** The
  Mac is green for both (above). A failing assert, or a test too slow at `-O0` for its timeout, is a finding in
  the engine or the test and not in this plan; it stops Step 1 and goes to Rob. A timeout is not a reason to
  raise the number.
- **No CI leg builds `release` or `debug`.** They are for local use, so a warning that only GCC 15 gives at `-O3`
  or at `-O0` would show only to someone who builds that kind with that compiler and
  `FLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON`. CI stays on `relwithdebinfo` so that what it proves does not change inside
  this plan (D9); covering the other kinds is a matrix edit with a cost of its own.
- **GCC 13 may warn where GCC 15 does not**, and `-Werror` turns that into a failed build. The repo's rule applies:
  fix the warning; silence one only as a confirmed compiler false positive, at that site, with a comment that
  names the bug.
- **Twelve parallel compilers on 7.7 GiB.** `--parallel 8` is a guess; if a compile dies with "Killed" the number
  goes down, and the number used is recorded in the Outcome.
- **The first Linux build is slow.** It reads and writes a bind mount between the Mac and the container's VM,
  and four tests build spdlog or the consumer again (their timeout is 600 s). ccache is on in the container by
  default, so a second build is fast. A timeout is a finding, not a reason to raise the number.
- **New trees nest inside the old one.** `build/` is today a tree with its own `CMakeCache.txt`, `CMakeFiles/`,
  `bin/` and `_deps/`. A tree in `build/relwithdebinfo-macos-arm64` shares nothing with it: a nested tree
  configures, builds and tests on its own, and neither cache changes when the other is built, cleaned or
  reconfigured (checked above, on 4.2.3 and 3.28.3), and Step 1 proves it on this project. The old tree stays
  buildable by a stale `cmake --build build`, which is the price of not deleting it first. `rm -rf build` would
  take the new trees with it.
- **The x86_64 proof is partial on purpose.** It shows the name, the guard and one binary, because the container
  is emulated; CI runs the whole suite on x86_64. If the emulated build is too slow, the binary is one object
  file, and the plan says so in the Outcome.
- **The guard changes what a top-level configure accepts.** A directory that claims a platform that is not the
  host's now fails. No such name exists upstream, which is why the release is a minor one. `build-linuxbrew` and
  `release-linuxbrew` are not examined, because `linuxbrew` is not `linux`. Running under Rosetta on an Apple
  Silicon Mac is a different platform (`x86_64`) from the native one, and the guard treats it as one.
- **The guard runs before `project()`, so a refused configure leaves a cache and no compiler detection.** It
  does not delete the directory it refuses. An existing tree keeps every value in its cache; CMake rewrites the
  file and the next build restores it (checked above, and in Step 4).
- **A runner that changes architecture fails loudly.** If GitHub moves a `-latest` image to another CPU, the
  Configure step stops and names the preset to use. That is a CI edit, and the guard has made the reason plain.
- **The two systems share the sources.** Editing a file while the other system builds, or running both builds at
  once, makes a loaded machine time-sensitive. The steps run one system at a time.
- **The bind mount is case-insensitive** (APFS), so the Linux build here cannot catch a filename-case bug. CI
  stays the check for that. It is also why the guard compares names in lower case.
- **Presets need CMake 3.21.** CMake 3.18.4 ignores them (checked), so the floor stays 3.18; CMake rejects a
  field it does not know, so a later edit that uses a newer field breaks older CMakes for `--preset` only.
- **VS Code's CMake Tools** reads the presets on open, so its own default of `build/` stops applying and it
  offers the presets. The build directory clash it caused earlier is the reason this is welcome, but it changes
  the editor's behaviour, and I cannot run the editor here to check it.
- **CI is proven only by pushing.** The runners' CMake is assumed to be 3.21 or later. A red Step 5 is fixed
  forward.
- **`.clangd` is wrong on another host** until its one line is edited (D5).
- **`cmake --preset relwithdebinfo-macos-arm64` alone is not CI's configure** (D4). The docs say so, with the
  flags to add.
- **The proof depends on the running container as configured today.** The plan records its toolchain so that a
  later change to it is visible.
- **The sanitizer probe may find real bugs.** That is what it is for. Each one is an issue and none is fixed
  here.
- **The leak audit.** The plan, the docs and the commit bodies write `flywheel-dag#N`, never a bare issue number.

## Progress

| Step | Status | Notes |
|---|---|---|
| 0 — Approval | Done | Approved as written on 2026-11-02. |
| 1 — Start from green | Done | 36 of 36 on the Mac and in the container, for `relwithdebinfo`; `release` and `debug` too in the container. |
| 2 — Platform tag and guard | Done | `777e43e`. ctest 38 of 38. CI green on both legs. |
| 3 — Presets and their test | Done | `ecdd86d`. ctest 39 of 39 for each kind. CI green on both legs. |
| 4 — Linux, through the presets | Done | No commit. 39 of 39 for each kind in the container. Three findings: flywheel-dag#47, flywheel-dag#48, flywheel-dag#49. |
| 5 — CI | Done | `c4289b2`. 39 of 39 through the presets on both legs. |
| 6 — Documentation | Done | `29f8597`. The leftover check returns the three README lines D7 keeps. |
| 7 — Release v0.3.0 | Done | `6ca804a`, tag `v0.3.0`, release published. |
| 8 — Close the plan | Done | The commit that adds the Outcome below. |

## Outcome

Released as [v0.3.0](https://github.com/tomlin256/flywheel-dag/releases/tag/v0.3.0), tag `v0.3.0` on `6ca804a`.

| Commit | Step | What | CI |
|---|---|---|---|
| `a710b09` | 0 | the approval, recorded | docs only |
| `777e43e` | 2 | the platform module, the guard and their two tests | [41426953335](https://github.com/tomlin256/flywheel-dag/actions/runs/41426953335) |
| `ecdd86d` | 3 | `CMakePresets.json` and `test_presets` | [41427735214](https://github.com/tomlin256/flywheel-dag/actions/runs/41427735214) |
| `c4289b2` | 5 | CI through the presets | [41430400829](https://github.com/tomlin256/flywheel-dag/actions/runs/41430400829) |
| `29f8597` | 6 | the documentation, `.clangd` and the benchmark comments | [41457555795](https://github.com/tomlin256/flywheel-dag/actions/runs/41457555795) |
| `6ca804a` | 7 | release v0.3.0 | [41458568953](https://github.com/tomlin256/flywheel-dag/actions/runs/41458568953) |

Every run is green on both legs: GCC 15.2.0 on `ubuntu-26.04` and Apple Clang 21 on macOS. ctest went from 36
tests to 38 to 39. Steps 1 and 4 have no commit.

**Step 1.** The baseline was green. The container is Ubuntu 24.04 on aarch64 with GCC 13.3.0, CMake 3.28.3, 12
CPUs and 7.7 GiB of RAM; it was the suite's first run on Linux/aarch64. Every tree: 36 of 36, no CMake warning,
no compiler warning, and for `relwithdebinfo` the invariants identical to the committed file. The old
`build/CMakeCache.txt` was unchanged by both. Wall times, cold except for the fetch of the dependencies:

| Tree | Configure | Build | ctest |
|---|---|---|---|
| Mac, `relwithdebinfo` | 23 s | 14 s | 30 s |
| container, `relwithdebinfo` | 24 s | 39 s | 52 s |
| container, `release` | 23 s | 29 s | 26 s |
| container, `debug` | 20 s | 18 s | 23 s |

The Mac's `Release` and `Debug` builds ran as probes before the approval, in trees outside the checkout: 36 of
36 in 24 s and 20 s, and the `Release` invariants matched. The first `Debug` probe failed `test_warning_flags`,
because I had handed it dependency sources inside the checkout, which that test counts as the project's own; the
rerun, with the sources outside, was green.

**Step 2, red first.** `test_platform_name` failed with the module missing. `test_tree_name` failed 8 of 9
cases without the guard: the lying configures succeeded and nothing was recorded. The ninth, the consumer, passed
and stayed the control that only a guard on a subproject can break. With the module and the wiring: 34 of 34
cases and 9 of 9. The first green run of `test_tree_name` failed 4 of 9 for a reason in the test: CMake wraps
the text of an error at about 80 columns, so a phrase split over two lines was not found; the test flattens
whitespace before it searches. All eighteen hand-made changes fail the case that names them, on the Mac. ctest
was 38 of 38 on the Mac and in the container, and `cmake --build` of the Step 1 tree reconfigured through the
guard and rebuilt nothing.

**Step 3.** `test_presets` failed with no file, then passed; all twenty hand-made changes fail the rule that
names them. Through `ctest --preset`: 39 of 39 for each kind on the Mac. The `relwithdebinfo` preset reused the
Step 1 tree and rebuilt nothing; `release` and `debug` configured in 24 s and 22 s and built in 13 s and 9 s,
without a warning. R1 to R7 also hold under CMake 3.28.3 as a Linux host.

**Step 4.** In the container, with the Step 1 trees removed first, through the presets: 39 of 39 for each of the
three kinds, no warning, `relwithdebinfo`'s invariants identical (configure 22 s, 45 s and 41 s; the builds took
3 s, 4 s and 8 s, on ccache hits).

- **Side by side.** `build/` held the Mac's three trees, the container's three and the old tree. The checksums of
  every `CMakeCache.txt` were the same before and after the other platform's run, in both directions; a build of
  each tree after the other's run recompiled nothing; `git status` stayed clean.
- **The x86_64 name.** In a throwaway `--platform linux/amd64` container, `cmake --preset
  relwithdebinfo-linux-arm64` was refused by the guard (`the build directory "relwithdebinfo-linux-arm64" is for
  linux-arm64, but this host is linux-x86_64`), and CMake's own path check could not have refused it. The x86_64
  preset configured in 50 s, built `test_ring_buffer` in 16 s with no warning, and the test passed. `readelf`
  reports x86-64 for that binary and AArch64 for the arm64 tree's.
- **The sanitizer probe.** The AddressSanitizer build was killed for memory at `--parallel 8` on 7.7 GiB ("Killed
  signal terminated program cc1plus", after 2 m 11 s) and finished at `--parallel 4` in 1 m 47 s. 36 of 39 tests
  pass. `test_value_slot` and `test_aad_node` abort with `alloc-dealloc-mismatch`
  ([flywheel-dag#47](https://github.com/tomlin256/flywheel-dag/issues/47)), and `test_dag_traversal` leaks a
  `shared_ptr` cycle ([flywheel-dag#48](https://github.com/tomlin256/flywheel-dag/issues/48)). Each passes once
  its own finding is switched off, and UndefinedBehaviorSanitizer reported nothing. ThreadSanitizer built in 52 s
  with 68 `-Wtsan` warnings, all from `CycleSeqLock`'s standalone fences, and passed 39 of 39 with no report
  ([flywheel-dag#49](https://github.com/tomlin256/flywheel-dag/issues/49)). GCC 13's runtimes start in the
  container; Clang 18's are not installed.

**Step 5.** The four steps of the workflow, run through `relwithdebinfo-macos-arm64` before the push, gave 39 of
39 and identical invariants. On CI, `cmake --preset`, `cmake --build --preset` and `ctest --preset` ran on both
legs, 39 of 39 each, building `build/relwithdebinfo-linux-x86_64` and `build/relwithdebinfo-macos-arm64`.

**Step 6.** The leftover check returned 24 lines before the commit and returns the three README lines D7 keeps
after it. `clangd --check=include/flywheel/dag.hpp` loads
`build/relwithdebinfo-macos-arm64/compile_commands.json`; it reports 51 errors ("Failed to resolve URI"), and so
it does against the old database, so they are not from this change.

**Step 7.** ctest 39 of 39 with no warning, the installed version file reports `0.3.0`, and
`git diff v0.2.7..HEAD --stat -- include` is empty.

**Where the work differs from the plan.**

- **A refused configure does not leave the cache byte-identical.** The plan said it did, from a scratch project
  of one directory. In this project CMake rewrites the file: the variables the command line gives
  (`CMAKE_BUILD_TYPE` from a preset, `FLYWHEEL_DAG_WARNINGS_AS_ERRORS`) lose their type and their help text, and
  two of the generator's own entries change (`CMAKE_NUMBER_OF_MAKEFILES`, and the `ADVANCED` flag of
  `CMAKE_CTEST_COMMAND` goes). No value changes. One
  `cmake --build` of that tree regenerated it, restored the file to the checksum it had before, recompiled
  nothing, and ctest passed. "What I checked" and Step 4 say so now.
- **The x86_64 container** was the amd64 build of ubuntu-dev's own image (GCC 13.3.0, CMake 3.28.3, git), not an
  `ubuntu:24.04` with packages installed. It fetched its dependencies over the network; the first revision's
  borrowed sources were dropped.
- **`debug` compiles with `-g` and no optimisation flag**, not `-O0 -g`. The compiler's default is `-O0`; D9
  and the README say what the flags are.
- **The sanitizer build needed `--parallel 4`**, as the self-review warned about memory.
- **Three issues, not in the plan**, one per finding: flywheel-dag#47, flywheel-dag#48 and flywheel-dag#49.
- The release carries two docs-only commits from before this plan (flywheel-dag#42 marked done, and the note on
  GCC 15's `std::function` warning in `CLAUDE.md`), and the notes say so.

**Left open.** [flywheel-dag#45](https://github.com/tomlin256/flywheel-dag/issues/45), the sanitizer CI, which
needs flywheel-dag#47 and flywheel-dag#48 fixed and flywheel-dag#49 decided first. The old tree in `build/` (611
MB) stays, as planned. The by-hand probe trees `build/asan-linux-arm64` (2.8 GB) and `build/tsan-linux-arm64`
(1.4 GB) are git-ignored and can be deleted.
