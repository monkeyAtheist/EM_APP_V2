// Electrostatics and magnetostatics against closed-form physics.
// Tests tagged [defect] encode the discretisation errors measured by the audit
// (2026-11-15); they are expected to fail until phase 1 replaces the kernels.

#include "qsig_test.h"
#include "reference_fields.h"

#include "qsig/electrostatics.h"
#include "qsig/magnetostatics.h"

#include <cmath>
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

TEST_CASE("electrostatics: infinite sources get a referenced potential whose gradient is -E", "[electrostatics]")
{
    using T = es::Source::Type;
    for (T type : {T::InfiniteLine, T::InfiniteCylinderZ, T::InfiniteHollowCylinderZ, T::InfinitePlane})
    {
        auto s = eSource(type, type == T::InfinitePlane ? 2e-11 : 1e-10);
        s.position = {0.1, -0.2, 0.05};
        s.radius = 0.25;
        s.innerRadius = 0.1;
        CHECK(es::extendsToInfinity(type));
        // Inside and outside the cylinders, across the hollow core, and on both sides of the plane.
        for (const Vec3 p : {Vec3{0.15, -0.18, 0.3}, Vec3{0.25, -0.1, -0.4}, Vec3{0.9, 0.7, 0.0}, Vec3{-0.6, -0.2, -1.1}})
        {
            const auto r = es::fieldFromSource(s, p);
            REQUIRE(r.relativePotentialDefined);
            CHECK(!r.potentialDefined);
            CHECK(r.potentialReferenced);
            const double h = 1e-6;
            for (int axis = 0; axis < 3; ++axis)
            {
                Vec3 a = p, b = p;
                (axis == 0 ? a.x : axis == 1 ? a.y : a.z) -= h;
                (axis == 0 ? b.x : axis == 1 ? b.y : b.z) += h;
                const double minusGrad =
                    -(es::fieldFromSource(s, b).relativePotential - es::fieldFromSource(s, a).relativePotential) / (2 * h);
                const double e = axis == 0 ? r.electricField.x : axis == 1 ? r.electricField.y : r.electricField.z;
                CHECK_NEAR(minusGrad, e, 1e-6, 1e-6 * r.electricField.norm() + 1e-9);
            }
        }
    }
    // Reference: V = 0 at 1 m from the axis of a line, and on the plane itself.
    auto line = eSource(T::InfiniteLine, 1e-10);
    CHECK_NEAR(es::fieldFromSource(line, {0.6, 0.8, 3.0}).relativePotential, 0.0, 0.0, 1e-12);
    // Continuity at the cylinder surface.
    auto cylinder = eSource(T::InfiniteCylinderZ, 1e-10);
    cylinder.radius = 0.3;
    CHECK_NEAR(es::fieldFromSource(cylinder, {0.3 - 1e-12, 0.0, 0.0}).relativePotential,
               es::fieldFromSource(cylinder, {0.3 + 1e-12, 0.0, 0.0}).relativePotential, 1e-9, 1e-9);
}

TEST_CASE("electrostatics: a scene with an infinite source keeps a usable relative potential", "[electrostatics]")
{
    using T = es::Source::Type;
    auto charge = eSource(T::PointCharge, 1e-9);
    charge.position = {-0.3, 0.0, 0.0};
    auto line = eSource(T::InfiniteLine, -2e-10);
    line.position = {0.3, 0.0, 0.0};
    const Vec3 p{0.05, 0.4, 0.0};
    const auto both = es::fieldAt({charge, line}, p);
    CHECK(!both.potentialDefined);
    CHECK(both.relativePotentialDefined);
    CHECK(both.potentialReferenced);
    CHECK_REL(both.relativePotential,
              es::fieldFromSource(charge, p).potential + es::fieldFromSource(line, p).relativePotential, 1e-14);
    // Finite sources only: the relative potential is the absolute one.
    const auto finite = es::fieldAt({charge}, p);
    CHECK(finite.relativePotential == finite.potential);
    CHECK(!finite.potentialReferenced);
    // On the line itself nothing is defined.
    CHECK(!es::fieldAt({charge, line}, {0.3, 0.0, 0.5}).relativePotentialDefined);
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

TEST_CASE("electrostatics: rectangular plate near field (audit: was +578 % at 5 mm)", "[electrostatics]")
{
    // App default plate: 1 m x 0.6 m, 1 nC. Exact Ez on the axis tends to sigma / 2 eps0 = 94 V/m.
    const auto s = eSource(es::Source::Type::RectangularPlate);
    for (double z : {0.02, 0.01, 0.005, 1e-4})
        CHECK_REL(es::fieldFromSource(s, {0.0, 0.0, z}).electricField.z, reference::rectangleAxisEz(1e-9, 1.0, 0.6, z),
                  1e-12);
}

TEST_CASE("electrostatics: circular plate near field (was -86 % at 5 mm)", "[electrostatics]")
{
    auto s = eSource(es::Source::Type::CircularPlate);
    s.radius = 0.4;
    for (double z : {0.02, 0.005, 1e-4})
        CHECK_REL(es::fieldFromSource(s, {0.0, 0.0, z}).electricField.z, reference::diskAxisEz(1e-9, 0.4, z), 1e-9);
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

TEST_CASE("magnetostatics: the line integral of H between two points follows Ampere's law", "[magnetostatics]")
{
    // Infinite wire on z: H = I / (2 pi rho) along phi, so the integral of H.dl is I x (angle swept) / 2 pi
    // whatever the path, as long as it does not cross the wire.
    ms::Source wire;
    wire.type = ms::Source::Type::InfiniteWireZ;
    wire.strength = 3.0;
    const std::vector<ms::Source> scene{wire};
    CHECK_REL(ms::magnetomotiveForce(scene, {1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}), 3.0 / 4.0, 1e-9);
    CHECK_REL(ms::magnetomotiveForce(scene, {0.0, 1.0, 0.2}, {1.0, 0.0, -0.4}), -3.0 / 4.0, 1e-9);
    CHECK_NEAR(ms::magnetomotiveForce(scene, {0.5, 0.0, 0.0}, {2.0, 0.0, 0.0}), 0.0, 0.0, 1e-12); // radial path
    // A closed triangle around the wire adds up to the enclosed current.
    const Vec3 a{1.0, 0.0, 0.0}, b{-0.5, 0.9, 0.0}, c{-0.5, -0.9, 0.0};
    CHECK_REL(ms::magnetomotiveForce(scene, a, b) + ms::magnetomotiveForce(scene, b, c) +
                  ms::magnetomotiveForce(scene, c, a),
              3.0, 1e-8);
    // Along the axis of a long solenoid: n I x length inside.
    ms::Source coil;
    coil.type = ms::Source::Type::Solenoid;
    coil.strength = 2.0;
    coil.turns = 2000;
    coil.length = 40.0;
    coil.radius = 0.05;
    coil.azimuthDeg = 0.0;
    coil.elevationDeg = 90.0;
    CHECK_REL(ms::magnetomotiveForce({coil}, {0.0, 0.0, -0.5}, {0.0, 0.0, 0.5}), 2.0 * 2000.0 / 40.0, 1e-4);
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
        CHECK_REL(b.z, bz, 1e-10);
        CHECK_NEAR(b.x, br, 1e-10, 1e-12 * std::abs(bz));
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
    CHECK_REL(ms::fieldAt({pair}, {0.0, 0.0, 0.0}).magneticFluxDensity.z, expected, 1e-12);
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

TEST_CASE("magnetostatics: solenoid near the winding (6.14: -17 % / +24 % at 2 cm)", "[magnetostatics]")
{
    ms::Source sol;
    sol.type = ms::Source::Type::Solenoid;
    for (double z : {0.0, 2.0 / 56.0})
    {
        const auto [br, bz] = reference::sheetSolenoidField(0.5, 2.0, 100, 1.0, 0.48, z);
        CHECK_REL(ms::fieldAt({sol}, {0.48, 0.0, z}).magneticFluxDensity.z, bz, 1e-6);
    }
}

TEST_CASE("magnetostatics: exact loop and solenoid against independent references", "[magnetostatics]")
{
    // Loop: elliptic closed form (independent AGM implementation), with a tilted axis and near the wire.
    ms::Source loop;
    loop.type = ms::Source::Type::CircularLoop;
    loop.radius = 0.4;
    loop.strength = 2.0;
    for (const auto &[rho, z] : {std::pair{0.0, 0.0}, std::pair{1e-7, 0.2}, std::pair{0.2, 0.1}, std::pair{0.6, -0.2},
                                 std::pair{0.399, 0.001}, std::pair{0.4, 1e-6}, std::pair{3.0, 4.0}})
    {
        const auto [br, bz] = reference::loopField(0.4, 2.0, rho, z);
        const auto b = ms::fieldAt({loop}, {rho, 0.0, z}).magneticFluxDensity;
        CHECK_NEAR(b.z, bz, 1e-9, 1e-12 * std::hypot(br, bz));
        CHECK_NEAR(b.x, br, 1e-9, 1e-12 * std::hypot(br, bz));
    }
    // Same loop with its axis along x (azimuth 0, elevation 0): the field is the rotated one.
    ms::Source tilted = loop;
    tilted.elevationDeg = 0.0;
    const auto [br, bz] = reference::loopField(0.4, 2.0, 0.2, 0.1);
    const auto bt = ms::fieldAt({tilted}, {0.1, 0.0, 0.2}).magneticFluxDensity;
    CHECK_REL(bt.x, bz, 1e-10);
    CHECK_REL(bt.z, br, 1e-10);

    // Solenoid (N > 24: current sheet): converged numerical integration of loops along the sheet.
    ms::Source sol;
    sol.type = ms::Source::Type::Solenoid;
    for (const auto &[rho, z] : {std::pair{0.0, 0.0}, std::pair{0.1, 0.3}, std::pair{0.48, 0.5}, std::pair{0.6, 1.2},
                                 std::pair{1.5, -2.0}, std::pair{0.2, 0.999}})
    {
        const auto [sr, sz] = reference::sheetSolenoidField(0.5, 2.0, 100, 1.0, rho, z, 20000);
        const auto b = ms::fieldAt({sol}, {rho, 0.0, z}).magneticFluxDensity;
        CHECK_NEAR(b.z, sz, 1e-6, 1e-9 * std::hypot(sr, sz));
        CHECK_NEAR(b.x, sr, 1e-6, 1e-9 * std::hypot(sr, sz));
    }
    // On the axis: the textbook formula, exactly.
    for (double z : {0.0, 0.7, -1.3, 4.0})
    {
        const double zp = z + 1.0, zm = z - 1.0;
        const double expected = k::mu0 * 50.0 / 2.0 * (zp / std::hypot(zp, 0.5) - zm / std::hypot(zm, 0.5));
        CHECK_REL(ms::fieldAt({sol}, {0.0, 0.0, z}).magneticFluxDensity.z, expected, 1e-12);
    }
    // Few turns: computed turn by turn, as the sum of exact loops.
    sol.turns = 5;
    ms::Source turn = loop;
    turn.radius = 0.5;
    turn.strength = 1.0;
    Vec3 sum;
    for (int i = 0; i < 5; ++i)
    {
        turn.position = {0.0, 0.0, -1.0 + (i + 0.5) * 0.4};
        sum += ms::fieldAt({turn}, {0.3, 0.2, 0.15}).magneticFluxDensity;
    }
    const auto five = ms::fieldAt({sol}, {0.3, 0.2, 0.15}).magneticFluxDensity;
    CHECK_REL(five.x, sum.x, 1e-13);
    CHECK_REL(five.z, sum.z, 1e-13);
}

TEST_CASE("magnetostatics: B is divergence-free and curl-free away from the currents", "[magnetostatics]")
{
    using T = ms::Source::Type;
    // Closed currents only: an open wire segment is not a static current distribution (its Biot-Savart
    // field is divergence-free but not curl-free).
    for (T type : {T::CircularLoop, T::Solenoid, T::HelmholtzPair, T::RectangularLoop, T::TriangularLoop})
    {
        ms::Source s;
        s.type = type;
        s.azimuthDeg = 30.0;
        s.elevationDeg = 60.0;
        for (const Vec3 p : {Vec3{0.1, 0.2, 0.05}, Vec3{0.9, -0.3, 0.4}, Vec3{-0.2, 0.7, 1.6}})
        {
            const double h = 1e-5;
            auto b = [&](Vec3 q) { return ms::fieldAt({s}, q).magneticFluxDensity; };
            const Vec3 dx{h, 0, 0}, dy{0, h, 0}, dz{0, 0, h};
            const Vec3 gx = (b(p + dx) - b(p - dx)) / (2 * h), gy = (b(p + dy) - b(p - dy)) / (2 * h),
                       gz = (b(p + dz) - b(p - dz)) / (2 * h);
            const double scale = b(p).norm() / 0.1; // field over a typical length
            CHECK_NEAR(gx.x + gy.y + gz.z, 0.0, 0.0, 1e-5 * scale);
            CHECK_NEAR(gy.z - gz.y, 0.0, 0.0, 1e-5 * scale);
            CHECK_NEAR(gz.x - gx.z, 0.0, 0.0, 1e-5 * scale);
            CHECK_NEAR(gx.y - gy.x, 0.0, 0.0, 1e-5 * scale);
        }
    }
}
