#include "qsig/sheet_integrals.h"

#include "qsig/constants.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace qsig::sheet
{
namespace
{
constexpr double kOnSurface = 1e-12; // m: closer than this to a sheet or an edge counts as "on" it

// Value integrated by the adaptive quadrature: (E_rho or E_x, E_y, E_z, V).
struct Sample
{
    double a = 0.0, b = 0.0, c = 0.0, v = 0.0;
    Sample &operator+=(const Sample &o)
    {
        a += o.a;
        b += o.b;
        c += o.c;
        v += o.v;
        return *this;
    }
};
Sample operator*(double s, const Sample &x)
{
    return {s * x.a, s * x.b, s * x.c, s * x.v};
}
Sample operator-(const Sample &x, const Sample &y)
{
    return {x.a - y.a, x.b - y.b, x.c - y.c, x.v - y.v};
}

// Gauss-Kronrod 7/15 nodes and weights on [-1, 1].
constexpr std::array<double, 8> kXgk{0.991455371120812639206854697526329, 0.949107912342758524526189684047851,
                                     0.864864423359769072789712788640926, 0.741531185599394439863864773280788,
                                     0.586087235467691130294144845693013, 0.405845151377397166906606412076961,
                                     0.207784955007898467600689403773245, 0.0};
constexpr std::array<double, 8> kWgk{0.022935322010529224963732008058970, 0.063092092629978553290700663189204,
                                     0.104790010322250183839876322541518, 0.140653259715525918745189590510238,
                                     0.169004726639267902826583426598550, 0.190350578064785409913256402421014,
                                     0.204432940075298892414161999234649, 0.209482141084727828012999174891714};
constexpr std::array<double, 4> kWg{0.129484966168869693270611432679082, 0.279705391489276667901467771423780,
                                    0.381830050505118944950369775488975, 0.417959183673469387755102040816327};

struct Segment
{
    double lo, hi;
    Sample value;
    double error;
};

template <class F> Segment gk15(const F &f, double lo, double hi)
{
    const double c = 0.5 * (lo + hi), h = 0.5 * (hi - lo);
    const Sample centre = f(c);
    Sample kronrod = kWgk[7] * centre, gauss = kWg[3] * centre;
    for (int i = 0; i < 7; ++i)
    {
        const Sample s = f(c - h * kXgk[i]);
        Sample t = f(c + h * kXgk[i]);
        t += s;
        kronrod += kWgk[i] * t;
        if (i % 2 == 1) gauss += kWg[i / 2] * t;
    }
    const Sample k = h * kronrod, diff = k - h * gauss;
    const double error = std::max({std::abs(diff.a), std::abs(diff.b), std::abs(diff.c), std::abs(diff.v)});
    return {lo, hi, k, error};
}

// Globally adaptive Gauss-Kronrod on [lo, hi] with optional interior break points.
template <class F> Sample integrate(const F &f, double lo, double hi, std::vector<double> breaks, double relTol)
{
    std::vector<Segment> segments;
    breaks.push_back(lo);
    breaks.push_back(hi);
    std::sort(breaks.begin(), breaks.end());
    for (std::size_t i = 0; i + 1 < breaks.size(); ++i)
        if (breaks[i] >= lo && breaks[i + 1] <= hi && breaks[i + 1] > breaks[i])
            segments.push_back(gk15(f, breaks[i], breaks[i + 1]));
    for (int iteration = 0; iteration < 600; ++iteration)
    {
        Sample total;
        double error = 0.0;
        std::size_t worst = 0;
        for (std::size_t i = 0; i < segments.size(); ++i)
        {
            total += segments[i].value;
            error += segments[i].error;
            if (segments[i].error > segments[worst].error) worst = i;
        }
        const double scale = std::max({std::abs(total.a), std::abs(total.b), std::abs(total.c), std::abs(total.v)});
        if (error <= relTol * scale || segments.empty()) return total;
        const Segment s = segments[worst];
        const double mid = 0.5 * (s.lo + s.hi);
        if (!(mid > s.lo && mid < s.hi)) return total; // interval exhausted (integrable singularity)
        segments[worst] = gk15(f, s.lo, mid);
        segments.push_back(gk15(f, mid, s.hi));
    }
    Sample total;
    for (const auto &s : segments) total += s.value;
    return total;
}

// log((R+ + l+) / (R- + l-)) along one edge, without cancellation.
double edgeLog(double lPlus, double lMinus, double r0Squared, bool &singular)
{
    const double rPlus = std::sqrt(lPlus * lPlus + r0Squared), rMinus = std::sqrt(lMinus * lMinus + r0Squared);
    if (lMinus >= 0.0) return std::log((rPlus + lPlus) / (rMinus + lMinus));
    if (lPlus <= 0.0) return std::log((rMinus - lMinus) / (rPlus - lPlus));
    if (r0Squared <= kOnSurface * kOnSurface)
    {
        singular = true;
        return 0.0;
    }
    return std::log((rPlus + lPlus) * (rMinus - lMinus) / r0Squared);
}
} // namespace

void ellipticKE(double k, double &K, double &E)
{
    ellipticKEc(k, std::sqrt(std::max(0.0, 1.0 - k * k)), K, E);
}

void ellipticKEc(double k, double kc, double &K, double &E)
{
    double a = 1.0, b = kc, c = k;
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
    K = constants::pi / (2.0 * a);
    E = K * (1.0 - sum);
}

Integrals convexPolygon(const std::vector<Point2> &v, const Vec3 &p)
{
    Integrals out;
    const std::size_t n = v.size();
    if (n < 3) return out;
    const double d = p.z, ad = std::abs(d);
    double sumPL = 0.0;
    bool inside = true;
    for (std::size_t i = 0; i < n; ++i)
    {
        const Point2 a = v[i], b = v[(i + 1) % n];
        const double ex = b.x - a.x, ey = b.y - a.y, len = std::hypot(ex, ey);
        if (len <= 0.0) continue;
        const double lx = ex / len, ly = ey / len;
        const double ux = ly, uy = -lx; // outward normal of a counter-clockwise polygon
        const double lPlus = (b.x - p.x) * lx + (b.y - p.y) * ly;
        const double lMinus = (a.x - p.x) * lx + (a.y - p.y) * ly;
        const double p0 = (a.x - p.x) * ux + (a.y - p.y) * uy; // > 0 when p projects inside
        inside = inside && p0 > 0.0;
        bool singular = false;
        const double L = edgeLog(lPlus, lMinus, p0 * p0 + d * d, singular);
        out.onEdge = out.onEdge || singular;
        out.field.x += ux * L;
        out.field.y += uy * L;
        sumPL += p0 * L;
    }

    // Solid angle of the polygon seen from p (fan of triangles, Van Oosterom & Strackee).
    double omega = 0.0;
    if (ad > kOnSurface)
    {
        const Vec3 r1{v[0].x - p.x, v[0].y - p.y, -d};
        const double n1 = r1.norm();
        for (std::size_t i = 1; i + 1 < n; ++i)
        {
            const Vec3 r2{v[i].x - p.x, v[i].y - p.y, -d}, r3{v[i + 1].x - p.x, v[i + 1].y - p.y, -d};
            const double n2 = r2.norm(), n3 = r3.norm();
            const double num = dot(r1, cross(r2, r3));
            const double den = n1 * n2 * n3 + dot(r1, r2) * n3 + dot(r1, r3) * n2 + dot(r2, r3) * n1;
            omega += 2.0 * std::atan2(num, den);
        }
        omega = std::abs(omega);
    }
    out.onSheet = ad <= kOnSurface && inside;
    out.field.z = (ad <= kOnSurface) ? 0.0 : std::copysign(omega, d);
    out.potential = sumPL - ad * omega;
    return out;
}

namespace
{
// Contribution of one boundary circle (radius a, centred on the origin) to the sheet integrals,
// observation point at in-plane distance rho on the +x axis, height z. By the divergence theorem
// in the plane, with s = |r' - P0|, R = sqrt(s^2 + z^2) and u the outward normal of the region:
//   potential   = contour of (r'-P0).u / (R + |z|) dl
//   solid angle = contour of (r'-P0).u / (R (R + |z|)) dl        (E_z = sign(z) * solid angle)
//   in-plane E  = contour of u / R dl
// Each integrand is smooth in phi except near phi = 0 when P0 is close to the circle, where the
// interval is graded geometrically. 'orientation' is +1 for an outer, -1 for an inner boundary.
Sample circleBoundary(double a, double orientation, double rho, double z)
{
    const double az = std::abs(z);
    auto integrand = [a, rho, z, az](double phi)
    {
        const double c = std::cos(phi);
        const double s2 = a * a + rho * rho - 2.0 * a * rho * c;
        const double R = std::sqrt(s2 + z * z);
        Sample out;
        if (!(R > 0.0)) return out;
        const double projected = (a - rho * c) * a; // (r'-P0).u dl / dphi
        out.a = c * a / R;                          // radial in-plane field
        out.c = projected / (R * (R + az));
        out.v = projected / (R + az);
        return out;
    };
    std::vector<double> breaks;
    const double width = std::hypot(a - rho, z) / std::max(a, 1e-300);
    for (double step = std::max(width, 1e-14); step < constants::pi; step *= 8.0) breaks.push_back(step);
    const Sample half = integrate(integrand, 0.0, constants::pi, breaks, 1e-12);
    return (2.0 * orientation) * half; // integrands are even in phi
}
} // namespace

Integrals annulus(double innerRadius, double outerRadius, const Vec3 &p)
{
    Integrals out;
    const double rho = std::hypot(p.x, p.y), z = p.z;
    const double r1 = std::max(0.0, innerRadius), r2 = outerRadius;
    if (!(r2 > r1)) return out;
    Sample total = circleBoundary(r2, 1.0, rho, z);
    if (r1 > 0.0) total += circleBoundary(r1, -1.0, rho, z);

    const bool inPlane = std::abs(z) <= kOnSurface;
    out.onSheet = inPlane && rho >= r1 && rho <= r2;
    out.onEdge = inPlane && ((r1 > 0.0 && std::abs(rho - r1) <= kOnSurface) || std::abs(rho - r2) <= kOnSurface);
    out.potential = total.v;
    out.field.z = inPlane ? 0.0 : std::copysign(std::abs(total.c), z);
    if (rho > 1e-12 * r2)
    {
        out.field.x = total.a * p.x / rho;
        out.field.y = total.a * p.y / rho;
    }
    return out;
}

Integrals polygonSlab(const std::vector<Point2> &v, double thickness, const Vec3 &p)
{
    Integrals out;
    const double h = 0.5 * thickness;
    if (!(h > 0.0)) return out;
    bool edge = false;
    auto layer = [&v, &p, &edge](double zs)
    {
        const Integrals s = convexPolygon(v, {p.x, p.y, p.z - zs});
        edge = edge || s.onEdge;
        return Sample{s.field.x, s.field.y, s.field.z, s.potential};
    };
    std::vector<double> breaks;
    if (p.z > -h && p.z < h) breaks.push_back(p.z);
    const Sample s = integrate(layer, -h, h, breaks, 1e-10);
    out.field = {s.a, s.b, s.c};
    out.potential = s.v;
    out.onEdge = edge;
    return out;
}
} // namespace qsig::sheet
