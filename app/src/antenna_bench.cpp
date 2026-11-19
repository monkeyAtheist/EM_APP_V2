#include "qsig/app/antenna_bench.h"

#include "qsig/constants.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>

namespace qsig::app
{
namespace
{
using Type = AntennaElement::Type;
using Verdict = AntennaBenchCase::Verdict;
using cplx = std::complex<double>;
constexpr double kPi = qsig::constants::pi;
constexpr double kFrequency = 300e6;
constexpr int kSteps = 7;

double lambda()
{
    return qsig::constants::c0 / kFrequency;
}

AntennaElement dipole(double lengthLambda, double radiusLambda)
{
    AntennaElement e;
    e.name = "D";
    e.type = Type::Dipole;
    e.length = lengthLambda * lambda();
    e.wireRadius = radiusLambda * lambda();
    return e;
}

void judge(AntennaBenchCase &c)
{
    c.verdict = !std::isfinite(c.deviation)  ? Verdict::Error
                : c.deviation <= c.passLimit ? Verdict::Pass
                : c.deviation <= c.warnLimit ? Verdict::Warning
                                             : Verdict::Fail;
}

AntennaBenchCase line(const char *group, const char *quantity, const char *unit, double value, double reference,
                      const char *source, bool relative, double passLimit, double warnLimit, const char *note = "")
{
    AntennaBenchCase c;
    c.group = group;
    c.quantity = quantity;
    c.unit = unit;
    c.value = value;
    c.reference = reference;
    c.source = source;
    c.relative = relative;
    c.passLimit = passLimit;
    c.warnLimit = warnLimit;
    c.note = note;
    if (!std::isfinite(value) || !std::isfinite(reference))
    {
        c.deviation = std::nan("");
        c.verdict = Verdict::Error;
        return c;
    }
    c.deviation = relative ? 100.0 * std::abs(value - reference) / std::max(std::abs(reference), 1e-300)
                           : std::abs(value - reference);
    judge(c);
    return c;
}

AntennaBenchCase failed(const char *group, const char *quantity, const std::string &error)
{
    AntennaBenchCase c;
    c.group = group;
    c.quantity = quantity;
    c.value = c.reference = c.deviation = std::nan("");
    c.verdict = Verdict::Error;
    c.note = error;
    return c;
}
} // namespace

const char *benchVerdictName(Verdict verdict)
{
    switch (verdict)
    {
    case Verdict::Pass:
        return "Conforme";
    case Verdict::Warning:
        return "Écart limite";
    case Verdict::Fail:
        return "Non conforme";
    case Verdict::Error:
        return "Erreur";
    }
    return "Erreur";
}

int antennaBenchSteps()
{
    return kSteps;
}

AntennaBenchReport runAntennaBench(const AntennaSettings &sceneSettings, const std::function<void(int, int)> &progress)
{
    const auto begin = std::chrono::steady_clock::now();
    AntennaBenchReport report;
    AntennaSettings settings;
    settings.frequency = kFrequency;
    settings.segmentsPerWavelength = sceneSettings.segmentsPerWavelength;
    report.segmentsPerWavelength = settings.segmentsPerWavelength;
    const double lam = lambda(), k = 2.0 * kPi / lam;
    AntennaSolveOptions noPattern;
    noPattern.pattern = false;
    int step = 0;
    auto advance = [&] { if (progress) progress(++step, kSteps); };
    auto add = [&](AntennaBenchCase c) { report.cases.push_back(std::move(c)); };

    const char *hallen = "Équation de Hallén pour le même fil (noyau réduit, source « gap »), résolue par un outil "
                         "indépendant (tools/reference/hallen_reference.py)";

    // 1. Half-wave dipole, a = lambda/1000.
    {
        const char *group = "Dipôle λ/2 (a = λ/1000)";
        const AntennaResult r = solveAntenna({dipole(0.5, 1e-3)}, settings);
        if (!r.valid || r.feeds.empty())
            add(failed(group, "Impédance", r.error));
        else
        {
            const cplx z = r.feeds[0].impedance;
            add(line(group, "Résistance d'entrée R", "Ω", z.real(), 88.2, hallen, true, 3.0, 8.0,
                     "R ne dépend presque pas du modèle de source : la référence est convergée à 0,3 %."));
            add(line(group, "Réactance d'entrée X", "Ω", z.imag(), 50.0, hallen, false, 15.0, 25.0,
                     "X suit la largeur du « gap » d'alimentation : la référence elle-même varie de ±15 Ω."));
            add(line(group, "Directivité", "dBi", r.directivityDbi, 2.15, "Courant sinusoïdal sur un fil mince (théorie)",
                     false, 0.05, 0.15));
        }
        advance();
    }

    // 2. Resonance (X = 0) of the same wire: bisection on the length.
    {
        const char *group = "Dipôle résonant (a = λ/1000)";
        auto reactance = [&](double lengthLambda)
        {
            const AntennaResult r = solveAntenna({dipole(lengthLambda, 1e-3)}, settings, noPattern);
            return r.valid && !r.feeds.empty() ? r.feeds[0].impedance.imag() : std::nan("");
        };
        double lo = 0.44, hi = 0.50;
        const double xLo = reactance(lo), xHi = reactance(hi);
        if (!(xLo < 0.0 && xHi > 0.0))
            add(failed(group, "Longueur de résonance", "Pas de changement de signe de X entre 0,44 et 0,50 λ."));
        else
        {
            for (int i = 0; i < 18; ++i)
            {
                const double mid = 0.5 * (lo + hi);
                (reactance(mid) > 0.0 ? hi : lo) = mid;
            }
            const double length = 0.5 * (lo + hi);
            const AntennaResult r = solveAntenna({dipole(length, 1e-3)}, settings, noPattern);
            add(line(group, "Longueur de résonance", "λ", length, 0.4727, hallen, true, 1.0, 2.0));
            add(line(group, "Résistance à la résonance", "Ω", r.valid ? r.feeds[0].impedance.real() : std::nan(""), 72.1,
                     hallen, true, 2.0, 5.0, "À la résonance, R ne dépend presque pas du modèle de source."));
        }
        advance();
    }

    // 3. Short dipole: Hertzian radiation of its current moment, Schelkunoff's reactance.
    {
        const char *group = "Doublet court (L = 0,05 λ, a = λ/5000)";
        const double length = 0.05, radius = 2e-4;
        const AntennaResult r = solveAntenna({dipole(length, radius)}, settings);
        if (!r.valid || r.feeds.empty())
            add(failed(group, "Rayonnement", r.error));
        else
        {
            add(line(group, "Directivité", "dBi", r.directivityDbi, 10.0 * std::log10(1.5),
                     "Dipôle de Hertz : D = 3/2", false, 0.03, 0.1));
            // Moment p = integral of I dl along the wire: P = eta k^2 |p|^2 / (12 pi).
            cplx px{}, py{}, pz{};
            for (const auto &s : r.segments)
            {
                const cplx i = 0.5 * (s.i0 + s.i1);
                const qsig::Vec3 d = s.p1 - s.p0;
                px += i * d.x;
                py += i * d.y;
                pz += i * d.z;
            }
            const double moment = std::norm(px) + std::norm(py) + std::norm(pz);
            add(line(group, "Puissance rayonnée", "W", r.radiatedPowerW,
                     qsig::constants::eta0 * k * k * moment / (12.0 * kPi),
                     "Rayonnement du moment du courant calculé, η k² |p|² / 12π", true, 0.5, 2.0,
                     "Vérifie le champ lointain et son intégration sur la sphère, sans dépendre de la source."));
            const double schelkunoff =
                -120.0 * (std::log(length / (2.0 * radius)) - 1.0) / std::tan(kPi * length);
            add(line(group, "Réactance d'entrée X", "Ω", r.feeds[0].impedance.imag(), schelkunoff,
                     "Formule approchée de Schelkunoff, −120 (ln(L/2a) − 1) / tan(πL/λ)", true, 5.0, 10.0,
                     "Formule de ligne de transmission : quelques % d'écart sont attendus."));
        }
        advance();
    }

    // 4. Small loop, circumference 0.05 lambda (16-sided polygon on the circle).
    {
        const char *group = "Petite boucle (C = 0,05 λ, a = λ/5000)";
        AntennaElement loop;
        loop.name = "B";
        loop.type = Type::CircularLoop;
        loop.radius = 0.05 * lam / (2.0 * kPi);
        loop.sides = 16;
        loop.wireRadius = 2e-4 * lam;
        const AntennaResult r = solveAntenna({loop}, settings);
        if (!r.valid || r.feeds.empty())
            add(failed(group, "Impédance", r.error));
        else
        {
            const double b = loop.radius, n = loop.sides;
            const double area = 0.5 * n * b * b * std::sin(2.0 * kPi / n); // of the polygon actually solved
            add(line(group, "Résistance de rayonnement", "Ω", r.feeds[0].impedance.real(),
                     320.0 * std::pow(kPi, 4) * std::pow(area / (lam * lam), 2),
                     "Boucle de courant uniforme, R = 320 π⁴ (A/λ²)², A : aire du polygone", true, 5.0, 10.0,
                     "Le courant n'est pas tout à fait uniforme : R calculée dépasse la formule de quelques %."));
            add(line(group, "Réactance (inductance)", "Ω", r.feeds[0].impedance.imag(),
                     k * qsig::constants::eta0 * b * (std::log(8.0 * b / loop.wireRadius) - 2.0),
                     "Inductance d'une spire circulaire, ωμ₀ b (ln(8b/a) − 2), a ≪ b ≪ λ", true, 3.0, 8.0));
            add(line(group, "Directivité", "dBi", r.directivityDbi, 10.0 * std::log10(1.5),
                     "Dipôle magnétique : D = 3/2", false, 0.05, 0.15));
        }
        advance();
    }

    // 5. Power balance of a parasitic array.
    {
        const char *group = "Bilan de puissance (Yagi 3 éléments)";
        const AntennaResult r = solveAntenna(antennaPreset("yagi3", kFrequency), settings);
        if (!r.valid)
            add(failed(group, "Puissance", r.error));
        else
            add(line(group, "Puissance rayonnée / fournie", "W", r.radiatedPowerW, r.acceptedPowerW,
                     "Conservation de l'énergie (fils sans pertes) : ½ Re(V I*) à la source", true, 0.5, 2.0,
                     "Intégrale du champ lointain sur la sphère (grille 3°) contre la puissance à la source."));
        advance();
    }

    // 6. Reciprocity of two different, offset dipoles.
    {
        const char *group = "Réciprocité (deux dipôles différents)";
        AntennaElement a = dipole(0.5, 1e-3), b = dipole(0.4, 5e-4);
        a.name = "1";
        b.name = "2";
        b.position = {0.3 * lam, 0.1 * lam, 0.05 * lam};
        b.elevationDeg = 70.0;
        const auto y = admittanceMatrix({a, b}, settings);
        if (y.size() != 2)
            add(failed(group, "Y₁₂ = Y₂₁", "Résolution impossible."));
        else
        {
            AntennaBenchCase c = line(group, "Admittance de transfert |Y₁₂|", "S", std::abs(y[0][1]), std::abs(y[1][0]),
                                      "Théorème de réciprocité : Y₁₂ = Y₂₁", true, 1e-4, 1e-2,
                                      "Écart en % sur |Y₁₂ − Y₂₁| (complexes) rapporté à |Y₂₁|.");
            c.deviation = 100.0 * std::abs(y[0][1] - y[1][0]) / std::abs(y[1][0]);
            judge(c);
            add(std::move(c));
        }
        advance();
    }

    // 7. Mesh convergence at the mesh of the scene.
    {
        const char *group = "Convergence du maillage (dipôle λ/2)";
        AntennaSettings fine = settings;
        fine.segmentsPerWavelength = std::min(2.0 * settings.segmentsPerWavelength, 240.0);
        const AntennaResult coarse = solveAntenna({dipole(0.5, 1e-3)}, settings, noPattern);
        const AntennaResult reference = solveAntenna({dipole(0.5, 1e-3)}, fine, noPattern);
        if (!coarse.valid || !reference.valid)
            add(failed(group, "Impédance", !coarse.valid ? coarse.error : reference.error));
        else
        {
            const cplx z = coarse.feeds[0].impedance, zf = reference.feeds[0].impedance;
            AntennaBenchCase c = line(group, "|Z| au maillage de la scène", "Ω", std::abs(z), std::abs(zf),
                                      "Même antenne, maillage deux fois plus fin", true, 2.0, 5.0,
                                      "Écart en % sur |Z − Z_fin| rapporté à |Z_fin|.");
            c.deviation = 100.0 * std::abs(z - zf) / std::abs(zf);
            judge(c);
            add(std::move(c));
        }
        advance();
    }

    for (const auto &c : report.cases)
    {
        if (c.verdict == Verdict::Pass)
            ++report.passed;
        else if (c.verdict == Verdict::Warning)
            ++report.warnings;
        else
            ++report.failed;
    }
    report.milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
    return report;
}
} // namespace qsig::app
