#include "qsig/app/antenna_optimizer.h"

#include "qsig/constants.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <numeric>

namespace qsig::app
{
namespace
{
using Type = AntennaElement::Type;

bool isStraight(const AntennaElement &e)
{
    return e.type == Type::Wire || e.type == Type::Dipole || e.type == Type::FoldedDipole;
}

// One optimisation variable, mapped to [0, 1].
struct Variable
{
    enum class Kind
    {
        Length,
        Spacing,
    };
    Kind kind;
    std::size_t index; // element (length) or gap after the sorted element 'index' (spacing)
    double lo, hi;     // [m]
    double toUnit(double value) const { return hi > lo ? std::clamp((value - lo) / (hi - lo), 0.0, 1.0) : 0.0; }
    double fromUnit(double u) const { return lo + std::clamp(u, 0.0, 1.0) * (hi - lo); }
};

struct Layout
{
    bool onLine = false;
    qsig::Vec3 origin, direction;   // boom: origin = the reference (first fed) element
    std::vector<std::size_t> order; // elements sorted along the boom
    std::size_t reference = 0;      // position of the reference element in 'order'
    std::vector<double> along;      // abscissa of each sorted element [m]
};

Layout boomLayout(const std::vector<AntennaElement> &elements, std::size_t reference, double lambda)
{
    Layout layout;
    const std::size_t n = elements.size();
    if (n < 2) return layout;
    // The farthest pair sets the line; every element must lie on it.
    std::size_t a = 0, b = 1;
    double best = -1.0;
    for (std::size_t i = 0; i < n; ++i)
        for (std::size_t j = i + 1; j < n; ++j)
        {
            const double d = (elements[j].position - elements[i].position).norm();
            if (d > best)
            {
                best = d;
                a = i;
                b = j;
            }
        }
    if (!(best > 1e-9 * lambda)) return layout;
    const qsig::Vec3 dir = (elements[b].position - elements[a].position) * (1.0 / best);
    layout.origin = elements[reference].position;
    layout.direction = dir;
    std::vector<double> s(n);
    for (std::size_t i = 0; i < n; ++i)
    {
        const qsig::Vec3 rel = elements[i].position - layout.origin;
        s[i] = rel.x * dir.x + rel.y * dir.y + rel.z * dir.z;
        if ((rel - dir * s[i]).norm() > 1e-6 * lambda) return layout; // off the line
    }
    layout.order.resize(n);
    std::iota(layout.order.begin(), layout.order.end(), std::size_t{0});
    std::sort(layout.order.begin(), layout.order.end(), [&](std::size_t i, std::size_t j) { return s[i] < s[j]; });
    for (std::size_t k = 0; k < n; ++k)
    {
        layout.along.push_back(s[layout.order[k]]);
        if (layout.order[k] == reference) layout.reference = k;
        if (k > 0 && !(layout.along[k] - layout.along[k - 1] > 1e-9 * lambda)) return layout; // stacked elements
    }
    layout.onLine = true;
    return layout;
}

// Elements for a point of the unit cube.
std::vector<AntennaElement> design(const std::vector<AntennaElement> &base, const Layout &layout,
                                   const std::vector<Variable> &variables, const std::vector<double> &x)
{
    std::vector<AntennaElement> out = base;
    std::vector<double> gaps;
    if (layout.onLine)
        for (std::size_t k = 0; k + 1 < layout.along.size(); ++k) gaps.push_back(layout.along[k + 1] - layout.along[k]);
    for (std::size_t v = 0; v < variables.size(); ++v)
    {
        const double value = variables[v].fromUnit(x[v]);
        if (variables[v].kind == Variable::Kind::Length)
            out[variables[v].index].length = value;
        else
            gaps[variables[v].index] = value;
    }
    if (layout.onLine && !gaps.empty())
    {
        // The reference element stays; the others follow the gaps outwards from it.
        std::vector<double> along(layout.along.size());
        along[layout.reference] = layout.along[layout.reference];
        for (std::size_t k = layout.reference + 1; k < along.size(); ++k) along[k] = along[k - 1] + gaps[k - 1];
        for (std::size_t k = layout.reference; k-- > 0;) along[k] = along[k + 1] - gaps[k];
        for (std::size_t k = 0; k < along.size(); ++k)
            out[layout.order[k]].position = layout.origin + layout.direction * along[k];
    }
    return out;
}
} // namespace

YagiGoal yagiGoal(YagiGoal::Kind kind, bool match)
{
    YagiGoal goal;
    goal.kind = kind;
    goal.match = match;
    switch (kind)
    {
    case YagiGoal::Kind::Gain:
        goal.gainWeight = 1.0;
        goal.frontToBackWeight = 0.05;
        break;
    case YagiGoal::Kind::FrontToBack:
        goal.gainWeight = 0.3;
        goal.frontToBackWeight = 1.0;
        break;
    case YagiGoal::Kind::Balanced:
        goal.gainWeight = 1.0;
        goal.frontToBackWeight = 0.25;
        break;
    }
    // 2 points of score per dB missing: a design under the target loses more than the front-to-back
    // ratio or the gain can give back, so the search settles on the matched side of the limit.
    goal.matchWeight = match ? 2.0 : 0.0;
    return goal;
}

double yagiScore(const SweepPoint &point, const YagiGoal &goal)
{
    if (!point.valid || !std::isfinite(point.gainDbi)) return -1e6;
    const double frontToBack = std::isfinite(point.frontToBackDb) ? point.frontToBackDb : goal.frontToBackCapDb;
    double score = goal.gainWeight * point.gainDbi + goal.frontToBackWeight * std::min(frontToBack, goal.frontToBackCapDb);
    if (goal.matchWeight > 0.0)
        score -= goal.matchWeight * std::max(0.0, goal.returnLossTargetDb - point.returnLossDb);
    return score;
}

YagiOptimizerResult optimiseYagi(const std::vector<AntennaElement> &elements, const AntennaSettings &settings,
                                 const YagiOptimizerOptions &options, const YagiProgress &progress)
{
    const auto begin = std::chrono::steady_clock::now();
    YagiOptimizerResult out;
    out.elements = elements;
    const double lambda = qsig::constants::c0 / std::max(1.0, settings.frequency);
    if (elements.size() < 2)
    {
        out.error = "Il faut au moins deux éléments (un radiateur et un élément parasite).";
        return out;
    }
    std::size_t reference = elements.size();
    for (std::size_t i = 0; i < elements.size() && reference == elements.size(); ++i)
        if (elements[i].fed != 0) reference = i;
    if (reference == elements.size())
    {
        out.error = "Aucun élément n'est alimenté.";
        return out;
    }

    const Layout layout = boomLayout(elements, reference, lambda);
    std::vector<Variable> variables;
    if (options.lengths)
        for (std::size_t i = 0; i < elements.size(); ++i)
            if (isStraight(elements[i]))
            {
                const double lo = std::min(options.minLengthLambda * lambda, elements[i].length);
                const double hi = std::max(options.maxLengthLambda * lambda, elements[i].length);
                variables.push_back({Variable::Kind::Length, i, lo, hi});
            }
    if (options.spacings && layout.onLine)
        for (std::size_t k = 0; k + 1 < layout.along.size(); ++k)
        {
            const double gap = layout.along[k + 1] - layout.along[k];
            variables.push_back({Variable::Kind::Spacing, k, std::min(options.minSpacingLambda * lambda, gap),
                                 std::max(options.maxSpacingLambda * lambda, gap)});
        }
    out.variables = static_cast<int>(variables.size());
    if (variables.empty())
    {
        out.error = "Rien à optimiser : aucun élément droit (fil, dipôle) ni espacement le long d'une ligne.";
        return out;
    }

    const int budget = std::max(static_cast<int>(variables.size()) + 2, options.maxEvaluations);
    const std::size_t n = variables.size();
    std::vector<double> x0(n);
    {
        std::vector<double> gaps;
        if (layout.onLine)
            for (std::size_t k = 0; k + 1 < layout.along.size(); ++k) gaps.push_back(layout.along[k + 1] - layout.along[k]);
        for (std::size_t v = 0; v < n; ++v)
            x0[v] = variables[v].toUnit(variables[v].kind == Variable::Kind::Length ? elements[variables[v].index].length
                                                                                    : gaps[variables[v].index]);
    }

    // The search runs on a mesh of at most 60 segments per wavelength (Yagi gain within 0.02 dB of the
    // fine mesh, twice faster); the start and the result are evaluated on the mesh of the scene.
    AntennaSettings searchSettings = settings;
    searchSettings.segmentsPerWavelength = std::min(settings.segmentsPerWavelength, 60.0);
    // On the coarser mesh the match is searched with half a decibel in hand: the return loss moves by
    // a few tenths between the two meshes, and the result must hold on the mesh of the scene.
    YagiGoal searchGoal = options.goal;
    if (searchSettings.segmentsPerWavelength < settings.segmentsPerWavelength) searchGoal.returnLossTargetDb += 0.5;
    out.before = solveSweepPoint(elements, settings, settings.frequency);
    out.scoreBefore = yagiScore(out.before, options.goal);

    // Minimised: f = -score. Every evaluation is reported; the best design is kept throughout.
    std::vector<double> bestX = x0;
    double bestF = std::numeric_limits<double>::infinity();
    SweepPoint bestPoint;
    bool stop = false;
    auto evaluate = [&](const std::vector<double> &x)
    {
        const SweepPoint point =
            solveSweepPoint(design(elements, layout, variables, x), searchSettings, settings.frequency);
        const double f = -yagiScore(point, searchGoal);
        ++out.evaluations;
        if (f < bestF)
        {
            bestF = f;
            bestX = x;
            bestPoint = point;
        }
        if (progress && !progress(out.evaluations, budget, bestPoint)) stop = true;
        return f;
    };
    auto clampUnit = [](std::vector<double> x)
    {
        for (double &v : x) v = std::clamp(v, 0.0, 1.0);
        return x;
    };

    // Bounded Nelder-Mead (points projected into the unit cube), restarted around the best point
    // while the budget lasts: the simplex collapses early on a ridge.
    double step = 0.12;
    std::vector<double> start = x0;
    while (!stop && out.evaluations + static_cast<int>(n) + 1 <= budget)
    {
        std::vector<std::vector<double>> simplex(n + 1, start);
        std::vector<double> f(n + 1);
        f[0] = evaluate(start);
        for (std::size_t i = 0; i < n && !stop; ++i)
        {
            const bool room = simplex[i + 1][i] + step <= 1.0;
            simplex[i + 1][i] += room ? step : -step;
            f[i + 1] = evaluate(simplex[i + 1]);
        }
        while (!stop && out.evaluations < budget)
        {
            // Best, worst and second worst vertices.
            std::size_t bestIndex = 0, worst = 0;
            for (std::size_t i = 1; i <= n; ++i)
            {
                if (f[i] < f[bestIndex]) bestIndex = i;
                if (f[i] > f[worst]) worst = i;
            }
            std::size_t second = worst == 0 ? 1 : 0;
            for (std::size_t i = 0; i <= n; ++i)
                if (i != worst && f[i] > f[second]) second = i;
            // Converged: values and points both close.
            double size = 0.0;
            for (std::size_t i = 0; i <= n; ++i)
                for (std::size_t v = 0; v < n; ++v) size = std::max(size, std::abs(simplex[i][v] - simplex[bestIndex][v]));
            if (std::abs(f[worst] - f[bestIndex]) < 1e-3 && size < 2e-3) break;

            std::vector<double> centroid(n, 0.0);
            for (std::size_t i = 0; i <= n; ++i)
                if (i != worst)
                    for (std::size_t v = 0; v < n; ++v) centroid[v] += simplex[i][v] / static_cast<double>(n);
            auto along = [&](double t)
            {
                std::vector<double> x(n);
                for (std::size_t v = 0; v < n; ++v) x[v] = centroid[v] + t * (simplex[worst][v] - centroid[v]);
                return clampUnit(x);
            };
            const std::vector<double> reflected = along(-1.0);
            const double fr = evaluate(reflected);
            if (fr < f[bestIndex])
            {
                if (stop || out.evaluations >= budget)
                {
                    simplex[worst] = reflected;
                    f[worst] = fr;
                    break;
                }
                const std::vector<double> expanded = along(-2.0);
                const double fe = evaluate(expanded);
                if (fe < fr)
                {
                    simplex[worst] = expanded;
                    f[worst] = fe;
                }
                else
                {
                    simplex[worst] = reflected;
                    f[worst] = fr;
                }
            }
            else if (fr < f[second])
            {
                simplex[worst] = reflected;
                f[worst] = fr;
            }
            else
            {
                if (stop || out.evaluations >= budget) break;
                const bool outside = fr < f[worst];
                const std::vector<double> contracted = along(outside ? -0.5 : 0.5);
                const double fc = evaluate(contracted);
                if (fc < std::min(fr, f[worst]))
                {
                    simplex[worst] = contracted;
                    f[worst] = fc;
                }
                else
                {
                    // Shrink towards the best point.
                    for (std::size_t i = 0; i <= n && !stop && out.evaluations < budget; ++i)
                    {
                        if (i == bestIndex) continue;
                        for (std::size_t v = 0; v < n; ++v)
                            simplex[i][v] = simplex[bestIndex][v] + 0.5 * (simplex[i][v] - simplex[bestIndex][v]);
                        f[i] = evaluate(simplex[i]);
                    }
                }
            }
        }
        start = bestX;
        step *= 0.5;
        if (step < 0.01) break;
    }

    out.cancelled = stop;
    out.valid = bestPoint.valid;
    if (!out.valid)
    {
        out.error = "Aucune solution valide pendant la recherche.";
        out.elements = elements;
        return out;
    }
    // On the mesh of the scene; the starting design is kept when it scores better there.
    out.elements = design(elements, layout, variables, bestX);
    out.after = solveSweepPoint(out.elements, settings, settings.frequency);
    out.scoreAfter = yagiScore(out.after, options.goal);
    if (!(out.scoreAfter > out.scoreBefore))
    {
        out.elements = elements;
        out.after = out.before;
        out.scoreAfter = out.scoreBefore;
    }
    out.improved = out.scoreAfter > out.scoreBefore;
    out.milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
    return out;
}
} // namespace qsig::app
