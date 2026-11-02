# One Build Tree Per Platform

**Status: Proposed (2026-11-01; revised 2026-11-02)** — awaiting approval. D1 to D9 are the places to edit. The
revision names the trees `build/<kind>-<os>-<arch>` where the first version had `build-<os>-<arch>` (D2), which
adds a kind to the name, and the two kinds that have presets, `release` and `debug` (D9).

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
(`release` or `debug`; the sanitizers are flywheel-dag#45's), and the operating system and architecture say
where: `build/release-macos-arm64`, `build/release-linux-arm64`, `build/debug-linux-x86_64`. Every place that
names the tree moves onto the new names, so that a Linux build can be made, and proven, on the machine the
change is written on.

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
   configure with an error that names the directory to use:
   - A build directory whose name has `macos` or `linux` among its `-`-separated words is a platform tree. It
     must be exactly `<kind>-<os>-<arch>` for the host the configure runs on: a kind of one lowercase word, then
     the host's OS and architecture in the canonical spelling, and nothing after. `release-linux-arm64` passes
     on an arm64 Linux host. `release-linux-x86_64` there does not, and neither do `release-linux` (no
     architecture), `linux-arm64` (no kind), `release-linux-aarch64` and `release-Linux-arm64` (not the
     canonical spelling) and `release-linux-arm64-lto` (a fourth word). Only the last component of the path is
     read, so the parent is `build/` by convention and not by rule. Other names (`build`, `scratch`,
     `build-vscode`) are not examined.
   - Every tree remembers the platform of its first configure, in an internal cache entry, and refuses a
     configure from another. That covers a free-form name, and two platforms that mount the checkout at the same
     path, which CMake's own path check (the error above) cannot see. A tree made before this change has no
     entry, and its next configure records one.

   It checks the top-level project only, so a consumer's tree is never examined. The error reads like this:

   ```text
   CMake Error at CMakeLists.txt:NN (message):
     flywheel-dag: the build directory "release-linux-x86_64" is for linux-x86_64, but this host is
     linux-arm64. A build tree belongs to one OS and one architecture: use release-linux-arm64
     (cmake --list-presets shows the presets for this host).
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

       { "name": "release", "hidden": true, "cacheVariables": { "CMAKE_BUILD_TYPE": "RelWithDebInfo" } },
       { "name": "debug",   "hidden": true, "cacheVariables": { "CMAKE_BUILD_TYPE": "Debug" } },

       { "name": "macos-arm64",  "hidden": true,
         "condition": { "type": "equals", "lhs": "${hostSystemName}", "rhs": "Darwin" } },
       { "name": "linux-arm64",  "hidden": true,
         "condition": { "type": "equals", "lhs": "${hostSystemName}", "rhs": "Linux" } },
       { "name": "linux-x86_64", "hidden": true,
         "condition": { "type": "equals", "lhs": "${hostSystemName}", "rhs": "Linux" } },

       { "name": "release-macos-arm64",  "inherits": ["base", "release", "macos-arm64"] },
       { "name": "debug-macos-arm64",    "inherits": ["base", "debug",   "macos-arm64"] },
       { "name": "release-linux-arm64",  "inherits": ["base", "release", "linux-arm64"] },
       { "name": "debug-linux-arm64",    "inherits": ["base", "debug",   "linux-arm64"] },
       { "name": "release-linux-x86_64", "inherits": ["base", "release", "linux-x86_64"] },
       { "name": "debug-linux-x86_64",   "inherits": ["base", "debug",   "linux-x86_64"] }
     ]
   }
   ```

   Each visible preset also has a build preset and a test preset of the same name, which inherit a hidden
   build (test) preset per host that carries the same condition. The test presets set `output.outputOnFailure`,
   `execution.timeout` 120 (what CI passes today) and `execution.noTestsAction` `error`. The commands are
   `cmake --preset release-macos-arm64`, `cmake --build --preset release-macos-arm64` and
   `ctest --preset release-macos-arm64`, and the same with `debug-…`, `linux-arm64` or `linux-x86_64`.
   `cmake --list-presets` shows the ones for the host's OS.

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
- A refused configure into an existing tree leaves its `CMakeCache.txt` byte-identical, mtime included, as long
  as the command line adds no new `-D`. A preset re-supplying `CMAKE_BUILD_TYPE` with the value the cache holds
  is not a new `-D` (2026-11-02, 4.2.3).
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
- The suite as a `Debug` build, on the Mac (2026-11-02; Apple Clang; `-Werror=dev -Werror=deprecated
  -DFLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON`; a tree outside the checkout): no CMake warning, no compiler warning,
  36 of 36 pass in 20 s. No CI leg and no default build had run the engine's tests with `assert()` on.
- `test_warning_flags` counts a translation unit that is inside the checkout and outside the tree as the
  project's own. Dependency sources borrowed from a directory inside the checkout therefore fail it (44 flags
  "missing" on spdlog and googletest in my first Debug run), so a borrowed set must come from outside.

## Decisions to confirm

My defaults, each one a place to edit before approving.

- **D1. Presets for the names, a guard for what a preset cannot say.** The tree's name is then written once, and
  the CLI, VS Code's CMake Tools and CI all read it. The alternative is to document
  `cmake -B build/release-macos-arm64` and rely on the guard alone: less to add, but the name would sit in five
  files and `--list-presets` would tell no one what exists. A wrapper script is code that needs its own tests
  and tells VS Code nothing.
- **D2. `build/<kind>-<os>-<arch>`, always.** One directory, `build/`, holds every tree, and each tree's name
  says how it is built and where. The kind is one lowercase word (D9 has the two that get presets). The OS is
  `macos` or `linux`, the architecture is `arm64` or `x86_64` (`aarch64` is spelled `arm64`, `amd64` is spelled
  `x86_64`), so a tree reads the same on every host. Presets exist for both kinds on the three hosts in use: the
  Mac, the arm64 container, and CI's and an emulated container's x86_64. Another host, or another kind, gets a
  preset when someone has one. The guard already accepts any lowercase kind, so the sanitizer trees
  flywheel-dag#45 will want (`build/asan-linux-arm64`) need nothing from this plan but the name.
- **D3. The guard in `CMakeLists.txt`, with both halves.** Without it the architecture in a name is a label that
  one wrong `--preset` turns into a lie: an arm64 tree called `release-linux-x86_64`, found out by a link error
  or a test that dies with `Exec format error`. A label nothing checks is a gap I would be leaving, so CMake
  checks it. The remembered platform is the second half because a free-form name such as `build` is not
  examined by the first. The cost is one regular expression on a directory name at configure time. The
  alternative is a convention and no guard.
- **D4. The presets name the tree and set the build type the kind names, and nothing else.** No warning flags.
  `FLYWHEEL_DAG_WARNINGS_AS_ERRORS` is off by default so that a newer compiler's new warning does not stop a
  contributor's build (`CMakeLists.txt` says so), and CI turns it on. A preset that set it would end that, and
  would reach anyone configuring from a checkout to install it. CI keeps its flags on its own command line, as
  now, and a strict local configure adds them:
  `cmake --preset release-macos-arm64 -DFLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON`.
- **D5. `.clangd` points at `build/release-macos-arm64`.** clangd runs on the editing host, which is the Mac
  here, and the compile commands differ between the kinds only in the optimisation flags. Another host edits the
  one line; the file says so.
- **D6. v0.3.0.** Before 1.0 this repo's rule is that a minor release may break: `find_package(flywheel_dag 0.2)`
  accepts any 0.2.z and nothing newer. This change moves every build command the repo documents, and refuses a
  directory name that used to be free (one with the word `macos` or `linux` in it that is not
  `<kind>-<os>-<arch>` for the host), so it is a minor release and not v0.2.8. It is not v1.0.0, which would end
  the pre-1.0 rule and promise an API this change says nothing about. No header changes, and no signature.
- **D7. The README's two consumer-facing snippets keep `cmake -B build`.** The install snippet and the
  quickstart are for someone with one platform, who needs no preset and may have CMake older than 3.21. The
  guard does not examine `build`. It is also the directory that holds the named trees, which does no harm: they
  are separate trees, and nothing reads across them. The contributor sections, "Building and testing" and
  "Benchmark", use the presets and explain the tree per platform.
- **D8. Pushes.** Each step's commit is pushed to `main` once it passes locally, as the earlier plans did, and
  CI's result on the pushed commit is part of that step's "done". The release step also pushes the tag and
  publishes the GitHub release. Approving the plan approves those pushes. Every push follows a dry run of the
  leak audit.
- **D9. `release` builds `RelWithDebInfo`, and `debug` builds `Debug`.** `release` is the project's own default
  build (`-O2 -g -DNDEBUG`), the one CI builds and `ci.yml` calls "the configuration consumers get", so moving CI
  onto the presets changes where it builds and nothing about what. CMake's `Release` (`-O3`, no symbols) is not
  a kind here; it would be a third one with a name of its own. `debug` is `-O0 -g` without `NDEBUG`, so the
  engine's `assert()`s run, which no CI leg does today. The Mac's suite is green as a Debug build (above), and
  Step 1 does the same on Linux before anything else is built on it. The alternative is kind names that are
  CMake's own, `release` for `Release` plus a `relwithdebinfo` kind, which is exact and longer, and CI would
  build `relwithdebinfo-…`.

## Steps

Every commit subject is scoped to flywheel-dag#46 in the repo's `type(#N): …` form. The subjects below omit the
scope.

### Step 0 — Approval

The plan is committed as Proposed so it can be read and edited. Nothing else lands until it is approved. A
comment on flywheel-dag#46 links the plan.

**Done when:** `git status` is clean on that commit and flywheel-dag#46 carries the link.

Commit: `docs: add plan for per-platform build trees` (the first version), then
`docs: name the trees build/<kind>-<os>-<arch> in the plan` (this revision).

### Step 1 — Start from green, on both systems

No commit. The old `build/` on the Mac goes first, after I list it: it is git-ignored, it is the Mac's
`RelWithDebInfo` tree from before this change, and `build/` is from now on the directory the trees live in. The
next configure fetches the three dependencies again.

The trees are then named by hand, since the presets do not exist yet, with the names the presets will give them
and the build type the kind will set. The commands are the ones `CLAUDE.md` gives. On the Mac, then in the
container from `/work/flywheel-dag` (`docker exec ubuntu-dev`; Ubuntu 24.04 on aarch64, GCC 13.3.0, Clang
18.1.3, CMake 3.28.3, ccache, 12 CPUs), one after the other and never at once:

```bash
cmake -B build/release-macos-arm64 -DCMAKE_BUILD_TYPE=RelWithDebInfo -Werror=dev -Werror=deprecated -DFLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON   # release-linux-arm64 in the container
cmake --build build/release-macos-arm64 --parallel 8
ctest --test-dir build/release-macos-arm64 --output-on-failure --timeout 120
cmake --build build/release-macos-arm64 --target bench_hot_path
./build/release-macos-arm64/bin/bench_hot_path --invariants | diff - benchmarks/expected_invariants.txt
```

and the same three lines for `debug-macos-arm64` (`debug-linux-arm64` in the container) with
`-DCMAKE_BUILD_TYPE=Debug`, without the benchmark.

**Done when:** on each system, for each kind, the configure prints no CMake warning, the build prints no compiler
warning and ctest passes in full (36 of 36 on the Mac; the Linux count is recorded), and for `release` the
invariants diff is empty. This is the suite's first run on Linux/aarch64 with GCC 13, and its first Debug run on
Linux. Anything else: stop and ask. A failure here is not this plan's to work around.

### Step 2 — The platform tag and the guard

New: `cmake/flywheel_dag_platform.cmake`, `tests/check_platform.cmake` and `tests/check_tree_name.cmake`; edited:
`CMakeLists.txt` and `tests/CMakeLists.txt`. ctest goes from 36 tests to 38.

The module has two functions, both pure, so that a test can feed them any host:

- `flywheel_dag_platform(<out> <system-name> <machine>)` sets `<out>` to the tag.
- `flywheel_dag_tree_problem(<out> <binary-dir> <platform> <recorded>)` sets `<out>` to empty, or to the error
  text for the first problem it finds. `<recorded>` is the platform the tree remembers, or empty on a first
  configure. The name is the last component of `<binary-dir>`.

`CMakeLists.txt` calls them before `project()`, for the top-level project only, with `CMAKE_HOST_SYSTEM_NAME` and
`cmake_host_system_information(QUERY OS_PLATFORM)`, stops with `FATAL_ERROR` on a problem, and otherwise records
the platform in the cache (`FLYWHEEL_DAG_TREE_PLATFORM`, `INTERNAL`).

**`test_platform_name`** (`check_platform.cmake`, run with `cmake -DSOURCE_DIR=… -P`) holds a fixed table, so it
does not depend on the host it runs on:

| Group | Cases |
|---|---|
| tag | `Darwin`/`arm64` is `macos-arm64`; `Linux`/`aarch64` is `linux-arm64`; `Linux`/`x86_64` and `Linux`/`AMD64` are `linux-x86_64`; `Darwin`/`x86_64` is `macos-x86_64`; `Windows`/`AMD64` is `windows-x86_64` |
| accepted | `release-linux-arm64`, `debug-linux-arm64` and `asan-linux-arm64` on `linux-arm64`; `build`, `scratch`, `build-vscode`, `build-linuxbrew` and `release_linux_x86_64` on any host; a tree that remembers its own platform; a first configure |
| refused, and the text names both platforms | `release-linux-x86_64` on `linux-arm64`; `release-macos-arm64` on `linux-arm64`; a tree that remembers `macos-arm64` on `linux-arm64` |
| refused, and the text names this host's platform | on `linux-arm64`: `release-linux-aarch64` and `release-Linux-arm64` (not the canonical spelling), `release-linux` (no architecture), `linux-arm64` (no kind), `Release-linux-arm64` (the kind is not a lowercase word), `release-linux-arm64-lto` (a fourth word), `asan-ubsan-linux-arm64` (a kind of two words) |

**`test_tree_name`** (`check_tree_name.cmake`) configures the real top-level project, with the tests, examples,
benchmarks and install rules off and the dependencies taken from the sources this build fetched, so it needs no
network and no compile. Under a scratch directory, with the host's own platform as the oracle and the other
architecture as the lie:

| Case | Expected |
|---|---|
| `release-<host platform>` | configures; the cache holds `FLYWHEEL_DAG_TREE_PLATFORM` set to the host's platform; configuring it again also succeeds |
| `build` | configures, and records the platform |
| a free-form tree seeded with `-DFLYWHEEL_DAG_TREE_PLATFORM=<the other platform>` | refused, and the output names both |
| `release-<host OS>-<other architecture>` | refused, the output names the tree, and the cache holds no `CMAKE_CXX_COMPILER` entry |
| `release-<host OS>` | refused |
| `<host platform>` (no kind) | refused |
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
| a fourth word after the architecture is accepted | table: `release-linux-arm64-lto` |
| a kind of two words is accepted | table: `asan-ubsan-linux-arm64` |
| a name for the wrong architecture is accepted | table: `release-linux-x86_64` |
| a name with no architecture is accepted | table: `release-linux` |
| a name with no kind is accepted | table: `linux-arm64`; configure: the tree named for the host platform |
| the operating system is looked for case-sensitively | table: `release-Linux-arm64` |
| the kind may be capitalised | table: `Release-linux-arm64` |
| the remembered platform is ignored | table: the tree that remembers `macos-arm64`; configure: the seeded tree |
| the wiring is removed from `CMakeLists.txt` | configure: the lying tree |
| the platform is never recorded | configure: the cache entry |
| the guard runs after `project()` | configure: the refused tree's compiler entry |
| the guard runs on a subproject too | configure: the consumer |
| the guard passes `CMAKE_HOST_SYSTEM_PROCESSOR`, which is empty before `project()` | configure: the truthful tree is refused |

**Done when:** both tests fail before the module exists and each change above fails as stated; ctest passes in
full on the Mac (38 of 38); `cmake --build build/release-macos-arm64` from Step 1 reconfigures through the guard
and rebuilds nothing; the CI run on the pushed commit is green on both legs, where the new tests run on x86_64
Linux and on arm64 macOS.

Commit: `build: refuse a build tree named for another platform`.

### Step 3 — The presets, and the test that pins them

New: `CMakePresets.json`, `tests/check_presets.cmake`, and a `test_presets` entry in `tests/CMakeLists.txt`
(guarded like `test_warning_flags`, for the top level and CMake 3.21 or later). ctest goes from 38 tests to 39.

`check_presets.cmake` runs as `cmake -DSOURCE_DIR=<repo> -DHOST_SYSTEM_NAME=<name> [-DPRESETS_DIR=<dir>] -P`.
It reads `<dir>/CMakePresets.json` with `string(JSON)` and fails, naming the rule, unless the following hold. Its
header holds the table of kinds (`release` is `RelWithDebInfo`, `debug` is `Debug`), so a new kind is a deliberate
edit to the test as well as to the file:

| Rule | Holds |
|---|---|
| R1 | `version` is at least 3 (the first with `condition`) |
| R2 | `base` is hidden, its `binaryDir` is `${sourceDir}/build/${presetName}`, and no other preset sets a `binaryDir` |
| R3 | each kind has a hidden configure preset that sets `CMAKE_BUILD_TYPE` to the table's value; each host (`macos-arm64`, `linux-arm64`, `linux-x86_64`, in the module's own spelling: `flywheel_dag_platform` returns the name for the OS's CMake name and that architecture) has a hidden configure preset whose `condition` is `${hostSystemName}` equals that OS's CMake name (`Darwin`, `Linux`) |
| R4 | the visible configure presets are exactly `<kind>-<host>` for each kind and each host, and each inherits `base`, its kind and its host |
| R5 | each has a build preset and a test preset of the same name, which name it as `configurePreset` and inherit a hidden preset for its host that carries the host's `condition` |
| R6 | each test preset resolves `execution.noTestsAction` to `error` |
| R7 | `cmake -S <dir> --list-presets=all` lists, for the configure, build and test presets, exactly the presets in the file that are for `HOST_SYSTEM_NAME`'s OS: one per kind on Darwin, two per kind on Linux, none elsewhere |

R7 is the end-to-end check that CMake agrees with R3's text.

The test is written first and shown red: run against a tree with no `CMakePresets.json`, it fails. Then the
presets are added. Each hand-made change below is made to a scratch copy and passed with `-DPRESETS_DIR`, and
must fail on the rule named:

| Change | Fails |
|---|---|
| no `CMakePresets.json` | the file is missing |
| `"version": 2` | R1 |
| `base`'s `binaryDir` is `${sourceDir}/build` | R2 |
| `base` loses `"hidden": true` | R2 |
| `release-linux-arm64` sets a `binaryDir` of its own | R2 |
| the `debug` kind sets `Release` | R3 |
| the `release` kind sets `Release` (the other reading of D9) | R3 |
| the `linux-arm64` host's `rhs` is `Darwin` | R3 |
| the `linux-arm64` host is renamed `linux-aarch64` | R3 |
| `debug-linux-arm64` loses its kind from `inherits` | R4 |
| the `debug-linux-x86_64` configure preset is deleted | R4 |
| the `release-linux-x86_64` build preset is deleted | R5 |
| the `release-macos-arm64` test preset inherits the `linux-arm64` host's | R5 |
| a test preset loses `noTestsAction` | R6 |
| the unmodified file, with `-DHOST_SYSTEM_NAME=Linux` on the Mac | R7 |

**Done when:** the test fails without the file and each change above fails as stated; ctest passes in full on
the Mac (39 of 39); for each kind, `cmake --preset <kind>-macos-arm64 -DFLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON` and
then `cmake --build --preset <kind>-macos-arm64` reuse the Step 1 tree and rebuild nothing, and
`ctest --preset <kind>-macos-arm64` passes in full; CI is green on both legs, which runs `test_presets` against
the runners' CMake.

Commit: `build: presets name one build tree per platform`.

### Step 4 — The real test: Linux, through the presets

No commit. In the container, from `/work/flywheel-dag`, with the Step 1 trees `build/release-linux-arm64` and
`build/debug-linux-arm64` removed first so the whole path runs, for each kind:
`cmake --preset <kind>-linux-arm64 -Werror=dev -Werror=deprecated -DFLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON`,
`cmake --build --preset <kind>-linux-arm64 --parallel 12` and `ctest --preset <kind>-linux-arm64`, then the
benchmark invariants for `release`. Then:

1. **The trees side by side.** `build/` now holds the Mac's two trees and the container's two. `shasum` of every
   `CMakeCache.txt` before and after the other platform's run is unchanged; a build of each tree after the
   other's run rebuilds nothing; `git status` is clean, so no tree is tracked.
2. **The architecture in the name, from an x86_64 host.** In a throwaway `--platform linux/amd64` container (an
   `ubuntu:24.04` image with `build-essential`, `cmake` and `git`, removed afterwards; `ubuntu-dev` is not
   touched), on the same checkout, which it sees at the same path the arm64 container does:
   - `cmake --preset release-linux-arm64` is refused by the guard, naming `linux-x86_64`, and the arm64 tree's
     `CMakeCache.txt` is byte-identical afterwards. CMake's own path check cannot refuse it, because the path is
     the same;
   - `cmake --preset release-linux-x86_64` configures (the dependency sources are copied from an arm64 tree's
     `_deps` to a directory outside the checkout, so it needs no network) and writes
     `build/release-linux-x86_64` beside `build/release-linux-arm64`;
   - `test_ring_buffer` builds in it and passes under `ctest --preset release-linux-x86_64 -R test_ring_buffer`,
     and `readelf -h` reports x86-64 for its binary and AArch64 for the arm64 tree's.
   The whole x86_64 suite stays CI's: here it would run emulated.
3. **A probe, by hand and not committed, of the reason for the issue.** AddressSanitizer with
   UndefinedBehaviorSanitizer over the full suite in `build/asan-linux-arm64`
   (`-DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer"`,
   so the fetched dependencies are instrumented too), and ThreadSanitizer in `build/tsan-linux-arm64`
   (`-fsanitize=thread`; with Clang 18 if GCC's runtime will not start). The kinds are `asan` and `tsan` because
   a kind is any lowercase word, and those are the names flywheel-dag#45's presets would give. The container's
   `vm.mmap_rnd_bits` is not readable from inside, and flywheel-dag#45 names it as the thing that stops
   ThreadSanitizer starting. A sanitizer finding is a bug outside this plan: one labelled issue per finding,
   reported, and this plan carries on. If ThreadSanitizer cannot start in the container, that is recorded with
   the reason, and the container's own repo is where it would be changed.

**Done when:** `ctest --preset release-linux-arm64` and `ctest --preset debug-linux-arm64` pass in full (39 of 39
each), the invariants diff is empty, the three checks in 1 hold, every line of 2 holds, and the ASan/UBSan and
TSan outcomes in 3 are recorded as a pass, as issues, or as "cannot start, and why".

### Step 5 — CI

`.github/workflows/ci.yml`: the matrix becomes `include` pairs `{ os: ubuntu-latest, preset: release-linux-x86_64 }`
and `{ os: macos-latest, preset: release-macos-arm64 }`. Configure is `cmake --preset ${{ matrix.preset }}`
followed by the flags it has now (`-Werror=dev -Werror=deprecated -DFLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON`) under the
same comment, whose last sentence now says that the build type is the preset's, `RelWithDebInfo`, the project's
own default and the configuration consumers get (D9); Build is `cmake --build --preset ${{ matrix.preset }}
--parallel 4`; Test is `ctest --preset ${{ matrix.preset }}` (the test preset carries `--output-on-failure
--timeout 120`); Benchmark builds the target through the preset and runs
`./build/${{ matrix.preset }}/bin/bench_hot_path`. The rule that the tree is `build/<preset>` is R2's, and the
guard makes a runner of another architecture fail at the Configure step, naming the preset to use.

It is its own commit because the workflow can be proven only by running it, and a red run should be the
workflow's and not the docs'.

Tests: none new; `test_presets` (R2) holds the name CI builds the path from. The workflow is parsed as YAML
before the push.

**Done when:** ctest passes in full locally (39 of 39) and CI is green on both legs, with the same Build, Test and
Benchmark steps as before (39 of 39 tests, invariants unchanged, the same build type).

Commit: `ci: configure, build and test through the presets`.

### Step 6 — Documentation, `.clangd` and the benchmark comments

- `.clangd`: `CompilationDatabase: build/release-macos-arm64`, with a comment that it is the editing host's tree
  (D5).
- `README.md`, "Building and testing": the three preset commands, the rule (a tree per kind, OS and
  architecture, `build/<kind>-<os>-<arch>`, and what `release` and `debug` build), why a tree cannot be shared
  (the error above) and what the guard refuses, how to build on a second platform from the same checkout, CI's
  flags and the strict local configure (D4), and that the presets need CMake 3.21. "Benchmark": the commands with
  `build/release-<os>-<arch>`.
- `CLAUDE.md`, "Build and test": the same commands, a bullet for the rule, the guard and their reason, the
  benchmark commands, and a sentence added to the `NDEBUG` bullet that `debug` is the build where `assert()`
  runs. The container's own commands stay out of the repo: they describe one machine.
- `benchmarks/CMakeLists.txt` and `benchmarks/bench_hot_path.cpp`: the usage comments name
  `build/release-<os>-<arch>`.

Tests: none new; prose, a config line and comments. The leftover check is
`git grep -n -E 'build/(bin|compile_commands|CMake|_deps)|(-B|--test-dir|--build|cd) build( |$)|Database: build$' -- . ':!docs/plans'`,
which finds the old forms (it lists 24 lines today) and must return only the README's install and quickstart
snippets (D7) and nothing else. `clangd --check=include/flywheel/dag.hpp` must report the compile command from
`build/release-macos-arm64/compile_commands.json`.

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
- **Other kinds**: CMake's `Release`, `MinSizeRel`, and the sanitizers. Each gets a preset, a line in the table
  in `check_presets.cmake` and a sentence in the README when someone needs it.
- **The older plans in `docs/plans/`** keep `build/`. They record what was run.
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
- **`debug` runs `assert()` over these tests for the first time on Linux.** The Mac is green (above). A failing
  assert, or a test too slow at `-O0` for its timeout, is a finding in the engine or the test and not in this
  plan; it stops Step 1 and goes to Rob. A timeout is not a reason to raise the number.
- **`release` is `RelWithDebInfo` (D9).** A reader who expects CMake's `Release` finds `-g` in the flags, so the
  README says what each kind builds. The choice keeps CI's compile exactly as it is; GCC 15's
  `-Wmaybe-uninitialized` and its neighbours depend on the optimisation level, and a move to `-O3` would be a
  change to what CI proves, to be made on purpose and not inside this one.
- **GCC 13 may warn where GCC 15 does not**, and `-Werror` turns that into a failed build. The repo's rule applies:
  fix the warning; silence one only as a confirmed compiler false positive, at that site, with a comment that
  names the bug.
- **The first Linux build is slow.** It reads and writes a bind mount between the Mac and the container's VM,
  and four tests build spdlog or the consumer again (their timeout is 600 s). ccache is on in the container by
  default, so a second build is fast. A timeout is a finding, not a reason to raise the number.
- **Step 1 deletes the Mac's old `build/`.** It is git-ignored build output, rebuilt in Step 1, and it held the
  Mac's copies of the three dependencies, which the next configure fetches again over the network. I list it
  before removing it, and nothing in it is tracked.
- **`build/` is both a place for trees and, after the README's quickstart, a tree.** The quickstart's
  `cmake -B build` makes a free-form tree in the directory that holds the named ones. They are separate trees and
  nothing reads across them, but `rm -rf build` removes every one.
- **The x86_64 proof is partial on purpose.** It shows the name, the guard and one binary, because the container
  is emulated; CI runs the whole suite on x86_64. If the emulated build is too slow, the binary is one object
  file, and the plan says so in the Outcome.
- **The guard changes what a top-level configure accepts.** A directory whose name has the word `macos` or
  `linux` in it and is not `<kind>-<os>-<arch>` for the host now fails. No such name exists upstream, which is why
  the release is a minor one. `build-linuxbrew` and `build_linux` are not examined: their words are `build` and
  `linuxbrew`, and `build_linux`, none of them `linux`. Running under Rosetta on an Apple Silicon Mac is a
  different platform (`x86_64`) from the native one, and the guard treats it as one.
- **The guard runs before `project()`, so a refused configure leaves a cache and no compiler detection.** It
  does not delete the directory it refuses. An existing tree's cache stays byte-identical (checked above).
- **A runner that changes architecture fails loudly.** If GitHub moves a `-latest` image to another CPU, the
  Configure step stops and names the preset to use. That is a CI edit, and the guard has made the reason plain.
- **The two systems share the sources.** Editing a file while the other system builds, or running both builds at
  once, makes a loaded machine time-sensitive. The steps run one system at a time.
- **The bind mount is case-insensitive** (APFS), so the Linux build here cannot catch a filename-case bug. CI
  stays the check for that.
- **Presets need CMake 3.21.** CMake 3.18.4 ignores them (checked), so the floor stays 3.18; CMake rejects a
  field it does not know, so a later edit that uses a newer field breaks older CMakes for `--preset` only.
- **VS Code's CMake Tools** reads the presets on open, so its own default of `build/` stops applying and it
  offers the presets. The build directory clash it caused earlier is the reason this is welcome, but it changes
  the editor's behaviour, and I cannot run the editor here to check it.
- **CI is proven only by pushing.** The runners' CMake is assumed to be 3.21 or later. A red Step 5 is fixed
  forward.
- **`.clangd` is wrong on another host** until its one line is edited (D5).
- **`cmake --preset release-macos-arm64` alone is not CI's configure** (D4). The docs say so, with the flags to
  add.
- **The proof depends on the running container as configured today.** The plan records its toolchain so that a
  later change to it is visible.
- **The sanitizer probe may find real bugs.** That is what it is for. Each one is an issue and none is fixed
  here.
- **The leak audit.** The plan, the docs and the commit bodies write `flywheel-dag#N`, never a bare issue number.

## Progress

| Step | Status | Notes |
|---|---|---|
| 0 — Approval | Proposed | revised 2026-11-02 for `build/<kind>-<os>-<arch>` |
| 1 — Start from green | | |
| 2 — Platform tag and guard | | |
| 3 — Presets and their test | | |
| 4 — Linux, through the presets | | |
| 5 — CI | | |
| 6 — Documentation | | |
| 7 — Release v0.3.0 | | |
| 8 — Close the plan | | |
