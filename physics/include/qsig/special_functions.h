#pragma once

// Special functions shared by the exact field kernels. No Qt, header only.

#include <cmath>
#include <limits>

namespace qsig::special
{
// Bulirsch's general complete elliptic integral
//   cel(kc, p, c, s) = integral over [0, pi/2] of (c cos^2 t + s sin^2 t) / ((cos^2 t + p sin^2 t) sqrt(cos^2 t + kc^2 sin^2 t)) dt
// by Bulirsch's quadratically convergent iteration (as given by Derby and Olbert, Am. J. Phys. 78, 229, 2010).
// Returns +infinity for kc == 0 (logarithmic singularity of the integrand).
inline double bulirschCel(double kc, double p, double c, double s)
{
    if (kc == 0.0) return std::numeric_limits<double>::infinity();
    constexpr double tolerance = 1e-15;
    double k = std::abs(kc);
    double pp = p, cc = c, ss = s, em = 1.0;
    if (p > 0.0)
    {
        pp = std::sqrt(p);
        ss = s / pp;
    }
    else
    {
        double f = kc * kc;
        double q = 1.0 - f;
        const double g = 1.0 - pp;
        f -= pp;
        q *= ss - c * pp;
        pp = std::sqrt(f / g);
        cc = (c - ss) / g;
        ss = -q / (g * g * pp) + cc * pp;
    }
    double f = cc;
    cc += ss / pp;
    double g = k / pp;
    ss = 2.0 * (ss + f * g);
    pp += g;
    g = em;
    em += k;
    double kk = k;
    for (int iteration = 0; iteration < 64 && std::abs(g - k) > g * tolerance; ++iteration)
    {
        k = 2.0 * std::sqrt(kk);
        kk = k * em;
        f = cc;
        cc += ss / pp;
        g = kk / pp;
        ss = 2.0 * (ss + f * g);
        pp += g;
        g = em;
        em += k;
    }
    return 0.5 * 3.14159265358979323846 * (ss + cc * em) / (em * (em + pp));
}
} // namespace qsig::special
