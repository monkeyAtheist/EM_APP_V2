#pragma once

// Independent closed-form references used by the tests. Deliberately NOT shared with the
// solvers: a reference must not inherit the bug it is meant to catch.

#include "qsig/constants.h"

#include <cmath>
#include <utility>

namespace reference
{
using qsig::constants::epsilon0;
using qsig::constants::mu0;
using qsig::constants::pi;

// Complete elliptic integrals K(k), E(k) of modulus k by the arithmetic-geometric mean.
// Portable (libc++ has no std::comp_ellint_*). Pass the complementary modulus kc = sqrt(1 - k^2) when
// it is known exactly: near k = 1, computing it from k loses most of its digits.
inline std::pair<double, double> ellipticKE(double k, double kc = -1.0)
{
    double a = 1.0, b = kc >= 0.0 ? kc : std::sqrt(1.0 - k * k), c = k;
    double sum = 0.5 * c * c, power = 0.5;
    for (int i = 0; i < 40 && std::abs(c) > 1e-16; ++i)
    {
        const double an = 0.5 * (a + b);
        const double bn = std::sqrt(a * b);
        c = 0.5 * (a - b);
        a = an;
        b = bn;
        power *= 2.0;
        sum += power * c * c;
    }
    const double K = pi / (2.0 * a);
    return {K, K * (1.0 - sum)};
}

// Exact B of a circular filament (radius R, current I, axis z, centre at origin) at (rho, z).
// Returns {B_rho, B_z} in tesla.
inline std::pair<double, double> loopField(double R, double I, double rho, double z)
{
    if (rho < 1e-12) return {0.0, mu0 * I * R * R / (2.0 * std::pow(R * R + z * z, 1.5))};
    const double q = (R + rho) * (R + rho) + z * z;
    const double k = std::sqrt(4.0 * R * rho / q);
    const double d = (R - rho) * (R - rho) + z * z;
    const auto [K, E] = ellipticKE(k, std::sqrt(d / q));
    const double c = mu0 * I / (2.0 * pi * std::sqrt(q));
    const double bz = c * (K + (R * R - rho * rho - z * z) / d * E);
    const double brho = c * z / rho * (-K + (R * R + rho * rho + z * z) / d * E);
    return {brho, bz};
}

// Ideal finite solenoid = uniform current sheet N*I/L on a cylinder, integrated with Simpson.
inline std::pair<double, double> sheetSolenoidField(double R, double L, int turns, double I, double rho, double z,
                                                    int slices = 4000)
{
    const double h = L / slices;
    double br = 0.0, bz = 0.0;
    for (int i = 0; i <= slices; ++i)
    {
        const double w = (i == 0 || i == slices) ? 1.0 : (i % 2 ? 4.0 : 2.0);
        const double zs = -0.5 * L + i * h;
        const auto [r, zz] = loopField(R, 1.0, rho, z - zs);
        br += w * r;
        bz += w * zz;
    }
    const double scale = turns * I / L * h / 3.0;
    return {br * scale, bz * scale};
}

// On-axis field of a uniformly charged rectangle (sides W x H, total charge Q) at height z.
inline double rectangleAxisEz(double Q, double W, double H, double z)
{
    const double sigma = Q / (W * H), a = 0.5 * W, b = 0.5 * H;
    return sigma / (pi * epsilon0) * std::atan(a * b / (z * std::sqrt(a * a + b * b + z * z)));
}

// On-axis field of a uniformly charged disk (radius R, total charge Q) at height z > 0.
inline double diskAxisEz(double Q, double R, double z)
{
    const double sigma = Q / (pi * R * R);
    return sigma / (2.0 * epsilon0) * (1.0 - z / std::sqrt(z * z + R * R));
}

// Mutual inductance of two coaxial circular filaments (radii a, b, axial distance d).
inline double coaxialLoopsMutual(double a, double b, double d)
{
    const double k = std::sqrt(4.0 * a * b / ((a + b) * (a + b) + d * d));
    const auto [K, E] = ellipticKE(k);
    return mu0 * std::sqrt(a * b) * ((2.0 / k - k) * K - 2.0 / k * E);
}

// Centre-fed dipole input resistance / reactance from an independent Hallen solver
// (tools/reference/hallen_reference.py: reduced kernel, pulse basis, delta gap).
// R converges (to ~0.3 %); X keeps drifting with the gap size, hence a wide band.
struct DipoleReference
{
    double lengthLambda;
    double radiusLambda;
    double resistanceOhm;
    double reactanceOhm;
    double reactanceBandOhm;
};
inline constexpr DipoleReference kHalfWaveDipole{0.5, 1e-3, 88.2, 50.0, 15.0};

// Resonance (X = 0) of the same thin dipole, Hallen reduced kernel (tools/reference/hallen_reference.py,
// bisection on the length): 0.47244 lambda at N = 121, 0.47300 at N = 161; R = 72.1 ohm at both.
// Unlike the reactance at L = lambda/2, it hardly depends on the delta-gap feed model.
inline constexpr double kThinDipoleResonantLengthLambda = 0.4727;
inline constexpr double kThinDipoleResonantResistanceOhm = 72.1;
} // namespace reference
