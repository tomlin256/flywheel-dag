# Engine rules — for code that builds nodes and drives an engine

The contract is written beside the code, in the comment blocks of the headers (`dag.hpp`, `dag_async.hpp`, `dag_engine.hpp`, `dag_state_store.hpp`, `dag_ops.hpp`, `dag_timeseries.hpp`, `dag_aad.hpp`, `dag_reduce.hpp`): read the block before editing the code under it. This file holds only what the code cannot say in one place.

## Writing nodes

- Build with the static `make()`, never a constructor (`shared_from_this()` wiring). Prefer a `ComputeNode` lambda with captured state. Otherwise derive `dag::ts::StatefulNodeBase<Derived, Out, In, State>` for incremental time-series state, `dag::ops::OpNodeImpl<Derived>` for a stateless op that needs its own type, or `aad::DifferentiableNode<N>` for a functor a tape must differentiate. A node of your own derives `dag::NodeBase`, never `dag::INode`.
- `InvalidationMode` is fixed at construction: a `make()` argument on the compute-node classes, fixed in the base for `dag::ops` and `ConditionNode` (`Lazy`) and for `dag::ts`, `GradientNode` and `TangentNode` (`Eager`). Set it where the functor is written, not at the graph site: purity is a property of the functor.
- `Eager` (the default) recomputes whenever anything upstream fired. `Lazy` recomputes only when an input's value changed, and is right only for a pure function of the declared inputs' values. A functor that reads state it did not declare must be `Eager`: mutated captured state, `mean()` and `stddev()` read off a captured `RollingStats`, `IWindowed::windowStatusNode()`.
- An equality policy gates downstream work only at a source, at a registered output, and at an intermediate node with `Lazy` consumers. An intermediate node whose consumers are all `Eager` gains nothing from comparing: give it `AlwaysChangedPolicy`. A policy compares against the last value *published*, not the last one computed.
- `InPlaceComputeNode<Out, Ins...>` is for an `Out` that owns heap. Its functor is `void(Out& out, const Ins&...)` and `out` arrives holding the previous evaluation's value: overwrite everything it owns on every path, early returns included. A functor that appends without clearing grows without bound, and only a test that evaluates twice catches it.
- A node that pulls inputs calls `beginEval()` before its first pull, and `endEval()` in place of `markClean()` wherever its evaluation ends, skip paths included (flywheel-dag#18). A source, or a tweaked node returning its frozen value, pulls nothing and calls `markClean()`. Override `propagate()` or `dirty()` only for the three reasons listed in `dag.hpp`. `downstream_` is private: reach it through `notifyDownstream()`.
- A `dag::Reduction` (`dag_reduce.hpp`) calls a functor at values the graph does not hold. It calls a `Lazy` compute node and refuses an `Eager` one, so `Lazy` vouches for purity there too. A node of your own joins by implementing `IReducible`.
- No node reads a wall clock. Time enters as a DAG input (`makeTimeDelayNode`), which keeps replay deterministic.

## Threads and the engine

- Eval is single-threaded with no locks during traversal. Feed threads call `post()` (mutex-guarded); the eval thread drains.
- A source may be posted to after its `Engine` is destroyed: its wake hook holds the wake state the `Engine` shares with every hook, not the `Engine` (flywheel-dag#42). Nothing else on a destroyed `Engine` may be used.
- Only the engine calls `flush()` on a live source (via `IFlushable` and `FeedRegistry`). Do not call `AsyncQueue::flush()` directly outside a unit test that owns the queue: a redundant flush after a drain replaces the cached batch with `[]` and silently drops it (`post(t); flush(); engine.step()` loses `t`). The engine flushes each source once per cycle, however many registries hold it (flywheel-dag#37). A group of your own overrides `members()` only if its `flush()` is a loop over that list and the list only grows, since the engine then skips the group's `flush()`; a `FeedRegistry`-derived class that overrides `flush()` is flushed through it unless it overrides `members()` too. The rest is in the `FeedRegistry` block of `dag_async.hpp`.
- Register outputs, install modules and call `step()` or `run()` between cycles: `addOutput()`, `addFeedback()`, `install()`, `step()` and `run()` throw inside a cycle (an output callback, an `eval()`, a source's `flush()`). A callback asks a running engine for another cycle with `poke()`.
- Do not call `CycleSeqLock::readConsistent()` while holding a lock an output callback needs: it waits on a cycle that cannot finish.

## State persistence

`Engine::saveState()` and `restoreState()` persist every `IStatefulNode` reachable from a registered output (`discoverStatefulNodes()`: BFS over `inputs()`, deduped); there is no list to maintain. A stateful node no output reaches is silently not persisted, so wire it. `persistState()` returning `false` excludes a node, and the engine asks it on every call. The walk is kept until the next `addOutput()`, so a node's `inputs()` must not change once it is built. Node names key the JSON: unique and stable across restarts (no timestamps or addresses).

## Traps GCC rejects and Apple Clang accepts

- Hold an evaluated `ValuePtr` in a local before binding a reference into it (`const ValuePtr v = node->eval(ctx); const T& x = get_value<T>(v);`): `-Wdangling-reference`.
- Brace an `if` whose body is a gtest `EXPECT_`/`ASSERT_`: `-Wdangling-else`.
- Do not copy a `std::function` that holds an empty lambda (GCC 15 `-Wmaybe-uninitialized`, flywheel-dag#44): hand `setWakeHook()` and its like a function, or a lambda that captures something.
