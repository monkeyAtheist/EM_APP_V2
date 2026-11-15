#pragma once

// Single source of truth for physical constants used by every solver.
//
// Values: CODATA 2022 (NIST, May 2024). Exact SI defining constants are marked "exact".
// mu0 is the measured CODATA value; epsilon0 is derived from it so that
// c^2 * mu0 * epsilon0 == 1 holds to rounding, whatever the CODATA revision.
// Never redefine these numbers locally: alias them instead.

#include <numbers>

namespace qsig::constants
{
inline constexpr double pi = std::numbers::pi;

inline constexpr double c0 = 299'792'458.0;                     // m/s, exact
inline constexpr double mu0 = 1.256'637'061'27e-6;              // N/A^2, CODATA 2022
inline constexpr double epsilon0 = 1.0 / (mu0 * c0 * c0);       // F/m, = 8.8541878188e-12
inline constexpr double eta0 = mu0 * c0;                        // ohm, impedance of free space
inline constexpr double coulombK = 1.0 / (4.0 * pi * epsilon0); // N m^2 / C^2
inline constexpr double biotSavartK = mu0 / (4.0 * pi);         // T m / A

inline constexpr double elementaryCharge = 1.602'176'634e-19; // C, exact
inline constexpr double planck = 6.626'070'15e-34;            // J s, exact
inline constexpr double boltzmann = 1.380'649e-23;            // J/K, exact
inline constexpr double avogadro = 6.022'140'76e23;           // 1/mol, exact
inline constexpr double electronMass = 9.109'383'7139e-31;    // kg, CODATA 2022
inline constexpr double gravitational = 6.674'30e-11;         // m^3/(kg s^2), CODATA 2022

inline constexpr double degToRad(double degrees) noexcept
{
    return degrees * pi / 180.0;
}
inline constexpr double radToDeg(double radians) noexcept
{
    return radians * 180.0 / pi;
}
} // namespace qsig::constants
