// =============================================================================
// flywheel-dag — A header-only C++23 reactive DAG computation engine
//
// Copyright (c) 2026 Rob Tomlin
//
// Licensed under the MIT License. See LICENSE file in the project root for
// full license information.
// =============================================================================

// fit.cpp — a solver as a node: a line fitted to three points that move.
//
// Three points (tᵢ, yᵢ) have fixed abscissae, and ordinates that arrive from
// sources. The graph holds a line through two inputs, a·t + b, and its three
// residuals a·tᵢ + b − yᵢ. A ReductionNode makes the residuals a function of
// (a, b) and hands it to a functor, which fits the line by Gauss–Newton: each
// step is one call with the Jacobian, at a trial (a, b) that the graph never
// holds. Whenever a point moves, the engine evaluates the node again, which
// binds the function to the points the graph holds now and fits again, and the
// engine delivers the line.
//
// Each line delivered is checked against the closed form of the least-squares
// line through the three points. The last line says whether they agree. The
// example doubles as a test, which matches that line.
//
// Engine::step() runs one cycle synchronously, so the example prints the same
// lines on every run.

#include <flywheel/dag.hpp>
#include <flywheel/dag_engine.hpp>
#include <flywheel/dag_ops.hpp>
#include <flywheel/dag_reduce.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

/// The abscissae of the three points.
constexpr std::array<double, 3> kT{1.0, 2.0, 4.0};

/// The most Gauss–Newton steps a fit takes.
constexpr int kMaxSteps = 10;

/// What the fit node publishes: the line, and the steps that found it.
struct Line {
    double slope     = 0.0;
    double intercept = 0.0;
    int    steps     = 0;
    bool operator==(const Line&) const = default;
};

/// Gauss–Newton on the residuals f, from the line the graph holds. The
/// residuals are linear in (a, b), so the first step lands on the
/// least-squares line, to rounding, and the second finds nothing to move.
Line fitLine(dag::Reduction& f) {
    Line line{f.argument(0), f.argument(1), 0};
    std::array<double, 3> r{};
    std::array<double, 6> j{};   // by rows: j[2i] = ∂rᵢ/∂a, j[2i + 1] = ∂rᵢ/∂b
    while (line.steps < kMaxSteps) {
        f.jacobian({line.slope, line.intercept}, r, j);
        ++line.steps;
        // The step δ solves the normal equations, JᵀJ·δ = −Jᵀr.
        double jtj00 = 0.0, jtj01 = 0.0, jtj11 = 0.0, jtr0 = 0.0, jtr1 = 0.0;
        for (std::size_t i = 0; i < r.size(); ++i) {
            jtj00 += j[2 * i] * j[2 * i];
            jtj01 += j[2 * i] * j[2 * i + 1];
            jtj11 += j[2 * i + 1] * j[2 * i + 1];
            jtr0  += j[2 * i] * r[i];
            jtr1  += j[2 * i + 1] * r[i];
        }
        const double det = jtj00 * jtj11 - jtj01 * jtj01;
        const double da  = (jtj01 * jtr1 - jtj11 * jtr0) / det;
        const double db  = (jtj01 * jtr0 - jtj00 * jtr1) / det;
        line.slope     += da;
        line.intercept += db;
        if (std::abs(da) + std::abs(db) < 1e-12) break;
    }
    return line;
}

/// The closed form of the least-squares line through (tᵢ, yᵢ): the slope and
/// the intercept.
std::pair<double, double> closedForm(const std::array<double, 3>& y) {
    const double tMean = (kT[0] + kT[1] + kT[2]) / 3.0;
    const double yMean = (y[0] + y[1] + y[2]) / 3.0;
    double sty = 0.0, stt = 0.0;
    for (std::size_t i = 0; i < kT.size(); ++i) {
        sty += (kT[i] - tMean) * (y[i] - yMean);
        stt += (kT[i] - tMean) * (kT[i] - tMean);
    }
    const double slope = sty / stt;
    return {slope, yMean - slope * tMean};
}

/// A point that moves, and where to.
struct Move {
    std::size_t point;
    double      to;
};

} // namespace

int main() try {
    using namespace dag;

    async::Engine engine;

    // The line, a·t + b. Its two inputs are the reduction's arguments, and
    // what they hold is where each fit starts.
    auto a = Input<double>::make("a", 0.0);
    auto b = Input<double>::make("b", 0.0);

    // A source for each ordinate, and the residual a·tᵢ + b − yᵢ over it.
    std::array<std::shared_ptr<async::AsyncInput<double>>, 3> y;
    std::vector<NodePtr> residuals;
    for (std::size_t i = 0; i < kT.size(); ++i) {
        const std::string n = std::to_string(i);
        y[i] = async::AsyncInput<double>::make("y" + n, 0.0);
        engine.addSource(y[i]);
        residuals.push_back(ops::DiffNode<>::make("r" + n,
            ops::SumNode<>::make("a.t" + n + "+b",
                {ops::ProductNode<>::make("a.t" + n, {a, Input<double>::make("t" + n, kT[i])}), b}),
            y[i]));
    }

    // The fit, a node over the three residuals, with (a, b) as the arguments
    // of its reduction. It has no other inputs.
    auto fit = ReductionNode<Line>::make("fit", residuals, {a, b}, {}, fitLine);

    Line delivered;
    long deliveries = 0;
    engine.addOutput<Line>(fit, [&](const Line& line) {
        delivered = line;
        ++deliveries;
    });

    // The points start on 3t − 1, and then one moves in each cycle.
    std::array<double, 3> held{2.0, 5.0, 11.0};
    constexpr std::array<Move, 6> kMoves{{{1, 6.0}, {2, 10.0}, {0, 1.5}, {1, 4.0}, {2, 12.5}, {0, 3.0}}};

    std::printf("%-6s %7s %7s %7s %17s %17s %6s %10s\n",
                "cycle", "y0", "y1", "y2", "slope", "intercept", "steps", "off by");
    double worst = 0.0;
    bool   converged = true;
    long   cycles = 0;
    const auto check = [&] {
        const auto [slope, intercept] = closedForm(held);
        const double off = std::max(std::abs(delivered.slope - slope),
                                    std::abs(delivered.intercept - intercept));
        worst = std::max(worst, off);
        converged = converged && delivered.steps < kMaxSteps;
        ++cycles;
        std::printf("%-6ld %7.3f %7.3f %7.3f %17.12f %17.12f %6d %10.2g\n",
                    cycles, held[0], held[1], held[2], delivered.slope, delivered.intercept,
                    delivered.steps, off);
    };

    for (std::size_t i = 0; i < held.size(); ++i) y[i]->post(held[i]);
    engine.step();
    check();
    for (const Move& move : kMoves) {
        held[move.point] = move.to;
        y[move.point]->post(move.to);
        engine.step();
        check();
    }

    // Each cycle moved a point and the line, so each delivered a fit.
    const bool agree = deliveries == cycles && converged && worst < 1e-12;
    std::printf("\nthe largest difference from the closed form: %.2g\n", worst);
    std::printf("\n%s\n", agree ? "the fit and the closed form agree"
                                : "the fit and the closed form DISAGREE");
    return agree ? 0 : 1;
} catch (const std::exception& e) {
    std::fprintf(stderr, "fit: %s\n", e.what());
    return 1;
}
