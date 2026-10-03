# CLAUDE.md — flywheel-dag

Header-only C++17 reactive DAG engine, all in `include/flywheel/` (README.md has
the header map). The contract is written beside the code, in the comment blocks
of the header you are changing: `dag.hpp` (invalidation, equality policies, node
classes), `dag_async.hpp`/`.inl` (staging, flushing), `dag_engine.hpp`/`.inl`
(cycle, outputs), `dag_state_store.hpp` (persistence), `dag_ops.hpp`,
`dag_timeseries.hpp`, `dag_aad.hpp`. Read the block before editing the code under
it; `docs/plans/` has the reasoning behind each fix. This file holds only what
the code cannot say: put a new rule's rationale in the comment over the code, and
add a line here only when the code cannot carry it.

- No `.cpp` in the engine. Template bodies live in the paired `.inl`, which its
  `.hpp` includes last. Include the `.hpp`, never the `.inl`. A non-template
  function in a header is `inline`.

## Build and test

```bash
cmake -B build && cmake --build build
ctest --test-dir build
```

- The default build is `RelWithDebInfo` with `-DNDEBUG`, so `assert()` never
  runs. Validation that must survive throws (see the `alpha` checks in
  `EWMANode::make`).
- The build is warning-free and CI keeps it so (flywheel-dag#4). Tests, example
  and benchmark build with `-Wall -Wextra -Wpedantic` on GCC and Clang, and CI
  adds `-DFLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON`; use it locally.
  - Fix a warning, never silence it. The one exception is a confirmed compiler
    false positive: silence it at that one site, for that one compiler, with a
    comment naming the bug (the counting `operator delete` in
    `test_value_slot.cpp`).
  - Hold an evaluated `ValuePtr` in a local before binding a reference into it:
    `const ValuePtr v = node->eval(ctx); const T& x = get_value<T>(v);`. GCC's
    `-Wdangling-reference` rejects a reference into the temporary.
  - Brace an `if` whose body is a gtest `EXPECT_`/`ASSERT_`. GCC's
    `-Wdangling-else` rejects it unbraced; Apple Clang does not warn, so only
    CI's GCC leg sees it.
  - The flags are the top level's alone. Never put one on `flywheel_dag`'s
    `INTERFACE` or in the cache; `test_warning_flags` and
    `test_consumer_subproject` check it.
- `benchmarks/bench_hot_path` is `EXCLUDE_FROM_ALL` and not a ctest test. Time a
  change by running it before and after, on the same machine:
  `cmake --build build --target bench_hot_path && ./build/bin/bench_hot_path`.
  Its exact columns (`allocs/cycle`, `callbacks`, `checksum`) show a speed-up did
  not come from doing less work. CI diffs `--invariants` against
  `benchmarks/expected_invariants.txt`:
  - Non-zero `allocs/cycle` is an engine regression. Fix the engine; never
    regenerate the file to pass CI.
  - A changed `callbacks` or `checksum` means the graph did different work.
    Regenerate (`./build/bin/bench_hot_path --invariants >
    benchmarks/expected_invariants.txt`) only when that is the point, and say why
    in the commit.
  - The checksum is platform-independent only because the target builds with
    `-ffp-contract=off` and the rows use nothing but `+ − × ÷` and `sqrt`. `exp`,
    `log` and `pow` differ between math libraries.
- The install (flywheel-dag#2) is the engine alone, on at the top level only
  (`FLYWHEEL_DAG_INSTALL`). The export names no dependency, and spdlog and
  nlohmann/json are never installed (rationale in `CMakeLists.txt`). A new
  dependency goes in three places: the root's FetchContent block and its
  `$<BUILD_INTERFACE:…>` link; the config's guarded `find_dependency()` and
  `set_property` link (`cmake/flywheel_dagConfig.cmake.in`); and
  `tests/install_dependencies.cmake` with the consumer's
  `CONSUMER_FETCHES_DEPENDENCIES` block. A new `.hpp` or `.inl` needs nothing.

## Writing nodes

- Build with the static `make()`, never a constructor (`shared_from_this()`
  wiring). Prefer a `ComputeNode` lambda with captured state. Otherwise derive
  `dag::ts::StatefulNodeBase<Derived, Out, In, State>` (CRTP hooks `doCompute`,
  `doSaveState`, `doRestoreState`) for incremental time-series state,
  `dag::ops::OpNodeImpl<Derived>` for a stateless op that needs its own type, or
  use `aad::DifferentiableNode<N>` for a functor a tape must differentiate. A
  node of your own derives `dag::NodeBase`, never `dag::INode`.
- `InvalidationMode` is fixed at construction. It is a `make()` argument on
  `ComputeNode`, `InPlaceComputeNode`, `TweakableComputeNode`,
  `MemoizedComputeNode` and `aad::DifferentiableNode`, and fixed in the base for
  `dag::ops` and `ConditionNode` (`Lazy`) and for `dag::ts`, `GradientNode` and
  `TangentNode` (`Eager`). Set it where the functor is written, not at the graph
  site: purity is a property of the functor.
- `Eager` (the default) recomputes whenever anything upstream fired. `Lazy`
  recomputes only when an input's value changed, and is right only for a pure
  function of the declared inputs' values. A functor that reads state it did not
  declare must be `Eager`: mutated captured state, `mean()` and `stddev()` read
  off a captured `RollingStats`, `IWindowed::windowStatusNode()`.
- An equality policy gates downstream work only at a source, at a registered
  output, and at an intermediate node with `Lazy` consumers. An intermediate node
  whose consumers are all `Eager` gains nothing from comparing: give it
  `AlwaysChangedPolicy`. A policy compares against the last value *published*,
  not the last one computed.
- `InPlaceComputeNode<Out, Ins...>` is for an `Out` that owns heap. Its functor
  is `void(Out& out, const Ins&...)`, and `out` arrives holding the previous
  evaluation's value: overwrite everything it owns on every path, early returns
  included. A functor that appends without clearing grows without bound, and
  only a test that evaluates twice catches it.
- A node that pulls inputs calls `beginEval()` before its first pull, and
  `endEval()` in place of `markClean()` wherever its evaluation ends, skip paths
  included (flywheel-dag#18). A source, or a tweaked node returning its frozen
  value, pulls nothing and calls `markClean()`. Override `propagate()` or
  `dirty()` only for the three reasons listed in `dag.hpp`. `downstream_` is
  private: reach it through `notifyDownstream()`.
- No node reads a wall clock. Time enters as a DAG input (`makeTimeDelayNode`),
  which keeps replay deterministic.

## Threads and the engine

- Eval is single-threaded with no locks during traversal. Feed threads call
  `post()` (mutex-guarded); the eval thread drains.
- Only the engine calls `flush()` on a live source (via `IFlushable` and
  `FeedRegistry`). Do not call `AsyncQueue::flush()` directly outside a unit test
  that owns the queue: it always refreshes the cached batch, so a redundant flush
  after a drain replaces the batch with `[]` and silently drops it
  (`post(t); flush(); engine.step()` loses `t`).
- Register outputs and install modules between cycles, never from an output
  callback or an `eval()` (flywheel-dag#24).
- Do not call `CycleSeqLock::readConsistent()` while holding a lock an output
  callback needs: it waits on a cycle that cannot finish.

## State persistence

`Engine::saveState()` and `restoreState()` persist every `IStatefulNode`
reachable from a registered output (`discoverStatefulNodes()`: BFS over
`inputs()`, deduped); there is no list to maintain. A stateful node no output
reaches is silently not persisted, so wire it. `persistState()` returning `false`
excludes a node. Node names key the JSON, so they must be unique and stable
across restarts (no timestamps or addresses).

## Ops and derivatives

- `dag::ops` nodes are aliases over `UnaryOpNode`, `BinaryOpNode` and
  `NAryOpNode<T, Op>`. Each alias stays its own concrete type, so
  `ops::Derivative<Op>` can supply closed-form partials. They are unguarded: IEEE
  inf and NaN propagate.
- Do not "simplify" the cancellation-free partials (divide, asin/acos, atan2).
  Apple Clang on arm64 fuses `1 − a·a` into one FMA, which hides an asin/acos
  regression that CI's Linux leg catches. Check a change to them locally with
  `-DCMAKE_CXX_FLAGS=-ffp-contract=off`.
- A tape evaluates nothing: run a pass where its roots are already clean (the
  root's output callback, or after evaluating it). Never make a tape evaluate a
  dirty root for its caller; `Engine::cycle()` snapshots its outputs' dirty flags
  first, so the callback would miss the change. A node with inputs and no
  `aad::IDifferentiable` partials is a barrier. The rest is at the top of
  `dag_aad.hpp`.

## Conventions

- Banner: copy it from any header (`flywheel-dag — A header-only C++17 reactive
  DAG computation engine`, `Copyright (c) 2026 Rob Tomlin`, MIT).
- Every change is unit-tested, deterministically: drive the engine with
  `Engine::step()` and bounded cycle counts, never sleeps or wall-clock timeouts.
  `tests/test_nodes.hpp` has always-dirty stand-ins for an application's
  clock-driven node (`AlwaysFiring`, `Tripwire`).
- A non-trivial change gets a GitHub issue and a plan in `docs/plans/`, linked
  both ways.
- Cite issues as `flywheel-dag#N` or by full URL in code and docs, never a bare
  `#N`. Commit subjects keep the `type(#N): …` form.
- This repo stands alone. Code, comments, tests, docs and commit messages
  describe the engine only: never name or describe a downstream application — its
  modules, types, figures, issues or plans — even as an example.
