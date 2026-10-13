# Hold `FunctionsMatchTheirDerivatives` to a Relative Slope Tolerance

**Status: Proposed.** Awaiting approval.

Closes [flywheel-dag#41](https://github.com/tomlin256/flywheel-dag/issues/41).

`AadDual.FunctionsMatchTheirDerivatives` accepts a slope within `1e-15 * max(1, |slope|)` of its oracle.
The plan drops the `max`: the bound becomes `1e-15 * |slope|`, the constant the check already uses,
applied to the slope alone. The 18 checks with |slope| ≥ 1 keep the tolerance they have, and the other 21
trade an absolute bound for a relative one. The Dual's slope and its oracle were measured on Apple Clang
and on GCC 13.3 with glibc 2.39 (arm64 and x86-64). They differ by at most 4.3e-16 (relative) at the 39
checks and by at most 8.0e-16 over 4.6 million swept points a platform, so every check passes with room.
v0.2.5's `1 − tanh²`, which the check accepted, fails it at x = 4.2. The change is one `EXPECT_NEAR` and
two comments in `tests/test_aad_dual.cpp`. Nothing under `include/` moves, so there is no release.

## Problem

```cpp
EXPECT_NEAR(r.d[0], c.slope(at), 1e-15 * std::max(1.0, std::abs(c.slope(at))))
```

Below a slope of 1 that is an absolute 1e-15, a relative error of 1e-15 / |slope|. 21 of the 39 checks
(13 functions at 0.3, 1.7 and 4.2) are below 1. Three are below 1e-3:

| Check | Slope | Relative error accepted |
|---|---|---|
| `tanh` at 4.2 | 9.0e-4 | 1.1e-12 |
| `erf` at 4.2 | 2.5e-8 | 4.1e-8 |
| `erfc` at 4.2 | 2.5e-8 | 4.1e-8 |

The other 18 accept between 1.0e-15 and 1.9e-14. It is why
[flywheel-dag#15](https://github.com/tomlin256/flywheel-dag/issues/15) went unseen. v0.2.5's `tanh` slope,
`1 − t·t`, is 5.6e-14 off its oracle at 4.2 (467 ulp). With HEAD's test file and v0.2.5's `dag_aad.inl`
(the one file under `include/` that differs, `git diff 9873f4a HEAD`), this check passes and only
flywheel-dag#15's two new tests fail.

## How the oracle and the Dual reach a slope

The issue warns that the two reach each slope by different routes, so the bound has to be measured on both
CI legs. They differ less than that suggests:

| Function | Oracle | `Dual` |
|---|---|---|
| `exp`, `log`, `sqrt`, `sin`, `cos`, `atan`, `abs` | the expression of `Derivative<Op>::d` | that expression |
| `erf`, `erfc` | `2/√π · exp(−v²)`, the constant computed | `kTwoOverRootPi · exp(−v²)`, the constant a literal |
| `tanh` | `1/(cosh·cosh)` | `1/cosh/cosh` |
| `tan` | `1/(cos·cos)` | `1 + tan²` |
| `asin`, `acos` | `1/√(1 − v²)` | `1/√((1 − v)(1 + v))` |

- **Same expression, same libm call, same bits.** Except where the compiler fuses `sin` and `cos` of one
  argument into one call. Apple Clang does, to `__sincos_stret`, whose sine differs from `sin` in 522 of
  400,001 points over [−10, 10]. That is the gap on the `cos` slope below.
- **`tanh`: four roundings, on any libm.** Both sides take the same `cosh` result `c`, so the gap is the
  rounding of `c·c` and of its reciprocal against two divisions: at most 4u = 4.4e-16, u = 2⁻⁵³.
- **`tan`, `asin`, `acos`: all their slopes are ≥ 1.** Their tolerance does not change, and CI has been
  green on both legs with it. `tan` takes the slope from two different libm calls, so it has the widest
  gap. `asin` and `acos` differ by an unfused `1 − v²` on x86-64 and a fused one on Apple.

## Measurement

A scratch program (not committed) holds the test's 13 cases verbatim and compares each `Dual` slope with
its oracle, relative to the oracle, at the 39 points and over dense sweeps, at the project's flags
(`-O2 -g -DNDEBUG -std=gnu++17 -Wall -Wextra -Wpedantic -Werror`). The platforms:

- **macOS arm64, Apple Clang 21, Apple libm:** this machine, and the compiler of CI's macOS leg.
- **Ubuntu 24.04 container, GCC 13.3.0, glibc 2.39, linux/arm64 and linux/amd64:** the compiler and C
  library of CI's ubuntu leg. The amd64 one runs emulated and its `/proc/cpuinfo` lists `fma` and `avx2`, so
  glibc picks its FMA variants as on a runner.

| Worst gap | Apple Clang, arm64 | GCC 13.3, arm64 | GCC 13.3, x86-64 |
|---|---|---|---|
| the 39 checks | 4.3e-16 (`tan` at 4.2) | 2.4e-16 (`tan` at 1.7) | 2.4e-16 (`tan` at 1.7) |
| checks with a nonzero gap | 2, both `tan` | 1 | 1 |
| sweep: `tan` | 8.0e-16 | 5.2e-16 | 5.3e-16 |
| sweep: `asin`, `acos` | 3.1e-16 | 3.1e-16 | 4.2e-16 |
| sweep: `tanh` | 3.2e-16 | 3.3e-16 | 3.3e-16 |
| sweep: `cos` | 2.2e-16 | 0 | 0 |
| sweep: `exp`, `log`, `sqrt`, `sin`, `atan`, `erf`, `erfc` | 0 | 0 | 0 |

The sweeps cover about 4.6 million points a platform: `tan` over [0.01, 4.6] away from π/2, `asin` and
`acos` over [−0.9, 0.9], `tanh` over [0, 20], `erf` over [0, 26], `erfc` over [0, 6], and the rest over
[−10, 10] or [0.01, 10]. Against `bc` at 110 digits, the oracle and the `Dual` are both within 1.7u of the true slope
at every point but three: `erf` and `erfc` at 4.2, 9.0u (1.0e-15) off in both, because the exponent −x²
rounds before `exp`; and the `Dual`'s `tan` at 4.2 on Apple, 3.5u.

## The decision

**`1e-15 * |slope|`.** The constant stays and the `max(1, …)` goes.

- **The margin.** 1e-15 is 2.3× the worst gap at the 39 checks, which is `tan`, a check whose tolerance
  does not change. For the 21 checks that tighten, the worst gap is `tanh`'s 4.4e-16 by construction, and
  the gap is 0 at every point of the other 18 on all three platforms.
- **Not tighter.** The gap is not 0 (`tan`, `tanh`, `cos` on Apple), and a sweep of `tan` reaches 8.0e-16.
- **Not `EXPECT_DOUBLE_EQ` (4 ulp).** It has no constant and no zero case to handle, but it would also
  change the tolerance of the 18 checks that keep theirs today, from 4.5 to 9 ulp down to 4, for no gain.
  `tan` is already 2 ulp off at its points, and 7 ulp in a sweep.
- **No floor for a slope of 0,** which the issue suggests. None of the 39 oracle slopes is 0, and an
  oracle of exactly 0 should be met exactly: `EXPECT_NEAR` with a tolerance of 0 accepts equal values, and
  rejects any other. A floor would only loosen that.
- **No per-function column.** Most gaps are 0 and the bound is one number, not 13 to keep right on two
  legs.
- **Not `bc` references for the small-slope rows,** as flywheel-dag#15's tests use. That asks whether the
  slope is accurate, where this check asks whether the `Dual` matches its textbook formula, and it is
  more machinery than the issue calls for.

## The change

In `FunctionsMatchTheirDerivatives`:

```diff
-            EXPECT_NEAR(r.d[0], c.slope(at), 1e-15 * std::max(1.0, std::abs(c.slope(at))))
-                << c.name << " at " << at;
+            const double slope = c.slope(at);
+            EXPECT_NEAR(r.d[0], slope, 1e-15 * std::abs(slope)) << c.name << " at " << at;
```

with a comment over the loop (`<algorithm>` stays: `OneBodyCompilesAtBothTypes` uses `std::max`):

```cpp
    // Relative, so that a small slope gets no more room than a large one: an
    // absolute 1e-15 let tanh through at 1.1e-12 (relative) at 4.2, and erf and
    // erfc at 4e-8 (flywheel-dag#41). The Dual and its oracle reach most slopes by
    // the same expression and agree to an ulp or two. erf and erfc at 4.2 are
    // 1e-15 from the true slope in both, since −x² rounds before exp, so a more
    // accurate slope would have to move the oracle too.
```

and the end of the comment over `TanhSlopeIsAccurateWhereTanhRoundsToOne`, which would otherwise say
something untrue, becomes:

```cpp
// so they do not lean on the C library's cosh. FunctionsMatchTheirDerivatives
// looks no further than 4.2, where the defect is 5.6e-14 off, and its tolerance
// was absolute below a slope of 1 until flywheel-dag#41: it accepted that.
```

## Tests and prototype

Modified: `AadDual.FunctionsMatchTheirDerivatives`. Written: none. The test is the thing under change, so
its bite is checked by mutation, in the repository's way: one change per scratch copy of `include/`, built
against the old and the new test file, with a control copy. The suite stays at 13 tests.

| | Header | Old check | New check |
|---|---|---|---|
| M1 | v0.2.5's `tanh` slope, `1 − t·t` | accepts, at 4.2 it is 5.6e-14 off | fails `tanh` at 4.2 |
| M2 | `erf` and `erfc` exponent one ulp up: `exp(-nextafter(x·x, ∞))` | accepts, 3.6e-15 off at 4.2 | fails `erf` and `erfc` at 4.2 |
| A1 | `tanh` slope as `1/(c·c)`, a sound form | passes | passes; the gap is 0 |
| A2 | `tanh` slope as `4e/(1 + e)²`, `e = exp(−2|x|)`, a sound form | passes | passes; the gap is 2.4e-16 at most at the three points |

M1 and M2 are the two kinds of error the old bound let through: a cancellation that grows with |x|, and a
last-bit error in an argument that a slope of 1 hides. M2 is 4.4e-16 off at 1.7 (inside the bound) and
3.6e-15 at 4.2. A1 and A2 are not defects. They show the bound does not reject a sound implementation.
A1 still fails `TanhSlopeReachesZeroOnlyWhereSechSquaredDoes`, as it did in flywheel-dag#15's mutation b.

Run on HEAD, the old test passes M1 and M2 and the new test fails them where the table says, with the
other tests unchanged (on M1 the two `tanh` tests of flywheel-dag#15 fail under either). The control copy
passes 13 of 13 under both files. The outcome is the same on all three platforms, built with
`-Werror`. The repository's tree is untouched.

## Steps

Every commit subject is scoped to flywheel-dag#41 in the repository's `type(#N): …` form. The subjects
below omit the scope. On approval, the plan is marked approved in its own commit first.

### Step 1 — The tolerance and the comments

Changed: `tests/test_aad_dual.cpp` alone (the check, and two comments).

**Done when:**

- `test_aad_dual` passes 13 of 13, the build prints no warnings with
  `-DFLYWHEEL_DAG_WARNINGS_AS_ERRORS=ON`, and ctest is green (36 of 36), from a full build;
- M1, M2, A1 and A2 give the outcomes above against the old and the new test file, one test at a time
  under a watchdog, on scratch copies of `include/`, with a control copy that passes all 13;
- the same table holds in the two GCC 13.3 containers;
- `git diff --stat` names `tests/test_aad_dual.cpp` and nothing under `include/`.

Commit: `test: hold FunctionsMatchTheirDerivatives to a relative slope tolerance`.

### Step 2 — Close out

- Mark this plan done, with the mutations rerun on the committed tree.
- Stop before the push for a go-ahead. Push `main`, watch CI on both legs, and close flywheel-dag#41 with
  a summary comment. If a leg fails the check, stop and report the gap measured there before the bound
  changes.
- No release. Nothing under `include/` changes, so v0.2.6 stays the latest and no tag is cut.

**Done when:** CI is green on the final commit on both legs, `build (ubuntu-latest)` and
`build (macos-latest)`, and flywheel-dag#41 is closed.

Commit: `docs: mark the plan done`.

## Not in this plan

- **More points.** The check looks at 0.3, 1.7 and 4.2. A relative bound sees the `1 − tanh²` defect at
  4.2 and not at the x = 10 and 20 that flywheel-dag#15's tests cover. Past 4.2 `erf` and `erfc` meet the
  limit above: the exponent rounds, in the oracle and the `Dual`, and the true slope drifts from both by
  about x²u.
- **The same `max(1, …)` shape elsewhere.** `test_aad_node.cpp`, `test_aad_reverse.cpp` and
  `test_aad_partials.cpp` use it against finite differences or composed closed forms, with tolerances of
  1e-6 to 1e-13. Their oracles are good to about that, so a floor of 1 is not what limits them.

## Self-review — risks and assumptions

- **A more accurate `erf` or `erfc` slope would fail this check at 4.2.** The oracle is 1.0e-15 off the
  true slope there, the bound is 1e-15, and so a slope within 1u of the truth lands on the edge. The
  comment says so. The fix then is a better oracle, not a looser bound.
- **The stand-ins are not the runners.** The container is Ubuntu 24.04 with the same GCC and glibc
  versions as CI's ubuntu leg, emulated on arm64 hardware, and the Apple libm is this machine's. The
  result leans on structure, the same expression on both sides and four roundings for `tanh`, and not on
  the last digit of any libm result, which is why it should carry. CI is the check. If a leg fails, the
  plan stops and reports the gap measured there.
- **`tan` is the tightest at 2.3×.** Its check does not change, and CI has been green with it on both
  legs. A different libm could widen it, and then the bound would have to move for `tan` alone or for
  all.
- **A NaN or infinite slope fails**, as it does today: the tolerance is NaN and `EXPECT_NEAR` rejects.
- **The measurement copies the 13 cases from the test, not the test itself.** The test's own 13 of 13 on
  all three platforms, and the mutation outcomes, are what bind it to the real code.
- **Assumptions:** the engine stays header-only C++17; the oracle lambdas stay as they are; and pushing is
  assumed, as in the earlier plans, with Step 2 stopping before it.
