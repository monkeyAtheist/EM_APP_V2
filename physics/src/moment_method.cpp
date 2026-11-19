#include "qsig/moment_method.h"

#include "qsig/constants.h"
#include "qsig/fast_math.h"
#include "qsig/linalg.h"
#include "wire_pair_integrals.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <string>
#include <utility>

namespace qsig::mom
{
namespace
{
constexpr double kPi = qsig::constants::pi;
using Index = std::size_t;

// ---- Quadrature rules ----

// Gauss-Legendre on (0, 1).
struct Rule
{
    std::vector<double> x, w;
};

Rule gaussLegendre(int n)
{
    Rule out;
    for (int i = 0; i < n; ++i)
    {
        // Newton on the Legendre polynomial, from the Chebyshev guess; x on (-1, 1).
        double x = std::cos(kPi * (i + 0.75) / (n + 0.5));
        double derivative = 1.0;
        for (int it = 0; it < 60; ++it)
        {
            double p0 = 1.0, p1 = x;
            for (int k = 2; k <= n; ++k)
            {
                const double p2 = ((2.0 * k - 1.0) * x * p1 - (k - 1.0) * p0) / k;
                p0 = p1;
                p1 = p2;
            }
            derivative = n * (x * p1 - p0) / (x * x - 1.0);
            const double dx = p1 / derivative;
            x -= dx;
            if (std::abs(dx) < 1e-15) break;
        }
        out.x.push_back(0.5 * (1.0 - x));
        out.w.push_back(1.0 / ((1.0 - x * x) * derivative * derivative));
    }
    return out;
}

const Rule &gauss(int n)
{
    static const std::vector<Rule> rules = []
    {
        std::vector<Rule> all(13);
        for (int i = 1; i <= 12; ++i) all[static_cast<Index>(i)] = gaussLegendre(i);
        return all;
    }();
    return rules[static_cast<Index>(std::clamp(n, 1, 12))];
}

// Rules on a triangle in barycentric coordinates; the weights add up to 1.
struct TriangleRule
{
    int count;
    double l[7][3];
    double w[7];
};

// Degree 5, 7 points (Radon, Hammer-Marlowe-Stroud).
constexpr double a1 = 0.0597158717897698, b1 = 0.4701420641051151, w1 = 0.1323941527885062;
constexpr double a2 = 0.7974269853530873, b2 = 0.1012865073234563, w2 = 0.1259391805448271;
constexpr TriangleRule kRule7{7,
                              {{1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0},
                               {a1, b1, b1},
                               {b1, a1, b1},
                               {b1, b1, a1},
                               {a2, b2, b2},
                               {b2, a2, b2},
                               {b2, b2, a2}},
                              {0.225, w1, w1, w1, w2, w2, w2}};
// Degree 2, 3 points.
constexpr TriangleRule kRule3{3,
                              {{2.0 / 3.0, 1.0 / 6.0, 1.0 / 6.0},
                               {1.0 / 6.0, 2.0 / 3.0, 1.0 / 6.0},
                               {1.0 / 6.0, 1.0 / 6.0, 2.0 / 3.0},
                               {0.0, 0.0, 0.0},
                               {0.0, 0.0, 0.0},
                               {0.0, 0.0, 0.0},
                               {0.0, 0.0, 0.0}},
                              {1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0, 0.0, 0.0, 0.0, 0.0}};

// ---- Mesh ----

struct Seg
{
    Vec3 p0, p1, t;
    double length = 0.0, radius = 0.0;
    int tag = -1;
    Index n0 = 0, n1 = 0;
};

struct Tri
{
    Index v[3] = {0, 0, 0};
    Vec3 p[3];
    Vec3 normal, centroid;
    Vec3 edgeDir[3], edgeOut[3]; // edge i goes from p[i] to p[(i + 1) % 3]
    double area = 0.0, size = 0.0;
    int tag = -1;
    int body = -1; // dielectric body whose skin it belongs to, -1 for a conductor
};

// A piece of a basis function on an element.
// Segment: shape 0 is the linear function that is 1 at p0, shape 1 the one that is 1 at p1; the
// current is 'coef' times the shape along p0 -> p1.
// Triangle: shapes 0..2 are (r - p[shape]) / (2 area) (half of a RWG function per unit of edge
// length); shapes 3..5 the junction function of apex p[shape - 3].
struct Piece
{
    Index basis;
    int shape;
    double coef;
};

Complex dotc(const Vec3 &a, const ComplexVec3 &b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

// exp(-j k R) / (4 pi R)
Complex green(double k, double r)
{
    double s, c;
    qsig::sinCos(k * r, s, c);
    return Complex(c, -s) / (4.0 * kPi * r);
}

// (exp(-j k R) - 1) / R, finite at R = 0.
Complex smoothGreen(double k, double r)
{
    if (r < 1e-12 / std::max(k, 1e-12)) return {0.0, -k};
    double s, c;
    qsig::sinCos(k * r, s, c);
    // cos - 1 = -2 sin^2(kR / 2): no cancellation for small kR.
    double sh, ch;
    qsig::sinCos(0.5 * k * r, sh, ch);
    return Complex(-2.0 * sh * sh, -s) / r;
}

// The same two kernels in a medium of complex wavenumber (a lossy dielectric: Im k < 0).
Complex green(Complex k, double r)
{
    return std::exp(Complex(0.0, -1.0) * k * r) / (4.0 * kPi * r);
}

Complex smoothGreen(Complex k, double r)
{
    const Complex x = Complex(0.0, -1.0) * k * r; // -j k R
    // (exp(x) - 1) / R = -j k (1 + x / 2 + x^2 / 6 + ...): no cancellation for small k R.
    if (std::abs(x) < 1e-2) return Complex(0.0, -1.0) * k * (1.0 + x * (0.5 + x * (1.0 / 6.0 + x * (1.0 / 24.0 + x / 120.0))));
    return (std::exp(x) - 1.0) / r;
}

// Kernel of the curl operator, (1 + j k R) exp(-j k R) / (4 pi R^3), less its singular part
// [1 / R^3 + k^2 / (2 R)] / (4 pi): finite at R = 0.
template <typename K> Complex smoothGradient(K k, double r)
{
    const Complex jk = Complex(0.0, 1.0) * k, x = jk * r;
    // (1 + x) exp(-x) = 1 - x^2 / 2 + sum over n >= 3 of (-1)^(n-1) (n - 1) / n! x^n.
    if (std::abs(x) < 0.4)
    {
        static constexpr double c[8] = {1.0 / 3.0, -1.0 / 8.0, 1.0 / 30.0, -1.0 / 144.0, 1.0 / 840.0, -1.0 / 5760.0, 1.0 / 45360.0, -1.0 / 403200.0};
        Complex sum = 0.0, power = 1.0;
        for (const double coefficient : c)
        {
            sum += coefficient * power;
            power *= x;
        }
        return jk * jk * jk * sum / (4.0 * kPi);
    }
    return ((1.0 + x) * std::exp(-x) - 1.0 + 0.5 * x * x) / (4.0 * kPi * r * r * r);
}

void staticIntegrals(const Tri &t, const Vec3 &r, double &inverseR, Vec3 &vectorOverR, Vec3 &rho)
{
    const double d = dot(t.normal, r - t.p[0]), ad = std::abs(d);
    rho = r - t.normal * d;
    inverseR = 0.0;
    vectorOverR = {};
    const double tiny = 1e-20 * t.size * t.size;
    for (Index i = 0; i < 3; ++i)
    {
        const Vec3 toStart = t.p[i] - rho, toEnd = t.p[(i + 1) % 3] - rho;
        const double lm = dot(toStart, t.edgeDir[i]), lp = dot(toEnd, t.edgeDir[i]);
        const double h = dot(toStart, t.edgeOut[i]); // > 0 when rho is on the inner side of the edge
        const double r02 = h * h + d * d;
        const double rm = std::sqrt(r02 + lm * lm), rp = std::sqrt(r02 + lp * lp);
        double f = 0.0;
        if (r02 > tiny)
        {
            // Two forms of the same logarithm: the one without cancellation.
            f = lp + lm >= 0.0 ? std::log((rp + lp) / (rm + lm)) : std::log((rm - lm) / (rp - lp));
            inverseR += h * f;
            if (ad > 0.0 && h != 0.0)
                inverseR -= ad * (std::atan(h * lp / (r02 + ad * rp)) - std::atan(h * lm / (r02 + ad * rm)));
        }
        vectorOverR += t.edgeOut[i] * (0.5 * (r02 * f + lp * rp - lm * rm));
    }
}

// p0 = ∫ G dS', pv = ∫ r' G dS' over the triangle, seen from r. K: double in air, Complex in a medium.
template <typename K> void triPotentials(const Tri &t, const Vec3 &r, K k, Complex &p0, ComplexVec3 &pv)
{
    const double distance = norm(r - t.centroid);
    p0 = {};
    Complex px{}, py{}, pz{};
    if (distance > 2.5 * t.size)
    {
        const TriangleRule &rule = distance > 8.0 * t.size ? kRule3 : kRule7;
        for (int q = 0; q < rule.count; ++q)
        {
            const Vec3 rq = t.p[0] * rule.l[q][0] + t.p[1] * rule.l[q][1] + t.p[2] * rule.l[q][2];
            const Complex g = green(k, norm(r - rq)) * (rule.w[q] * t.area);
            p0 += g;
            px += g * rq.x;
            py += g * rq.y;
            pz += g * rq.z;
        }
        pv = {px, py, pz};
        return;
    }
    double inverseR = 0.0;
    Vec3 vectorOverR, rho;
    staticIntegrals(t, r, inverseR, vectorOverR, rho);
    for (int q = 0; q < kRule7.count; ++q)
    {
        const Vec3 rq = t.p[0] * kRule7.l[q][0] + t.p[1] * kRule7.l[q][1] + t.p[2] * kRule7.l[q][2];
        const Complex s = smoothGreen(k, norm(r - rq)) * (kRule7.w[q] * t.area);
        p0 += s;
        px += s * rq.x;
        py += s * rq.y;
        pz += s * rq.z;
    }
    const double c = 1.0 / (4.0 * kPi);
    const Vec3 stat = vectorOverR + rho * inverseR;
    pv = {(px + stat.x) * c, (py + stat.y) * c, (pz + stat.z) * c};
    p0 = (p0 + inverseR) * c;
}

// Curl operator of the three half RWG shapes of a triangle, seen from r:
//     kv[j] = (r - p[j]) x ∫ (r' - p[j]) g(R) dS',   g = (1 + j k R) exp(-j k R) / (4 pi R^3),
// so that the current X = c (r' - p[j]) gives ∫ X x grad G dS' = c kv[j] (grad G = -(r - r') g). The
// singular part of g, [1 / R^3 + k^2 / (2 R)] / (4 pi), is integrated exactly:
//     ∫ (r' - rho) / R^3 = -sum of u_i f_i,   ∫ 1 / R^3 = (sum of beta_i) / |d|
// (Wilton et al. 1984, Graglia 1993; rho the foot of r on the plane, d its height, u_i, f_i, beta_i the
// outward normal, the logarithm and the angle of each edge). Principal value: in the plane of the
// triangle the term in 1 / |d| is dropped; it is the jump of the field across the sheet.
template <typename K> void triCurl(const Tri &t, const Vec3 &r, K k, ComplexVec3 kv[3])
{
    const Complex jk = Complex(0.0, 1.0) * k;
    auto crossc = [](const Vec3 &a, const ComplexVec3 &b) { return ComplexVec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; };
    const double distance = norm(r - t.centroid);
    if (distance > 2.5 * t.size)
    {
        const TriangleRule &rule = distance > 8.0 * t.size ? kRule3 : kRule7;
        Complex g0{}, gx{}, gy{}, gz{};
        for (int q = 0; q < rule.count; ++q)
        {
            const Vec3 rq = t.p[0] * rule.l[q][0] + t.p[1] * rule.l[q][1] + t.p[2] * rule.l[q][2];
            const double range = norm(r - rq);
            const Complex g = (1.0 + jk * range) * std::exp(-jk * range) * (rule.w[q] * t.area / (4.0 * kPi * range * range * range));
            g0 += g;
            gx += g * rq.x;
            gy += g * rq.y;
            gz += g * rq.z;
        }
        for (Index j = 0; j < 3; ++j)
            kv[j] = crossc(r - t.p[j], {gx - g0 * t.p[j].x, gy - g0 * t.p[j].y, gz - g0 * t.p[j].z});
        return;
    }
    // The static integrals, edge by edge.
    const double d = dot(t.normal, r - t.p[0]), ad = std::abs(d);
    const Vec3 rho = r - t.normal * d;
    double inverseR = 0.0, angle = 0.0;
    Vec3 vectorOverR, vectorOverR3;
    const double tiny = 1e-20 * t.size * t.size;
    for (Index i = 0; i < 3; ++i)
    {
        const Vec3 toStart = t.p[i] - rho, toEnd = t.p[(i + 1) % 3] - rho;
        const double lm = dot(toStart, t.edgeDir[i]), lp = dot(toEnd, t.edgeDir[i]);
        const double h = dot(toStart, t.edgeOut[i]);
        const double r02 = h * h + d * d;
        const double rm = std::sqrt(r02 + lm * lm), rp = std::sqrt(r02 + lp * lp);
        double f = 0.0;
        if (r02 > tiny)
        {
            f = lp + lm >= 0.0 ? std::log((rp + lp) / (rm + lm)) : std::log((rm - lm) / (rp - lp));
            inverseR += h * f;
            if (ad > 0.0 && h != 0.0)
            {
                const double beta = std::atan(h * lp / (r02 + ad * rp)) - std::atan(h * lm / (r02 + ad * rm));
                inverseR -= ad * beta;
                angle += beta;
            }
        }
        vectorOverR += t.edgeOut[i] * (0.5 * (r02 * f + lp * rp - lm * rm));
        vectorOverR3 -= t.edgeOut[i] * f;
    }
    // What is left of the kernel is smooth.
    Complex s0{}, sx{}, sy{}, sz{};
    for (int q = 0; q < kRule7.count; ++q)
    {
        const Vec3 rq = t.p[0] * kRule7.l[q][0] + t.p[1] * kRule7.l[q][1] + t.p[2] * kRule7.l[q][2];
        const Complex smooth = smoothGradient(k, norm(r - rq)) * (kRule7.w[q] * t.area);
        s0 += smooth;
        sx += smooth * rq.x;
        sy += smooth * rq.y;
        sz += smooth * rq.z;
    }
    const double c = 1.0 / (4.0 * kPi);
    const Complex halfK2 = 0.5 * Complex(k) * Complex(k);
    const double jump = d > 0.0 ? angle : d < 0.0 ? -angle : 0.0;
    for (Index j = 0; j < 3; ++j)
    {
        const Vec3 a = r - t.p[j], toRho = rho - t.p[j];
        // (r - p) x (rho - p) = d n x (rho - p): the 1 / |d| of the integral of 1 / R^3 goes with it.
        const Vec3 cubic = (cross(a, vectorOverR3) + cross(t.normal, toRho) * jump) * c;
        const Vec3 linear = cross(a, vectorOverR + toRho * inverseR) * c;
        const ComplexVec3 rest = crossc(a, {sx - s0 * t.p[j].x, sy - s0 * t.p[j].y, sz - s0 * t.p[j].z});
        kv[j] = {cubic.x + halfK2 * linear.x + rest.x, cubic.y + halfK2 * linear.y + rest.y, cubic.z + halfK2 * linear.z + rest.z};
    }
}

// s0 = ∫ (1 - u) G dl', s1 = ∫ u G dl' along the segment (u from p0 to p1), reduced kernel.
void segPotentials(const Seg &s, const Vec3 &r, double k, Complex &s0, Complex &s1)
{
    const Rule &rule = gauss(4);
    const Vec3 rel = r - s.p0;
    const double along = dot(rel, s.t);
    const double d2 = std::max(0.0, dot(rel, rel) - along * along) + s.radius * s.radius, dd = std::sqrt(d2);
    const double asinhSpan = std::asinh((s.length - along) / dd) - std::asinh(-along / dd);
    const double rEnd = std::sqrt(d2 + (s.length - along) * (s.length - along)), rStart = std::sqrt(d2 + along * along);
    Complex i0 = asinhSpan, i1 = (rEnd - rStart + along * asinhSpan) / s.length;
    const int panels = dd < 0.6 * s.length ? 4 : 1;
    for (int panel = 0; panel < panels; ++panel)
        for (Index g = 0; g < rule.x.size(); ++g)
        {
            const double u = (panel + rule.x[g]) / panels;
            const double distance = std::sqrt(d2 + (u * s.length - along) * (u * s.length - along));
            const Complex smooth = smoothGreen(k, distance) * (rule.w[g] * s.length / panels);
            i0 += smooth;
            i1 += smooth * u;
        }
    s1 = i1 / (4.0 * kPi);
    s0 = i0 / (4.0 * kPi) - s1;
}

// Junction function of the triangle 't' with its apex at p[apex], in the coordinates
// r = V + u w(v), w(v) = (1 - v)(B - V) + v (C - V):  Lambda dS = (1 - u^2) w(v) du dv, and its
// divergence is -1 / area (uniform). One sample of the function as a test function:
struct JunctionSample
{
    Vec3 r;
    Vec3 current;      // (1 - u^2) w(v) du dv
    double divergence; // -2 u du dv
};

struct Junction
{
    Index tri = 0;
    int apex = 0;
    Index basis = 0;
    double coef = 0.0; // share of the wire current that enters this triangle
    std::vector<JunctionSample> samples;
};

std::vector<JunctionSample> junctionSamples(const Tri &t, int apex)
{
    // u = s^2: the potentials of the attached wire and of the junction itself vary as log(u) near the apex.
    const Rule &ru = gauss(6), &rv = gauss(6);
    const Vec3 vertex = t.p[static_cast<Index>(apex)];
    const Vec3 b = t.p[static_cast<Index>((apex + 1) % 3)] - vertex, c = t.p[static_cast<Index>((apex + 2) % 3)] - vertex;
    std::vector<JunctionSample> samples;
    for (Index i = 0; i < ru.x.size(); ++i)
        for (Index j = 0; j < rv.x.size(); ++j)
        {
            const double s = ru.x[i], u = s * s, du = 2.0 * s * ru.w[i];
            const double v = rv.x[j], dv = rv.w[j];
            const Vec3 w = b * (1.0 - v) + c * v;
            samples.push_back({vertex + w * u, w * ((1.0 - u * u) * du * dv), -2.0 * u * du * dv});
        }
    return samples;
}

// ∫ Lambda G dS' of the junction function, seen from r.
ComplexVec3 junctionPotential(const Tri &t, int apex, const Vec3 &r, double k)
{
    const Vec3 vertex = t.p[static_cast<Index>(apex)];
    const Vec3 b = t.p[static_cast<Index>((apex + 1) % 3)] - vertex, c = t.p[static_cast<Index>((apex + 2) % 3)] - vertex;
    Complex ax{}, ay{}, az{};
    if (norm(r - t.centroid) > 2.5 * t.size)
    {
        const Rule &rule = gauss(4);
        for (Index i = 0; i < rule.x.size(); ++i)
            for (Index j = 0; j < rule.x.size(); ++j)
            {
                const double u = rule.x[i], v = rule.x[j];
                const Vec3 w = b * (1.0 - v) + c * v;
                const Complex g = green(k, norm(r - vertex - w * u)) * ((1.0 - u * u) * rule.w[i] * rule.w[j]);
                ax += g * w.x;
                ay += g * w.y;
                az += g * w.z;
            }
        return {ax, ay, az};
    }
    // Near or inside the triangle. Along each ray from the apex, R^2 = |w|^2 (u - u0)^2 + d^2: the
    // substitution u = u0 + (d / |w|) sinh(s) removes 1/R. Across the rays, the result varies as
    // log|v - v*| around the ray that goes through r: the rule is graded towards v*.
    const Vec3 rv = r - vertex;
    const double den = dot(t.normal, cross(rv, c - b));
    double vStar = std::abs(den) > 1e-14 * t.size * t.size ? -dot(t.normal, cross(rv, b)) / den : 0.5;
    vStar = std::clamp(vStar, 0.0, 1.0);
    const Rule &outer = gauss(8), &inner = gauss(4);
    for (int side = 0; side < 2; ++side)
    {
        const double length = side == 0 ? vStar : 1.0 - vStar;
        if (length < 1e-12) continue;
        for (Index i = 0; i < outer.x.size(); ++i)
        {
            const double g = outer.x[i];
            const double v = side == 0 ? vStar - length * g * g : vStar + length * g * g;
            const double dv = 2.0 * g * length * outer.w[i];
            const Vec3 w = b * (1.0 - v) + c * v;
            const double wn = norm(w);
            const double u0 = dot(rv, w) / (wn * wn);
            const double d = std::max(std::sqrt(std::max(0.0, dot(rv, rv) - u0 * u0 * wn * wn)), 1e-9 * wn);
            const double sa = std::asinh(-u0 * wn / d), sb = std::asinh((1.0 - u0) * wn / d);
            const int panels = std::max(1, static_cast<int>(std::ceil((sb - sa) / 1.5)));
            Complex sum{};
            for (int panel = 0; panel < panels; ++panel)
                for (Index q = 0; q < inner.x.size(); ++q)
                {
                    const double s = sa + (sb - sa) * (panel + inner.x[q]) / panels;
                    const double u = u0 + (d / wn) * std::sinh(s);
                    double sn, cs;
                    qsig::sinCos(k * d * std::cosh(s), sn, cs);
                    sum += Complex(cs, -sn) * ((1.0 - u * u) * inner.w[q] * (sb - sa) / panels);
                }
            const Complex ray = sum * (dv / (4.0 * kPi * wn));
            ax += ray * w.x;
            ay += ray * w.y;
            az += ray * w.z;
        }
    }
    return {ax, ay, az};
}

double pointTriangleDistance(const Tri &t, const Vec3 &r)
{
    const double d = dot(t.normal, r - t.p[0]);
    const Vec3 rho = r - t.normal * d;
    double outside = 0.0;
    for (Index i = 0; i < 3; ++i) outside = std::max(outside, -dot(t.p[i] - rho, t.edgeOut[i]));
    return std::max(std::abs(d), outside);
}

struct Node
{
    Vec3 p;
    std::vector<std::pair<Index, int>> arms; // segment, end (0: its p0 is here, 1: its p1)
    int vertex = -1;                         // mesh vertex at this point
    std::vector<Index> basis;
};

Tri makeTri(const Vec3 &a, const Vec3 &b, const Vec3 &c, int tag)
{
    Tri t;
    t.p[0] = a;
    t.p[1] = b;
    t.p[2] = c;
    t.tag = tag;
    const Vec3 n = cross(b - a, c - a);
    t.area = 0.5 * norm(n);
    t.normal = t.area > 0.0 ? n / (2.0 * t.area) : Vec3{0.0, 0.0, 1.0};
    t.centroid = (a + b + c) / 3.0;
    for (Index i = 0; i < 3; ++i)
    {
        const Vec3 e = t.p[(i + 1) % 3] - t.p[i];
        const double length = norm(e);
        t.size = std::max(t.size, length);
        t.edgeDir[i] = length > 0.0 ? e / length : Vec3{1.0, 0.0, 0.0};
        t.edgeOut[i] = cross(t.edgeDir[i], t.normal);
    }
    return t;
}
} // namespace

void staticTriangleIntegrals(const Vec3 &a, const Vec3 &b, const Vec3 &c, const Vec3 &r, double &inverseR,
                             Vec3 &vectorOverR)
{
    Vec3 rho;
    staticIntegrals(makeTri(a, b, c, -1), r, inverseR, vectorOverR, rho);
}

void curlTriangleIntegrals(const Vec3 &a, const Vec3 &b, const Vec3 &c, const Vec3 &r, Complex k, ComplexVec3 out[3])
{
    triCurl(makeTri(a, b, c, -1), r, k, out);
}

Result solve(const Input &input)
{
    Result out;
    const double frequency = input.frequency;
    if (!(frequency > 0.0) || !std::isfinite(frequency))
    {
        out.error = "La fréquence doit être positive.";
        return out;
    }
    const double lambda = qsig::constants::c0 / frequency, k = 2.0 * kPi / lambda;
    const double omega = 2.0 * kPi * frequency;
    const double tol = std::max(input.tolerance, 1e-12);
    const Complex jwmu(0.0, omega * qsig::constants::mu0);
    const Complex invjweps = 1.0 / Complex(0.0, omega * qsig::constants::epsilon0);

    // ---- Wires: split where they touch and at the sources, then cut into segments ----
    const Index wireCount = input.wires.size();
    std::vector<std::vector<double>> cuts(wireCount);
    for (Index i = 0; i < wireCount; ++i)
    {
        const Wire &w = input.wires[i];
        const double length = norm(w.b - w.a);
        if (!(length > 10.0 * tol) || !(w.radius > 0.0) || !std::isfinite(length))
        {
            out.error = "Chaque fil doit avoir une longueur et un rayon.";
            return out;
        }
        cuts[i] = {0.0, 1.0};
    }
    auto addCut = [&](Index wire, double t)
    {
        const double margin = tol / norm(input.wires[wire].b - input.wires[wire].a);
        if (t > margin && t < 1.0 - margin) cuts[wire].push_back(t);
    };
    for (Index i = 0; i < wireCount; ++i)
        for (Index j = 0; j < wireCount; ++j)
        {
            if (i == j) continue;
            const Wire &wi = input.wires[i], &wj = input.wires[j];
            // An end of j on the body of i.
            for (const Vec3 &end : {wj.a, wj.b})
            {
                const Vec3 d = wi.b - wi.a;
                const double t = dot(end - wi.a, d) / dot(d, d);
                if (t > 0.0 && t < 1.0 && norm(end - (wi.a + d * t)) < tol) addCut(i, t);
            }
            if (j < i) continue;
            // Two wires that cross.
            double s = 0.0, t = 0.0;
            if (detail::segmentSegmentDistance3(wi.a, wi.b, wj.a, wj.b, &s, &t) < tol)
            {
                addCut(i, s);
                addCut(j, t);
            }
        }
    std::vector<Vec3> wireFeedPoints(input.feeds.size());
    for (Index f = 0; f < input.feeds.size(); ++f)
    {
        if (input.feeds[f].onSurface) continue;
        double best = std::numeric_limits<double>::infinity();
        Index bestWire = 0;
        double bestT = 0.0;
        for (Index i = 0; i < wireCount; ++i)
        {
            const Wire &w = input.wires[i];
            const Vec3 d = w.b - w.a;
            const double t = std::clamp(dot(input.feeds[f].position - w.a, d) / dot(d, d), 0.0, 1.0);
            const double distance = norm(input.feeds[f].position - (w.a + d * t));
            if (distance < best)
            {
                best = distance;
                bestWire = i;
                bestT = t;
            }
        }
        if (!(best < 10.0 * tol))
        {
            out.error = "Une source n'est sur aucun fil.";
            return out;
        }
        addCut(bestWire, bestT);
        wireFeedPoints[f] = input.wires[bestWire].a + (input.wires[bestWire].b - input.wires[bestWire].a) * bestT;
    }

    std::vector<Node> nodes;
    auto nodeAt = [&](const Vec3 &p)
    {
        for (Index n = 0; n < nodes.size(); ++n)
            if (norm(nodes[n].p - p) < tol) return n;
        nodes.push_back({p, {}, -1, {}});
        return nodes.size() - 1;
    };
    std::vector<Seg> segs;
    const double segmentLength = lambda / std::clamp(input.segmentsPerWavelength, 4.0, 1000.0);
    for (Index i = 0; i < wireCount; ++i)
    {
        const Wire &w = input.wires[i];
        std::sort(cuts[i].begin(), cuts[i].end());
        const double length = norm(w.b - w.a);
        double previous = 0.0;
        for (Index c = 1; c < cuts[i].size(); ++c)
        {
            const double t = cuts[i][c];
            if ((t - previous) * length < tol) continue; // the same point twice
            const Vec3 from = w.a + (w.b - w.a) * previous, to = w.a + (w.b - w.a) * t;
            const int count = std::max(1, static_cast<int>(std::ceil((t - previous) * length / segmentLength - 1e-9)));
            for (int q = 0; q < count; ++q)
            {
                Seg s;
                s.n0 = nodeAt(from + (to - from) * (double(q) / count));
                s.n1 = nodeAt(from + (to - from) * (double(q + 1) / count));
                s.p0 = nodes[s.n0].p;
                s.p1 = nodes[s.n1].p;
                s.length = norm(s.p1 - s.p0);
                s.t = (s.p1 - s.p0) / s.length;
                s.radius = w.radius;
                s.tag = w.tag;
                nodes[s.n0].arms.push_back({segs.size(), 0});
                nodes[s.n1].arms.push_back({segs.size(), 1});
                segs.push_back(s);
            }
            previous = t;
        }
    }

    // ---- Surfaces: shared vertices, edges ----
    std::vector<Vec3> vertices;
    std::vector<Tri> tris;
    {
        // Vertices sorted along x to find the neighbours of a point quickly.
        std::vector<std::pair<Vec3, Index>> corners;
        for (Index t = 0; t < input.triangles.size(); ++t)
        {
            const Triangle &tr = input.triangles[t];
            corners.push_back({tr.a, 3 * t});
            corners.push_back({tr.b, 3 * t + 1});
            corners.push_back({tr.c, 3 * t + 2});
        }
        std::sort(corners.begin(), corners.end(), [](const auto &a, const auto &b) { return a.first.x < b.first.x; });
        std::vector<Index> vertexOfCorner(corners.size(), 0);
        std::vector<std::pair<double, Index>> recent; // x, vertex: the vertices within tol along x
        Index first = 0;
        for (const auto &corner : corners)
        {
            while (first < recent.size() && recent[first].first < corner.first.x - tol) ++first;
            Index found = vertices.size();
            for (Index q = first; q < recent.size(); ++q)
                if (norm(vertices[recent[q].second] - corner.first) < tol)
                {
                    found = recent[q].second;
                    break;
                }
            if (found == vertices.size())
            {
                vertices.push_back(corner.first);
                recent.push_back({corner.first.x, found});
            }
            vertexOfCorner[corner.second] = found;
        }
        for (Index t = 0; t < input.triangles.size(); ++t)
        {
            const Index a = vertexOfCorner[3 * t], b = vertexOfCorner[3 * t + 1], c = vertexOfCorner[3 * t + 2];
            Tri tri = makeTri(vertices[a], vertices[b], vertices[c], input.triangles[t].tag);
            tri.body = input.triangles[t].body;
            if (a == b || b == c || a == c || !(tri.area > 1e-6 * tri.size * tri.size))
            {
                out.error = "Un triangle du maillage d'une surface est dégénéré.";
                return out;
            }
            tri.v[0] = a;
            tri.v[1] = b;
            tri.v[2] = c;
            tris.push_back(tri);
        }
    }
    std::vector<std::vector<std::pair<Index, int>>> trianglesOfVertex(vertices.size()); // triangle, its corner
    for (Index t = 0; t < tris.size(); ++t)
        for (int corner = 0; corner < 3; ++corner) trianglesOfVertex[tris[t].v[corner]].push_back({t, corner});

    // Wire nodes that sit on a mesh vertex.
    for (Node &node : nodes)
    {
        for (Index v = 0; v < vertices.size(); ++v)
            if (norm(vertices[v] - node.p) < tol)
            {
                node.vertex = static_cast<int>(v);
                break;
            }
        if (node.vertex >= 0 || node.arms.size() != 1) continue;
        for (const Tri &t : tris)
            if (norm(node.p - t.centroid) < 2.0 * t.size && pointTriangleDistance(t, node.p) < tol)
            {
                out.error = "Un fil se termine sur une surface, mais pas sur un sommet de son maillage (sur un disque : trop près du bord).";
                return out;
            }
    }

    // ---- Basis functions ----
    std::vector<std::vector<Piece>> segPieces(segs.size()), triPieces(tris.size());
    std::vector<Junction> junctions;
    Index basisCount = 0;
    auto armPiece = [&](const std::pair<Index, int> &arm, Index basis, bool towardsNode)
    {
        // Along p0 -> p1, the current runs towards the node when the node is p1.
        const double coef = (arm.second == 1) == towardsNode ? 1.0 : -1.0;
        segPieces[arm.first].push_back({basis, arm.second, coef});
    };
    for (Node &node : nodes)
    {
        if (node.arms.empty()) continue;
        // In by the first arm, out by another one (or into the surface): the currents of the node add up to zero.
        for (Index arm = 1; arm < node.arms.size(); ++arm)
        {
            armPiece(node.arms[0], basisCount, true);
            armPiece(node.arms[arm], basisCount, false);
            node.basis.push_back(basisCount++);
        }
        if (node.vertex < 0) continue;
        const auto &around = trianglesOfVertex[static_cast<Index>(node.vertex)];
        std::vector<double> angles;
        double total = 0.0;
        for (const auto &[t, corner] : around)
        {
            const Tri &tri = tris[t];
            const Vec3 b = tri.p[static_cast<Index>((corner + 1) % 3)] - tri.p[static_cast<Index>(corner)];
            const Vec3 c = tri.p[static_cast<Index>((corner + 2) % 3)] - tri.p[static_cast<Index>(corner)];
            angles.push_back(std::atan2(norm(cross(b, c)), dot(b, c)));
            total += angles.back();
        }
        armPiece(node.arms[0], basisCount, true);
        for (Index a = 0; a < around.size(); ++a)
        {
            Junction j;
            j.tri = around[a].first;
            j.apex = around[a].second;
            j.basis = basisCount;
            j.coef = angles[a] / total;
            j.samples = junctionSamples(tris[j.tri], j.apex);
            junctions.push_back(std::move(j));
        }
        node.basis.push_back(basisCount++);
        ++out.attachments;
    }
    out.wireUnknowns = static_cast<int>(basisCount);

    // RWG functions: one per pair of triangles around an edge (n - 1 for an edge shared by n triangles).
    struct EdgeUse
    {
        Index tri;
        int opposite;
    };
    std::map<std::pair<Index, Index>, std::vector<EdgeUse>> edges;
    for (Index t = 0; t < tris.size(); ++t)
        for (int corner = 0; corner < 3; ++corner)
        {
            const Index a = tris[t].v[(corner + 1) % 3], b = tris[t].v[(corner + 2) % 3];
            edges[{std::min(a, b), std::max(a, b)}].push_back({t, corner});
        }
    struct EdgeBasis
    {
        Index basis;
        Vec3 centre;
        double length;
    };
    std::vector<EdgeBasis> edgeBases;
    for (const auto &[key, uses] : edges)
    {
        const double length = norm(vertices[key.first] - vertices[key.second]);
        for (Index u = 1; u < uses.size(); ++u)
        {
            triPieces[uses[0].tri].push_back({basisCount, uses[0].opposite, length});
            triPieces[uses[u].tri].push_back({basisCount, uses[u].opposite, -length});
            if (uses.size() == 2) edgeBases.push_back({basisCount, (vertices[key.first] + vertices[key.second]) * 0.5, length});
            ++basisCount;
        }
    }
    out.surfaceUnknowns = static_cast<int>(basisCount) - out.wireUnknowns;

    // ---- Dielectric bodies: each RWG function of their skins carries a magnetic current too ----
    // The magnetic unknown is M / eta0 and the H-rows are multiplied by eta0, so that the whole matrix
    // is in ohms.
    const Index none = std::numeric_limits<Index>::max();
    const Index bodyCount = input.dielectrics.size();
    std::vector<Index> magneticOf(basisCount, none);
    Index totalCount = basisCount;
    struct Medium
    {
        Complex k, jwmu, invjweps, relativePermittivity;
    };
    std::vector<Medium> media;
    for (const Dielectric &dielectric : input.dielectrics)
    {
        const Complex epsilon = dielectric.relativePermittivity;
        if (!(epsilon.real() > 0.0) || epsilon.imag() > 0.0 || !std::isfinite(epsilon.real()) || !std::isfinite(epsilon.imag()))
        {
            out.error = "La permittivité d'un diélectrique doit être positive (pertes : partie imaginaire négative).";
            return out;
        }
        media.push_back({k * std::sqrt(epsilon), jwmu, invjweps / epsilon, epsilon});
    }
    {
        std::vector<int> bodyOfBasis(basisCount, -2); // -2: not seen yet
        for (Index t = 0; t < tris.size(); ++t)
        {
            if (tris[t].body >= static_cast<int>(bodyCount))
            {
                out.error = "Un triangle désigne un diélectrique qui n'existe pas.";
                return out;
            }
            for (const Piece &piece : triPieces[t])
            {
                int &body = bodyOfBasis[piece.basis];
                if (body == -2) body = tris[t].body;
                if (body != tris[t].body)
                {
                    out.error = tris[t].body >= 0 && body >= 0 ? "Deux diélectriques se touchent : ce n'est pas encore calculé."
                                                               : "Un conducteur touche un diélectrique : ce n'est pas encore calculé.";
                    return out;
                }
            }
        }
        for (const Junction &junction : junctions)
            if (tris[junction.tri].body >= 0)
            {
                out.error = "Un fil est posé sur un diélectrique : il ne s'attache qu'à un conducteur.";
                return out;
            }
        // A closed skin: every edge of a dielectric triangle belongs to two of them.
        for (const auto &[key, uses] : edges)
            if (tris[uses[0].tri].body >= 0 && uses.size() != 2)
            {
                out.error = "La peau d'un diélectrique doit être fermée.";
                return out;
            }
        for (Index b = 0; b < basisCount; ++b)
            if (bodyOfBasis[b] >= 0) magneticOf[b] = totalCount++;
    }
    out.magneticUnknowns = static_cast<int>(totalCount - basisCount);
    out.unknowns = static_cast<int>(totalCount);
    if (basisCount == 0)
    {
        out.error = "Rien à résoudre : aucun courant ne peut circuler dans cette géométrie.";
        return out;
    }
    if (out.unknowns > input.maxUnknowns)
    {
        out.error = "Trop d'inconnues (" + std::to_string(out.unknowns) + ", limite " + std::to_string(input.maxUnknowns) +
                    ") : maillage plus grossier ou surfaces plus petites.";
        return out;
    }

    // ---- Sources ----
    std::vector<Complex> rhs(totalCount);
    struct FeedBinding
    {
        Index basis;
        double scale; // current of the gap per unit of the unknown
        Vec3 position;
    };
    std::vector<FeedBinding> bindings;
    for (Index f = 0; f < input.feeds.size(); ++f)
    {
        const Feed &feed = input.feeds[f];
        if (feed.onSurface)
        {
            const EdgeBasis *best = nullptr;
            double bestDistance = std::numeric_limits<double>::infinity();
            for (const EdgeBasis &e : edgeBases)
                if (const double distance = norm(e.centre - feed.position); distance < bestDistance)
                {
                    bestDistance = distance;
                    best = &e;
                }
            if (!best)
            {
                out.error = "Une source de surface demande une arête intérieure du maillage.";
                return out;
            }
            rhs[best->basis] += feed.voltage * best->length;
            bindings.push_back({best->basis, best->length, best->centre});
            continue;
        }
        const Node *at = nullptr;
        for (const Node &node : nodes)
            if (norm(node.p - wireFeedPoints[f]) < tol)
            {
                at = &node;
                break;
            }
        if (!at || at->basis.empty())
        {
            out.error = "Une source au bout libre d'un fil ne fait circuler aucun courant : posez ce bout sur une surface ou déplacez la source.";
            return out;
        }
        if (at->basis.size() != 1)
        {
            out.error = "Une source ne peut pas se trouver là où plus de deux conducteurs se rejoignent.";
            return out;
        }
        rhs[at->basis[0]] += feed.voltage;
        bindings.push_back({at->basis[0], 1.0, at->p});
    }
    // A plane wave falling on the scene: <f, E inc> on every electric function, eta0 <f, H inc> on the
    // H-rows of the dielectric skins.
    const Vec3 waveField = input.incident.field;
    const bool wave = dot(waveField, waveField) > 0.0;
    if (wave)
    {
        const double directionNorm = norm(input.incident.direction);
        if (!(directionNorm > 0.0))
        {
            out.error = "L'onde incidente n'a pas de direction.";
            return out;
        }
        const Vec3 direction = input.incident.direction / directionNorm;
        const Vec3 waveH = cross(direction, waveField); // eta0 H
        auto phaseAt = [&](const Vec3 &r)
        {
            double sn, cs;
            qsig::sinCos(k * dot(direction, r), sn, cs);
            return Complex(cs, -sn);
        };
        const Rule &lineRule = gauss(4);
        for (Index e = 0; e < segs.size(); ++e)
            for (Index g = 0; g < lineRule.x.size(); ++g)
            {
                const double u = lineRule.x[g];
                const Complex along = dot(segs[e].t, waveField) * phaseAt(segs[e].p0 + (segs[e].p1 - segs[e].p0) * u) * (lineRule.w[g] * segs[e].length);
                for (const Piece &piece : segPieces[e]) rhs[piece.basis] += piece.coef * (piece.shape == 0 ? 1.0 - u : u) * along;
            }
        for (Index e = 0; e < tris.size(); ++e)
            for (int q = 0; q < kRule7.count; ++q)
            {
                const Vec3 r = tris[e].p[0] * kRule7.l[q][0] + tris[e].p[1] * kRule7.l[q][1] + tris[e].p[2] * kRule7.l[q][2];
                const Complex phase = phaseAt(r) * (kRule7.w[q] * 0.5); // weight times area, over 2 area
                for (const Piece &piece : triPieces[e])
                {
                    const Vec3 f = (r - tris[e].p[static_cast<Index>(piece.shape)]) * piece.coef;
                    rhs[piece.basis] += dot(f, waveField) * phase;
                    if (magneticOf[piece.basis] != none) rhs[magneticOf[piece.basis]] += dot(f, waveH) * phase;
                }
            }
        for (const Junction &junction : junctions)
            for (const JunctionSample &sample : junction.samples)
                rhs[junction.basis] += junction.coef * dot(sample.current, waveField) * phaseAt(sample.r);
    }
    if (bindings.empty() && !wave)
    {
        out.error = "Aucune source.";
        return out;
    }

    // ---- Matrix ----
    linalg::DenseMatrix<Complex> z(totalCount, totalCount);

    // Wire with wire.
    for (Index e = 0; e < segs.size(); ++e)
    {
        if (segPieces[e].empty()) continue;
        for (Index s = 0; s < segs.size(); ++s)
        {
            if (segPieces[s].empty()) continue;
            const auto variants = detail::integrateRooftopPairMixedPotential(
                segs[e].p0, segs[e].p1, segs[e].t, segs[s].p0, segs[s].p1, segs[s].t, 0.5 * (segs[e].radius + segs[s].radius), k,
                omega);
            for (const Piece &pm : segPieces[e])
                for (const Piece &pn : segPieces[s])
                    z(pm.basis, pn.basis) += pm.coef * pn.coef * variants[detail::rooftopVariant(pm.shape == 0, pn.shape == 0)];
        }
    }

    // Wire (tested) with triangle (source), and its mirror.
    const Rule &rule4 = gauss(4);
    for (Index e = 0; e < segs.size(); ++e)
    {
        if (segPieces[e].empty()) continue;
        const Seg &seg = segs[e];
        const Vec3 middle = (seg.p0 + seg.p1) * 0.5;
        for (Index s = 0; s < tris.size(); ++s)
        {
            if (triPieces[s].empty()) continue;
            const Tri &tri = tris[s];
            const int panels = norm(middle - tri.centroid) < 2.5 * tri.size + seg.length ? 3 : 1;
            Complex vec[2][3] = {}, scalar{};
            for (int panel = 0; panel < panels; ++panel)
                for (Index g = 0; g < rule4.x.size(); ++g)
                {
                    const double u = (panel + rule4.x[g]) / panels, w = rule4.w[g] * seg.length / panels;
                    Complex p0;
                    ComplexVec3 pv;
                    triPotentials(tri, seg.p0 + (seg.p1 - seg.p0) * u, k, p0, pv);
                    const Complex along = dotc(seg.t, pv);
                    for (Index j = 0; j < 3; ++j)
                    {
                        const Complex a = (along - dot(seg.t, tri.p[j]) * p0) * (w / (2.0 * tri.area));
                        vec[0][j] += a * (1.0 - u);
                        vec[1][j] += a * u;
                    }
                    scalar += p0 * w;
                }
            for (const Piece &pm : segPieces[e])
            {
                const double slope = (pm.shape == 0 ? -1.0 : 1.0) / seg.length;
                for (const Piece &pn : triPieces[s])
                {
                    const Complex value = pm.coef * pn.coef *
                                          (jwmu * vec[pm.shape][pn.shape] + invjweps * (slope / tri.area) * scalar);
                    z(pm.basis, pn.basis) += value;
                    z(pn.basis, pm.basis) += value;
                }
            }
        }
    }

    // Triangle with triangle.
    for (Index e = 0; e < tris.size(); ++e)
    {
        if (triPieces[e].empty()) continue;
        const Tri &test = tris[e];
        for (Index s = e; s < tris.size(); ++s)
        {
            if (triPieces[s].empty()) continue;
            const Tri &source = tris[s];
            const TriangleRule &rule =
                norm(test.centroid - source.centroid) > 8.0 * std::max(test.size, source.size) ? kRule3 : kRule7;
            Complex vec[3][3] = {}, scalar{};
            for (int q = 0; q < rule.count; ++q)
            {
                const Vec3 r = test.p[0] * rule.l[q][0] + test.p[1] * rule.l[q][1] + test.p[2] * rule.l[q][2];
                Complex p0;
                ComplexVec3 pv;
                triPotentials(source, r, k, p0, pv);
                for (Index i = 0; i < 3; ++i)
                {
                    const Vec3 f = r - test.p[i];
                    const Complex fpv = dotc(f, pv);
                    for (Index j = 0; j < 3; ++j) vec[i][j] += rule.w[q] * (fpv - dot(f, source.p[j]) * p0);
                }
                scalar += rule.w[q] * p0;
            }
            Complex block[3][3];
            for (Index i = 0; i < 3; ++i)
                for (Index j = 0; j < 3; ++j)
                    block[i][j] = jwmu * vec[i][j] / (4.0 * source.area) + invjweps * scalar / source.area;
            if (e == s)
                for (Index i = 0; i < 3; ++i)
                    for (Index j = i + 1; j < 3; ++j) block[i][j] = block[j][i] = 0.5 * (block[i][j] + block[j][i]);
            // Two triangles of the skin of one dielectric body: the same operator in its medium (the
            // opposite currents radiate inside), added to the one in air.
            const bool inside = test.body >= 0 && test.body == source.body;
            Complex inner[3][3] = {};
            if (inside)
            {
                const Medium &medium = media[static_cast<Index>(test.body)];
                Complex vecIn[3][3] = {}, scalarIn{};
                for (int q = 0; q < kRule7.count; ++q)
                {
                    const Vec3 r = test.p[0] * kRule7.l[q][0] + test.p[1] * kRule7.l[q][1] + test.p[2] * kRule7.l[q][2];
                    Complex p0;
                    ComplexVec3 pv;
                    triPotentials(source, r, medium.k, p0, pv);
                    for (Index i = 0; i < 3; ++i)
                    {
                        const Vec3 f = r - test.p[i];
                        const Complex fpv = dotc(f, pv);
                        for (Index j = 0; j < 3; ++j) vecIn[i][j] += kRule7.w[q] * (fpv - dot(f, source.p[j]) * p0);
                    }
                    scalarIn += kRule7.w[q] * p0;
                }
                for (Index i = 0; i < 3; ++i)
                    for (Index j = 0; j < 3; ++j)
                        inner[i][j] = medium.jwmu * vecIn[i][j] / (4.0 * source.area) + medium.invjweps * scalarIn / source.area;
                if (e == s)
                    for (Index i = 0; i < 3; ++i)
                        for (Index j = i + 1; j < 3; ++j) inner[i][j] = inner[j][i] = 0.5 * (inner[i][j] + inner[j][i]);
            }
            const bool magnetic = test.body >= 0 && source.body >= 0;
            for (const Piece &pm : triPieces[e])
                for (const Piece &pn : triPieces[s])
                {
                    const Complex value = pm.coef * pn.coef * block[pm.shape][pn.shape];
                    const Complex valueIn = pm.coef * pn.coef * inner[pm.shape][pn.shape];
                    z(pm.basis, pn.basis) += value + valueIn;
                    if (e != s) z(pn.basis, pm.basis) += value + valueIn;
                    if (!magnetic) continue;
                    // H-rows, magnetic currents: the dual operator, Z / eta^2 in each medium; with M / eta0
                    // as the unknown and the rows times eta0, Z in air and eps_r Z inside.
                    const Complex dual = value + (inside ? media[static_cast<Index>(test.body)].relativePermittivity * valueIn : Complex{});
                    const Index mm = magneticOf[pm.basis], mn = magneticOf[pn.basis];
                    z(mm, mn) += dual;
                    if (e != s) z(mn, mm) += dual;
                }
        }
    }

    // ---- Curl operator K: electric currents make H on the dielectric skins, magnetic currents make E ----
    // K[m][n] = <f_m, ∫ f_n x grad G>, in air between any two functions, and in the medium of a body
    // between two functions of its skin. E-rows: -eta0 K on the magnetic columns; H-rows: +eta0 K on
    // the electric columns. Nothing between two triangles of one plane (and of a triangle with itself).
    const double eta0 = qsig::constants::eta0;
    if (totalCount > basisCount)
    {
        for (Index e = 0; e < tris.size(); ++e)
        {
            if (triPieces[e].empty()) continue;
            const Tri &test = tris[e];
            for (Index s = 0; s < tris.size(); ++s)
            {
                if (s == e || triPieces[s].empty()) continue;
                const Tri &source = tris[s];
                if (test.body < 0 && source.body < 0) continue;
                double offPlane = 0.0;
                for (const Vec3 &corner : test.p) offPlane = std::max(offPlane, std::abs(dot(source.normal, corner - source.p[0])));
                if (offPlane < 1e-9 * source.size) continue;
                const bool inside = test.body >= 0 && test.body == source.body;
                // Neighbours: the field of the source varies fast over the test triangle, cut in four.
                const double gap = norm(test.centroid - source.centroid);
                const bool neighbours = gap < 2.5 * std::max(test.size, source.size);
                const TriangleRule &rule = gap > 8.0 * std::max(test.size, source.size) ? kRule3 : kRule7;
                Complex air[3][3] = {}, medium[3][3] = {};
                const int parts = neighbours ? 4 : 1;
                for (int part = 0; part < parts; ++part)
                {
                    // The four triangles of the mid-points: three at the corners, one in the middle.
                    Vec3 corner[3] = {test.p[0], test.p[1], test.p[2]};
                    if (neighbours)
                    {
                        const Vec3 m01 = (test.p[0] + test.p[1]) * 0.5, m12 = (test.p[1] + test.p[2]) * 0.5, m20 = (test.p[2] + test.p[0]) * 0.5;
                        if (part == 0) corner[1] = m01, corner[2] = m20;
                        if (part == 1) corner[0] = m01, corner[2] = m12;
                        if (part == 2) corner[0] = m20, corner[1] = m12;
                        if (part == 3) corner[0] = m01, corner[1] = m12, corner[2] = m20;
                    }
                    for (int q = 0; q < rule.count; ++q)
                    {
                        const Vec3 r = corner[0] * rule.l[q][0] + corner[1] * rule.l[q][1] + corner[2] * rule.l[q][2];
                        const double w = rule.w[q] / parts;
                        ComplexVec3 kv[3];
                        triCurl(source, r, k, kv);
                        for (Index i = 0; i < 3; ++i)
                            for (Index j = 0; j < 3; ++j) air[i][j] += w * dotc(r - test.p[i], kv[j]);
                        if (!inside) continue;
                        triCurl(source, r, media[static_cast<Index>(test.body)].k, kv);
                        for (Index i = 0; i < 3; ++i)
                            for (Index j = 0; j < 3; ++j) medium[i][j] += w * dotc(r - test.p[i], kv[j]);
                    }
                }
                for (const Piece &pm : triPieces[e])
                    for (const Piece &pn : triPieces[s])
                    {
                        // f_m = coef (r - p) / (2 area), the integral over the test triangle is its area times the sum.
                        const Complex value = eta0 * pm.coef * pn.coef * (air[pm.shape][pn.shape] + medium[pm.shape][pn.shape]) / (4.0 * source.area);
                        if (source.body >= 0) z(pm.basis, magneticOf[pn.basis]) -= value;
                        if (test.body >= 0) z(magneticOf[pm.basis], pn.basis) += value;
                    }
            }
        }
        // Wires with the magnetic currents (in air): tested along the wire; the mirror term, the H of the
        // wire tested on the skin, is the same number (K is symmetric).
        const Rule &lineRule = gauss(4);
        for (Index e = 0; e < segs.size(); ++e)
        {
            if (segPieces[e].empty()) continue;
            const Seg &seg = segs[e];
            const Vec3 middle = (seg.p0 + seg.p1) * 0.5;
            for (Index s = 0; s < tris.size(); ++s)
            {
                const Tri &source = tris[s];
                if (source.body < 0 || triPieces[s].empty()) continue;
                const int panels = norm(middle - source.centroid) < 2.5 * source.size + seg.length ? 3 : 1;
                Complex along[2][3] = {};
                for (int panel = 0; panel < panels; ++panel)
                    for (Index g = 0; g < lineRule.x.size(); ++g)
                    {
                        const double u = (panel + lineRule.x[g]) / panels, w = lineRule.w[g] * seg.length / panels;
                        ComplexVec3 kv[3];
                        triCurl(source, seg.p0 + (seg.p1 - seg.p0) * u, k, kv);
                        for (Index j = 0; j < 3; ++j)
                        {
                            const Complex a = dotc(seg.t, kv[j]) * w;
                            along[0][j] += a * (1.0 - u);
                            along[1][j] += a * u;
                        }
                    }
                for (const Piece &pm : segPieces[e])
                    for (const Piece &pn : triPieces[s])
                    {
                        const Complex value = eta0 * pm.coef * pn.coef * along[pm.shape][pn.shape] / (2.0 * source.area);
                        z(pm.basis, magneticOf[pn.basis]) -= value;
                        z(magneticOf[pn.basis], pm.basis) += value;
                    }
            }
        }
        // Junction functions with the magnetic currents, in the same way.
        for (const Junction &jt : junctions)
            for (Index s = 0; s < tris.size(); ++s)
            {
                const Tri &source = tris[s];
                if (source.body < 0 || triPieces[s].empty()) continue;
                Complex across[3] = {};
                for (const JunctionSample &sample : jt.samples)
                {
                    ComplexVec3 kv[3];
                    triCurl(source, sample.r, k, kv);
                    for (Index j = 0; j < 3; ++j) across[j] += dotc(sample.current, kv[j]);
                }
                for (const Piece &pn : triPieces[s])
                {
                    const Complex value = eta0 * jt.coef * pn.coef * across[pn.shape] / (2.0 * source.area);
                    z(jt.basis, magneticOf[pn.basis]) -= value;
                    z(magneticOf[pn.basis], jt.basis) += value;
                }
            }
    }

    // Junction functions (tested) with everything.
    for (const Junction &jt : junctions)
    {
        for (Index s = 0; s < segs.size(); ++s)
        {
            if (segPieces[s].empty()) continue;
            Complex vec[2] = {}, scalar{};
            for (const JunctionSample &sample : jt.samples)
            {
                Complex s0, s1;
                segPotentials(segs[s], sample.r, k, s0, s1);
                const double along = dot(sample.current, segs[s].t);
                vec[0] += along * s0;
                vec[1] += along * s1;
                scalar += sample.divergence * (s0 + s1);
            }
            for (const Piece &pn : segPieces[s])
            {
                const double slope = (pn.shape == 0 ? -1.0 : 1.0) / segs[s].length;
                const Complex value = jt.coef * pn.coef * (jwmu * vec[pn.shape] + invjweps * slope * scalar);
                z(jt.basis, pn.basis) += value;
                z(pn.basis, jt.basis) += value;
            }
        }
        for (Index s = 0; s < tris.size(); ++s)
        {
            if (triPieces[s].empty()) continue;
            const Tri &source = tris[s];
            Complex vec[3] = {}, scalar{};
            for (const JunctionSample &sample : jt.samples)
            {
                Complex p0;
                ComplexVec3 pv;
                triPotentials(source, sample.r, k, p0, pv);
                const Complex along = dotc(sample.current, pv);
                for (Index j = 0; j < 3; ++j) vec[j] += along - dot(sample.current, source.p[j]) * p0;
                scalar += sample.divergence * p0;
            }
            for (const Piece &pn : triPieces[s])
            {
                const Complex value =
                    jt.coef * pn.coef * (jwmu * vec[pn.shape] / (2.0 * source.area) + invjweps * scalar / source.area);
                z(jt.basis, pn.basis) += value;
                z(pn.basis, jt.basis) += value;
            }
        }
        for (const Junction &js : junctions)
        {
            const Tri &source = tris[js.tri];
            Complex vec{}, scalar{};
            for (const JunctionSample &sample : jt.samples)
            {
                vec += dotc(sample.current, junctionPotential(source, js.apex, sample.r, k));
                Complex p0;
                ComplexVec3 pv;
                triPotentials(source, sample.r, k, p0, pv);
                scalar += sample.divergence * p0;
            }
            z(jt.basis, js.basis) += jt.coef * js.coef * (jwmu * vec - invjweps * scalar / source.area);
        }
    }

    // ---- Solution ----
    std::vector<Complex> x;
    linalg::PivotStats stats;
    std::vector<Complex> b = rhs;
    if (!linalg::solveComplexBlocked(z, b, x, stats))
    {
        out.error = "Le système de l'antenne est singulier.";
        return out;
    }
    for (const Complex &value : x)
        if (!std::isfinite(value.real()) || !std::isfinite(value.imag()))
        {
            out.error = "La solution n'est pas finie.";
            return out;
        }

    for (Index e = 0; e < segs.size(); ++e)
    {
        Segment s{segs[e].p0, segs[e].p1, segs[e].radius, segs[e].tag, {}, {}};
        for (const Piece &piece : segPieces[e]) (piece.shape == 0 ? s.i0 : s.i1) += piece.coef * x[piece.basis];
        out.segments.push_back(s);
    }
    std::vector<std::vector<Index>> junctionsOfTri(tris.size());
    for (Index j = 0; j < junctions.size(); ++j) junctionsOfTri[junctions[j].tri].push_back(j);
    for (Index e = 0; e < tris.size(); ++e)
    {
        const Tri &tri = tris[e];
        Facet facet;
        facet.a = tri.p[0];
        facet.b = tri.p[1];
        facet.c = tri.p[2];
        facet.tag = tri.tag;
        facet.body = tri.body;
        ComplexVec3 *shown[3] = {&facet.ja, &facet.jb, &facet.jc};
        ComplexVec3 *shownMagnetic[3] = {&facet.ma, &facet.mb, &facet.mc};
        for (int q = 0; q < kRule3.count; ++q)
        {
            const Vec3 r = tri.p[0] * kRule3.l[q][0] + tri.p[1] * kRule3.l[q][1] + tri.p[2] * kRule3.l[q][2];
            ComplexVec3 j{}, m{};
            for (const Piece &piece : triPieces[e])
            {
                const Vec3 f = (r - tri.p[static_cast<Index>(piece.shape)]) * (piece.coef / (2.0 * tri.area));
                j.x += f.x * x[piece.basis];
                j.y += f.y * x[piece.basis];
                j.z += f.z * x[piece.basis];
                if (magneticOf[piece.basis] == none) continue;
                const Complex strength = eta0 * x[magneticOf[piece.basis]]; // the unknown is M / eta0
                m.x += f.x * strength;
                m.y += f.y * strength;
                m.z += f.z * strength;
            }
            // The moments of the junction currents come from their own samples, below.
            const double w = kRule3.w[q] * tri.area;
            out.moments.push_back({r, {j.x * w, j.y * w, j.z * w}});
            if (tri.body >= 0)
            {
                out.magneticMoments.push_back({r, {m.x * w, m.y * w, m.z * w}});
                *shownMagnetic[q] = m;
            }
            for (const Index index : junctionsOfTri[e])
            {
                // Lambda = (1 - u^2) / (2 area u^2) (r - apex), u = 1 - (barycentric coordinate of the apex).
                const Junction &junction = junctions[index];
                const double u = 1.0 - kRule3.l[q][junction.apex];
                const Vec3 f = (r - tri.p[static_cast<Index>(junction.apex)]) * (junction.coef * (1.0 - u * u) / (2.0 * tri.area * u * u));
                j.x += f.x * x[junction.basis];
                j.y += f.y * x[junction.basis];
                j.z += f.z * x[junction.basis];
            }
            *shown[q] = j;
        }
        out.facets.push_back(facet);
    }
    for (const Junction &j : junctions)
        for (const JunctionSample &sample : j.samples)
        {
            const Complex current = j.coef * x[j.basis];
            out.moments.push_back(
                {sample.r, {sample.current.x * current, sample.current.y * current, sample.current.z * current}});
        }
    for (Index f = 0; f < bindings.size(); ++f)
    {
        FeedResult r;
        r.position = bindings[f].position;
        r.voltage = input.feeds[f].voltage;
        r.current = bindings[f].scale * x[bindings[f].basis];
        r.impedance = std::abs(r.current) > 0.0 ? r.voltage / r.current : Complex{};
        out.feeds.push_back(r);
    }
    out.valid = true;
    return out;
}

// ---- Far field ----

Radiator::Radiator(const Result &result, double k) : m_k(k)
{
    for (const Segment &s : result.segments) addSegment(s.p0, s.p1, s.i0, s.i1);
    for (const Moment &m : result.moments) addMoment(m.position, m.moment);
    for (const Moment &m : result.magneticMoments) addMagneticMoment(m.position, m.moment);
}

void Radiator::addSegment(const Vec3 &p0, const Vec3 &p1, const Complex &i0, const Complex &i1)
{
    static constexpr double g2x[2] = {-0.5773502691896258, 0.5773502691896258};
    static constexpr double g2w[2] = {1.0, 1.0};
    static constexpr double g4x[4] = {-0.8611363115940526, -0.3399810435848563, 0.3399810435848563, 0.8611363115940526};
    static constexpr double g4w[4] = {0.3478548451374538, 0.6521451548625461, 0.6521451548625461, 0.3478548451374538};
    const Vec3 d = p1 - p0;
    const double length = norm(d);
    if (!(length > 0.0)) return;
    const Vec3 t = d / length;
    const bool shortSegment = m_k * length <= 0.8;
    const int count = shortSegment ? 2 : 4;
    for (int g = 0; g < count; ++g)
    {
        const double x = shortSegment ? g2x[g] : g4x[g], w = shortSegment ? g2w[g] : g4w[g];
        const double u = 0.5 * (x + 1.0);
        const Complex current = (1.0 - u) * i0 + u * i1;
        const Complex moment = current * (0.5 * w * length);
        addMoment(p0 + u * d, {moment * t.x, moment * t.y, moment * t.z});
    }
}

void Radiator::addMoment(const Vec3 &position, const ComplexVec3 &moment)
{
    m_px.push_back(position.x);
    m_py.push_back(position.y);
    m_pz.push_back(position.z);
    m_mx.push_back(moment.x);
    m_my.push_back(moment.y);
    m_mz.push_back(moment.z);
}

void Radiator::addMagneticMoment(const Vec3 &position, const ComplexVec3 &moment)
{
    m_qx.push_back(position.x);
    m_qy.push_back(position.y);
    m_qz.push_back(position.z);
    m_lx.push_back(moment.x);
    m_ly.push_back(moment.y);
    m_lz.push_back(moment.z);
}

ComplexVec3 Radiator::field(const Vec3 &dir) const
{
    Complex fx{0.0, 0.0}, fy{0.0, 0.0}, fz{0.0, 0.0};
    const double kx = m_k * dir.x, ky = m_k * dir.y, kz = m_k * dir.z;
    for (std::size_t p = 0; p < m_px.size(); ++p)
    {
        double s, c;
        qsig::sinCos(kx * m_px[p] + ky * m_py[p] + kz * m_pz[p], s, c);
        const Complex phase(c, s);
        fx += m_mx[p] * phase;
        fy += m_my[p] * phase;
        fz += m_mz[p] * phase;
    }
    const Complex along = fx * dir.x + fy * dir.y + fz * dir.z;
    ComplexVec3 transverse{fx - along * dir.x, fy - along * dir.y, fz - along * dir.z};
    if (m_qx.empty()) return transverse;
    // Magnetic currents: -(r^ x L) / eta0.
    Complex lx{0.0, 0.0}, ly{0.0, 0.0}, lz{0.0, 0.0};
    for (std::size_t p = 0; p < m_qx.size(); ++p)
    {
        double s, c;
        qsig::sinCos(kx * m_qx[p] + ky * m_qy[p] + kz * m_qz[p], s, c);
        const Complex phase(c, s);
        lx += m_lx[p] * phase;
        ly += m_ly[p] * phase;
        lz += m_lz[p] * phase;
    }
    const double scale = 1.0 / qsig::constants::eta0;
    transverse.x -= (dir.y * lz - dir.z * ly) * scale;
    transverse.y -= (dir.z * lx - dir.x * lz) * scale;
    transverse.z -= (dir.x * ly - dir.y * lx) * scale;
    return transverse;
}

double Radiator::intensity(const Vec3 &dir) const
{
    const ComplexVec3 f = field(dir);
    return std::norm(f.x) + std::norm(f.y) + std::norm(f.z);
}

double Radiator::power(double intensityIntegral) const
{
    return qsig::constants::eta0 * m_k * m_k * intensityIntegral / (32.0 * kPi * kPi);
}

// ---- Surface meshes ----

std::vector<Triangle> meshRectangle(const Vec3 &centre, const Vec3 &u, const Vec3 &v, double width, double height,
                                    double step, const std::vector<Vec3> &points, int tag)
{
    std::vector<Triangle> mesh;
    if (!(width > 0.0) || !(height > 0.0) || !(step > 0.0)) return mesh;
    // Grid lines at the edges and through the points, the intervals between them cut evenly.
    auto lines = [&](double size, const Vec3 &axis, const Vec3 &other, double otherSize)
    {
        std::vector<double> breaks{-0.5 * size, 0.5 * size};
        const double eps = 1e-9 * std::max(width, height);
        for (const Vec3 &p : points)
        {
            const Vec3 rel = p - centre;
            const double a = dot(rel, axis), b = dot(rel, other);
            const double off = norm(rel - axis * a - other * b);
            if (off < 1e-6 * std::max(width, height) && std::abs(a) < 0.5 * size - eps && std::abs(b) < 0.5 * otherSize + eps)
                breaks.push_back(a);
        }
        std::sort(breaks.begin(), breaks.end());
        std::vector<double> out{breaks.front()};
        for (Index i = 1; i < breaks.size(); ++i)
        {
            const double from = out.back(), span = breaks[i] - from;
            if (span < eps) continue;
            const int count = std::max(1, static_cast<int>(std::ceil(span / step - 1e-9)));
            for (int q = 1; q <= count; ++q) out.push_back(q == count ? breaks[i] : from + span * q / count);
        }
        return out;
    };
    const std::vector<double> xs = lines(width, u, v, height), ys = lines(height, v, u, width);
    auto point = [&](Index i, Index j) { return centre + u * xs[i] + v * ys[j]; };
    for (Index i = 0; i + 1 < xs.size(); ++i)
        for (Index j = 0; j + 1 < ys.size(); ++j)
        {
            const Vec3 p00 = point(i, j), p10 = point(i + 1, j), p11 = point(i + 1, j + 1), p01 = point(i, j + 1);
            // Diagonals in a checkerboard: no preferred direction.
            if ((i + j) % 2 == 0)
            {
                mesh.push_back({p00, p10, p11, tag});
                mesh.push_back({p00, p11, p01, tag});
            }
            else
            {
                mesh.push_back({p00, p10, p01, tag});
                mesh.push_back({p10, p11, p01, tag});
            }
        }
    return mesh;
}

std::vector<Triangle> meshDisc(const Vec3 &centre, const Vec3 &u, const Vec3 &v, double radius, double step,
                               const std::vector<Vec3> &points, int tag)
{
    std::vector<Triangle> mesh;
    if (!(radius > 0.0) || !(step > 0.0)) return mesh;
    const int rings = std::max(1, static_cast<int>(std::ceil(radius / step - 1e-9)));
    // Ring r has 6 r vertices; the centre is ring 0.
    std::vector<std::vector<Vec3>> ring(static_cast<Index>(rings) + 1);
    ring[0] = {centre};
    for (int r = 1; r <= rings; ++r)
        for (int q = 0; q < 6 * r; ++q)
        {
            const double angle = 2.0 * kPi * q / (6.0 * r), rho = radius * r / rings;
            ring[static_cast<Index>(r)].push_back(centre + u * (rho * std::cos(angle)) + v * (rho * std::sin(angle)));
        }
    // A wire lands on the centre, or the nearest inner vertex moves under it.
    std::vector<std::pair<int, int>> taken;
    for (const Vec3 &p : points)
    {
        const Vec3 rel = p - centre;
        const double a = dot(rel, u), b = dot(rel, v);
        if (norm(rel - u * a - v * b) > 1e-6 * radius || std::hypot(a, b) > radius * (1.0 + 1e-9)) continue;
        if (std::hypot(a, b) < 1e-9 * radius) continue; // the centre vertex
        int bestRing = -1, bestIndex = -1;
        double best = std::numeric_limits<double>::infinity();
        for (int r = 1; r < rings; ++r)
            for (int q = 0; q < 6 * r; ++q)
            {
                if (std::find(taken.begin(), taken.end(), std::pair<int, int>{r, q}) != taken.end()) continue;
                const double distance = norm(ring[static_cast<Index>(r)][static_cast<Index>(q)] - p);
                if (distance < best)
                {
                    best = distance;
                    bestRing = r;
                    bestIndex = q;
                }
            }
        if (bestRing < 0 || best > 0.75 * radius / rings) continue; // too near the rim: the solver reports it
        ring[static_cast<Index>(bestRing)][static_cast<Index>(bestIndex)] = centre + u * a + v * b;
        taken.push_back({bestRing, bestIndex});
    }
    for (int r = 1; r <= rings; ++r)
    {
        const auto &outer = ring[static_cast<Index>(r)], &inner = ring[static_cast<Index>(r) - 1];
        for (int sector = 0; sector < 6; ++sector)
            for (int q = 0; q < r; ++q)
            {
                const Index o0 = static_cast<Index>(sector * r + q), o1 = (o0 + 1) % outer.size();
                const Index i0 = r == 1 ? 0 : static_cast<Index>(sector * (r - 1) + q) % inner.size();
                mesh.push_back({outer[o0], outer[o1], inner[i0], tag});
                if (q + 1 < r)
                {
                    const Index i1 = static_cast<Index>(sector * (r - 1) + q + 1) % inner.size();
                    mesh.push_back({inner[i0], outer[o1], inner[i1], tag});
                }
            }
    }
    return mesh;
}

std::vector<Triangle> meshRevolution(const Vec3 &centre, const Vec3 &u, const Vec3 &v, const Vec3 &w,
                                     const std::vector<std::pair<double, double>> &profile, double step,
                                     const std::vector<Vec3> &points, int tag)
{
    std::vector<Triangle> mesh;
    if (profile.size() < 2 || !(step > 0.0)) return mesh;
    // The rings: the points of the profile and those that cut its stretches evenly.
    std::vector<std::pair<double, double>> rings;
    for (const auto &[radius, height] : profile)
    {
        const double r = std::max(0.0, radius);
        if (!rings.empty())
        {
            const double r0 = rings.back().first, h0 = rings.back().second;
            const double span = std::hypot(r - r0, height - h0);
            if (span < 1e-12 * step) continue;
            const int count = std::max(1, static_cast<int>(std::ceil(span / step - 1e-9)));
            for (int q = 1; q < count; ++q) rings.push_back({r0 + (r - r0) * q / count, h0 + (height - h0) * q / count});
        }
        rings.push_back({r, height});
    }
    // Their vertices: one on the axis, else a regular polygon with a vertex on u.
    std::vector<std::vector<Vec3>> vertices;
    for (const auto &[r, h] : rings)
    {
        std::vector<Vec3> ring;
        if (r < 1e-9 * step)
            ring.push_back(centre + w * h);
        else
        {
            const int sides = 4 * std::max(2, static_cast<int>(std::ceil(2.0 * kPi * r / (4.0 * step) - 1e-9)));
            for (int q = 0; q < sides; ++q)
            {
                const double angle = 2.0 * kPi * q / sides;
                ring.push_back(centre + w * h + u * (r * std::cos(angle)) + v * (r * std::sin(angle)));
            }
        }
        vertices.push_back(ring);
    }
    // A wire lands on a vertex: the nearest one moves under it.
    std::vector<std::pair<Index, Index>> taken;
    for (const Vec3 &p : points)
    {
        Index bestRing = 0, bestIndex = 0;
        double best = std::numeric_limits<double>::infinity();
        for (Index k = 0; k < vertices.size(); ++k)
            for (Index q = 0; q < vertices[k].size(); ++q)
            {
                if (std::find(taken.begin(), taken.end(), std::pair<Index, Index>{k, q}) != taken.end()) continue;
                const double distance = norm(vertices[k][q] - p);
                if (distance < best)
                {
                    best = distance;
                    bestRing = k;
                    bestIndex = q;
                }
            }
        if (!(best <= step)) continue; // not on this surface
        vertices[bestRing][bestIndex] = p;
        taken.push_back({bestRing, bestIndex});
    }
    // Sewing two rings: step along the one whose next vertex comes first around the axis.
    for (Index k = 0; k + 1 < vertices.size(); ++k)
    {
        const std::vector<Vec3> &a = vertices[k], &b = vertices[k + 1];
        const Index na = a.size(), nb = b.size();
        if (na == 1 && nb == 1) continue; // a stretch along the axis: no surface
        if (na == 1)
        {
            for (Index j = 0; j < nb; ++j) mesh.push_back({a[0], b[j], b[(j + 1) % nb], tag});
            continue;
        }
        if (nb == 1)
        {
            for (Index i = 0; i < na; ++i) mesh.push_back({a[i], a[(i + 1) % na], b[0], tag});
            continue;
        }
        Index i = 0, j = 0;
        while (i < na || j < nb)
        {
            if (j >= nb || (i < na && (i + 1) * nb <= (j + 1) * na))
            {
                mesh.push_back({a[i % na], a[(i + 1) % na], b[j % nb], tag});
                ++i;
            }
            else
            {
                mesh.push_back({a[i % na], b[(j + 1) % nb], b[j % nb], tag});
                ++j;
            }
        }
    }
    return mesh;
}

std::vector<Triangle> meshSweep(const std::vector<Vec3> &profile, const Vec3 &along, double length, double step,
                                const std::vector<Vec3> &points, int tag)
{
    std::vector<Triangle> mesh;
    if (profile.size() < 2 || !(step > 0.0) || !(length > 0.0)) return mesh;
    // Across: the points of the profile and those that cut its stretches evenly.
    std::vector<Vec3> across{profile.front()};
    for (Index k = 1; k < profile.size(); ++k)
    {
        const Vec3 from = across.back(), to = profile[k];
        const double span = norm(to - from);
        if (span < 1e-12 * step) continue;
        const int count = std::max(1, static_cast<int>(std::ceil(span / step - 1e-9)));
        for (int q = 1; q <= count; ++q) across.push_back(from + (to - from) * (static_cast<double>(q) / count));
    }
    // Along: evenly.
    const int rows = std::max(1, static_cast<int>(std::ceil(length / step - 1e-9)));
    std::vector<std::vector<Vec3>> grid(static_cast<Index>(rows) + 1);
    for (int j = 0; j <= rows; ++j)
        for (const Vec3 &q : across) grid[static_cast<Index>(j)].push_back(q + along * (length * (static_cast<double>(j) / rows - 0.5)));
    // A wire lands on a vertex: the nearest one moves under it.
    std::vector<std::pair<Index, Index>> taken;
    for (const Vec3 &p : points)
    {
        Index bestRow = 0, bestIndex = 0;
        double best = std::numeric_limits<double>::infinity();
        for (Index j = 0; j < grid.size(); ++j)
            for (Index i = 0; i < grid[j].size(); ++i)
            {
                if (std::find(taken.begin(), taken.end(), std::pair<Index, Index>{j, i}) != taken.end()) continue;
                const double distance = norm(grid[j][i] - p);
                if (distance < best)
                {
                    best = distance;
                    bestRow = j;
                    bestIndex = i;
                }
            }
        if (!(best <= step)) continue; // not on this surface
        grid[bestRow][bestIndex] = p;
        taken.push_back({bestRow, bestIndex});
    }
    // Cells in two triangles, the diagonals in a checkerboard.
    for (Index j = 0; j + 1 < grid.size(); ++j)
        for (Index i = 0; i + 1 < across.size(); ++i)
        {
            const Vec3 p00 = grid[j][i], p10 = grid[j][i + 1], p11 = grid[j + 1][i + 1], p01 = grid[j + 1][i];
            if ((i + j) % 2 == 0)
            {
                mesh.push_back({p00, p10, p11, tag});
                mesh.push_back({p00, p11, p01, tag});
            }
            else
            {
                mesh.push_back({p00, p10, p01, tag});
                mesh.push_back({p10, p11, p01, tag});
            }
        }
    return mesh;
}
} // namespace qsig::mom
