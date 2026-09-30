# flywheel-dag

[![CI](https://github.com/tomlin256/flywheel-dag/actions/workflows/ci.yml/badge.svg)](https://github.com/tomlin256/flywheel-dag/actions/workflows/ci.yml)

A header-only C++17 reactive DAG computation engine.

Build a graph of typed nodes once, feed it values, and get a callback when an output changes.
Every node caches its value and recomputes only when something it depends on moved. Feed
threads post into thread-safe inputs; a single-threaded, event-driven engine drains them and
evaluates only the part of the graph they dirtied.

## Features

- **Typed nodes** — `ComputeNode<Out, Ins...>` over a functor, with cached values and pluggable
  equality policies that decide what counts as a change.
- **Per-node invalidation** — `Eager` (recompute whenever anything upstream fired) or `Lazy`
  (recompute only when an input's value actually changed).
- **Thread-safe sources** — `AsyncInput<T>` (latest value wins) and `AsyncQueue<T>` (every value,
  delivered as a per-cycle batch), drained by an `Engine` that sleeps until data arrives.
- **An allocation-free steady state** — value buffers are recycled, and `InPlaceComputeNode`
  covers outputs that own heap.
- **Incremental time-series nodes** (`dag::ts`) — rolling mean/stddev, EWMA, rolling min/max and
  sum, z-score, tick- and time-based delays, thresholds with hysteresis, debouncing and rate
  limiting.
- **Arithmetic and trigonometric op nodes** (`dag::ops`) — each op its own type, with
  closed-form partial derivatives.
- **Algorithmic differentiation** (`dag::aad`) — an output's derivative with respect to every
  input in one reverse sweep, or every output's derivative in one direction in one forward sweep,
  at the values the graph holds. A compute node written as a generic lambda is differentiated with
  dual numbers, and a gradient node delivers sensitivities through the engine like any other
  output.
- **Snapshot and restore** of stateful nodes, discovered by walking the graph; a JSON file store
  writes atomically.
- **Deterministic replay** of recorded sessions through an unmodified graph, with no threads or
  timers.
- **DOT/SVG export** of the graph, and composable modules for building it in pieces.

## Quick start

### Add it to a CMake project

```cmake
include(FetchContent)
FetchContent_Declare(flywheel_dag
  GIT_REPOSITORY https://github.com/tomlin256/flywheel-dag.git
  GIT_TAG        v0.1.8)
FetchContent_MakeAvailable(flywheel_dag)

target_link_libraries(my_app PRIVATE flywheel::dag)
```

The engine depends on [spdlog](https://github.com/gabime/spdlog) and
[nlohmann/json](https://github.com/nlohmann/json). It fetches them itself unless your project
already provides `spdlog::spdlog` and `nlohmann_json::nlohmann_json`. Its tests and examples build
only when flywheel-dag is the top-level project.

### Or install it and find it with `find_package`

From a checkout, configure and install. Nothing needs building first:

```bash
cmake -B build
cmake --install build --prefix /opt/flywheel-dag
```

The prefix holds the engine alone: its headers, a `find_package` config and the LICENSE file.
Configure your project with `-DCMAKE_PREFIX_PATH=/opt/flywheel-dag`, and find it:

```cmake
find_package(flywheel_dag 0.1 CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE flywheel::dag)
```

Your project provides spdlog and nlohmann/json, in one of two ways:

- **As installed packages.** The engine's config finds them with `find_package`.
- **Through your own FetchContent**, made available before `find_package(flywheel_dag)`. The
  config uses a `spdlog::spdlog` or `nlohmann_json::nlohmann_json` target that already exists,
  and looks for a package only when there is none, so your build holds one copy of each.

The engine is tested with spdlog 1.17.0 and nlohmann/json 3.12.0. Before 1.0 a minor release may
break the API, so `find_package(flywheel_dag 0.1)` accepts any 0.1.z release and nothing newer.

### A first graph

```cpp
#include <flywheel/dag.hpp>

using namespace dag;

auto a   = Input<double>::make("a", 2.0);
auto b   = Input<double>::make("b", 3.0);
auto sum = ComputeNode<double, double, double>::make(
    "sum", {a, b}, [](const double& x, const double& y) { return x + y; });

EvalContext ctx;
get_value<double>(sum->eval(ctx));   // 5
a->set(4.0);                         // marks sum dirty
get_value<double>(sum->eval(ctx));   // 7
```

### An engine with an async source

```cpp
#include <flywheel/dag_engine.hpp>
#include <flywheel/dag_timeseries.hpp>

using namespace dag;

async::Engine engine;

auto signal = async::AsyncInput<double>::make("signal", 0.0);
engine.addSource(signal);

auto z     = ts::ZScoreNode::make("signal.z", signal, 50);   // over a 50-sample window
auto alert = ts::ThresholdNode<double>::make("signal.alert", z, 3.0);

engine.addOutput<bool>(alert, [](const bool& on) { /* fires only when it changes */ });

// Any thread: signal->post(value);
engine.run();   // blocks; call engine.stop() from a callback or another thread
```

[`examples/quickstart.cpp`](examples/quickstart.cpp) builds a complete graph — a rolling z-score,
a threshold with hysteresis, a warm-up gate and a latched alert — and drives it deterministically
with `Engine::step()`:

```bash
cmake -B build && cmake --build build
./build/bin/quickstart
```

### Derivatives

`dag_aad.hpp` takes derivatives over the graph, at the values it holds, in either direction:

```cpp
#include <flywheel/dag_aad.hpp>
#include <flywheel/dag_ops.hpp>

using namespace dag;

auto x = Input<double>::make("x", 2.0);
auto y = Input<double>::make("y", 3.0);
auto f = ops::ProductNode<>::make("f", {x, ops::ExpNode<>::make("exp(y)", y)});   // x·eʸ

EvalContext ctx;
f->eval(ctx);                       // a pass reads evaluated values only

aad::adjoints(f, {x, y});           // reverse, one sweep: {eʸ, x·eʸ}
aad::tangents({f}, {{x, 1.0}});     // forward, one sweep: {eʸ}, the derivative along x
```

- The ops supply their partials in closed form. A `ComputeNode`'s lambda is opaque, so for a
  functor a pass can see into, use `aad::DifferentiableNode<N>`. Its functor is written once, as a
  generic lambda, and differentiated with dual numbers.
  [`examples/aad.cpp`](examples/aad.cpp) prices a call that way, and checks its sensitivities both
  ways against bumps.
- A pass evaluates nothing, advances no stateful node, and never reads the branch a `ConditionNode`
  did not take. Its root must already be evaluated, so run it in the root's output callback, or
  after evaluating the root.
- A node with inputs but no partials is a barrier: every `dag::ts` node, and every opaque
  `ComputeNode`. A derivative through one throws `std::domain_error` rather than answer 0.
- An output callback fires only when the output's value changes, and a gradient can change while
  the value does not: x·y is 6 at (2, 3) and at (3, 2). For an engine to deliver sensitivities,
  register an `aad::GradientNode`, whose value is the gradient:

  ```cpp
  auto grad = aad::GradientNode::make("grad", f, {x, y});
  engine.addOutput<std::vector<double>>(grad, [](const std::vector<double>& g) {
      // g[0] = ∂f/∂x, g[1] = ∂f/∂y
  });
  ```

  It recomputes whenever anything upstream of `f` fires, and records a tape each time, which costs
  about 20 evaluations of `f`.

## Concepts

- **Build nodes with `make()`**, never a constructor: `make()` wires the node into the graph once
  it is owned by a `shared_ptr`.
- **Change is a policy.** Each node compares its new value with its cached one through an
  equality policy — `TypedEqualityPolicy<T>` (the default, `operator==`), `EpsilonPolicy<T>`,
  `AlwaysChangedPolicy` or `PredicateEqualityPolicy`. Only a change notifies downstream.
- **Eager or Lazy, per node.** An `Eager` node recomputes whenever anything upstream fired — the
  right answer for any node whose output is not a pure function of its inputs' values, such as
  every stateful time-series node. A `Lazy` node recomputes only when an input's value changed.
- **Sources and the engine.** Feed threads `post()` into async sources. On each wake the engine
  flushes every source, evaluates the dirty outputs, and fires a callback only when an output's
  value changed.
- **State.** Stateful nodes implement `IStatefulNode`. `Engine::saveState()` and
  `restoreState()` find them by walking up from the registered outputs, so nothing needs
  registering twice.
- **Replay.** `ReplayCoordinator`, `ReplayClock`, `ReplayInput<T>` and `ReplayQueue<T>` play a
  recorded session through the same graph the live sources feed.

## Headers

All headers live under `include/flywheel/`; include the `.hpp`, never the `.inl`.

| Header | Namespace | Provides |
|---|---|---|
| `dag.hpp` | `dag` | `Input`, `ComputeNode`, `InPlaceComputeNode`, `TweakableComputeNode`, `ConditionNode`, `Graph`, equality policies, `NodeBase`, `aad::IDifferentiable` |
| `dag_async.hpp` | `dag::async` | `AsyncInput`, `AsyncQueue`, `FeedRegistry`, `TickLoop` |
| `dag_engine.hpp` | `dag::async` | `Engine`, `CycleSeqLock` |
| `dag_compute_module.hpp` | `dag::async` | `IComputeModule` — self-contained subgraphs |
| `dag_timeseries.hpp` | `dag::ts` | `RollingStats`, `RollingSumNode`, `RollingMinMaxNode`, `EWMANode`, `EWMATickRateNode`, `DeltaNode`, `DelayNode`, `makeTimeDelayNode`, `ThresholdNode`, `ZScoreNode`, `OutlierGateNode`, `RateLimiterNode`, `DebounceCountNode`, `LatchedDebounceNode`, `WindowNode` |
| `dag_ops.hpp` | `dag::ops` | `SumNode`, `ProductNode`, `DiffNode`, `DivideNode`, `NegateNode`, `ExpNode`, `LnNode`, `PowerNode`, `SqrtNode`, `SinNode`, `CosNode`, `TanNode`, `AsinNode`, `AcosNode`, `AtanNode`, `Atan2Node`, `Derivative` |
| `dag_aad.hpp` | `dag::aad` | `Tape`, `adjoints`, `tangents`, `DifferentiableNode`, `Dual`, `GradientNode` |
| `dag_state_store.hpp` | `dag` | `IStatefulNode`, `IStateStore`, `InMemoryStateStore`, `JsonFileStateStore` |
| `dag_memoize.hpp` | `dag` | `MemoizedComputeNode` |
| `dag_replay.hpp` | `dag::async` | `ReplayCoordinator`, `ReplayClock`, `ReplayInput`, `ReplayQueue` |
| `dag_traversal.hpp` | `dag::traversal` | `bfs_upstream`, `BfsRange` |
| `dag_graph.hpp` | `dag` | `GraphExporter` — DOT and SVG |
| `dag_window_status.hpp` | `dag::ts` | `WindowStatus`, `IWindowed` |
| `dag_ring_buffer.hpp` | `dag` | `RingBuffer` |

## Building and testing

Requires a C++17 compiler and CMake 3.18 or later. CI builds and tests on Linux (GCC) and macOS
(Apple Clang).

```bash
cmake -B build          # RelWithDebInfo unless you ask for another build type
cmake --build build
cd build && ctest
```

`FLYWHEEL_DAG_BUILD_TESTS`, `FLYWHEEL_DAG_BUILD_EXAMPLES`, `FLYWHEEL_DAG_BUILD_BENCHMARKS` and
`FLYWHEEL_DAG_INSTALL` are on when flywheel-dag is the top-level project and off when it is a
subproject. A project that pulls the engine in as a subproject, and wants it in its own install,
sets `FLYWHEEL_DAG_INSTALL` itself.

ctest also installs the engine into a prefix under the build directory. It then builds a small
consumer against that prefix with `find_package` twice. The first build finds spdlog and
nlohmann/json as installed packages, and the second fetches the consumer's own copies. The
consumer searches only the prefixes it is given, so a package already installed on the machine
cannot stand in for one of them.

The tests, the example and the benchmark compile the engine's headers with `-Wall -Wextra
-Wpedantic` on GCC and Clang. CI also sets `-DFLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON`, so any warning
fails the build. None of these flags reaches a consumer or a dependency. They are set only when
flywheel-dag is the top-level project, and never on `flywheel::dag`.

## Benchmark

[`benchmarks/bench_hot_path.cpp`](benchmarks/bench_hot_path.cpp) measures what one engine cycle
costs on three generic workloads:

| Row | Workload |
|---|---|
| `chain` | An `AsyncInput<double>` feeding 11 compute and time-series nodes, with 4 registered outputs |
| `idle-queues` | 32 `AsyncQueue`s, stepped with nothing posted |
| `ingest` | A heap-owning value, with one `post()` and one `flush()` per cycle |

It is built on demand and is not a ctest test:

```bash
cmake --build build --target bench_hot_path
./build/bin/bench_hot_path                # 200000 measured cycles per row
./build/bin/bench_hot_path --invariants   # the exact columns only
```

`ns/cycle` depends on the machine and its load, so compare it only with another run on the same
machine. The other columns are exact, and the same on every platform:

- `allocs/cycle` is 0 in every row;
- `callbacks` counts the output callbacks that fired;
- `checksum` is the sum of the values they received.

CI prints the timings and fails if `--invariants` differs from
[`benchmarks/expected_invariants.txt`](benchmarks/expected_invariants.txt).

## Documentation

[`CLAUDE.md`](CLAUDE.md) is the engine's full contract: the invalidation protocol, what an equality
policy can and cannot gate, the rules for flushing and allocation, and state persistence. It is
written for contributors and coding agents alike.

## License

MIT — see [`LICENSE`](LICENSE).
