# A Public Micro-Benchmark for the Engine's Hot Path

**Status: Done (2026-09-28).** Approved 2026-09-28. All three steps landed. CI builds and runs the
benchmark on Linux and macOS, and gates its exact columns.

Closes [flywheel-dag#3](https://github.com/tomlin256/flywheel-dag/issues/3).

## Problem

The repo has no benchmark. Unit tests pin the engine's allocation-free steady state, but nothing
measures what a cycle costs, and nothing lets a change be timed against the commit before it. The
issue asks for a small, generic benchmark:

- a chain of compute and time-series nodes over an `AsyncInput`;
- a set of idle `AsyncQueue`s stepped with nothing posted;
- ingest of a heap-owning value, one `post()` + `flush()` per cycle.

It reports ns/cycle, allocations/cycle and a checksum. It is built on demand
(`EXCLUDE_FROM_ALL`) and is not a ctest test. Allocation counts and checksums are exact and could
gate CI; timings on shared runners are only reported.

## Design

### The three rows

Each row builds its graph on an `async::Engine` and runs 1,000 warm-up cycles. It then measures
`N` cycles (default 200,000) with a global `operator new` counter switched on, the instrument
`test_value_slot.cpp` already uses. Every drive is a pure function of the cycle index, so every
run sees the same inputs.

| Row | Graph | Drive, per cycle |
|---|---|---|
| `chain` | One `AsyncInput<double>` feeding 11 nodes with 4 registered outputs: `EWMANode` → `DeltaNode`; `RollingStats(64)` → `ZScoreNode` → `abs` (a `Lazy` `ComputeNode`) → `ThresholdNode` with hysteresis; `RollingMinMaxNode(64)` → its width (a `Lazy` `ComputeNode`); `DelayNode(16)` → `ops::DiffNode` → `RollingSumNode(64)` | Post the quickstart's signal: splitmix64 noise with a five-cycle spike every 250 cycles, so the threshold fires |
| `idle-queues` | 32 `AsyncQueue<double>`s, each a source and a registered output | Nothing posted; `step()` only |
| `ingest` | `AsyncInput<Frame>` with `AlwaysChangedPolicy` → a `Lazy` `ComputeNode` summing the frame | Rewrite one of the frame's 64 samples in place, `post()` the frame as an lvalue, `step()` |

`Frame` holds a label longer than any small-string buffer and 64 doubles, so both members own
heap. Its source takes `AlwaysChangedPolicy` because every post differs, so a comparison could only
add cost.

### Columns

| Column | Exact? | Meaning |
|---|---|---|
| `ns/cycle` | No | Wall time over the measured cycles, drive included |
| `allocs/cycle` | Yes | `operator new` calls over the measured cycles, divided by `N` |
| `callbacks` | Yes | Output callbacks fired over the measured cycles |
| `checksum` | Yes | The sum of every value those callbacks received |

`callbacks` is not in the issue. It catches what a sum can hide: an output that fires more or
less often with values that happen to sum the same. In `idle-queues` both exact columns are 0 by
construction. Nothing is posted, so any callback there is a bug.

There are two output modes:

- The default prints a table for people.
- `--invariants` prints only the exact columns, each double at `%.17g` so it round-trips. CI
  compares that text with a committed file.

The argument parsing and both output formats live in `benchmarks/bench_report.{hpp,cpp}`, apart
from the measurement, so they are unit-tested without running a benchmark.

### Exact on every platform

One committed file serves both CI legs only if the checksum is bit-identical on Linux (GCC,
x86-64) and macOS (Apple Clang, arm64). Two things make it so:

1. The rows use only `+`, `−`, `×`, `÷` and `sqrt`, which IEEE 754 rounds exactly. No row calls
   `exp`, `log` or `pow`, whose results differ between math libraries.
2. The benchmark compiles with `-ffp-contract=off`. Apple Clang fuses `a * b + c` into a single
   fused multiply-add by default on arm64; x86-64 without `-mfma` cannot.

A prototype found five fused instructions in the default arm64 build, in `EWMANode` and
`RollingStats`. They changed individual values: a bitwise hash of the `chain` row's `DeltaNode`
outputs differs between the two builds. The summed checksum did not change, because the sum
absorbed the last-bit differences. That is luck, and the flag makes it a guarantee. It costs
nothing measurable: `chain` read 357.5 ns/cycle with the flag and 360.8 without.

### Prototype results

A scratch build at `-O2 -DNDEBUG`, Apple M4 Pro, Apple Clang 21, 1,000 warm-up cycles:

| Row | ns/cycle at 200k | allocs over 1k / 10k / 200k cycles | callbacks at 200k |
|---|---|---|---|
| `chain` | 245–300 | 0 / 0 / 0 | 415,725 |
| `idle-queues` | 220–245 | 0 / 0 / 0 | 0 |
| `ingest` | 107–110 | 0 / 0 / 0 | 200,000 |

The allocations the first cycles make, by warm-up length:

| Warm-up cycles | `chain` | `idle-queues` | `ingest` |
|---|---|---|---|
| 0 | 24 | 0 | 12 |
| 1 | 12 | 0 | 6 |
| 2 | 1 | 0 | 0 |
| 4 or more | 0 | 0 | 0 |

The first cycles allocate every value buffer. 1,000 is a wide margin, and it fills every 64-sample
window before measuring. An `-O0` build gave the same exact columns as `-O2`.

### CI

CI builds the target explicitly, because nothing else will: it is `EXCLUDE_FROM_ALL`. It prints
the table, then diffs `--invariants` against `benchmarks/expected_invariants.txt`. Timings are
never compared.

The committed file makes any change to the exact columns visible in review:

- A non-zero `allocs/cycle` is an engine regression. Fix the engine; do not regenerate the file.
- A changed `callbacks` or `checksum` means the graph did different work. Regenerate the file only
  when that is the point of the change, and say why in the commit.

Rejected: report-only CI, which builds and runs the benchmark but gates nothing. It keeps the code
compiling but leaves the exact columns unchecked, and checking them is what the issue says they
are for.

### Where it lives

```
benchmarks/
  CMakeLists.txt            bench_hot_path (EXCLUDE_FROM_ALL), test_bench_report
  bench_hot_path.cpp        the three rows and main()
  bench_report.hpp / .cpp   Row, argument parsing, the table and --invariants output
  test_bench_report.cpp     unit tests for bench_report
  expected_invariants.txt   what CI compares --invariants with
```

The root `CMakeLists.txt` gains `FLYWHEEL_DAG_BUILD_BENCHMARKS`. Like the tests and examples
options, it is on when flywheel-dag is the top-level project and off for a consumer.

Nothing under `include/` changes, so there is no release.

## Steps

Every commit subject is scoped to flywheel-dag#3 in the repo's `type(#N): …` form. The subjects
below omit the scope.

### Step 1 — The benchmark and its report format

Add `benchmarks/` with the benchmark, `bench_report`, and the `FLYWHEEL_DAG_BUILD_BENCHMARKS`
option. `bench_hot_path` alone compiles with `-ffp-contract=off`.

The rows themselves are measured, not unit-tested: the issue rules out a ctest test. Step 2 gates
their exact columns in CI, and `test_value_slot.cpp` already pins the allocation behaviour they
exercise. `bench_report` gets a new ctest suite, `benchmarks/test_bench_report.cpp`:

| Test | Asserts |
|---|---|
| `BenchArgs.DefaultsTo200000Cycles` | No arguments: 200,000 cycles, table output |
| `BenchArgs.TakesACycleCount` | `5000` sets the cycle count |
| `BenchArgs.TakesInvariantsBeforeOrAfterTheCount` | `--invariants 5000` and `5000 --invariants` agree |
| `BenchArgs.RejectsAnUnknownFlag` | `--json` is an error |
| `BenchArgs.RejectsABadCycleCount` | `0`, `-5`, `12x`, `abc`, an empty string and an out-of-range count are errors |
| `BenchArgs.RejectsASecondCycleCount` | `100 200` is an error |
| `BenchReport.InvariantsRoundTripEveryDouble` | Checksums such as `0.1 + 0.2` and `1e-300`, printed and parsed back, are bit-identical |
| `BenchReport.InvariantsIgnoreTiming` | Rows that differ only in ns/cycle print the same invariants |
| `BenchReport.InvariantsNameTheCycleCount` | The first line is `cycles=N` |
| `BenchReport.TableShowsAStrayAllocation` | One allocation in 200,000 cycles does not print as `0` |

The last test exists because a fixed-point format such as `%.1f` prints one allocation in 200,000
cycles as `0.0`, which hides exactly the regression the column is for.

**Done when:**

- the full build and ctest are green: 22 of 22;
- a clean build of `all` does not produce `bench_hot_path`;
- `bench_hot_path` reports 0 allocs/cycle on every row at 1,000, 10,000 and 200,000 cycles;
- two runs, and a `Debug` build, print identical `--invariants` output.

Commit: `bench: add a hot-path benchmark with exact allocation, callback and checksum columns`.

### Step 2 — CI gates the exact columns

1. Generate `benchmarks/expected_invariants.txt` from a default run.
2. Add a `Benchmark` step to `ci.yml`, after `Test`. It writes the output to a file before diffing,
   so a crash fails the step rather than feeding `diff` a short input:

   ```yaml
   - name: Benchmark
     run: |
       cmake --build build --target bench_hot_path
       ./build/bin/bench_hot_path
       ./build/bin/bench_hot_path --invariants > invariants.txt
       diff -u benchmarks/expected_invariants.txt invariants.txt
   ```

3. Audit the workflow's actions for deprecation warnings while the file is open.

**Done when:**

- CI is green on Linux and macOS, and both legs print the table;
- locally, the diff exits non-zero against a copy of the file with one checksum digit changed;
- the run shows no deprecation annotations.

Commit: `ci: build and run the benchmark, and gate its exact columns`.

### Step 3 — Docs

- `README.md`: a Benchmark section: how to build and run it, what the rows and columns mean, and
  what CI checks.
- `CLAUDE.md`: the commands, and the two rules for the committed file (see CI above).
- Mark this plan done and close flywheel-dag#3 with a summary comment.

**Done when:** build and ctest are green, and the issue is closed.

Commit: `docs: describe the benchmark`.

## Self-review — risks and assumptions

- **The Linux checksum is unverified.** The prototype ran only on macOS. Step 2's first CI run is
  the test. If Linux differs, find out why before reaching for per-platform files. The design says
  a difference cannot happen, so one means an assumption above is wrong. *Resolved in Step 2: the
  first run matched on both legs.*
- **Allocation counts can differ by standard library.** libstdc++ allocates in places libc++ does
  not; `flush()`'s former local deque is the precedent. The committed file says 0 for both. A
  non-zero count on one leg is an engine finding, to fix or raise as an issue, never to commit.
- **The committed file is coupled to the engine's arithmetic.** An intended change to a
  time-series node's arithmetic changes `chain`'s checksum, and the file must be regenerated.
  That is the point: the change becomes visible in review.
- **A sum is a coarse check.** The prototype showed that it absorbs last-bit differences, so it
  will not see every change in rounding. It does see work that is skipped or repeated, which is
  what it is for, and `callbacks` counts what fires.
- **`-ffp-contract=off` makes the benchmark's arithmetic differ from a default arm64 build of an
  application.** Five fused instructions per cycle; the timing difference was not measurable.
- **Timings include the drive**: generating noise, rewriting one sample. The drive does the same
  work in every run, so a before/after comparison is unaffected.
- **Only `operator new(std::size_t)` is counted.** Aligned `new` is not, and the engine has no
  over-aligned types.
- **Assumptions:**
  - CI stays on `ubuntu-latest` (GCC, x86-64) and `macos-latest` (Apple Clang, arm64).
  - A compiler update on a runner image cannot change the checksum: with contraction off and no
    library math, IEEE 754 fixes every result. A standard-library update can change an allocation
    count, which would be a real finding, as above.

## Progress

| Step | Status | Notes |
|---|---|---|
| 1 — The benchmark and its report format | Done | ctest 22 / 22. The 10 `test_bench_report` tests pass; printing allocs/cycle at `%.1f` and doubles at `%.15g` turns two of them red. Building `all` does not produce `bench_hot_path`. Every row reads 0 allocs/cycle at 1,000, 10,000 and 200,000 cycles. Two runs and a `Debug` build print identical `--invariants`. At 200,000 cycles on an M4 Pro: `chain` 338.6, `idle-queues` 248.8, `ingest` 108.8 ns/cycle |
| 2 — CI gates the exact columns | Done | CI run 36413514158 is green on Linux and macOS. Both legs printed the table, and both matched the committed file, so the checksum is bit-identical on GCC x86-64 and Apple Clang arm64. Runner timings in ns/cycle for `chain`, `idle-queues` and `ingest`: Linux 518.6, 436.4 and 217.2; macOS 757.8, 813.5 and 360.2. Locally the gate's commands exit 0, and 1 against a copy of the file with one checksum digit changed. ctest 22 / 22. No run carries a deprecation annotation. `actions/checkout@v6` stays: v6.1.0 shipped the same day as v7.0.1, so v6 is still maintained. The Linux job's only annotation is a notice that `ubuntu-latest` moves to Ubuntu 26 from 2026-10-19. That brings a new GCC and libstdc++, so the gate will meet them then |
| 3 — Docs | Done | The README has a Benchmark section. `CLAUDE.md` has the command and the rules for the committed file. ctest 22 / 22. CI run 36413877632 is green on Linux and macOS. flywheel-dag#3 is closed |
