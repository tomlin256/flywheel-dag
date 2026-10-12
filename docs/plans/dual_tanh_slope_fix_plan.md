# Dual `tanh`: Take the Slope as sech², Not 1 − tanh²

**Status: Proposed.** Awaiting approval.

Closes [flywheel-dag#15](https://github.com/tomlin256/flywheel-dag/issues/15).

`aad::Dual<N>`'s `tanh` takes its derivative as 1 − tanh²(x). Once tanh(x) rounds close to ±1 that
subtraction cancels, and from |x| ≈ 19.06 it reads exactly 0. The plan takes sech²(x) as
`1/cosh(x)/cosh(x)` instead, which is within 3 ulp of sech² across the sweep (0 < x ≤ 374) and reads 0
at exactly the points where sech² rounds to 0. The change is one function and the comment over it, and
costs 2.4 ns a call. Two new tests pin it, with a relative tolerance. The test the issue would extend,
`FunctionsMatchTheirDerivatives`, accepts the defect at x = 10 and at x = 20.

## Problem

In `dag_aad.inl`:

```cpp
template<std::size_t N> Dual<N> tanh(const Dual<N>& x) {
    const double t = std::tanh(x.value);
    return chain(x, t, 1.0 - t * t);
}
```

For large |x|, tanh(x) = 1 − 2e^(−2|x|) + …, and the slope is the part below 1 that rounding t to a
double has already thrown away. The subtraction then has nothing left to recover. Measured on the
repository's header at its default flags (Apple Clang 21, arm64, `-O2`), against sech² from `bc` at 30
digits:

| x | Slope today | sech²(x) | Relative error |
|---|---|---|---|
| 5 | 1.8158323094e-4 | 1.8158323094e-4 | 2.5e-13 |
| 10 | 8.2446145296e-9 | 8.2446144558e-9 | 9.0e-9 |
| 15 | 3.7436720390e-13 | 3.7430491875e-13 | 1.7e-4 |
| 18 | 8.8817841970e-16 | 9.2780913210e-16 | 4.3e-2 |
| 19.0625 | 0 | 1.1081099863e-16 | 1 |
| 20 | 0 | 1.6993417021e-17 | 1 |
| 100 | 0 | 5.5355861069e-87 | 1 |

tanh(x) rounds to ±1 from |x| ≈ 19.06, where 2e^(−2|x|) falls below half the spacing under 1. The
slope is 0 from there on, for every |x|, although sech² is nonzero until |x| ≈ 373.3. A gradient that
runs through a saturated `tanh` is silently flat. Neither the compiler's fused multiply-add nor
(1 − t)(1 + t) changes that: both read 0 from the same x ("The decision").

The issue says that a check at x = 10 and x = 20 would pin the fix. In `FunctionsMatchTheirDerivatives`
it would not. That test accepts a slope within `1e-15 * max(1, |slope|)`, which is an absolute 1e-15
for any slope below 1. Run on the unfixed header with the test's own oracle and tolerance, the
defect's error is 7.4e-17 at x = 10, 6.2e-17 at 15, 4.0e-17 at 18 and 1.7e-17 at 20, and every one is
accepted. Its point list is also shared by all 13 functions, and 10 and 20 would put `asin` and `acos`,
which it scales to v/5, outside [−1, 1]. Two new tests with a relative tolerance do the pinning. The
weakness of that tolerance for the other functions is
[flywheel-dag#41](https://github.com/tomlin256/flywheel-dag/issues/41).

## The decision

The issue names the form: sech²(x) as `1/cosh(x)/cosh(x)`. What is left to decide is whether it is the
right one. Each row is the worst error over a sweep of 26,949 doubles in 0 ≤ x ≤ 374, in ulps of the
correctly rounded sech² (a `bc` value at 30 digits for each x, taken at the exact double so that the
input does not round), built with `-ffp-contract=off` so that each form is what it says:

| Slope as | Worst error | First reads 0 |
|---|---|---|
| `1 - t*t`, today | all of the value | x = 19.06 |
| `(1 - t)*(1 + t)`, and `fma(-t, t, 1)` | all of the value | x = 19.06 |
| `1/cosh/cosh` (this plan) | 3 ulp | never before sech² rounds to 0, at x = 373.3 |
| `1/(cosh*cosh)` | 3 ulp | x = 355.6, where sech² is subnormal (from x = 354.9) |
| `4e/(1 + e)²`, `e = exp(-2*fabs(x))` | 3 ulp | x = 372.6 |

- **No rearrangement of 1 − t² recovers the slope.** The two rearranged forms read 0 from the same x as
  the original, because t is what has lost the answer, not the subtraction.
- **`1/cosh/cosh` over `1/(cosh*cosh)`.** cosh overflows past |x| ≈ 710.5 and its square past 355.6,
  where sech² is still a subnormal number. Dividing twice keeps the slope alive to the end of the
  subnormals, at no cost in accuracy.
- **`1/cosh/cosh` over the exponential form.** The exponential form is as accurate, but it takes
  `fabs` to stay finite: without it the form reads 0 for x below −177, where (1 + e)² overflows, and
  NaN below −355, where `e` does (mutation e). cosh is even, so the plan's form has no sign to get
  wrong, and it is one library call where the other is an `exp` and three more operations.
- **The cost.** Two divisions and a `cosh` that was not called before. In a loop of 2 × 10⁷ calls, the
  median of 7 runs, builds alternated twice: `Dual<1>` goes from 3.1 to 5.5 ns a call and `Dual<4>`
  from 3.9 to 6.1 ns. A functor that does not call `tanh` pays nothing, and `bench_hot_path` does not
  include `dag_aad.hpp`.

## The fix

```cpp
// tanh′ is sech², taken as 1/cosh/cosh. Not 1 − tanh²: once tanh rounds to ±1,
// from |x| ≈ 19, that reads 0, and no rearrangement of it helps, because
// rounding tanh has already lost the answer. Not 1/(cosh·cosh) either: cosh²
// overflows past |x| ≈ 355, where sech² is still a subnormal number, though
// cosh itself overflows only past 710.
template<std::size_t N> Dual<N> tanh(const Dual<N>& x) {
    const double c = std::cosh(x.value);
    return chain(x, std::tanh(x.value), 1.0 / c / c);
}
```

- **The value is `std::tanh(x.value)`, as before.** Only the slope changes.
- **The edges.** At 0 the slope is 1. At ±∞, and wherever cosh overflows (|x| above 710.4758600739439),
  1/∞/∞ is 0, not NaN. A NaN argument gives NaN. All as today.
- **Every slope moves by a few ulp, not only the large ones.** Below |x| = 2 the old form is within 8 ulp
  of sech² and the new within 2, so the two need not agree in the last digits. A caller that compares
  gradients through `tanh` bit for bit across the release sees them change. The release notes say so.
- **`cosh` raises the overflow flag past |x| ≈ 710.5,** where the old code raised nothing. `Dual`'s
  `exp` raises it at its own limit already, and nothing in the engine traps on a floating-point
  exception.
- **Source-compatible,** with no signature change, so it ships as v0.2.6.
- **The comment carries the rule.** `CLAUDE.md` gets no line: its rule over cancellation-free partials
  is about the ops, and the tests fail a return to 1 − t², so the code and the tests say what a line
  there would.

## Tests

In `tests/test_aad_dual.cpp`, in the `AadDual` suite, after `FunctionsMatchTheirDerivatives`. The suite goes
from 11 tests to 13. Every test is deterministic: it calls `tanh` on a `Dual` and compares numbers. The
references in test 1 are 4/(eˣ + e⁻ˣ)² from `bc` (`echo 'scale=400; x=10; 4/((e(x)+e(-x))^2)' | bc -l`),
written to 17 digits, so they do not depend on the C library's `cosh`. The tolerance is 1e-14 relative.
The fix measures 3 ulp (6.7e-16) at worst on Apple's libm, and the defect is 2.5e-13 off at the first
point, 5.

| # | Test | Asserts | Fails under |
|---|---|---|---|
| 1 | `TanhSlopeIsAccurateWhereTanhRoundsToOne` | at ±5, ±10, ±15, ±18, ±20, ±30, ±100 and ±300 the value is `std::tanh(x)` and the slope is within 1e-14 (relative) of sech² | a, c, d, e, f, g |
| 2 | `TanhSlopeReachesZeroOnlyWhereSechSquaredDoes` | at ±360 the slope is positive and below the smallest normal double; at ±400, ±710, ±711, ±1e300 and ±∞ it is exactly 0 | a, b, c, d, e, g |

`FunctionsMatchTheirDerivatives` is not touched. Its `tanh` row checks the value exactly and the slope
at 0.3, 1.7 and 4.2, and it passes with the change: on this machine the new slope equals its oracle at
all three points.

## Prototype

Built on scratch copies of the repository (`git archive` of `9873f4a`, and the same with the change and
the tests), with the flags of `build/compile_commands.json` and `-Werror`, under Apple Clang 21 on
arm64. The repository tree is untouched. The code is the sketch above, and the tests are the table.

- **Tests.** A full build of the change with `-DFLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON` prints no warnings,
  and ctest passes 36 of 36. `test_aad_dual` passes 13 of 13. On `9873f4a`'s headers with the new test
  file, tests 1 and 2 fail and the other 11 pass.
- **Mutations.** One change per scratch copy of `include/`, against the new test file, with a control
  copy that passes all 13. Every test ran on its own under a watchdog, so a crash or a hang loses only
  that test. None crashed or hung.

  | | Change to the fix | Fails |
  |---|---|---|
  | a | `1.0 - t * t` (the defect) | 1, 2 |
  | b | `1.0 / (c * c)` | 2 |
  | c | `1.0 / c`, which is sech and not sech² | 1, 2, and `FunctionsMatchTheirDerivatives` |
  | d | `-1.0 / c / c` | 1, 2, and `FunctionsMatchTheirDerivatives` |
  | e | `4e/(1 + e)²` with `e = exp(-2x)`, no `fabs` | 1, 2 |
  | f | the value taken as `sinh(x) / c` | 1, and `FunctionsMatchTheirDerivatives` |
  | g | `c` held in a `float` | 1, 2, and `FunctionsMatchTheirDerivatives` |

- **The accuracy sweep** of "The decision", and the table of "Problem", against `bc` references.
- **Hot path.** `bench_hot_path --invariants` matches `benchmarks/expected_invariants.txt`.
- **Not run.** GCC and glibc: Docker's daemon was not running and no GCC is installed, so CI's ubuntu
  leg is the first to build the tests with GCC's warnings and to run them on glibc's `cosh`. No
  sanitizer: the change is arithmetic on locals.

## Steps

Every commit subject is scoped to flywheel-dag#15 in the repository's `type(#N): …` form. The subjects
below omit the scope.

### Step 1 — The tests and the fix

Written: tests 1 and 2. Changed: `dag_aad.inl` (`tanh` and the comment over it).

**Done when:**

- tests 1 and 2 fail on `9873f4a`'s headers and pass with the change, and the other 11 tests of
  `test_aad_dual` pass either way;
- each mutation above fails the tests it lists and no others, one test at a time under a watchdog, on
  scratch copies of `include/`, since the repository's own tree is never mutated, with a control copy
  that passes all 13;
- the build prints no warnings with `-DFLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON`, and ctest is green (36 of
  36);
- `bench_hot_path --invariants` matches `benchmarks/expected_invariants.txt`.

Commit: `fix: compute Dual tanh's slope as sech²`.

### Step 2 — Release and close out

- Release v0.2.6 as v0.2.5 was released: the project version and the FetchContent tag in
  `CMakeLists.txt` and the README move to 0.2.6 and v0.2.6. Then the tag, and a GitHub release with
  notes. Under **Fixed** they say that `Dual`'s `tanh` slope was 1 − tanh², which read 0 from
  |x| ≈ 19.06; under **Changed**, that the slope is `1/cosh/cosh` now, within 3 ulp of sech² over its
  whole range, that every slope moves by a few ulp, and that a call costs 2.4 ns more. No signature
  changes. `git log v0.2.5..HEAD` and `git diff v0.2.5..HEAD --stat -- include` are read first, so that
  the notes cover everything the tag will carry.
- Mark this plan done, with the mutations rerun on the committed tree.
- Push `main`, watch CI on both legs, and close flywheel-dag#15 with a summary comment.
- Stop before the push, the tag and the release for a go-ahead.

**Done when:** the build prints no warnings with `-DFLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON` and ctest is
green (36 of 36), CI is green on the release commit on both legs, the installed version file reports
0.2.6, the release is published, and flywheel-dag#15 is closed.

Commits: `build: release v0.2.6` and `docs: mark the plan done`.

## Not in this plan

- **A relative tolerance for `FunctionsMatchTheirDerivatives`.** Three of its 39 checks have a slope
  below 1e-3 (`tanh`, `erf` and `erfc` at 4.2), and its tolerance is blind there to a relative error of
  1e-12 and 4e-8. `tanh` is covered by tests 1 and 2. The rest is
  [flywheel-dag#41](https://github.com/tomlin256/flywheel-dag/issues/41), which needs a bound measured on
  both CI legs.
- **A `TanhOp` with a `Derivative<TanhOp>`.**
  [flywheel-dag#14](https://github.com/tomlin256/flywheel-dag/issues/14) left the hyperbolic ops out. An
  op added later takes this slope, and then `Dual`'s `tanh` takes it from the op, as `sin` does.

## Self-review — risks and assumptions

- **Every call to `tanh` costs 2.4 ns more.** A second library call and two divisions, measured in a
  loop that does nothing else. A tape or a functor that calls `tanh` once per node pays it once per
  evaluation. The engine's cycle does not, and `bench_hot_path` does not move.
- **The slope changes in the last digits everywhere,** not only where the old form was wrong. "The fix"
  has the numbers, and the release notes name it under **Changed**.
- **The result leans on the C library's `cosh`.** The test tolerance is 1e-14 against 6.7e-16 measured,
  a margin of 15, so a libm a few ulp worse still passes. glibc has not run it. If CI's ubuntu leg fails
  a test, the plan stops and reports the error measured there before anything changes.
- **Test 2 needs subnormal numbers.** At ±360 the slope is 8.1e-313. A build that flushes subnormals to
  zero (`-ffast-math`, or a floating-point mode set by the caller) reads 0 there and fails the test.
  This repository builds with neither, and the discriminating range, 355.6 to 373.3, is subnormal by
  construction.
- **The references are outside the code under test.** They come from `bc`, not from `cosh`, so a fault
  in the library's `cosh` cannot hide in them. They are 17 digits of a 30-digit value, so the last
  digit of each is a rounding.
- **GCC is unverified locally.** The new tests hold no reference into a temporary and no unbraced `if`
  around a gtest macro, the two patterns `CLAUDE.md` lists. CI's GCC leg builds them with warnings as
  errors.
- **The timings are one machine and one compiler,** Apple Clang 21 on arm64.
- **Assumptions:** the engine stays header-only C++17. `Dual` has no other path to `tanh`: the only
  other mention in the repository is the declaration. Pushing and releasing are assumed, as in the
  earlier plans; if either is to be done by hand, Step 2 stops before it.
