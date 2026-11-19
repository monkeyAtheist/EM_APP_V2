#pragma once

// Integrals shared by the wire solver (numerical_em_solvers.cpp) and the wire + surface solver
// (moment_method.cpp): one validated implementation of the thin-wire pair of segments.

#include "qsig/constants.h"
#include "qsig/fast_math.h"
#include "qsig/vec3.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <cstddef>

namespace qsig::detail
{
inline constexpr double Pi = qsig::constants::pi;
inline constexpr double Mu0 = qsig::constants::mu0;

inline double segmentSegmentDistance3(const Vec3 &p0, const Vec3 &p1, const Vec3 &q0, const Vec3 &q1, double *sOut = nullptr,
                               double *tOut = nullptr, Vec3 *midOut = nullptr)
{
    // Closest points on two finite 3D segments (Ericson-style clamped solution).  This is
    // used only for CAD/topology contact detection; electromagnetic coupling still uses the
    // full EFIE kernel even when conductors are not galvanically connected.
    const Vec3 d1 = p1 - p0, d2 = q1 - q0, r = p0 - q0;
    const double a = dot(d1, d1), e = dot(d2, d2), f = dot(d2, r);
    double s = 0.0, t = 0.0;
    if (a <= 1e-30 && e <= 1e-30)
    {
        if (sOut) *sOut = 0.0;
        if (tOut) *tOut = 0.0;
        if (midOut) *midOut = (p0 + q0) * 0.5;
        return norm(p0 - q0);
    }
    if (a <= 1e-30)
        t = std::clamp(f / e, 0.0, 1.0);
    else
    {
        const double c = dot(d1, r);
        if (e <= 1e-30)
            s = std::clamp(-c / a, 0.0, 1.0);
        else
        {
            const double b = dot(d1, d2), den = a * e - b * b;
            if (std::abs(den) > 1e-30) s = std::clamp((b * f - c * e) / den, 0.0, 1.0);
            t = (b * s + f) / e;
            if (t < 0.0)
            {
                t = 0.0;
                s = std::clamp(-c / a, 0.0, 1.0);
            }
            else if (t > 1.0)
            {
                t = 1.0;
                s = std::clamp((b - c) / a, 0.0, 1.0);
            }
        }
    }
    const Vec3 cp = p0 + d1 * s, cq = q0 + d2 * t;
    if (sOut) *sOut = s;
    if (tOut) *tOut = t;
    if (midOut) *midOut = (cp + cq) * 0.5;
    return norm(cp - cq);
}

// Galerkin integrals of a pair of piecewise-linear rooftop supports, in the mixed-potential form of
// the reduced-kernel EFIE. The projected kernel jωμ t_o·(I + ∇∇/k²)G·t_s is integrated by parts twice
// along the wires: both derivatives move onto the rooftops (constant ±1/L on a span) and the
// boundary terms cancel over each KCL-safe function, so
//     Z = jωμ [ (t_o·t_s) ∫∫ f_o f_s G  −  (1/k²) ∫∫ f_o' f_s' G ],   G = e^{-jkR}/(4πR),
//     R = sqrt(|r − r'|² + a²)
// is the same operator with only G left to integrate (the ∇∇G form needs a sampling far below the
// radius: it broke down for thin wires, L/a > 40). The 1/R part of G is integrated exactly over the
// source span (asinh), the smooth remainder (e^{-jkR} − 1)/R by Gauss; the observation span takes a
// composite Gauss rule on the radius scale for self and near pairs.
// The four shape combinations of the pair (peak of the observation / source function at p0 or p1):
// index (obsPeakAtP0 ? 2 : 0) + (srcPeakAtP0 ? 1 : 0).
using RooftopPairVariants = std::array<std::complex<double>, 4>;

inline constexpr std::size_t rooftopVariant(bool obsPeakAtP0, bool srcPeakAtP0)
{
    return (obsPeakAtP0 ? 2u : 0u) + (srcPeakAtP0 ? 1u : 0u);
}

inline RooftopPairVariants integrateRooftopPairMixedPotential(const Vec3 &obsP0, const Vec3 &obsP1, const Vec3 &obsTangent,
                                                       const Vec3 &srcP0, const Vec3 &srcP1, const Vec3 &srcTangent,
                                                       double radiusM, double k, double omega)
{
    constexpr std::array<double, 4> gx{-0.8611363115940526, -0.3399810435848563, 0.3399810435848563,
                                       0.8611363115940526};
    constexpr std::array<double, 4> gw{0.3478548451374538, 0.6521451548625461, 0.6521451548625461, 0.3478548451374538};
    const std::complex<double> j(0.0, 1.0);

    RooftopPairVariants total{};
    const double lo = norm(obsP1 - obsP0), ls = norm(srcP1 - srcP0);
    if (!(lo > 1e-18) || !(ls > 1e-18)) return total;
    const double a = std::max(radiusM, 1e-12);
    const double d = segmentSegmentDistance3(obsP0, obsP1, srcP0, srcP1);
    const bool nearPair = d < std::max(5.0 * a, 0.60 * std::max(lo, ls));
    // Observation panels on the radius scale near the source (the inner integral varies there as
    // asinh(s/a)); the smooth remainder needs only a few source panels.
    const int po = nearPair ? std::clamp(static_cast<int>(std::ceil(lo / a)) + 2, 4, 64) : 1;
    const int ps = nearPair ? 4 : 1;

    const Vec3 od = obsP1 - obsP0, ts = (srcP1 - srcP0) * (1.0 / ls);
    // V[o][s] = ∫∫ f_o f_s G (shape index 1: peak at p0), S = ∫∫ G.
    std::complex<double> V[2][2] = {{{0.0, 0.0}, {0.0, 0.0}}, {{0.0, 0.0}, {0.0, 0.0}}};
    std::complex<double> S{0.0, 0.0};
    for (int io = 0; io < po; ++io)
    {
        const double uo0 = double(io) / double(po), uo1 = double(io + 1) / double(po);
        const double uoc = 0.5 * (uo0 + uo1), uoh = 0.5 * (uo1 - uo0);
        for (int go = 0; go < 4; ++go)
        {
            const double uo = uoc + uoh * gx[static_cast<std::size_t>(go)];
            const Vec3 ro = obsP0 + od * uo;
            const double wo = gw[static_cast<std::size_t>(go)] * lo * uoh;
            const double fo[2] = {uo, 1.0 - uo};

            // Source span: s in [0, ls] from srcP0, R(s) = sqrt(D² + (s − s0)²).
            const Vec3 rel = ro - srcP0;
            const double s0 = dot(rel, ts);
            const Vec3 perp = rel - ts * s0;
            const double d2 = dot(perp, perp) + a * a, dd = std::sqrt(d2);
            const double asinhSpan = std::asinh((ls - s0) / dd) - std::asinh(-s0 / dd);
            const double rEnd = std::sqrt(d2 + (ls - s0) * (ls - s0)), rStart = std::sqrt(d2 + s0 * s0);
            std::complex<double> i0 = asinhSpan;                         // ∫ ds / R
            std::complex<double> i1 = (rEnd - rStart + s0 * asinhSpan) / ls; // ∫ (s/ls) ds / R
            for (int is = 0; is < ps; ++is)
            {
                const double us0 = double(is) / double(ps), us1 = double(is + 1) / double(ps);
                const double usc = 0.5 * (us0 + us1), ush = 0.5 * (us1 - us0);
                for (int gs = 0; gs < 4; ++gs)
                {
                    const double us = usc + ush * gx[static_cast<std::size_t>(gs)];
                    const double sAlong = us * ls;
                    const double r = std::sqrt(d2 + (sAlong - s0) * (sAlong - s0));
                    double sinKr, cosKr;
                    qsig::sinCos(k * r, sinKr, cosKr);
                    const std::complex<double> smooth = std::complex<double>(cosKr - 1.0, -sinKr) / r;
                    const double ws = gw[static_cast<std::size_t>(gs)] * ls * ush;
                    i0 += ws * smooth;
                    i1 += ws * us * smooth;
                }
            }
            const std::complex<double> g0 = i0 / (4.0 * Pi), g1 = i1 / (4.0 * Pi);
            const std::complex<double> fs[2] = {g1, g0 - g1}; // ∫ f_s G for the peak at p1, at p0
            for (int o = 0; o < 2; ++o)
                for (int sIndex = 0; sIndex < 2; ++sIndex) V[o][sIndex] += wo * fo[o] * fs[sIndex];
            S += wo * g0;
        }
    }
    const double tt = dot(obsTangent, srcTangent);
    const std::complex<double> jwmu = j * omega * Mu0;
    for (int o = 0; o < 2; ++o)
        for (int sIndex = 0; sIndex < 2; ++sIndex)
        {
            // Rooftop derivatives along the oriented spans: -1/L with the peak at p0, +1/L at p1.
            const double slope = (o == 1 ? -1.0 : 1.0) * (sIndex == 1 ? -1.0 : 1.0) / (lo * ls);
            total[rooftopVariant(o == 1, sIndex == 1)] = jwmu * (tt * V[o][sIndex] - slope * S / (k * k));
        }
    return total;
}
} // namespace qsig::detail
