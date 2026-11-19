// Wires and conducting surfaces in one system (qsig::mom): the solver that replaces the hybrid
// wire + RWG solver of 6.14.

#include "qsig_test.h"

#include "qsig/constants.h"
#include "qsig/moment_method.h"
#include "qsig/numerical_em_solvers.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

namespace k = qsig::constants;
namespace mom = qsig::mom;
using qsig::Vec3;

namespace
{
constexpr double kFrequency = 300e6;
const double kLambda = k::c0 / kFrequency;

// Radiated power over delivered power, and directivity, on a 3-degree grid.
struct Balance
{
    double powerRatio = 0.0, directivityDbi = 0.0;
};

Balance balance(const mom::Result &r)
{
    const mom::Radiator radiator(r, 2.0 * k::pi / kLambda);
    const int nt = 60, np = 120;
    double integral = 0.0, peak = 0.0;
    for (int t = 0; t < nt; ++t)
        for (int p = 0; p < np; ++p)
        {
            const double theta = (t + 0.5) * k::pi / nt, phi = (p + 0.5) * 2.0 * k::pi / np;
            const double v = radiator.intensity({std::sin(theta) * std::cos(phi), std::sin(theta) * std::sin(phi), std::cos(theta)});
            integral += v * std::sin(theta) * (k::pi / nt) * (2.0 * k::pi / np);
            peak = std::max(peak, v);
        }
    double delivered = 0.0;
    for (const auto &f : r.feeds) delivered += 0.5 * std::real(f.voltage * std::conj(f.current));
    return {radiator.power(integral) / delivered, 10.0 * std::log10(4.0 * k::pi * peak / integral)};
}

// Quarter-wave monopole (radius lambda / 1000) at the centre of a square plate, fed at its foot.
mom::Input monopoleOnPlate(double sideLambda, double cellsPerLambda)
{
    mom::Input in;
    in.frequency = kFrequency;
    in.wires = {{{0.0, 0.0, 0.0}, {0.0, 0.0, 0.25 * kLambda}, kLambda / 1000.0, 0}};
    in.triangles = mom::meshRectangle({0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, sideLambda * kLambda, sideLambda * kLambda,
                                      kLambda / cellsPerLambda, {{0.0, 0.0, 0.0}});
    in.feeds = {{{0.0, 0.0, 0.0}, {1.0, 0.0}, false}};
    in.segmentsPerWavelength = 60;
    return in;
}

// Brute force for the 1/R integrals: midpoint rule on 4^depth sub-triangles.
void subdivided(const Vec3 &a, const Vec3 &b, const Vec3 &c, const Vec3 &r, const Vec3 &rho, int depth, double &inverseR, Vec3 &vectorOverR)
{
    if (depth == 0)
    {
        const Vec3 m = (a + b + c) / 3.0;
        const double weight = 0.5 * qsig::norm(qsig::cross(b - a, c - a)) / qsig::norm(r - m);
        inverseR += weight;
        vectorOverR += (m - rho) * weight;
        return;
    }
    const Vec3 ab = (a + b) * 0.5, bc = (b + c) * 0.5, ca = (c + a) * 0.5;
    subdivided(a, ab, ca, r, rho, depth - 1, inverseR, vectorOverR);
    subdivided(ab, b, bc, r, rho, depth - 1, inverseR, vectorOverR);
    subdivided(ca, bc, c, r, rho, depth - 1, inverseR, vectorOverR);
    subdivided(ab, bc, ca, r, rho, depth - 1, inverseR, vectorOverR);
}
} // namespace

TEST_CASE("surfaces: the 1/R integrals over a triangle, against a fine subdivision and across its plane", "[surfaces]")
{
    const Vec3 a{0.0, 0.0, 0.0}, b{1.0, 0.1, 0.0}, c{0.3, 0.8, 0.0};
    for (const Vec3 r : {Vec3{0.4, 0.3, 0.2}, Vec3{1.5, 0.2, 0.3}, Vec3{-0.3, -0.2, 0.1}, Vec3{0.0, 0.0, 0.1}, Vec3{2.0, 0.2, 0.0}})
    {
        double exact = 0.0, brute = 0.0;
        Vec3 exactVector, bruteVector;
        mom::staticTriangleIntegrals(a, b, c, r, exact, exactVector);
        subdivided(a, b, c, r, {r.x, r.y, 0.0}, 6, brute, bruteVector);
        CHECK_REL(exact, brute, 2e-4);
        CHECK(qsig::norm(exactVector - bruteVector) < 2e-4);
    }
    // In the plane of the triangle the integrals are finite and continuous: inside, on an edge, at a vertex.
    for (const Vec3 r : {Vec3{0.4, 0.3, 0.0}, Vec3{0.5, 0.05, 0.0}, Vec3{0.0, 0.0, 0.0}})
    {
        double onPlane = 0.0, above = 0.0;
        Vec3 onPlaneVector, aboveVector;
        mom::staticTriangleIntegrals(a, b, c, r, onPlane, onPlaneVector);
        mom::staticTriangleIntegrals(a, b, c, r + Vec3{0.0, 0.0, 1e-7}, above, aboveVector);
        CHECK(std::isfinite(onPlane));
        CHECK_REL(onPlane, above, 1e-6);
        CHECK(qsig::norm(onPlaneVector - aboveVector) < 1e-6);
    }
}

TEST_CASE("surfaces: meshes of a rectangle and of a disc, with a vertex where a wire lands", "[surfaces]")
{
    auto area = [](const std::vector<mom::Triangle> &mesh)
    {
        double sum = 0.0;
        for (const auto &t : mesh) sum += 0.5 * qsig::norm(qsig::cross(t.b - t.a, t.c - t.a));
        return sum;
    };
    auto hasVertex = [](const std::vector<mom::Triangle> &mesh, const Vec3 &p)
    {
        return std::any_of(mesh.begin(), mesh.end(), [&](const mom::Triangle &t)
                           { return qsig::norm(t.a - p) < 1e-12 || qsig::norm(t.b - p) < 1e-12 || qsig::norm(t.c - p) < 1e-12; });
    };
    const Vec3 centre{0.2, -0.1, 0.5}, u{1.0, 0.0, 0.0}, v{0.0, 0.0, 1.0};
    const Vec3 landing = centre + u * 0.137 + v * (-0.061);
    const auto plain = mom::meshRectangle(centre, u, v, 0.8, 0.5, 0.1);
    CHECK(plain.size() == 2u * 8u * 5u);
    CHECK_REL(area(plain), 0.4, 1e-12);
    CHECK(!hasVertex(plain, landing));
    const auto pierced = mom::meshRectangle(centre, u, v, 0.8, 0.5, 0.1, {landing, centre + u * 2.0}, 7); // the second one is outside
    CHECK_REL(area(pierced), 0.4, 1e-12);
    CHECK(hasVertex(pierced, landing));
    CHECK(pierced.size() > plain.size());
    CHECK(pierced.front().tag == 7);

    // Disc: 6 n^2 triangles for n rings, the area of the inscribed polygon of 6 n sides.
    const auto disc = mom::meshDisc(centre, u, v, 0.5, 0.1);
    CHECK(disc.size() == 6u * 25u);
    CHECK_REL(area(disc), 0.5 * 30.0 * 0.25 * std::sin(2.0 * k::pi / 30.0), 1e-12);
    CHECK(hasVertex(disc, centre));
    const Vec3 offCentre = centre + u * 0.23 + v * 0.11;
    const auto moved = mom::meshDisc(centre, u, v, 0.5, 0.1, {offCentre});
    CHECK(hasVertex(moved, offCentre));
    CHECK(moved.size() == disc.size());
    CHECK_REL(area(moved), area(disc), 1e-12); // an inner vertex moved: the same disc

    // Surfaces of revolution, the skins of the solids: closed (every edge belongs to two triangles), of
    // the area of the solid inscribed, with a vertex where a wire lands.
    auto closed = [](const std::vector<mom::Triangle> &mesh)
    {
        // Every edge, by the indices of its ends among the distinct vertices.
        std::vector<Vec3> seen;
        auto indexOf = [&seen](const Vec3 &p)
        {
            for (std::size_t k = 0; k < seen.size(); ++k)
                if (qsig::norm(seen[k] - p) < 1e-9) return k;
            seen.push_back(p);
            return seen.size() - 1;
        };
        std::vector<std::pair<std::size_t, std::size_t>> edges;
        for (const mom::Triangle &t : mesh)
        {
            const std::size_t ia = indexOf(t.a), ib = indexOf(t.b), ic = indexOf(t.c);
            if (ia == ib || ib == ic || ia == ic) return false; // a flat triangle
            edges.push_back(std::minmax(ia, ib));
            edges.push_back(std::minmax(ib, ic));
            edges.push_back(std::minmax(ia, ic));
        }
        std::sort(edges.begin(), edges.end());
        for (std::size_t k = 0; k < edges.size(); k += 2)
            if (k + 1 >= edges.size() || edges[k] != edges[k + 1] || (k + 2 < edges.size() && edges[k + 2] == edges[k])) return false;
        return true;
    };
    const Vec3 w = qsig::cross(u, v);
    const double R = 0.25, H = 0.6;
    const auto cylinder = mom::meshRevolution(centre, u, v, w, {{0.0, -0.5 * H}, {R, -0.5 * H}, {R, 0.5 * H}, {0.0, 0.5 * H}}, 0.1, {}, 3);
    CHECK(closed(cylinder));
    CHECK(cylinder.front().tag == 3);
    // 16 sides (2 pi R / 0.1 = 15.7): the prism inscribed, its two ends in three rings.
    const double side16 = 2.0 * R * std::sin(k::pi / 16.0), ends16 = 0.5 * 16.0 * R * R * std::sin(2.0 * k::pi / 16.0);
    CHECK_REL(area(cylinder), 16.0 * side16 * H + 2.0 * ends16, 0.02);
    CHECK(hasVertex(cylinder, centre + w * (0.5 * H)));
    CHECK(hasVertex(cylinder, centre + u * R - w * (0.5 * H)));
    const auto cone = mom::meshRevolution(centre, u, v, w, {{0.0, -0.5 * H}, {R, -0.5 * H}, {0.0, 0.5 * H}}, 0.1);
    CHECK(closed(cone));
    CHECK_REL(area(cone), k::pi * R * R + k::pi * R * std::hypot(R, H), 0.05);
    CHECK(hasVertex(cone, centre + w * (0.5 * H)));
    std::vector<std::pair<double, double>> meridian;
    const int bands = 8; // pi R / 0.1 = 7.85
    for (int q = 0; q <= bands; ++q)
        meridian.push_back({q == 0 || q == bands ? 0.0 : R * std::sin(k::pi * q / bands), -R * std::cos(k::pi * q / bands)});
    const auto sphere = mom::meshRevolution(centre, u, v, w, meridian, 0.1);
    CHECK(closed(sphere));
    CHECK_REL(area(sphere), 4.0 * k::pi * R * R, 0.06);
    for (const mom::Triangle &t : sphere) CHECK_REL(qsig::norm(t.a - centre), R, 1e-12);
    // A wire lands on the wall of the cylinder: the nearest vertex moves there, the skin stays closed.
    const Vec3 onWall = centre + (u * std::cos(0.3) + v * std::sin(0.3)) * R + w * 0.07;
    const auto landed = mom::meshRevolution(centre, u, v, w, {{0.0, -0.5 * H}, {R, -0.5 * H}, {R, 0.5 * H}, {0.0, 0.5 * H}}, 0.1,
                                            {onWall, centre + u * 3.0}); // the second one is far from the skin
    CHECK(hasVertex(landed, onWall));
    CHECK(closed(landed));
    CHECK(landed.size() == cylinder.size());
}

TEST_CASE("surfaces: wires alone, the impedance of the wire solver", "[surfaces]")
{
    // The same integrals, another assembly: the two solvers must agree far below the accuracy of either.
    auto both = [](const std::vector<mom::Wire> &wires, const Vec3 &feed, std::complex<double> &fresh, std::complex<double> &reference)
    {
        mom::Input in;
        in.frequency = kFrequency;
        in.wires = wires;
        in.feeds = {{feed, {1.0, 0.0}, false}};
        in.segmentsPerWavelength = 100;
        const mom::Result r = mom::solve(in);
        NumericalEM::WireNetworkMomInput old;
        old.frequencyHz = kFrequency;
        old.segmentsPerWavelength = 100;
        old.maxUnknowns = 1500;
        old.maxSegmentLengthRadiusFactor = 50.0;
        old.computeFarField = false;
        for (const auto &w : wires) old.wires.push_back({"w", w.a, w.b, w.radius});
        old.feeds.push_back({"f", feed, {1.0, 0.0}, 50.0});
        const auto o = NumericalEM::solveWireNetworkMom(old);
        REQUIRE(r.valid);
        REQUIRE(o.valid);
        CHECK(r.surfaceUnknowns == 0);
        fresh = r.feeds.front().impedance;
        reference = o.feeds.front().activeImpedanceOhm;
    };
    std::complex<double> z, ref;
    both({{{0.0, 0.0, -0.235 * kLambda}, {0.0, 0.0, 0.235 * kLambda}, 1e-3, 0}}, {0.0, 0.0, 0.0}, z, ref);
    CHECK(std::abs(z - ref) < 1e-8 * std::abs(ref));
    both({{{0.0, 0.0, -0.24 * kLambda}, {0.0, 0.0, 0.24 * kLambda}, 2e-3, 0},
          {{-0.2 * kLambda, 0.0, -0.25 * kLambda}, {-0.2 * kLambda, 0.0, 0.25 * kLambda}, 2e-3, 1},
          {{0.2 * kLambda, 0.0, -0.22 * kLambda}, {0.2 * kLambda, 0.0, 0.22 * kLambda}, 2e-3, 2}},
         {0.0, 0.0, 0.0}, z, ref);
    CHECK(std::abs(z - ref) < 1e-8 * std::abs(ref));
    // A branch: the wire solver refines its mesh around the junction, this one does not.
    both({{{0.0, 0.0, -0.25 * kLambda}, {0.0, 0.0, 0.25 * kLambda}, 1e-3, 0}, {{0.0, 0.0, 0.1 * kLambda}, {0.2 * kLambda, 0.0, 0.1 * kLambda}, 1e-3, 1}},
         {0.0, 0.0, -0.05 * kLambda}, z, ref);
    CHECK(std::abs(z - ref) < 5e-4 * std::abs(ref));
}

TEST_CASE("surfaces: a strip dipole is the wire dipole of a quarter of its width in radius", "[surfaces]")
{
    // Surface functions alone, the source across an edge of the mesh. A strip of width w radiates as a
    // wire of radius w / 4 (equivalent radius of a thin strip).
    const double width = 0.004, length = 0.47 * kLambda;
    mom::Input strip;
    strip.frequency = kFrequency;
    strip.triangles = mom::meshRectangle({0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {0.0, 0.0, 1.0}, width, length, length / 40.0);
    strip.feeds = {{{0.0, 0.0, 0.0}, {1.0, 0.0}, true}};
    const mom::Result s = mom::solve(strip);
    mom::Input wire;
    wire.frequency = kFrequency;
    wire.wires = {{{0.0, 0.0, -0.5 * length}, {0.0, 0.0, 0.5 * length}, 0.25 * width, 0}};
    wire.feeds = {{{0.0, 0.0, 0.0}, {1.0, 0.0}, false}};
    wire.segmentsPerWavelength = 100;
    const mom::Result w = mom::solve(wire);
    REQUIRE(s.valid);
    REQUIRE(w.valid);
    CHECK(s.wireUnknowns == 0);
    CHECK(std::abs(s.feeds.front().impedance - w.feeds.front().impedance) < 2.5); // 68.8 - j11.8 against 69.2 - j10.2
    const Balance bs = balance(s), bw = balance(w);
    CHECK_NEAR(bs.powerRatio, 1.0, 0.0, 1e-3);
    CHECK_NEAR(bw.powerRatio, 1.0, 0.0, 1e-3);
    CHECK_NEAR(bs.directivityDbi, bw.directivityDbi, 0.0, 0.02);
}

TEST_CASE("surfaces: monopole on a plate - mesh convergence, power balance, image theory", "[surfaces]")
{
    const mom::Result coarse = mom::solve(monopoleOnPlate(1.0, 8.0)), fine = mom::solve(monopoleOnPlate(1.0, 12.0));
    REQUIRE(coarse.valid);
    REQUIRE(fine.valid);
    CHECK(coarse.attachments == 1);
    const auto zc = coarse.feeds.front().impedance, zf = fine.feeds.front().impedance;
    CHECK(std::abs(zc - zf) < 1.0); // 45.3 + j18.9 and 44.7 + j18.9 (6.14: 65 + j35 down to 56 + j9, never settling)
    CHECK_NEAR(zf.real(), 44.7, 0.0, 0.6);
    CHECK_NEAR(zf.imag(), 18.9, 0.0, 0.6);
    // Everything delivered at the foot of the wire is radiated (6.14: 0.78 to 0.92).
    CHECK_NEAR(balance(coarse).powerRatio, 1.0, 0.0, 2e-3);
    CHECK_NEAR(balance(fine).powerRatio, 1.0, 0.0, 2e-3);

    // On an endless plane the monopole is half the dipole of twice its length (image theory). A finite
    // plate oscillates around that value as its edges move: within 5 ohm at 1 and 1.5 wavelengths, far
    // from it at half a wavelength.
    mom::Input dipole;
    dipole.frequency = kFrequency;
    dipole.wires = {{{0.0, 0.0, -0.25 * kLambda}, {0.0, 0.0, 0.25 * kLambda}, kLambda / 1000.0, 0}};
    dipole.feeds = {{{0.0, 0.0, 0.0}, {1.0, 0.0}, false}};
    dipole.segmentsPerWavelength = 60;
    const mom::Result d = mom::solve(dipole);
    REQUIRE(d.valid);
    const auto image = 0.5 * d.feeds.front().impedance;
    CHECK(std::abs(zc - image) < 5.0);
    const mom::Result larger = mom::solve(monopoleOnPlate(1.5, 8.0)), small = mom::solve(monopoleOnPlate(0.5, 8.0));
    REQUIRE(larger.valid);
    REQUIRE(small.valid);
    CHECK(std::abs(larger.feeds.front().impedance - image) < 5.0);
    CHECK(zc.real() > image.real());                              // above at one wavelength,
    CHECK(larger.feeds.front().impedance.real() < image.real());  // below at one and a half
    CHECK(small.feeds.front().impedance.real() < 30.0);
}

TEST_CASE("surfaces: the junction function against a T junction of surface functions", "[surfaces]")
{
    // The same monopole twice on a small plate meshed finely (lambda / 40): a wire attached by the
    // junction function of Hwu and Wilton, and a strip of four times its radius joined to the plate
    // along an edge (three triangles around that edge). Two formulations, one antenna.
    const double width = 0.008, length = 0.25 * kLambda, side = 0.3 * kLambda, step = kLambda / 40.0;
    const int cells = 16;
    mom::Input strip;
    strip.frequency = kFrequency;
    strip.triangles = mom::meshRectangle({0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, side, side, step,
                                         {{-0.5 * width, 0.0, 0.0}, {0.5 * width, 0.0, 0.0}});
    const auto ribbon = mom::meshRectangle({0.0, 0.0, 0.5 * length}, {1.0, 0.0, 0.0}, {0.0, 0.0, 1.0}, width, length, length / cells);
    strip.triangles.insert(strip.triangles.end(), ribbon.begin(), ribbon.end());
    strip.feeds = {{{0.0, 0.0, length / cells}, {1.0, 0.0}, true}};
    const mom::Result s = mom::solve(strip);
    mom::Input wire;
    wire.frequency = kFrequency;
    wire.triangles = mom::meshRectangle({0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, side, side, step, {{0.0, 0.0, 0.0}});
    wire.wires = {{{0.0, 0.0, 0.0}, {0.0, 0.0, length}, 0.25 * width, 0}};
    wire.feeds = {{{0.0, 0.0, length / cells}, {1.0, 0.0}, false}};
    wire.segmentsPerWavelength = 4.0 * cells;
    const mom::Result w = mom::solve(wire);
    REQUIRE(s.valid);
    REQUIRE(w.valid);
    CHECK(w.attachments == 1);
    CHECK(s.attachments == 0);
    const auto zs = s.feeds.front().impedance, zw = w.feeds.front().impedance;
    CHECK_NEAR(zw.real(), zs.real(), 0.0, 0.3); // 22.96 and 22.92 ohm
    CHECK_NEAR(zw.imag(), zs.imag(), 0.0, 1.5); // 5.2 and 6.3 ohm; they meet at lambda / 80 (5.5 ohm)
}

TEST_CASE("surfaces: a plate in the plane of symmetry of a dipole carries no current", "[surfaces]")
{
    // The field of a dipole has no component along its equatorial plane: a conducting plate laid there,
    // whatever its shape, changes nothing - even joined to the wire. Every sign of the coupling between
    // wires, junction and surface functions has to be right for the currents to cancel.
    auto dipole = [](bool plate)
    {
        mom::Input in;
        in.frequency = kFrequency;
        const double half = 0.24 * kLambda, gap = kLambda / 50.0;
        in.wires = {{{0.0, 0.0, -half}, {0.0, 0.0, 0.0}, 1e-3, 0}, {{0.0, 0.0, 0.0}, {0.0, 0.0, half}, 1e-3, 0}};
        in.feeds = {{{0.0, 0.0, gap}, {1.0, 0.0}, false}, {{0.0, 0.0, -gap}, {1.0, 0.0}, false}};
        in.segmentsPerWavelength = 100;
        if (plate) in.triangles = mom::meshRectangle({0.07, -0.04, 0.0}, {1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, 0.8, 0.6, 0.1, {{0.0, 0.0, 0.0}});
        return mom::solve(in);
    };
    const mom::Result alone = dipole(false), through = dipole(true);
    REQUIRE(alone.valid);
    REQUIRE(through.valid);
    CHECK(through.attachments == 1);
    CHECK(through.surfaceUnknowns > 100);
    for (std::size_t f = 0; f < 2; ++f)
        CHECK(std::abs(through.feeds[f].impedance - alone.feeds[f].impedance) < 1e-9 * std::abs(alone.feeds[f].impedance));
    double wireCurrent = 0.0, surfaceCurrent = 0.0;
    for (const auto &s : through.segments) wireCurrent = std::max(wireCurrent, std::abs(s.i0));
    for (const auto &facet : through.facets)
        for (const auto &j : {facet.ja, facet.jb, facet.jc})
            surfaceCurrent = std::max(surfaceCurrent, std::sqrt(std::norm(j.x) + std::norm(j.y) + std::norm(j.z)));
    CHECK(wireCurrent > 1e-2);
    CHECK(surfaceCurrent < 1e-9 * wireCurrent / kLambda);
}

TEST_CASE("surfaces: sources and wires the solver refuses, with the reason", "[surfaces]")
{
    mom::Input in = monopoleOnPlate(0.5, 6.0);
    REQUIRE(mom::solve(in).valid);
    // The foot of the wire a centimetre above the plate: a free end, no current through the source.
    in.wires[0].a.z = 0.01;
    in.feeds[0].position.z = 0.01;
    mom::Result r = mom::solve(in);
    CHECK(!r.valid);
    CHECK(r.error.find("bout libre") != std::string::npos);
    // On the plate, but away from the vertices of its mesh.
    in = monopoleOnPlate(0.5, 6.0);
    in.wires[0].a = {0.013, 0.021, 0.0};
    in.wires[0].b = {0.013, 0.021, 0.25 * kLambda};
    in.feeds[0].position = in.wires[0].a;
    r = mom::solve(in);
    CHECK(!r.valid);
    CHECK(r.error.find("sommet") != std::string::npos);
    // A second wire from the same foot: three conductors meet where the source is.
    in = monopoleOnPlate(0.5, 6.0);
    in.wires.push_back({{0.0, 0.0, 0.0}, {0.1, 0.0, 0.2}, kLambda / 1000.0, 1});
    r = mom::solve(in);
    CHECK(!r.valid);
    CHECK(r.error.find("plus de deux") != std::string::npos);
    // Too fine a mesh for the limit.
    in = monopoleOnPlate(1.0, 12.0);
    in.maxUnknowns = 200;
    r = mom::solve(in);
    CHECK(!r.valid);
    CHECK(r.error.find("Trop d'inconnues") != std::string::npos);
}

TEST_CASE("surfaces: the curl operator of a triangle against brute force, in air and in a lossy medium", "[surfaces]")
{
    using C = std::complex<double>;
    // out[j] = (r - p_j) x ∫ (r' - p_j) (1 + jkR) exp(-jkR) / (4 pi R^3) dS', by 2 x 200^2 points.
    const Vec3 a{0.0, 0.0, 0.0}, b{0.1, 0.01, 0.0}, c{0.03, 0.09, 0.0};
    auto brute = [&](const Vec3 &r, C k, mom::ComplexVec3 out[3])
    {
        const int n = 200;
        const C jk = C(0.0, 1.0) * k;
        const Vec3 p[3] = {a, b, c};
        const double area = 0.5 * qsig::norm(qsig::cross(b - a, c - a));
        C sum[3][3] = {};
        for (int i = 0; i < n; ++i)
            for (int j = 0; j < n - i; ++j)
                for (int half = 0; half < 2; ++half)
                {
                    if (half == 1 && j == n - i - 1) continue;
                    const double u = (i + (half ? 2.0 : 1.0) / 3.0) / n, v = (j + (half ? 2.0 : 1.0) / 3.0) / n;
                    const Vec3 q = a + (b - a) * u + (c - a) * v;
                    const double range = qsig::norm(r - q);
                    const C g = (1.0 + jk * range) * std::exp(-jk * range) / (4.0 * k::pi * range * range * range) * (area / (double(n) * n));
                    for (int m = 0; m < 3; ++m)
                    {
                        const Vec3 d = q - p[m];
                        sum[m][0] += g * d.x;
                        sum[m][1] += g * d.y;
                        sum[m][2] += g * d.z;
                    }
                }
        for (int m = 0; m < 3; ++m)
        {
            const Vec3 e = r - p[m];
            out[m] = {e.y * sum[m][2] - e.z * sum[m][1], e.z * sum[m][0] - e.x * sum[m][2], e.x * sum[m][1] - e.y * sum[m][0]};
        }
    };
    // Above the triangle, beside it close to its plane, far away (the three branches of the integral).
    for (const C wavenumber : {C(2.0 * k::pi, 0.0), C(12.5, -0.3)})
        for (const Vec3 &r : {Vec3{0.04, 0.03, 0.02}, Vec3{-0.02, 0.05, 0.002}, Vec3{0.05, -0.01, 0.01}, Vec3{0.15, 0.05, 0.03}, Vec3{0.5, 0.2, 0.3}})
        {
            mom::ComplexVec3 exact[3], rough[3];
            mom::curlTriangleIntegrals(a, b, c, r, wavenumber, exact);
            brute(r, wavenumber, rough);
            double worst = 0.0, scale = 0.0;
            for (int m = 0; m < 3; ++m)
            {
                worst = std::max(worst, std::max(std::abs(exact[m].x - rough[m].x), std::max(std::abs(exact[m].y - rough[m].y), std::abs(exact[m].z - rough[m].z))));
                scale = std::max(scale, std::max(std::abs(rough[m].x), std::max(std::abs(rough[m].y), std::abs(rough[m].z))));
            }
            CHECK(worst < 2e-4 * scale);
        }
    // In its plane the principal value is across the triangle: nothing along it.
    mom::ComplexVec3 inPlane[3];
    mom::curlTriangleIntegrals(a, b, c, Vec3{0.2, 0.1, 0.0}, C(2.0 * k::pi, 0.0), inPlane);
    for (int m = 0; m < 3; ++m) CHECK(std::abs(inPlane[m].x) + std::abs(inPlane[m].y) < 1e-15);
}

TEST_CASE("surfaces: a dielectric sphere in a plane wave against the Mie series, a dipole beside it", "[surfaces]")
{
    using C = std::complex<double>;
    const double f0 = 300e6, lambda = k::c0 / f0, wavenumber = 2.0 * k::pi / lambda;
    // The skin of a sphere of radius a, in triangles of 'step', blown up to the area of the sphere (the
    // polyhedron inscribed is smaller).
    auto sphere = [&](const Vec3 &centre, double a, double step)
    {
        std::vector<std::pair<double, double>> meridian;
        const int bands = std::max(4, static_cast<int>(std::ceil(k::pi * a / step)));
        for (int q = 0; q <= bands; ++q) meridian.push_back({q == 0 || q == bands ? 0.0 : a * std::sin(k::pi * q / bands), -a * std::cos(k::pi * q / bands)});
        auto mesh = mom::meshRevolution({0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, 0.0, 1.0}, meridian, step);
        double area = 0.0;
        for (const mom::Triangle &t : mesh) area += 0.5 * qsig::norm(qsig::cross(t.b - t.a, t.c - t.a));
        const double blow = std::sqrt(4.0 * k::pi * a * a / area);
        for (mom::Triangle &t : mesh)
        {
            t.a = centre + t.a * blow;
            t.b = centre + t.b * blow;
            t.c = centre + t.c * blow;
            t.body = 0;
        }
        return mesh;
    };
    // Scattering and backscattering efficiencies of a sphere of size parameter x and index m (Mie).
    auto mie = [](double x, double m, double &scattering, double &backscattering)
    {
        // Spherical Bessel functions j_n and y_n for n = 0..21 (not every standard library has them):
        // y by its upward recurrence, j downward from far above the order (Miller), scaled by j_0.
        const int top = 21;
        auto bessel = [top](double z, std::vector<double> &j, std::vector<double> &y)
        {
            j.assign(static_cast<std::size_t>(top) + 1, 0.0);
            y.assign(static_cast<std::size_t>(top) + 1, 0.0);
            y[0] = -std::cos(z) / z;
            y[1] = -std::cos(z) / (z * z) - std::sin(z) / z;
            for (int n = 1; n < top; ++n) y[static_cast<std::size_t>(n) + 1] = (2 * n + 1) / z * y[static_cast<std::size_t>(n)] - y[static_cast<std::size_t>(n) - 1];
            const int start = top + 40;
            double above = 0.0, here = 1e-30;
            std::vector<double> raw(static_cast<std::size_t>(start) + 1, 0.0);
            raw[static_cast<std::size_t>(start)] = here;
            for (int n = start; n > 0; --n)
            {
                const double below = (2 * n + 1) / z * here - above;
                above = here;
                here = below;
                raw[static_cast<std::size_t>(n) - 1] = here;
            }
            const double scale = (std::sin(z) / z) / raw[0];
            for (int n = 0; n <= top; ++n) j[static_cast<std::size_t>(n)] = raw[static_cast<std::size_t>(n)] * scale;
        };
        std::vector<double> jOut, yOut, jIn, yIn;
        bessel(x, jOut, yOut);
        bessel(m * x, jIn, yIn);
        auto at = [](const std::vector<double> &values, int n) { return values[static_cast<std::size_t>(n)]; };
        // psi_n(z) = z j_n(z), xi_n(z) = z (j_n + i y_n)(z), and their derivatives z f_(n-1) - n f_n.
        auto psi = [&](int n, bool inner) { return (inner ? m * x : x) * at(inner ? jIn : jOut, n); };
        auto dpsi = [&](int n, bool inner) { return (inner ? m * x : x) * at(inner ? jIn : jOut, n - 1) - n * at(inner ? jIn : jOut, n); };
        auto xi = [&](int n) { return C(x * at(jOut, n), x * at(yOut, n)); };
        auto dxi = [&](int n) { return C(x * at(jOut, n - 1) - n * at(jOut, n), x * at(yOut, n - 1) - n * at(yOut, n)); };
        C back = 0.0;
        scattering = 0.0;
        for (int n = 1; n <= 20; ++n)
        {
            const C an = (m * psi(n, true) * dpsi(n, false) - psi(n, false) * dpsi(n, true)) / (m * psi(n, true) * dxi(n) - xi(n) * dpsi(n, true));
            const C bn = (psi(n, true) * dpsi(n, false) - m * psi(n, false) * dpsi(n, true)) / (psi(n, true) * dxi(n) - m * xi(n) * dpsi(n, true));
            scattering += (2 * n + 1) * (std::norm(an) + std::norm(bn));
            back += double(2 * n + 1) * (n % 2 ? -1.0 : 1.0) * (an - bn);
        }
        scattering *= 2.0 / (x * x);
        backscattering = std::norm(back) / (x * x);
    };
    // Integral of the intensity of the far field over the sphere of directions.
    auto wholeSphere = [](const mom::Radiator &radiator)
    {
        double integral = 0.0;
        const int nt = 24, np = 48;
        for (int i = 0; i < nt; ++i)
            for (int j = 0; j < np; ++j)
            {
                const double theta = k::pi * (i + 0.5) / nt, phi = 2.0 * k::pi * (j + 0.5) / np;
                integral += radiator.intensity({std::sin(theta) * std::cos(phi), std::sin(theta) * std::sin(phi), std::cos(theta)}) * std::sin(theta) *
                            (k::pi / nt) * (2.0 * k::pi / np);
            }
        return integral;
    };

    // Sphere of relative permittivity 4 (index 2), k a = 1, in a wave of 1 V/m going along z: the total
    // scattered power and the power sent back, in units of pi a^2, within 4 % of Mie with 8 triangles
    // per wavelength in the dielectric (the error falls to 1 % with 12).
    {
        const double a = 1.0 / wavenumber, epsilon = 4.0;
        mom::Input in;
        in.frequency = f0;
        in.triangles = sphere({0.0, 0.0, 0.0}, a, lambda / std::sqrt(epsilon) / 8.0);
        in.dielectrics.push_back({C(epsilon, 0.0)});
        in.incident.direction = {0.0, 0.0, 1.0};
        in.incident.field = {1.0, 0.0, 0.0};
        const mom::Result r = mom::solve(in);
        REQUIRE(r.valid);
        CHECK(r.magneticUnknowns * 2 == r.unknowns);
        CHECK(r.magneticMoments.size() == r.moments.size());
        const mom::Radiator radiator(r, wavenumber);
        const double unit = k::pi * a * a;
        const double scattered = 2.0 * k::eta0 * radiator.power(wholeSphere(radiator)) / unit;
        const double back = wavenumber * wavenumber * k::eta0 * k::eta0 / (4.0 * k::pi) * radiator.intensity({0.0, 0.0, -1.0}) / unit;
        double scatteredMie = 0.0, backMie = 0.0;
        mie(1.0, 2.0, scatteredMie, backMie);
        CHECK_REL(scatteredMie, 0.7968, 1e-3); // the series itself
        CHECK_REL(scattered, scatteredMie, 0.04);
        CHECK_REL(back, backMie, 0.04);
    }

    // A half-wave dipole at 0.3 wavelength of the centre of a sphere of 0.16 wavelength of radius.
    auto beside = [&](C epsilon, bool withSphere, double &balance)
    {
        mom::Input in;
        in.frequency = f0;
        in.wires.push_back({{0.0, 0.0, -0.235 * lambda}, {0.0, 0.0, 0.235 * lambda}, 1e-3 * lambda, 0});
        in.feeds.push_back({{0.0, 0.0, 0.0}, {1.0, 0.0}, false});
        if (withSphere)
        {
            in.triangles = sphere({0.3 * lambda, 0.0, 0.0}, 0.16 * lambda, 0.06 * lambda);
            in.dielectrics.push_back({epsilon});
        }
        const mom::Result r = mom::solve(in);
        balance = 0.0;
        if (!r.valid) return C{};
        const mom::Radiator radiator(r, wavenumber);
        balance = radiator.power(wholeSphere(radiator)) / (0.5 * std::real(r.feeds[0].voltage * std::conj(r.feeds[0].current)));
        return r.feeds[0].impedance;
    };
    double balance = 0.0;
    const C alone = beside(1.0, false, balance);
    CHECK_NEAR(balance, 1.0, 0.0, 2e-3);
    // A body of the permittivity of air is not there: the wires, the electric and the magnetic currents
    // of the skin cancel exactly.
    const C transparent = beside(C(1.0001, 0.0), true, balance);
    CHECK(std::abs(transparent - alone) < 0.05);
    // Permittivity 4: the dipole sees the sphere, and all the power fed is radiated.
    const C lossless = beside(C(4.0, 0.0), true, balance);
    CHECK(std::abs(lossless - alone) > 3.0);
    CHECK_NEAR(balance, 1.0, 0.0, 2e-3);
    // With losses (tan delta = 0.1), a part of it heats the sphere.
    beside(C(4.0, -0.4), true, balance);
    CHECK(balance > 0.95);
    CHECK(balance < 0.99);

    // What is refused, with its reason: a conductor that touches the dielectric, an open skin.
    {
        mom::Input in;
        in.frequency = f0;
        in.triangles = sphere({0.0, 0.0, 0.0}, 0.16 * lambda, 0.08 * lambda);
        in.dielectrics.push_back({C(4.0, 0.0)});
        in.incident.field = {1.0, 0.0, 0.0};
        mom::Input touching = in;
        const mom::Triangle first = in.triangles.front();
        touching.triangles.push_back({first.a, first.b, first.a + (first.a + first.b) * 0.5, 1, -1}); // a conducting fin on an edge
        const mom::Result fin = mom::solve(touching);
        CHECK(!fin.valid);
        CHECK(fin.error.find("touche") != std::string::npos);
        mom::Input open = in;
        open.triangles.pop_back();
        const mom::Result holed = mom::solve(open);
        CHECK(!holed.valid);
        CHECK(holed.error.find("ferm") != std::string::npos);
    }
}

TEST_CASE("surfaces: a profile swept along a direction - flat, parabolic, with a vertex under a wire", "[surfaces]")
{
    auto area = [](const std::vector<mom::Triangle> &mesh)
    {
        double total = 0.0;
        for (const mom::Triangle &t : mesh) total += 0.5 * qsig::norm(qsig::cross(t.b - t.a, t.c - t.a));
        return total;
    };
    // A straight profile of 0.8 swept over 0.5: the rectangle, in cells of 0.1 at most.
    const auto flat = mom::meshSweep({Vec3{-0.4, 0.0, 0.0}, Vec3{0.4, 0.0, 0.0}}, {0.0, 1.0, 0.0}, 0.5, 0.1, {}, 4);
    CHECK(flat.size() == 2u * 8u * 5u);
    CHECK_REL(area(flat), 0.4, 1e-12);
    CHECK(flat.front().tag == 4);
    // A parabola z = x^2 / (4 f), f = 0.5, x from -1 to 1, swept over 0.6: its arc length times 0.6.
    std::vector<Vec3> parabola;
    for (int q = -40; q <= 40; ++q)
    {
        const double x = q / 40.0;
        parabola.push_back({x, 0.0, x * x / 2.0});
    }
    const double arc = 2.0 * 0.5 * (std::sqrt(2.0) + std::asinh(1.0)); // 2 f (t sqrt(1 + t^2) + asinh t), t = 1
    CHECK_REL(area(mom::meshSweep(parabola, {0.0, 1.0, 0.0}, 0.6, 0.08)), 0.6 * arc, 1e-3);
    // A wire lands on the surface: the nearest vertex moves there; a point far from it changes nothing.
    const Vec3 landing{0.13, 0.07, 0.0};
    const auto pierced = mom::meshSweep({Vec3{-0.4, 0.0, 0.0}, Vec3{0.4, 0.0, 0.0}}, {0.0, 1.0, 0.0}, 0.5, 0.1, {landing, Vec3{3.0, 0.0, 0.0}});
    CHECK(pierced.size() == flat.size());
    CHECK(std::any_of(pierced.begin(), pierced.end(), [&](const mom::Triangle &t) { return qsig::norm(t.a - landing) < 1e-12 || qsig::norm(t.b - landing) < 1e-12 || qsig::norm(t.c - landing) < 1e-12; }));
}

