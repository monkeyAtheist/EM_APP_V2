// Electrostatics and magnetostatics against closed-form physics.
// Tests tagged [defect] encode the discretisation errors measured by the audit
// (2026-11-15); they are expected to fail until phase 1 replaces the kernels.

#include "qsig_test.h"
#include "reference_fields.h"

#include "qsig/electrostatics.h"
#include "qsig/magnetostatics.h"

#include <vector>

namespace es = Electrostatics;
namespace ms = Magnetostatics;
namespace k = qsig::constants;
using qsig::Vec3;

namespace
{
es::Source eSource(es::Source::Type type, double q = 1e-9)
{
    es::Source s;
    s.type = type;
    s.strength = q;
    return s;
}

// Flux of E through a sphere of radius r centred at the origin (midpoint rule in theta, phi).
double electricFlux(const std::vector<es::Source> &sources, double r, int nTheta = 90, int nPhi = 180)
{
    double flux = 0.0;
    const double dt = k::pi / nTheta, dp = 2.0 * k::pi / nPhi;
    for (int i = 0; i < nTheta; ++i)
    {
        const double t = (i + 0.5) * dt;
        for (int j = 0; j < nPhi; ++j)
        {
            const double p = (j + 0.5) * dp;
            const Vec3 n{std::sin(t) * std::cos(p), std::sin(t) * std::sin(p), std::cos(t)};
            flux += dot(es::fieldAt(sources, n * r).electricField, n) * r * r * std::sin(t) * dt * dp;
        }
    }
    return flux;
}

// Line integral of B around a circle of radius r in the XY plane, centred at the origin.
double ampereLoop(const std::vector<ms::Source> &sources, double r, int n = 720)
{
    double sum = 0.0;
    for (int i = 0; i < n; ++i)
    {
        const double p = 2.0 * k::pi * (i + 0.5) / n;
        const Vec3 at{r * std::cos(p), r * std::sin(p), 0.0};
        const Vec3 tangent{-std::sin(p), std::cos(p), 0.0};
        sum += dot(ms::fieldAt(sources, at).magneticFluxDensity, tangent) * r * 2.0 * k::pi / n;
    }
    return sum;
}
} // namespace

// ---------------------------------------------------------------- electrostatics

TEST_CASE("electrostatics: point charge is Coulomb", "[electrostatics]")
{
    const auto r = es::fieldAt({eSource(es::Source::Type::PointCharge, 3e-9)}, {0.3, -0.4, 1.2});
    const double d = std::sqrt(0.09 + 0.16 + 1.44);
    CHECK_REL(r.electricField.norm(), k::coulombK * 3e-9 / (d * d), 1e-14);
    CHECK_REL(r.potential, k::coulombK * 3e-9 / d, 1e-14);
}

TEST_CASE("electrostatics: uniform sphere inside, outside, potential continuity", "[electrostatics]")
{
    auto s = eSource(es::Source::Type::SolidSphere, 2e-9);
    s.radius = 0.5;
    const auto inside = es::fieldFromSource(s, {0.2, 0.0, 0.0});
    CHECK_REL(inside.electricField.x, k::coulombK * 2e-9 * 0.2 / 0.125, 1e-14);
    const auto below = es::fieldFromSource(s, {0.5 - 1e-9, 0.0, 0.0});
    const auto above = es::fieldFromSource(s, {0.5 + 1e-9, 0.0, 0.0});
    CHECK_REL(below.potential, above.potential, 1e-7);
    CHECK_REL(below.electricField.x, above.electricField.x, 1e-7);
}

TEST_CASE("electrostatics: spherical shell has no field inside", "[electrostatics]")
{
    auto s = eSource(es::Source::Type::SphericalShell);
    s.radius = 0.4;
    const auto r = es::fieldFromSource(s, {0.1, 0.2, -0.1});
    CHECK(r.electricField.norm() == 0.0);
    CHECK_REL(r.potential, k::coulombK * 1e-9 / 0.4, 1e-14);
}

TEST_CASE("electrostatics: infinite line and plane", "[electrostatics]")
{
    const auto line = es::fieldFromSource(eSource(es::Source::Type::InfiniteLine, 1e-10), {0.3, 0.4, 7.0});
    CHECK_REL(line.electricField.norm(), 1e-10 / (2.0 * k::pi * k::epsilon0 * 0.5), 1e-14);
    CHECK(!line.potentialDefined);
    const auto plane = es::fieldFromSource(eSource(es::Source::Type::InfinitePlane, 2e-11), {5.0, -3.0, 0.2});
    CHECK_REL(plane.electricField.z, 2e-11 / (2.0 * k::epsilon0), 1e-14);
}

TEST_CASE("electrostatics: Gauss law holds for every finite source", "[electrostatics]")
{
    using T = es::Source::Type;
    for (T type : {T::SolidSphere, T::SphericalShell, T::ThickSphericalShell, T::FiniteLine, T::RectangularPlate,
                   T::RectangularVolume, T::CircularPlate, T::AnnularPlate, T::TriangularPlate})
    {
        auto s = eSource(type, 1e-9);
        s.angleDeg = 20.0;
        CHECK_REL(electricFlux({s}, 1.5), 1e-9 / k::epsilon0, 2e-3);
    }
}

TEST_CASE("electrostatics: far field tends to a point charge", "[electrostatics]")
{
    using T = es::Source::Type;
    for (T type : {T::FiniteLine, T::RectangularPlate, T::CircularPlate, T::TriangularPlate, T::RectangularVolume})
    {
        const auto r = es::fieldFromSource(eSource(type), {0.0, 30.0, 40.0});
        CHECK_REL(r.potential, k::coulombK * 1e-9 / 50.0, 5e-4);
    }
}

TEST_CASE("electrostatics: superposition", "[electrostatics]")
{
    std::vector<es::Source> scene{eSource(es::Source::Type::PointCharge, 1e-9),
                                  eSource(es::Source::Type::FiniteLine, -2e-9)};
    scene[0].position = {0.5, 0.0, 0.0};
    const Vec3 p{0.1, 0.7, -0.2};
    const auto sum = es::fieldFromSource(scene[0], p).electricField + es::fieldFromSource(scene[1], p).electricField;
    const auto both = es::fieldAt(scene, p).electricField;
    CHECK_REL(both.x, sum.x, 1e-15);
    CHECK_REL(both.y, sum.y, 1e-15);
}

TEST_CASE("electrostatics: rectangular plate near field (audit: +578 % at 5 mm)", "[electrostatics][defect]")
{
    // App default plate: 1 m x 0.6 m, 1 nC. Exact Ez on the axis tends to sigma / 2 eps0 = 94 V/m.
    const auto s = eSource(es::Source::Type::RectangularPlate);
    for (double z : {0.02, 0.01, 0.005})
        CHECK_REL(es::fieldFromSource(s, {0.0, 0.0, z}).electricField.z, reference::rectangleAxisEz(1e-9, 1.0, 0.6, z),
                  0.01);
}

TEST_CASE("electrostatics: circular plate near field", "[electrostatics][defect]")
{
    auto s = eSource(es::Source::Type::CircularPlate);
    s.radius = 0.4;
    for (double z : {0.02, 0.005})
        CHECK_REL(es::fieldFromSource(s, {0.0, 0.0, z}).electricField.z, reference::diskAxisEz(1e-9, 0.4, z), 0.01);
}

// ---------------------------------------------------------------- magnetostatics

TEST_CASE("magnetostatics: infinite wire, Ampere's law, cylinders", "[magnetostatics]")
{
    ms::Source wire;
    wire.type = ms::Source::Type::InfiniteWireZ;
    wire.strength = 3.0;
    const auto b = ms::fieldAt({wire}, {0.0, 0.25, 0.0}).magneticFluxDensity;
    CHECK_REL(-b.x, k::mu0 * 3.0 / (2.0 * k::pi * 0.25), 1e-14);
    CHECK_REL(ampereLoop({wire}, 0.3), k::mu0 * 3.0, 1e-9);

    ms::Source hollow;
    hollow.type = ms::Source::Type::HollowCurrentCylinderZ;
    hollow.strength = 2.0;
    hollow.radius = 0.5;
    hollow.innerRadius = 0.25;
    CHECK_NEAR(ampereLoop({hollow}, 0.2), 0.0, 0.0, 1e-15);
    const double enclosed = 2.0 * (0.4 * 0.4 - 0.25 * 0.25) / (0.5 * 0.5 - 0.25 * 0.25);
    CHECK_REL(ampereLoop({hollow}, 0.4), k::mu0 * enclosed, 1e-9);
}

TEST_CASE("magnetostatics: circular loop matches the exact elliptic solution", "[magnetostatics]")
{
    ms::Source loop;
    loop.type = ms::Source::Type::CircularLoop;
    loop.radius = 0.4;
    loop.strength = 2.0;
    for (const auto &[rho, z] : {std::pair{0.0, 0.0}, std::pair{0.0, 0.3}, std::pair{0.2, 0.1}, std::pair{0.6, -0.2}})
    {
        const auto [br, bz] = reference::loopField(0.4, 2.0, rho, z);
        const auto b = ms::fieldAt({loop}, {rho, 0.0, z}).magneticFluxDensity;
        CHECK_REL(b.z, bz, 2e-4);
        CHECK_NEAR(b.x, br, 2e-4, 1e-4 * std::abs(bz));
    }
}

TEST_CASE("magnetostatics: Helmholtz pair centre field", "[magnetostatics]")
{
    ms::Source pair;
    pair.type = ms::Source::Type::HelmholtzPair;
    pair.radius = 0.3;
    pair.length = 0.3;
    pair.strength = 1.5;
    const double expected = std::pow(0.8, 1.5) * k::mu0 * 1.5 / 0.3;
    CHECK_REL(ms::fieldAt({pair}, {0.0, 0.0, 0.0}).magneticFluxDensity.z, expected, 2e-4);
}

TEST_CASE("magnetostatics: small loop far field equals a magnetic dipole", "[magnetostatics]")
{
    ms::Source loop, dipole;
    loop.type = ms::Source::Type::CircularLoop;
    loop.radius = 0.05;
    loop.strength = 1.0;
    dipole.type = ms::Source::Type::MagneticDipole;
    dipole.strength = k::pi * 0.05 * 0.05;
    const Vec3 p{2.0, 1.0, 3.0};
    const auto a = ms::fieldAt({loop}, p).magneticFluxDensity, b = ms::fieldAt({dipole}, p).magneticFluxDensity;
    CHECK_REL(a.norm(), b.norm(), 1e-3);
}

TEST_CASE("magnetostatics: solenoid centre field matches the ideal current sheet", "[magnetostatics]")
{
    ms::Source sol; // defaults: N = 100, R = 0.5 m, L = 2 m, I = 1 A
    sol.type = ms::Source::Type::Solenoid;
    const double sheet = k::mu0 * 100.0 / 2.0 * 2.0 / std::sqrt(4.0 + 4.0 * 0.25);
    CHECK_REL(ms::fieldAt({sol}, {0.0, 0.0, 0.0}).magneticFluxDensity.z, sheet, 1e-3);
}

TEST_CASE("magnetostatics: solenoid near the winding (audit: -17 % / +24 % at 2 cm)", "[magnetostatics][defect]")
{
    ms::Source sol;
    sol.type = ms::Source::Type::Solenoid;
    for (double z : {0.0, 2.0 / 56.0})
    {
        const auto [br, bz] = reference::sheetSolenoidField(0.5, 2.0, 100, 1.0, 0.48, z);
        CHECK_REL(ms::fieldAt({sol}, {0.48, 0.0, z}).magneticFluxDensity.z, bz, 0.02);
    }
}
