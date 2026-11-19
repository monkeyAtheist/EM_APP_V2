// Exact plate / disk / slab kernels (phase 1) against independent high-order quadratures.
// The references tile the exact geometry (no staircase at the edges) and are only used at
// distances where Gauss-Legendre on the sub-cells is converged.

#include "qsig_test.h"

#include "qsig/electrostatics.h"
#include "qsig/sheet_integrals.h"

#include <array>
#include <functional>

namespace es = Electrostatics;
namespace k = qsig::constants;
using qsig::Vec3;

namespace
{
constexpr std::array<double, 4> kGx{-0.8611363115940526, -0.3399810435848563, 0.3399810435848563, 0.8611363115940526};
constexpr std::array<double, 4> kGw{0.3478548451374538, 0.6521451548625461, 0.6521451548625461, 0.3478548451374538};

struct Ref
{
    Vec3 field;
    double potential = 0.0;
};

// Integrates a unit-density source given by a map (u, v) in [0,1]^2 -> (point, jacobian).
Ref tensorQuadrature(const std::function<void(double, double, Vec3 &, double &)> &map, const Vec3 &p, int cells)
{
    Ref out;
    const double h = 1.0 / cells;
    for (int i = 0; i < cells; ++i)
        for (int j = 0; j < cells; ++j)
            for (int a = 0; a < 4; ++a)
                for (int b = 0; b < 4; ++b)
                {
                    const double u = (i + 0.5 + 0.5 * kGx[a]) * h, v = (j + 0.5 + 0.5 * kGx[b]) * h;
                    Vec3 r;
                    double jac = 0.0;
                    map(u, v, r, jac);
                    const double w = kGw[a] * kGw[b] * 0.25 * h * h * jac;
                    const Vec3 d = p - r;
                    const double R = d.norm();
                    out.field += d * (w / (R * R * R));
                    out.potential += w / R;
                }
    return out;
}

Ref rectangleRef(double W, double H, const Vec3 &p)
{
    return tensorQuadrature(
        [W, H](double u, double v, Vec3 &r, double &jac)
        {
            r = {(u - 0.5) * W, (v - 0.5) * H, 0.0};
            jac = W * H;
        },
        p, 120);
}

Ref triangleRef(const qsig::sheet::Point2 &A, const qsig::sheet::Point2 &B, const qsig::sheet::Point2 &C, const Vec3 &p)
{
    // Duffy map of the unit square onto the triangle: r = A + u (B - A) + u v (C - B).
    const double area2 = std::abs((B.x - A.x) * (C.y - B.y) - (B.y - A.y) * (C.x - B.x));
    return tensorQuadrature(
        [A, B, C, area2](double u, double v, Vec3 &r, double &jac)
        {
            r = {A.x + u * (B.x - A.x) + u * v * (C.x - B.x), A.y + u * (B.y - A.y) + u * v * (C.y - B.y), 0.0};
            jac = u * area2;
        },
        p, 120);
}

Ref annulusRef(double r1, double r2, const Vec3 &p)
{
    return tensorQuadrature(
        [r1, r2](double u, double v, Vec3 &r, double &jac)
        {
            const double rr = r1 + u * (r2 - r1), phi = 2.0 * k::pi * v;
            r = {rr * std::cos(phi), rr * std::sin(phi), 0.0};
            jac = (r2 - r1) * 2.0 * k::pi * rr;
        },
        p, 160);
}

void checkAgainst(const qsig::sheet::Integrals &g, const Ref &ref, double tol)
{
    const double scale = ref.field.norm();
    CHECK_NEAR(g.field.x, ref.field.x, 0.0, tol * scale);
    CHECK_NEAR(g.field.y, ref.field.y, 0.0, tol * scale);
    CHECK_NEAR(g.field.z, ref.field.z, 0.0, tol * scale);
    CHECK_REL(g.potential, ref.potential, tol);
}

const std::array<Vec3, 6> kProbes{Vec3{0.12, -0.07, 0.03}, Vec3{0.62, 0.21, 0.05}, Vec3{-0.3, 0.45, -0.02},
                                  Vec3{0.0, 0.0, 0.4},     Vec3{1.4, -0.9, 0.3},   Vec3{0.49, 0.29, 0.015}};
} // namespace

TEST_CASE("sheets: rectangle matches a converged quadrature everywhere", "[electrostatics][exact]")
{
    const std::vector<qsig::sheet::Point2> rect{{-0.5, -0.3}, {0.5, -0.3}, {0.5, 0.3}, {-0.5, 0.3}};
    for (const auto &p : kProbes) checkAgainst(qsig::sheet::convexPolygon(rect, p), rectangleRef(1.0, 0.6, p), 1e-7);
}

TEST_CASE("sheets: triangle matches a converged quadrature everywhere", "[electrostatics][exact]")
{
    const qsig::sheet::Point2 A{-0.5, -0.2}, B{0.5, -0.2}, C{0.0, 0.4};
    for (const auto &p : kProbes) checkAgainst(qsig::sheet::convexPolygon({A, B, C}, p), triangleRef(A, B, C, p), 1e-7);
}

TEST_CASE("sheets: disk and annulus match a converged quadrature everywhere", "[electrostatics][exact]")
{
    for (const auto &p : kProbes)
    {
        checkAgainst(qsig::sheet::annulus(0.0, 0.4, p), annulusRef(0.0, 0.4, p), 1e-7);
        checkAgainst(qsig::sheet::annulus(0.15, 0.4, p), annulusRef(0.15, 0.4, p), 1e-7);
    }
}

TEST_CASE("sheets: normal field jumps by sigma/eps0, everything else is continuous", "[electrostatics][exact]")
{
    const std::vector<qsig::sheet::Point2> rect{{-0.5, -0.3}, {0.5, -0.3}, {0.5, 0.3}, {-0.5, 0.3}};
    for (const auto &[x, y] : {std::pair{0.1, 0.05}, std::pair{0.45, -0.25}})
    {
        const auto above = qsig::sheet::convexPolygon(rect, {x, y, 1e-9});
        const auto below = qsig::sheet::convexPolygon(rect, {x, y, -1e-9});
        const auto on = qsig::sheet::convexPolygon(rect, {x, y, 0.0});
        CHECK_REL(above.field.z - below.field.z, 4.0 * k::pi, 1e-7); // sigma/eps0 once multiplied by 1/(4 pi eps0)
        CHECK(on.onSheet);
        CHECK_REL(on.field.x, above.field.x, 1e-6);
        CHECK_REL(on.potential, above.potential, 1e-7);
        CHECK_REL(below.potential, above.potential, 1e-7);

        const auto dAbove = qsig::sheet::annulus(0.1, 0.4, {x * 0.5, y * 0.5 + 0.2, 1e-9});
        const auto dBelow = qsig::sheet::annulus(0.1, 0.4, {x * 0.5, y * 0.5 + 0.2, -1e-9});
        CHECK_REL(dAbove.field.z - dBelow.field.z, 4.0 * k::pi, 1e-6);
        CHECK_REL(dAbove.potential, dBelow.potential, 1e-7);
    }
}

TEST_CASE("sheets: slab obeys Gauss's law inside and tends to a sheet when thin", "[electrostatics][exact]")
{
    const std::vector<qsig::sheet::Point2> rect{{-0.5, -0.3}, {0.5, -0.3}, {0.5, 0.3}, {-0.5, 0.3}};
    const double t = 0.1;
    // Flux through a small cube fully inside the slab equals its volume (unit density, 4 pi factor).
    const Vec3 c{0.1, -0.05, 0.01};
    const double s = 0.02;
    double flux = 0.0;
    for (int axis = 0; axis < 3; ++axis)
        for (double side : {-1.0, 1.0})
            for (int a = 0; a < 4; ++a)
                for (int b = 0; b < 4; ++b)
                {
                    Vec3 p = c;
                    double *coord[3] = {&p.x, &p.y, &p.z};
                    *coord[axis] += 0.5 * s * side;
                    *coord[(axis + 1) % 3] += 0.5 * s * kGx[a];
                    *coord[(axis + 2) % 3] += 0.5 * s * kGx[b];
                    const auto g = qsig::sheet::polygonSlab(rect, t, p);
                    const double fn = axis == 0 ? g.field.x : axis == 1 ? g.field.y : g.field.z;
                    flux += side * fn * kGw[a] * kGw[b] * 0.25 * s * s;
                }
    CHECK_REL(flux, 4.0 * k::pi * s * s * s, 1e-6);
    CHECK_NEAR(qsig::sheet::polygonSlab(rect, t, {0.1, 0.05, 0.0}).field.z, 0.0, 0.0, 1e-12); // mid-plane symmetry

    const Vec3 p{0.2, 0.1, 0.02};
    const auto thin = qsig::sheet::polygonSlab(rect, 1e-5, p);
    const auto sheet = qsig::sheet::convexPolygon(rect, p);
    CHECK_REL(thin.field.z / 1e-5, sheet.field.z, 1e-6);
    CHECK_REL(thin.potential / 1e-5, sheet.potential, 1e-6);
}

TEST_CASE("electrostatics: exact plates honour rotation and position", "[electrostatics][exact]")
{
    es::Source plate;
    plate.type = es::Source::Type::RectangularPlate;
    plate.position = {0.3, -0.2, 0.1};
    plate.angleDeg = 90.0; // 1 m x 0.6 m rotated: becomes 0.6 m along x, 1 m along y
    es::Source turned = plate;
    turned.angleDeg = 0.0;
    turned.width = 0.6;
    turned.height = 1.0;
    for (const Vec3 &p : {Vec3{0.5, 0.4, 0.15}, Vec3{-0.1, -0.6, 0.05}})
    {
        const auto a = es::fieldFromSource(plate, p), b = es::fieldFromSource(turned, p);
        CHECK_NEAR(a.electricField.x, b.electricField.x, 1e-12, 1e-12);
        CHECK_NEAR(a.electricField.y, b.electricField.y, 1e-12, 1e-12);
        CHECK_REL(a.potential, b.potential, 1e-12);
    }
}
