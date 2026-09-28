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
- **Arithmetic op nodes** (`dag::ops`) — each op its own type, so a later pass can attach
  closed-form derivatives.
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
  GIT_TAG        v0.1.2)
FetchContent_MakeAvailable(flywheel_dag)

target_link_libraries(my_app PRIVATE flywheel::dag)
```

The engine depends on [spdlog](https://github.com/gabime/spdlog) and
[nlohmann/json](https://github.com/nlohmann/json). It fetches them itself unless your project
already provides `spdlog::spdlog` and `nlohmann_json::nlohmann_json`. Its tests and examples build
only when flywheel-dag is the top-level project.

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
| `dag.hpp` | `dag` | `Input`, `ComputeNode`, `InPlaceComputeNode`, `TweakableComputeNode`, `ConditionNode`, `Graph`, equality policies, `NodeBase` |
| `dag_async.hpp` | `dag::async` | `AsyncInput`, `AsyncQueue`, `FeedRegistry`, `TickLoop` |
| `dag_engine.hpp` | `dag::async` | `Engine`, `CycleSeqLock` |
| `dag_compute_module.hpp` | `dag::async` | `IComputeModule` — self-contained subgraphs |
| `dag_timeseries.hpp` | `dag::ts` | `RollingStats`, `RollingSumNode`, `RollingMinMaxNode`, `EWMANode`, `EWMATickRateNode`, `DeltaNode`, `DelayNode`, `makeTimeDelayNode`, `ThresholdNode`, `ZScoreNode`, `OutlierGateNode`, `RateLimiterNode`, `DebounceCountNode`, `LatchedDebounceNode`, `WindowNode` |
| `dag_ops.hpp` | `dag::ops` | `SumNode`, `ProductNode`, `DiffNode`, `DivideNode`, `NegateNode`, `ExpNode`, `LnNode`, `PowerNode`, `SqrtNode` |
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

`FLYWHEEL_DAG_BUILD_TESTS` and `FLYWHEEL_DAG_BUILD_EXAMPLES` are on when flywheel-dag is the
top-level project and off when it is a subproject.

## Documentation

[`CLAUDE.md`](CLAUDE.md) is the engine's full contract: the invalidation protocol, what an equality
policy can and cannot gate, the rules for flushing and allocation, and state persistence. It is
written for contributors and coding agents alike.

## License

MIT — see [`LICENSE`](LICENSE).
