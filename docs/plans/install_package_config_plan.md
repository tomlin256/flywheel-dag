# Install Rules and a `find_package(flywheel_dag)` Package Config

**Status: Approved (2026-09-29).**

Closes [flywheel-dag#2](https://github.com/tomlin256/flywheel-dag/issues/2).

## Problem

Today the only ways to consume flywheel-dag are FetchContent and `add_subdirectory`. The issue asks for
`install()` rules and a CMake package config, so that a consumer can
`find_package(flywheel_dag CONFIG REQUIRED)` and link `flywheel::dag` against an installed prefix.

`flywheel::dag` links `spdlog::spdlog` and `nlohmann_json::nlohmann_json`. The installed config
must therefore either `find_dependency()` both or vendor nlohmann/json's headers. Either way, it
must not break a consumer that provides them through FetchContent.

**Done when** a CI job installs the engine into a prefix and builds a consumer against it with
`find_package`.

## What a plain export runs into

A scratch prototype on `80a2d59` (CMake 4.2.3, Apple Clang 21) first tried the textbook export:
`install(TARGETS flywheel_dag EXPORT …)`, with the link line unchanged. Configure fails:

```
install(EXPORT "flywheel_dagTargets" ...) includes target "flywheel_dag"
which requires target "spdlog" that is not in any export set.
```

It fails the same way for `nlohmann_json`. At the top level the engine fetches both dependencies.
Their targets therefore belong to this build instead of being imported, and CMake exports a link to
such a target only if that target is installed and exported too. There are two ways out, and the
prototype built both:

| | Bundle the fetched dependencies | Install the engine alone |
|---|---|---|
| How | Turn on spdlog's `SPDLOG_INSTALL` and nlohmann/json's `JSON_Install` when the engine fetches them | Link the dependencies through `$<BUILD_INTERFACE:…>`, so the export does not name them. The package config links them after finding them |
| Prefix | 188 files: the engine, plus spdlog (headers, `libspdlog.a`, config, pkg-config) and nlohmann/json | 27 files: the engine's headers, its package config and the LICENSE file |
| Build before install | Yes, for `libspdlog.a` | No: every installed file exists once configure finishes |
| Side effects | spdlog's install rules `include(CPack)`, which adds 15 `CPACK_*` cache entries and a `package` target named for spdlog. An install into `/usr/local` overwrites any spdlog or nlohmann/json already there | None |
| A parent that provides its own spdlog and turns install on | CMake export error | Works |

**Decision (2026-09-28): install the engine alone.** The consumer brings the dependencies itself,
either as installed packages or through its own FetchContent before `find_package`.

Vendoring nlohmann/json is rejected. spdlog would still need `find_dependency()`, and the vendored
copy would be a second nlohmann/json in every consumer that has its own.

## Design

### The target

```cmake
add_library(flywheel_dag INTERFACE)
add_library(flywheel::dag ALIAS flywheel_dag)
target_include_directories(flywheel_dag INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(flywheel_dag INTERFACE
    $<BUILD_INTERFACE:spdlog::spdlog>
    $<BUILD_INTERFACE:nlohmann_json::nlohmann_json>)
target_compile_features(flywheel_dag INTERFACE cxx_std_17)
set_target_properties(flywheel_dag PROPERTIES EXPORT_NAME dag)
```

- In a build tree, `$<BUILD_INTERFACE:x>` is `x`, so a FetchContent or `add_subdirectory` consumer
  compiles and links exactly as before. `test_consumer_subproject` holds that.
- In the export, `$<BUILD_INTERFACE:x>` is empty. Without it, the include path would be a
  source-tree path, which CMake refuses to export, and the dependencies would hit the error above.
- `EXPORT_NAME dag`, under the `flywheel::` namespace, installs the target as `flywheel::dag`,
  the name consumers already link.

### The install rules

A new option, `FLYWHEEL_DAG_INSTALL` ("Generate the install rules and the package config"), is on
at the top level and off in a subproject, like the tests. So a subproject installs nothing into its
parent's prefix unless the parent asks. A parent that does ask, such as a superbuild, gets a
working install whatever its dependencies are, because the export names none of them.

```cmake
if(FLYWHEEL_DAG_INSTALL)
    include(GNUInstallDirs)
    include(CMakePackageConfigHelpers)
    set(_flywheel_dag_config_dir ${CMAKE_INSTALL_DATADIR}/cmake/flywheel_dag)

    install(TARGETS flywheel_dag EXPORT flywheel_dagTargets
            INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    install(DIRECTORY include/flywheel DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
            FILES_MATCHING PATTERN "*.hpp" PATTERN "*.inl")
    install(EXPORT flywheel_dagTargets NAMESPACE flywheel::
            DESTINATION ${_flywheel_dag_config_dir})
    configure_package_config_file(cmake/flywheel_dagConfig.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/flywheel_dagConfig.cmake
        INSTALL_DESTINATION ${_flywheel_dag_config_dir})
    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/flywheel_dagConfigVersion.cmake
        COMPATIBILITY SameMinorVersion ARCH_INDEPENDENT)
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/flywheel_dagConfig.cmake
                  ${CMAKE_CURRENT_BINARY_DIR}/flywheel_dagConfigVersion.cmake
            DESTINATION ${_flywheel_dag_config_dir})
    install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/flywheel_dag)
endif()
```

- **`share/cmake/flywheel_dag/`.** A header-only package does not depend on the architecture, and
  `find_package` searches `<prefix>/share/cmake/<name>*/`. nlohmann/json installs its config
  there too.
- **`FILES_MATCHING`.** Only `.hpp` and `.inl` files are installed, so an editor backup or a
  `.DS_Store` never is. `test_install` fails if a header with any other extension is added and
  therefore left out.
- **`SameMinorVersion`.** Before 1.0 a minor release may break the API, so a request for 0.1 accepts
  any 0.1.z and nothing else. At 1.0 the rule becomes `SameMajorVersion`. `ARCH_INDEPENDENT` stops the version file from rejecting
  a consumer whose pointer size differs from that of the machine that configured the engine. That
  difference cannot matter for headers.
- **The LICENSE file.** Every header's banner points to it, and the MIT License asks that the notice go
  with every copy.
- **Nothing to build.** Every installed file exists once configure finishes, so
  `cmake -B build && cmake --install build --prefix <dir>` is enough. Configure still fetches
  spdlog and nlohmann/json, because the build tree links them.
- `GNUInstallDirs` and `CMakePackageConfigHelpers` are included inside the block, so a subproject
  that does not install adds nothing to its parent's cache.

### The package config

`cmake/flywheel_dagConfig.cmake.in`:

```cmake
@PACKAGE_INIT@

include(CMakeFindDependencyMacro)

# A dependency the consumer already provides (through its own FetchContent, or
# an earlier find_package) is used as it is, so a build holds one copy of each.
if(NOT TARGET spdlog::spdlog)
    find_dependency(spdlog)
endif()
if(NOT TARGET nlohmann_json::nlohmann_json)
    find_dependency(nlohmann_json)
endif()

include("${CMAKE_CURRENT_LIST_DIR}/flywheel_dagTargets.cmake")

# The exported target names no dependency, because the engine's build links
# whichever copies it has. The package links the ones found above.
set_property(TARGET flywheel::dag PROPERTY INTERFACE_LINK_LIBRARIES
             spdlog::spdlog nlohmann_json::nlohmann_json)

check_required_components(flywheel_dag)
```

- **The guard keeps a FetchContent consumer working.** Without it, `find_dependency(spdlog)`
  looks for an installed spdlog package that such a consumer does not have, and
  `find_package(flywheel_dag)` fails. The prototype showed exactly that: "Could not find a package
  configuration file provided by "spdlog"". If the machine does have a package, the unguarded call
  finds a second copy instead.
- **A consumer provides a dependency before it calls `find_package(flywheel_dag)`.** The
  FetchContent path already has this rule: the engine uses a parent's target only if the target
  exists when the engine looks for it.
- **The config asks for no particular version of either dependency.** The engine is tested only
  with the pinned tags (spdlog 1.17.0 and nlohmann/json 3.12.0), so any minimum version would be a
  guess. The README states the tested versions.
- **`set_property` without `APPEND`** is safe if `find_package` runs twice in one directory. The
  second time, the targets file returns early and this line sets the same value again.

The prototype built and ran both package consumers described below against this config.

### Tests

The new tests are registered from `FLYWHEEL_DAG_INSTALL`, the option that promises install rules,
never from a variable the install block sets. flywheel-dag#4 found why that matters: a test
registered from a variable that the code it tests sets can be switched off by the very change it
should catch.

`tests/consumer/` becomes one downstream project, which three tests build and run:

| Test | How the consumer gets the engine | Where spdlog and nlohmann/json come from |
|---|---|---|
| `test_consumer_subproject` (exists) | FetchContent of this checkout | The engine fetches them |
| `test_consumer_package` (new) | `find_package(flywheel_dag)`, with `CMAKE_PREFIX_PATH` listing the engine's prefix and the dependencies' prefix | Installed packages, found by the config's `find_dependency()` |
| `test_consumer_package_own_dependencies` (new) | `find_package(flywheel_dag)`, with `CMAKE_PREFIX_PATH` listing the engine's prefix alone | The consumer's own FetchContent, declared before `find_package` |

- **`main.cpp` also uses both dependencies.** Today it includes only `dag.hpp`, which uses neither,
  so it would compile even if neither dependency could be found. It gains a save and a restore of
  an `EWMANode` through `JsonFileStateStore`, which is the engine code that uses both: nlohmann/json writes the
  snapshot and spdlog logs the save. The program exits non-zero unless the restored node computes
  the same value as the original. It gives the snapshot an absolute path under its working
  directory, because a bare filename trips flywheel-dag#9.
- **The package consumers search only the prefixes they are given.** They turn `CMAKE_FIND_USE_*`
  off for the system paths, the environment and the package registries, which leaves only
  `CMAKE_PREFIX_PATH`. A package installed on the machine therefore cannot stand in for the one the
  test put in its prefix. On this Mac, Homebrew's nlohmann-json would otherwise satisfy an
  unguarded `find_dependency()`.
- **The package consumers ask for the exact version.** They call
  `find_package(flywheel_dag <version> EXACT CONFIG REQUIRED)`, with the version passed in from
  `PROJECT_VERSION`, so the version file is checked too.
- **Every consumer checks** that `flywheel::dag` passes it no compile options and leaves its
  `CMAKE_CXX_FLAGS` alone, as the subproject consumer checks today. The subproject consumer also
  checks that `FLYWHEEL_DAG_INSTALL` is off, because a subproject must not install itself into its
  parent's prefix.

Two fixture tests (`FIXTURES_SETUP`) prepare the prefixes. Each one empties its prefix first, so a
file from an earlier run cannot satisfy a test:

- **`test_install`** runs `tests/check_install.cmake`, which runs `cmake --install` on this build
  into `<build>/tests/package/engine`. It fails unless the prefix holds every file under
  `include/flywheel/`, the three package config files and the LICENSE file, and nothing else.
- **`install_dependencies`** runs `tests/install_dependencies.cmake`. It configures, builds and
  installs spdlog and nlohmann/json into `<build>/tests/package/dependencies`, the way a package
  manager would provide them. It builds them from the sources this build already fetched, so it
  needs no network.

### How this meets the issue

CI's Test step runs ctest on both legs, and ctest now runs `test_install` and
`test_consumer_package`. Together they install the engine into a prefix and build a consumer
against it with `find_package`, which is the issue's criterion. Because the check is a ctest
test, the same command runs it locally, and `ci.yml` does not change.

### Release

`include/` does not change, and a FetchContent consumer compiles and links as before, so this is
**v0.1.5**, the first release that can be installed.

## Steps

Every commit subject is scoped to flywheel-dag#2 in the repo's `type(#N): …` form. The subjects
below leave the scope out. Every checkpoint runs a full build, including `bench_hot_path`, and a
full ctest run, and both must pass. Each step is pushed, and CI must be green on both legs before
the next step starts.

### Step 1 — Install the engine

- Root `CMakeLists.txt`: add `FLYWHEEL_DAG_INSTALL`, the `$<BUILD_INTERFACE:…>` include path and
  links, `EXPORT_NAME` and the install block.
- Add `cmake/flywheel_dagConfig.cmake.in`.
- Add `tests/check_install.cmake`, and register `test_install`.
- `tests/consumer/CMakeLists.txt`: the subproject consumer checks that `FLYWHEEL_DAG_INSTALL` is
  off.

Tests: `test_install` is new, and `test_consumer_subproject` gains one check. Each must be seen to
fail before it is trusted:

| Change made by hand, then reverted | Must fail |
|---|---|
| Add `include/flywheel/extra.h` | `test_install` |
| Make `FLYWHEEL_DAG_INSTALL` default to on in a subproject | `test_consumer_subproject` |

**Done when:**

- a configure with CI's flags (`-Werror=dev -Werror=deprecated -DFLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON`)
  prints no warnings;
- `cmake --install` of a tree that has been configured but not built installs exactly the 27
  files;
- ctest is green: 24 of 24;
- each hand-made change above fails its test;
- CI is green on both legs.

Commit: `build: install the engine and a find_package(flywheel_dag) config`.

### Step 2 — A consumer finds the package and its dependencies

- `tests/consumer/`: add the package mode, with `find_package` of the exact version and the
  search confined to the given prefixes. Add the `JsonFileStateStore` round trip to `main.cpp`.
- Add `tests/install_dependencies.cmake`, and register `install_dependencies`.
- Register `test_consumer_package`.

Tests: `install_dependencies` and `test_consumer_package` are new. `test_consumer_subproject`
runs the round trip too.

| Change made by hand, then reverted | Must fail |
|---|---|
| Misspell `find_dependency(spdlog)` in the config | `test_consumer_package` |
| Drop the config's `set_property(… INTERFACE_LINK_LIBRARIES …)` | `test_consumer_package` |
| Restore into a node with a different name, so it starts cold | every consumer test |

**Done when:** ctest is green (26 of 26), each hand-made change above fails as stated, and CI is
green on both legs, with `test_consumer_package` passing in each leg's log.

Commit: `test: build a consumer against the installed package`.

### Step 3 — A consumer that fetches its own dependencies

- `tests/consumer/`: with `CONSUMER_FETCHES_DEPENDENCIES`, the consumer declares spdlog and
  nlohmann/json and makes them available before `find_package`. It uses the sources this build
  already fetched.
- Register `test_consumer_package_own_dependencies`, against the engine's prefix alone.

| Change made by hand, then reverted | Must fail |
|---|---|
| Drop the `if(NOT TARGET …)` guards from the config | `test_consumer_package_own_dependencies` |
| Drop the config's `set_property(… INTERFACE_LINK_LIBRARIES …)` | `test_consumer_package_own_dependencies` |

**Done when:** ctest is green (27 of 27), each hand-made change above fails as stated, and CI is
green on both legs. That CI run meets the issue's criterion.

Commit: `test: build a consumer that fetches its own dependencies against the package`.

### Step 4 — Docs and release

- `README.md`:
  - Under "Quick start", a new section explains how to install the engine and find it with
    `find_package`. It gives the install commands, the consumer's `find_package` and
    `target_link_libraries` lines, and `CMAKE_PREFIX_PATH`.
  - It explains that the install holds the engine alone, and that the consumer provides spdlog and
    nlohmann/json, either as installed packages or through FetchContent before `find_package`. It
    names the tested versions.
  - Under "Building and testing", `FLYWHEEL_DAG_INSTALL` joins the options that are on only at the
    top level.
- `CLAUDE.md`, under Build & Test, gets the rules:
  - the export names no dependency, and the config links them;
  - a new dependency goes in three places: the root's FetchContent and `$<BUILD_INTERFACE:…>`
    link, the config's guarded `find_dependency()` and link, and the consumer test;
  - the version file uses `SameMinorVersion` until 1.0;
  - a new header needs nothing extra, as long as it is a `.hpp` or an `.inl`.
- Root `CMakeLists.txt`: the header comment gains the `find_package` form.
- Release v0.1.5:
  - set the project version and the FetchContent snippets in `CMakeLists.txt` and the README;
  - tag and push `v0.1.5`;
  - publish a GitHub release with notes, as for v0.1.4.
- Mark this plan done, and close flywheel-dag#2 with a summary comment.

**Done when:** build and ctest are green, CI is green on the release commit, the release is
published and the issue is closed.

Commits: `docs: say how to install the engine and find it`, `build: release v0.1.5` and
`docs: mark the plan done`.

## Self-review — risks and assumptions

- **The config links the dependencies, not the exported target.** This pattern is less common.
  Anything that reads `flywheel_dagTargets.cmake` without going through `find_package` sees a
  target with no dependencies. The config says why, `CLAUDE.md` gives the rule, and both package
  consumers fail if the link is removed.
- **Each dependency is named in three places.** They are the root's FetchContent and link, the
  config's `find_dependency()` and link, and the consumer test. If a new dependency is missing from
  the config, the package consumers fail only if `main.cpp` reaches code that uses it.
  `CLAUDE.md` lists the three places.
- **A consumer must provide its dependencies before it calls `find_package(flywheel_dag)`.** A
  dependency declared afterwards is too late, because the config has already looked for a package.
  It has either failed or found a second copy. The README says so, as it already does for the
  FetchContent path.
- **Untested dependency versions.** `find_dependency()` accepts whatever spdlog or nlohmann/json
  the consumer has, but the engine is tested only with the pinned tags, and the README names them.
  A future spdlog 2 could break the engine, and nothing would warn at configure time.
- **A generator expression shows in the properties.** A FetchContent consumer that reads
  `flywheel_dag`'s `INTERFACE_INCLUDE_DIRECTORIES` or `INTERFACE_LINK_LIBRARIES` directly now gets
  `$<BUILD_INTERFACE:…>` instead of a path or a target name. Compiling and linking do not change,
  and `test_consumer_subproject` holds that.
- **CI time.** `install_dependencies` and the own-dependencies consumer each build spdlog, as
  `test_consumer_subproject` already does. In run 36420431251 that test took 10 s on macOS and
  20 s on Ubuntu, so the two new builds add about 20–40 s per leg. The prototype built the
  dependencies' prefix in 6 s on this Mac.
- **Configure still needs the dependencies' sources**, even to install the engine alone, because
  the build tree links them. With CMake 3.24 or later, a packager can make FetchContent use
  installed copies with `FETCHCONTENT_TRY_FIND_PACKAGE_MODE=ALWAYS`. This plan neither documents
  nor tests that path. If it is wanted, it gets its own issue.
- **`ubuntu-latest` moves to Ubuntu 26 from 2026-10-19** (flywheel-dag#7). The new tests use CMake
  features that have been stable since 3.16.
- **Multi-config generators** (Xcode and Visual Studio) are passed `--config`, but no CI leg runs
  one.
- **A test runs `cmake --install` on the tree that ctest is testing.** It installs only files that
  configure writes, which the build does not rewrite, and each run empties its prefix first.
- **Assumptions:** the minimum CMake version stays 3.18. Everything this plan uses is older:
  `SameMinorVersion` (3.11), `ARCH_INDEPENDENT` (3.14), `cmake --install --prefix` (3.15) and
  `CMAKE_FIND_USE_*` (3.16). CI stays on `ubuntu-latest` and `macos-latest`.

## Progress

| Step | Status | Notes |
|---|---|---|
| 1 — Install the engine | Done locally | ctest 24 / 24, and `--invariants` is unchanged. `test_install` finds the engine alone: 27 files. A fresh configure with CI's flags prints no warnings. `cmake --install` of that tree, configured and never built, installs the same 27 files. Both hand-made changes failed their test. `include/flywheel/extra.h` gave "Not installed: include/flywheel/extra.h". An `ON` default gave "flywheel-dag turned on its install rules in its consumer". For that second check, the local consumer build directory had to be removed first: its cache keeps the option's first value. CI always configures fresh |
| 2 — A consumer finds the package and its dependencies | Not started | |
| 3 — A consumer that fetches its own dependencies | Not started | |
| 4 — Docs and release | Not started | |
