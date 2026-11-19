#pragma once

// sin and cos of one angle together, for the hot loops of the antenna code (MoM kernel, far field).
// The math library of MinGW-w64 makes sin/cos several times slower than plain arithmetic there
// (measured: transcendental-heavy tests 4 to 7 times slower than on Linux, FDTD as fast); this
// version is arithmetic only. Cody-Waite reduction by pi/2 in three parts (exact quotient up to
// |x| ~ 1e6) and the fdlibm minimax kernels on [-pi/4, pi/4]: absolute error below 5e-16 for
// |x| < 1e5 (tested against std::sin/cos). Beyond, it falls back to the library.

#include <cmath>

namespace qsig
{
inline void sinCos(double x, double &s, double &c)
{
    if (!(std::abs(x) < 1e5))
    {
        s = std::sin(x);
        c = std::cos(x);
        return;
    }
    constexpr double twoOverPi = 6.36619772367581382433e-01;
    constexpr double pio2a = 1.57079632673412561417e+00; // 33 bits: n * pio2a exact for |n| < 2^20
    constexpr double pio2b = 6.07710050630396597660e-11;
    constexpr double pio2c = 2.02226624871116645580e-21;
    const double t = x * twoOverPi;
    const long long n = static_cast<long long>(t + (t >= 0.0 ? 0.5 : -0.5));
    const double nd = static_cast<double>(n);
    const double r = ((x - nd * pio2a) - nd * pio2b) - nd * pio2c;
    const double z = r * r;
    // fdlibm __kernel_sin / __kernel_cos coefficients.
    const double sr =
        r + r * z *
                (-1.66666666666666324348e-01 +
                 z * (8.33333333332248946124e-03 +
                      z * (-1.98412698298579493134e-04 +
                           z * (2.75573137070700676789e-06 + z * (-2.50507602534068634195e-08 + z * 1.58969099521155010221e-10)))));
    const double cr =
        1.0 - 0.5 * z +
        z * z *
            (4.16666666666666019037e-02 +
             z * (-1.38888888888741095749e-03 +
                  z * (2.48015872894767294178e-05 +
                       z * (-2.75573143513906633035e-07 + z * (2.08757232129817482790e-09 + z * -1.13596475577881948265e-11)))));
    switch (n & 3)
    {
    case 0:
        s = sr;
        c = cr;
        break;
    case 1:
        s = cr;
        c = -sr;
        break;
    case 2:
        s = -sr;
        c = -cr;
        break;
    default:
        s = -cr;
        c = sr;
        break;
    }
}
} // namespace qsig
