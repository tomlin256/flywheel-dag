# Close Out the Benchmark Coverage Milestone

**Status: Approved (2026-11-14).** The plan was approved as written: D1 to D11 stood at their defaults. Everything under
"Measured" was taken read-only at `528e466`, before the approval, in scratch copies and by hand-made builds outside the
checkout. The Progress table at the end tracks the steps.

Closes [flywheel-dag#52](https://github.com/tomlin256/flywheel-dag/issues/52) and
[flywheel-dag#38](https://github.com/tomlin256/flywheel-dag/issues/38), the two issues of the
[Benchmark coverage milestone](https://github.com/tomlin256/flywheel-dag/milestone/3): "hot-path benchmark rows and
invariants for what no row prices today, and an allocation counter that is complete under sanitizers". One plan for the
two, because both change `benchmarks/bench_hot_path.cpp`, they share the gates, and the new rows should first be
committed under the instrument they will be read with.

| Issue | The change | What holds it afterwards |
|---|---|---|
| flywheel-dag#52 | `bench_hot_path` drops its three allocation functions and links `tests/allocation_counter.cpp`, which replaces all twenty | `test_allocation_functions`, new: no other file of the checkout defines one |
| flywheel-dag#38 | three rows, `ops-compute`, `ops-moving` and `ops-held`: one graph of op nodes, folded two ways and driven two ways | their lines in `benchmarks/expected_invariants.txt`, which CI diffs; `test_bench_invariants`, new; four unit tests, new, that an op node which skips does not call its op |

Four things turned out differently from how the issues read, and each is under "Measured":

- **The benchmark's figures move when it counts through the complete counter, and the engine has not changed.** Run as
  the benchmark runs them, its rows read between 1.3% less and 0.9% more; timed in rotation, as earlier plans swept,
  0.8% to 2.5% more, at every code alignment. The counter is not the cause: today's benchmark defines `operator new`
  in its own unit, so the compiler inlines it into the engine's allocation branches, and no application is built that
  way. D2 asks whether you accept the new figures.
- **"Slower at 16 of 16 alignments" is not always a cost.** Adding the three rows, which changes no function that
  `chain` runs, makes `chain` read 2.3% more in rotation, at 16 of 16.
- **No test fails when a unary or a binary op node never skips.** The suite pins what a skip leaves behind, never the
  skip. Step 3 pins it.
- **`callbacks=0` in the held row says that the op nodes skipped only if they publish whenever they recompute.** With
  the default policy the figure survives op nodes that never skip. D6.

## Measured

Apple M4 Pro, Apple clang 21.0.0, CMake 4.4.5, LLVM 23.1.4, tree `relwithdebinfo-macos-arm64`; `ubuntu-dev` (aarch64,
GCC 13.3.0), trees `relwithdebinfo-linux-arm64` and `asan-linux-arm64`; and GCC 15.2.0, CI's compiler on Linux, in a
throwaway `ubuntu:26.04` container. At `528e466` the strict configure and the build print no warning and
`bench_hot_path --invariants` is the committed file's, on the Mac and in `ubuntu-dev`; `ctest` passes 89 of 89 on the
Mac and 134 of 134 in the container, whose tree has the clang-tidy gate on. Every prototype was compiled by hand with
the flags the tree's `compile_commands.json` gives the benchmark, `-Werror` among them.

### flywheel-dag#52 — the benchmark's counter

**The prototype.** `bench_hot_path.cpp` less its three functions, the GCC pragma over two of them, and `<atomic>`,
`<cstdlib>` and `<new>`. `measure()` calls `allocation_counter::start()` before the timed loop and `stop()` after it,
and the target compiles `tests/allocation_counter.cpp`. None of the three compilers warns, and clang-tidy 23 has no
finding. `--invariants` is the committed file's to the byte from all three: the count is 0 in all four rows with all
twenty functions counted, the aligned forms among them, which today's counter cannot see.

**It closes what the issue describes.** In `ubuntu-dev`'s AddressSanitizer tree, today's benchmark runs 2,000 cycles
to exit 0, as the issue says. With a `std::stable_sort` of 64 integers added to `main()` by hand, it aborts before the
first row:

```text
ERROR: AddressSanitizer: alloc-dealloc-mismatch (operator new vs free)
```

for memory from the sanitizer's `operator new(unsigned long, std::nothrow_t const&)`, given to `free()`. The prototype
with the same sort runs to exit 0 with nothing on stderr. So does the prototype with the three rows below, at the
default count, and it prints the exact columns of the plain build.

**The machine code.** Every function of the two Mac binaries, disassembled and normalised so that a function which only
moved compares equal (branches as offsets, stubs and literals by name):

- Of the 797 functions that both hold, 647 are the same once a release through `free()` and one through
  `operator delete` count as the same call. `Engine::cycle()`, the destructor of its timing guard, every node's
  `eval()`, `NodeBase::propagate()`, every policy's `equal()` and `AsyncInput<double>::flush()` are among them.
- The other 150 differ, and each is code that allocates, itself or through a helper that the two builds inline
  differently. Today's binary carries the counter's test and increment, `malloc()` and the throw at each site, in 92
  of them. Nearly all of it is construction and growth. Seven are on a cycle's path: the four
  `ValueSlot<T>::emitImpl()`, `AsyncQueue<double>::flush()`, `AsyncInput<Frame>::flush()` and `Reduction::bind()`.
  The steady state does not take their allocation branch, and still runs different code around it. In `emitImpl()`
  the recycling path is the same instructions, 32 bytes nearer its entry, because the branch between is nine
  instructions shorter. `AsyncQueue<double>::flush()` saves two more pairs of registers on entry, and the idle path
  pays for them.

**The timings.** Each benchmark source was built behind a pad of 0 to 15 nops, so that `Engine::cycle()` lands at each
of the 16 alignments of a cache line. The binaries were run in shuffled order and paired by where `cycle()` lands. A
row was timed two ways, because the two disagree:

- **each row once**, as `bench_hot_path` runs them, in a process warmed for 300 ms: 15 runs per binary, 200,000 cycles;
- **in rotation**, as earlier plans swept: each row five times in turn in one process, and the median of the five; 12
  runs per binary, 100,000 cycles.

| ns/cycle over the 16 alignments | `chain` | `idle-queues` | `ingest` | `solve` |
|---|---|---|---|---|
| **Each row once.** Today's benchmark | 264.90 | 230.31 | 122.40 | 409.99 |
| Through the complete counter | 265.26 | 232.28 | 120.77 | 413.80 |
| Mean change | +0.14% | +0.86% | −1.33% | +0.93% |
| Range | −0.31% to +0.68% | +0.65% to +1.03% | −2.21% to +1.38% | −1.04% to +1.68% |
| Slower at | 12 of 16 | 16 of 16 | 1 of 16 | 14 of 16 |
| The control against today's | +0.18%, 12 of 16 | +0.89%, 16 of 16 | −1.67%, 0 of 16 | +1.04%, 14 of 16 |
| The complete counter against the control | −0.04%, 7 of 16 | −0.03%, 8 of 16 | +0.34%, 11 of 16 | −0.11%, 6 of 16 |
| **In rotation.** Today's benchmark | 265.44 | 230.18 | 122.25 | 410.86 |
| Through the complete counter | 271.25 | 232.05 | 123.96 | 421.27 |
| Mean change | +2.19% | +0.81% | +1.41% | +2.53% |
| Range | +1.55% to +3.36% | +0.50% to +1.08% | +0.44% to +2.28% | +1.43% to +3.47% |
| Slower at | 16 of 16 | 16 of 16 | 16 of 16 | 16 of 16 |
| The control against today's | +2.16%, 16 of 16 | +0.80%, 16 of 16 | +1.39%, 16 of 16 | +2.51%, 16 of 16 |
| The complete counter against the control | +0.03%, 8 of 16 | +0.02%, 10 of 16 | +0.01%, 8 of 16 | +0.03%, 7 of 16 |

Two more sessions in rotation read the same: +2.13%, +0.76%, +1.50% and +2.19%, and +2.26%, +0.79%, +1.25% and +2.57%.

**The control** is today's benchmark with `[[gnu::noinline]]` on its three functions and no other change. Every engine
function in it is, instruction for instruction, the one in the complete-counter binary: of the 817 functions that the
two hold, 809 are the same, and the other 8 are the allocation functions, the four row functions, which hold
`measure()`, and one outlined helper. It reads what the complete counter reads, both ways. So the complete counter
costs nothing, and every difference from today's benchmark is that the allocator is called and not inlined.

What that difference comes to depends on how a row is timed. `idle-queues` reads 0.8% to 0.9% more both ways, and its
extra instructions are in plain sight: the two pairs of registers above, 32 times a cycle. The other three rows move by
−1.3% to +0.9% when each is run once, and by +1.4% to +2.5% in rotation, where a row runs on the heap and in the
caches that the rows before it left.

**What a sweep cannot say.** In earlier plans "slower at 16 of 16 alignments" has meant a cost in the code. It does not
always. The prototype of the three rows changes no function that `chain` or `ingest` runs: of the 840 functions that
the four-row and the seven-row binaries hold, 828 are the same, and the other 12 are `main()`, cold fragments and
outlined helpers. In rotation `chain` nonetheless reads 2.33% more with the rows in the file, at 16 of 16 alignments,
and `ingest` 1.78% more, at 16 of 16. Run once, `chain` reads the same (−0.18%, slower at 5 of 16) and `ingest` still
1.55% more, at 15 of 16. A pad moves every function together. It averages over where the code lands in a cache line,
and not over which function sits beside which, or what the rows before left on the heap. So a figure from this
benchmark is comparable only with one from the same benchmark source, timed the same way.

### flywheel-dag#38 — the rows

**The graph.** One `AsyncInput<double>`, eight `Lazy` compute nodes that each scale the capped signal,
`k·min(s, 2)` for k = 1 … 8, a fold of the eight, its square root, and the fold less the root:

```text
signal ─┬─ 1·min(s, 2) ─┐
        ├─ 2·min(s, 2) ─┤
        ⋮               ├─ total ─┬─ SqrtNode ─┐
        └─ 8·min(s, 2) ─┘         └────────────┴─ DiffNode   → out
```

| Row | The fold | The signal | What its difference from `ops-moving` is |
|---|---|---|---|
| `ops-compute` | a `Lazy` `ComputeNode` of eight inputs that adds them in order from 0 | in [1, 2): every holder moves | the op-node shape: a `SumNode` pulls every input to learn whether it can skip, and again to fold |
| `ops-moving` | `SumNode` | in [1, 2) | |
| `ops-held` | `SumNode` | in [3, 4), a new value on every cycle: every holder recomputes, to the 2k it already publishes | the skip: the fold, the root and the difference each pull their inputs and skip |

`ops-moving` is also the issue's third row, a chain of an n-ary, a unary and a binary op node with nothing skipping.
The signal is the benchmark's splitmix64 noise, so no two cycles in a row post the same value.

**The exact columns**, the same text from Apple clang, GCC 13.3 and GCC 15.2, with `allocs/cycle` 0 at 1,000, 10,000
and 200,000 cycles:

```text
ops-compute allocs/cycle=0 callbacks=200000 checksum=9333831.3161690123
ops-moving allocs/cycle=0 callbacks=200000 checksum=9333831.3161690123
ops-held allocs/cycle=0 callbacks=0 checksum=0
```

The first four lines of the file do not change. A plain loop with no engine, compiled with `-ffp-contract=off` as the
benchmark is, gives the same two figures for the moving signal, 200,000 and 9333831.3161690123, and 0 and 0 for the
held one. clang-tidy 23 has no finding in the prototype. It had one, the base type of an `enum`, which is fixed.

**The timings**, at the 16 alignments and both ways. Each difference is taken inside one binary, so that its two rows
share a layout:

| ns/cycle over the 16 alignments | Each row once | In rotation |
|---|---|---|
| `ops-compute` | 269.18, from 266.65 to 276.72 | 274.89, from 273.77 to 278.49 |
| `ops-moving` | 291.41, from 289.40 to 295.42 | 294.66, from 293.01 to 298.00 |
| `ops-held` | 187.63, from 186.06 to 188.86 | 194.68, from 193.24 to 196.00 |
| `ops-moving` less `ops-compute`: the op-node shape | +22.23, from +18.70 to +24.91 | +19.77, from +18.15 to +21.15 |
| `ops-moving` less `ops-held`: the skip | +103.78, from +101.25 to +107.58 | +99.98, from +98.87 to +102.52 |

A row moves by 3 to 10 ns between alignments and by up to 7 between the two ways of timing it. The two differences
are about 20 ns and 100 ns at every alignment, either way, which is what makes them worth having: an op node's shape
costs 7% of the row, and a skip saves a third of it.

**What the rows catch.** Each change is hand-made, in a copy of `include/`, and the benchmark is built against the copy:

| Change to the engine | The exact columns |
|---|---|
| E1. `NAryOpNode::eval()` folds as it pulls, in one pass | none moves; the timings do, below |
| E2. the three op nodes never skip | `ops-held` reads `callbacks=200000` |
| E3. `NAryOpNode::eval()` decides to skip before it pulls | `ops-moving` reads `callbacks=0`, and `ops-compute` does not move |
| E4. the fold leaves out its first input | `ops-moving` reads another checksum, and `ops-compute` does not move |
| E5. a `ComputeNode` tells its consumers "changed" whatever its policy says | `ops-held` reads `callbacks=200000` |
| E6. `TypedEqualityPolicy` never says "equal" | `ops-held` reads `callbacks=200000`, and `chain` moves too |
| E7. the fold runs over its inputs in reverse | none moves |
| E2 or E5, with the rows' op nodes on the default policy | none moves |

E1 is what the rows are for. Built at the sixteen alignments, beside the unchanged headers:

| E1 against the unchanged headers | Mean change | Range | Slower at |
|---|---|---|---|
| `ops-moving` | −4.75%, 14.0 ns of the 19.8 that the shape costs | −5.53% to −3.99% | 0 of 16 |
| `ops-held` | +2.84%, 5.5 ns: a skip then folds for nothing | +1.82% to +4.10% | 16 of 16 |
| `ops-compute` | +0.17% | −0.47% to +0.64% | 11 of 16 |
| `chain`, `idle-queues`, `ingest`, `solve` | −0.15% to +0.07% | −1.77% to +2.11% | 5 to 8 of 16 |

The change shows in the two rows whose fold is an op node and in no other, and the exact columns say that the gain is
not work left undone.

E7 is the limit of a checksum that is a sum. The reversed fold gives another total in 102,620 of the 200,000 cycles,
in its last bits, and the same checksum to the last digit: the running sum is a hundred thousand times larger than a
total, and rounds the difference away. The benchmark's first plan said so of every row. So the pair's equal
checksums say that the two folds read the same inputs once each, and not that they add them in the same order.

The last line is why D6 gives the rows' op nodes `AlwaysChangedPolicy`. Under the default policy an op node that
recomputes to the value it already publishes tells nobody, so `callbacks=0` is true with or without the skip.

**The unit tests.** The same holds for the suite. With one template at a time changed so that it never skips, in a copy
of `include/`, and all 31 suites built against the copy:

| Never skips | The 31 suites at `528e466` | The four new tests |
|---|---|---|
| `UnaryOpNode` | all pass | `AUnaryOpIsNotCalledWhenItsInputDidNotMove` fails |
| `BinaryOpNode` | all pass | `ABinaryOpIsNotCalledWhenItsInputsDidNotMove` fails |
| `NAryOpNode` | one test fails, `CleanInputs/LazyPairKind.StaysDirtyWhenItSkips/NAryOp`, on the node's dirty state | `AnNAryOpIsNotCalledWhenItsInputsDidNotMove` fails |
| none | all pass | all four pass |

`LazyInvalidation.OpNodesSkipWhenTheirInputsDidNotMove` counts the runs of a node below a `SumNode`, which stand still
whether the `SumNode` skips or recomputes to the same value. The new tests give the op node an op that counts its own
calls.

### The whole change, once

A scratch copy of the checkout with every edit of Steps 2 to 4 applied, configured offline on the Mac with the strict
flags and the clang-tidy gate on. The configure and the build print no warning, over 51 objects. `ctest` passes 136 of
136, which is the 91 of the plain suite and the 45 `clang_tidy_` tests. `test_clang_tidy_wiring`, `clang_tidy_units`,
`test_warning_flags`, `test_comment_refs` and `test_claude_md_budget` are among them, and none needed an edit.
`bench_hot_path --invariants` prints the seven lines. The hand-made changes that Steps 2 and 4 list for the two new
checks, A1 to A5 and B1 to B6, give the results stated there. GCC 15.2 compiles the four new tests with no warning.

## Decisions to confirm

Each has a default, which is what the steps below do.

- **D1. One plan, and how its commits are scoped.** A commit that serves one issue cites that issue. The plan's own
  commits cite flywheel-dag#52.
- **D2. The benchmark counts through the complete counter, and its figures move: by −1.3% to +0.9% as it runs its
  rows, and by +0.8% to +2.5% timed in rotation.** Nothing in the engine changes, and the counter itself costs
  nothing. What changes is that `operator new` is a call, as it is in an application, where today the compiler
  inlines the benchmark's own into the engine's allocation branches. So the new figures are those of the code an
  application runs, and today's are not. *Alternative A:* include `allocation_counter.cpp` in the benchmark's own
  unit, so that the twenty are inlined as the three are now. It keeps today's figures: against today's benchmark it
  reads −0.49% to +0.23% with each row run once and −0.22% to +0.18% in rotation, slower at 4 to 13 of 16
  alignments. GCC 13.3 refuses it without the pragma, at the counter's `free()` (`-Wmismatched-new-delete`, GCC bug
  103993), so the pragma would move into the counter; GCC 15.2 accepts it. It also keeps a benchmark that is built as
  no application is, and a `.cpp` that is included as a header. *Alternative B:* leave the benchmark as it is and
  close flywheel-dag#52 unchanged, which is the opposite of what the milestone's description asks.
- **D3. The counter stays in `tests/`, and the benchmark compiles it from there.** `benchmarks/CMakeLists.txt` names
  `tests/allocation_counter.cpp` and adds `tests/` to the target's include path. The counter needs no gtest, so a tree
  with the tests off still builds the benchmark. *Alternative:* a directory of its own for the two files. Every check
  that lists the checkout's source directories would then learn a new one (`test_comment_refs`,
  `test_clang_tidy_wiring`, the clang-tidy header filter), for two files.
- **D4. flywheel-dag#52 gets a check, not only the move.** The move closes the instance. `test_allocation_functions`
  closes the cause: it reads every `.hpp`, `.inl` and `.cpp` under `include/`, `tests/`, `benchmarks/` and `examples/`
  and fails on a global allocation function declared anywhere but in the counter, and on a counter that defines other
  than eight `operator new` and twelve `operator delete`. Today the rule is a sentence in `CLAUDE.md`, and a program
  that does not link gtest can break it without a word, as the benchmark has. Its cost is one `cmake -P` script.
- **D5. Three rows, one graph, in place of the issue's two pairs and a chain.** `ops-moving` is an arm of both pairs
  and is the chain, so three rows say what five would. *Alternative:* a graph for each pair.
- **D6. The rows' three op nodes take `AlwaysChangedPolicy`.** Below moving inputs every recompute differs from the
  last, so a comparison can only add cost, which is `ingest`'s reason for its source. It also makes `ops-held`'s zero
  mean one thing: an op node that recomputed would publish, and the callback would fire. *Alternative:* the default
  policy, which is what `SumNode::make(name, inputs)` gives. Then the zero survives E2 and E5 above, and only the
  timing shows a lost skip, which CI does not compare.
- **D7. The op nodes' skip gets unit tests, in this plan.** The issue says that nothing pins the skip, and that is true
  of the suite as well as of the benchmark. Four tests in `tests/test_lazy_invalidation.cpp`, with ops that count
  their calls.
- **D8. The committed file gets a check, `test_bench_invariants`.** CI's diff holds the benchmark to the file, and
  nothing holds the file. The check holds it to four things that are true however the rows are computed: the cycle
  count is the default run's, every row reads `allocs/cycle=0`, `idle-queues` and `ops-held` read 0 and 0, and
  `ops-compute` and `ops-moving` read the same. So a file regenerated over a regression fails a test, where today it
  is a rule in `CLAUDE.md`. Its cost is one `cmake -P` script, which names four rows.
- **D9. No release.** Nothing under `include/` changes.
- **D10. No sanitizer job runs the benchmark.** The nightly job already runs `test_allocation_counter`, which shows in
  a sanitized build that each of the twenty functions is the counter's, and the unit suites cover the cycle path the
  rows run. *Alternative:* a ctest test, in the sanitizer trees only, that runs the benchmark for a short count. It
  cannot use the default count or the committed file: under the workflow's `ASAN_OPTIONS` a cycle costs 126 to 568 µs
  in `ubuntu-dev`, against 0.8 to 2.4 µs without `detect_stack_use_after_return`, so the default run takes 538 s
  there, and one of 2,000 cycles 8 s.
- **D11. Two issues to open at close-out**, with the figures of Step 4, each labelled `enhancement` and with no
  milestone, since none of the open ones is about the hot path's speed:
  - `nodes`: `NAryOpNode::eval()` walks its inputs twice. One pass (E1) reads 14.0 ns less on `ops-moving` and 5.5 ns
    more on `ops-held`, and which of the two a graph wants is the issue's question.
  - `engine`: under Apple clang, `AsyncQueue<double>::flush()` saves two more pairs of registers on entry once
    `operator new` is a call, which it is in an application, and its idle path pays for the arrival path's.
    `idle-queues` reads 0.8% to 0.9% more for it, and today's benchmark cannot see it.

## Steps

Every commit subject is scoped to its issue in the repo's `type(#N): …` form, and the subjects below omit the scope.
Every checkpoint is a full build with no warning and `ctest --preset relwithdebinfo-macos-arm64` on the Mac, passing in
full, and a push; CI is read in passing and not waited for. Steps 2 and 4 change what is compiled, so each also builds
and tests in `ubuntu-dev` (`relwithdebinfo-linux-arm64`), runs the clang-tidy gate on both hosts, and builds the
benchmark with GCC 15.2 in a throwaway container. Hand-made changes are made to a scratch copy, never the tree, each
after an assertion that its old text occurs as often as expected, beside an unchanged control. The ctest count goes
from 89 to 91.

### Step 1 — The plan

Commit: `docs: add plan for closing out the Benchmark coverage milestone`. On approval it is pushed, and a comment on
each of the two issues names this file. **Tests: none.** **Done when:** the plan is committed, you have approved it, it
is pushed, and each issue names it.

### Step 2 — flywheel-dag#52: the benchmark counts through the counter

**Tests new:** `tests/check_allocation_functions.cmake` (`test_allocation_functions`, registered at the top level, a
plain `cmake -P` script like `test_stateful_node_list`). Its file and its roots are parameters, so that a hand-made
change is made to a copy.

**The change:**

- `benchmarks/bench_hot_path.cpp`: the three functions, the pragma and the three includes go. `measure()` calls
  `allocation_counter::start()` and `stop()`. The header comment says that `allocs/cycle` counts calls to any of the
  twenty functions.
- `benchmarks/CMakeLists.txt`: the target compiles `tests/allocation_counter.cpp`, with `tests/` on its include path,
  and the comment says why.
- `tests/allocation_counter.hpp` and `tests/CMakeLists.txt`: the comments that say who links the counter name the
  benchmark.
- `CLAUDE.md`: the bullet that says the benchmark keeps a counter of its own says that it links the shared one, and
  that `test_allocation_functions` holds the rule; the warnings bullet loses its example, the one silenced site that
  this step removes. The file is 6,492 bytes against a budget of 6,500, and the two edits take 91 out.

The order is red then green: the check goes in first and must fail naming `benchmarks/bench_hot_path.cpp`, "1 operator
new and 2 operator delete of its own", and no other file. The prototype of the check does.

**Done when:**

- the check is red as stated, then green, and each hand-made change gives the result stated:

  | Change, in a copy | The check reports |
  |---|---|
  | A1. `void* operator new(std::size_t)` defined in `examples/quickstart.cpp` | that file, 1 `operator new` |
  | A2. `void  operator delete [] (void* p) noexcept`, spaced so, in a test | that file, 1 `operator delete` |
  | A3. the nothrow `operator new` removed from the counter | the counter defines 7 and 12 |
  | A4. the counter renamed | the counter is missing |
  | A5. a comment that names `void* operator new(`, and a call `::operator new(8)`, in a test | nothing: the control |

- the Mac and `ubuntu-dev` build with no warning and pass `ctest`, 90 of 90, and the clang-tidy gate passes on both;
- `bench_hot_path --invariants` equals the committed file, which this step does not touch, on both;
- a tree configured with `-DFLYWHEEL_DAG_BUILD_TESTS=OFF` builds `bench_hot_path`;
- in `ubuntu-dev`'s AddressSanitizer tree, the benchmark with a hand-made `std::stable_sort` in `main()` runs to exit 0
  with nothing on stderr, where the same change to `528e466` aborts;
- GCC 15.2, in a throwaway container, builds the benchmark with no warning: the pragma was for GCC, and CI's GCC is
  the one that the container does not have;
- the alignment sweep of "Measured" is run again on the committed files, and its table is in the Outcome.

Commit: `bench: bench_hot_path counts through the allocation counter, held by test_allocation_functions`. Its body says
that the figures move from this commit on, by how much under each way of timing, and why.

### Step 3 — flywheel-dag#38: the op nodes' skip, pinned

**Tests new:** four in `tests/test_lazy_invalidation.cpp`, under "The op nodes' skip path", with three ops that count
their calls (a unary, a binary, and an n-ary with its `identity()`):

| Test | Pins |
|---|---|
| `AUnaryOpIsNotCalledWhenItsInputDidNotMove` | over a `Lazy` node that republishes 2 when its source moves, the op node is dirty, returns the value it had, and its op's count stands still |
| `ABinaryOpIsNotCalledWhenItsInputsDidNotMove` | the same for a `BinaryOpNode` |
| `AnNAryOpIsNotCalledWhenItsInputsDidNotMove` | the same for an `NAryOpNode` of three inputs: three calls on the first evaluation and none after |
| `EachOpIsCalledWhenAnInputMoved` | the control: with one input that follows the source, each count goes up, by one, one and three |

No engine code changes, so the tests go in green, and the hand-made changes are their red.

**Done when:** the suite passes, 27 tests where it had 23; each of the three changes in the table under "Measured"
fails its test and no other of the four; and the Mac's checkpoint passes. `ubuntu-dev` is not run: the step adds tests
of headers that do not change, and CI runs them on both legs.

Commit: `test: pin the op nodes' skip with ops that count their calls`.

### Step 4 — flywheel-dag#38: the three rows

**Tests new:** `tests/check_bench_invariants.cmake` (`test_bench_invariants`, at the top level, a plain `cmake -P`
script). The file it reads is a parameter.

**The change:**

- `benchmarks/bench_hot_path.cpp`: `opsCompute()`, `opsMoving()` and `opsHeld()` over one `opsRow()`, as under
  "Measured", after `solve` in `main()`; `<cstddef>` and `<tuple>`, which the new code uses; the header comment's count
  of workloads.
- `benchmarks/expected_invariants.txt`: the three lines. The first four do not change.
- `README.md`: the Benchmark section's table and its count, and a sentence on what the two differences mean.
- `CLAUDE.md`: the benchmark bullet names `test_bench_invariants`, within the budget.

The order is red then green: the check goes in first and must fail on the two rules that name rows the file does not
have. The prototype of the check does.

**Done when:**

- the check is red as stated, then green, and each hand-made change to a copy of the file gives the result stated:

  | Change | The check reports |
  |---|---|
  | B1. `chain` reads `allocs/cycle=5.0000000000000004e-06` | `chain`, a regression in the engine |
  | B2. the last digit of `ops-compute`'s checksum changed | the two folds differ |
  | B3. `ops-held` reads `callbacks=1` | `ops-held` delivers something |
  | B4. the `ops-held` line removed | `ops-held` is missing |
  | B5. the first line reads `cycles=1000` | not the default run |
  | B6. a row's line printed twice | the row has two lines |

- `bench_hot_path` prints seven rows, with `allocs/cycle` 0 in each at 1,000, 10,000 and 200,000 cycles, on the Mac
  and in `ubuntu-dev`; `--invariants` is the same text from Apple clang, GCC 13.3 and GCC 15.2, and it is the
  committed file; and the plain loop gives the two `ops` figures;
- E1 to E7, repeated on the committed files, move the columns as the table says;
- the Mac and `ubuntu-dev` build with no warning and pass `ctest`, 91 of 91, and the clang-tidy gate passes on both;
- the two differences are measured on the committed files at the sixteen alignments, both ways, and are in the
  Outcome, beside what the four older rows read with the three in the file and without;
- CI's two legs, read once they finish, match the committed file. The x86-64 checksum is CI's alone to show: every
  local build is arm64.

Commit: `bench: three rows for the op nodes and the path on which they skip, held by test_bench_invariants`. Its body
says why the file has three new lines.

### Step 5 — Close out

The Outcome below, Status Done, a closing comment on each of the two issues and then each closed by hand, the two
issues of D11 opened, and the milestone closed. **Tests: none.** Commit:
`docs: record the outcome and close out the Benchmark coverage milestone`.

## Not in this plan

- **A one-pass `NAryOpNode::eval()`, and a smaller idle path for `AsyncQueue::flush()`**, which are D11's issues. Each
  changes a header, so each wants a release, and the first a decision about a skip that then folds for nothing.
- **A checksum that sees the order of a fold.** The column is a sum in every row, and E7 is what a sum cannot see.
- **The benchmark as a ctest test, or under a sanitizer in CI** (D10).
- **Moving the counter out of `tests/`** (D3).
- **Rows for the other op nodes.** `chain` has a `DiffNode` and `solve` a `ProductNode` and a `DiffNode`. No row calls
  `exp`, `log`, `pow` or a trigonometric function, whose results differ between math libraries.

## Self-review — risks and assumptions

- **The x86-64 checksum is not measured.** The rows use `+`, `−`, `×`, `√` and a comparison, the plain loop agrees to
  the digit, and Apple clang and GCC agree on arm64. If CI's Linux leg differs, an assumption here is wrong, and the
  file is not regenerated to find out.
- **Steps 2 and 4 each move the figures that the benchmark prints**: Step 2 as D2 says, and Step 4 because rows in the
  file move the rows beside them, `ingest` by 1.5% run once and `chain` by 2.3% in rotation. A timing from before
  either and one from after are from two instruments. The commits say so, and the way a change is timed does not
  depend on it: both sets of headers are built against one benchmark source.
- **A sweep is not proof.** It averages over where the code lands in a cache line, and "What a sweep cannot say" is
  what it leaves out. What shows that the counter costs nothing is the control, whose engine code is the complete
  counter's to the instruction.
- **`test_allocation_functions` reads text.** It takes `void* operator new` and `void operator delete` for a
  declaration, array forms and any spacing included, and a class's own `operator new` would match. There is none; one
  that is added fails the check, whose message states the rule.
- **`test_bench_invariants` names four rows.** Renaming one means editing the check. That is its point: a rule that
  named no row could pass by reading nothing.
- **`ops-held`'s zero depends on D6.** A later edit that gives the rows' op nodes the default policy weakens it with
  no test going red. The row's comment says what the policy is for.
- **The rows are a synthetic graph.** A difference between two of them means something. Their ns/cycle alone does not.
- **`CLAUDE.md` has 8 bytes to spare.** Step 2 takes it from 6,492 bytes to 6,401 and Step 4 to 6,466, in the
  rehearsal, and `test_claude_md_budget` fails the checkpoint otherwise.
- **A push cancels the `CI` run in flight**, so a mid-plan checkpoint may get no macOS result; the next commit's run
  covers it, and the Outcome will say which runs were read.
- **This repo is public.** The plan, the commits and the issue comments name the engine only, and the leak audit runs
  over each before its push.

## Progress

| Step | Status |
|---|---|
| 1 — The plan | Done: approved as written on 2026-11-14 |
| 2 — flywheel-dag#52: the benchmark counts through the counter | Done, but for the sweep on the committed files, which follows the push. The check went in red as stated, then green, and A1 to A5 are reported as stated. The Mac passes 90 of 90 and `ubuntu-dev` 135 of 135, its clang-tidy gate on. The README's line on `allocs/cycle` is edited here, where it becomes true, and not in Step 4; `CLAUDE.md` is 6,404 bytes |
| 3 — flywheel-dag#38: the op nodes' skip, pinned | Not started |
| 4 — flywheel-dag#38: the three rows | Not started |
| 5 — Close out | Not started |
