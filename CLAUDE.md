# CLAUDE.md — flywheel-dag

Header-only C++23 reactive DAG engine, all in `include/flywheel/` (README.md has the header map; `docs/plans/` has the reasoning behind each fix). The rules for building nodes and driving an engine, which the code cannot say in one place, are in `docs/engine_rules.md`, imported here and by any project that builds on the engine.

@docs/engine_rules.md

**Keeping this file short.** A plan's docs step updates the comment at the declaration it changes. Add a line to this file or to `docs/engine_rules.md` only for an invariant, a trap or a contract the code cannot carry; never a mechanism, a count or an issue narrative. `test_claude_md_budget` holds both files, and any `CLAUDE.md`, to a size (the budgets are in `tests/CMakeLists.txt`).

- No `.cpp` in the engine. Template bodies live in the paired `.inl`, which its `.hpp` includes last. Include the `.hpp`, never the `.inl`. A non-template function in a header is `inline`.
- Ops and AAD: the rules are in the comments at the top of `dag_aad.hpp`, in `dag_ops.hpp` (each op alias stays its own concrete type; they are unguarded, so IEEE inf and NaN propagate) and over the partials in `dag_ops.inl`. Do not "simplify" the cancellation-free partials (divide, asin/acos, atan2). Check a change to a partial locally with `-DCMAKE_CXX_FLAGS=-ffp-contract=off`: Apple Clang on arm64 fuses `1 − a·a`, which hides a regression that CI's Linux leg catches.
- Releasing: `docs/releasing.md`.

## Build and test

```bash
cmake --preset relwithdebinfo-macos-arm64        # -linux-arm64 or -linux-x86_64 on Linux
cmake --build --preset relwithdebinfo-macos-arm64
ctest --preset relwithdebinfo-macos-arm64
```

README.md ("Building and testing", "clang-tidy", "Benchmark") covers the trees, the presets, the kinds, the strict flags, clang-tidy and the benchmark. The rules it does not state:

- A build tree belongs to one OS and one architecture (`build/<kind>-<os>-<arch>`); the top-level `CMakeLists.txt` refuses a name that claims another platform and a tree first configured on another (`cmake/flywheel_dag_platform.cmake` has the rules and the reasons; `test_platform_name`, `test_tree_name`, `test_presets`; flywheel-dag#46). Name a variant after the architecture (`relwithdebinfo-linux-arm64-gcc13`), or give it a name that claims no platform (`build`, `scratch`). The presets set the tree and the build type and nothing else.
- `relwithdebinfo-…` builds with `-DNDEBUG`, so `assert()` never runs: validation that must survive throws (see the `alpha` checks in `EWMANode::make`).
- The build is warning-free and CI keeps it so (flywheel-dag#4): CI adds `-Werror=author -Werror=deprecated -DFLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON` (`test_cmake_diagnostics` holds the first two); use them locally, after the preset.
  - Fix a warning, never silence it. Two exceptions: a confirmed compiler false positive, silenced at that one site for that one compiler with a comment naming the bug (the counting `operator delete` in `benchmarks/bench_hot_path.cpp`); and GCC's `-Wtsan` at a standalone fence, which is true and costs nothing for a fence that orders atomics only: issue it through `dag::threadFence<Order>()` (`dag_thread_fence.hpp`, flywheel-dag#49), never `std::atomic_thread_fence` (`test_tsan_fences` fails on a bare one).
  - The flags are the top level's alone. Never put one on `flywheel_dag`'s `INTERFACE` or in the cache (`test_warning_flags`, `test_consumer_subproject`).
- Fix a clang-tidy finding (flywheel-dag#60). `NOLINT(<check>)` is for a tool that is wrong or cannot see an invariant, by name and with a comment that says which (`~TimeOnExit` in `dag_engine.inl`). The tool is one LLVM major, 23, and `clang_tidy_version` fails on any other; moving is one commit (`tests/CMakeLists.txt`, the install steps in `ci.yml`, what the new major reports).
- `benchmarks/bench_hot_path` is `EXCLUDE_FROM_ALL` and not a ctest test; time a change before and after, on the same machine. CI diffs `--invariants` against `benchmarks/expected_invariants.txt`: non-zero `allocs/cycle` is an engine regression (fix the engine, never regenerate the file to pass CI); a changed `callbacks` or `checksum` means the graph did different work (regenerate only when that is the point, and say why in the commit). The checksum is the same on every platform only because the target builds with `-ffp-contract=off` and the rows use nothing but `+ − × ÷` and `sqrt`: `exp`, `log` and `pow` differ between math libraries.
- The install (flywheel-dag#2) is the engine alone, on at the top level only (`FLYWHEEL_DAG_INSTALL`); spdlog and nlohmann/json are never installed (rationale in `CMakeLists.txt`). A new dependency goes in three places: the root's FetchContent block and `$<BUILD_INTERFACE:…>` link; the config's guarded `find_dependency()` and `set_property` link (`cmake/flywheel_dagConfig.cmake.in`); `tests/install_dependencies.cmake` and the consumer's `CONSUMER_FETCHES_DEPENDENCIES` block. A new `.hpp` or `.inl` needs nothing.

## Conventions

- Banner: copy it from any header. Every change is unit-tested, and the tests are deterministic (`Engine::step()` and bounded cycle counts, never sleeps or wall-clock timeouts); `tests/test_nodes.hpp` has always-dirty stand-ins for an application's clock-driven node (`AlwaysFiring`, `Tripwire`).
- A test that counts allocations links `tests/allocation_counter.cpp` and calls `allocation_counter::allocations_during()`; it never defines a global `operator new` or `operator delete` of its own, because a sanitizer supplies its own for every form a program leaves out and the suite then aborts under AddressSanitizer (flywheel-dag#47). `benchmarks/bench_hot_path.cpp` keeps a counter of its own (flywheel-dag#52).
- A non-trivial change gets a GitHub issue and a plan in `docs/plans/`, linked both ways. Cite issues as `flywheel-dag#N` or by full URL in code and docs, never a bare `#N`; commit subjects keep the `type(#N): …` form.
- A comment states what is true of the code now: no closed issue, no plan, no retold bug (`test_comment_refs` holds the first two). An open issue is cited only where the comment states the limitation it tracks, and its number goes in that test's `OPEN_ISSUES` (`tests/CMakeLists.txt`).
- This repo stands alone. Code, comments, tests, docs and commit messages describe the engine only: never name or describe a downstream application — its modules, types, figures, issues or plans — even as an example.
