#include "qsig/app/antenna_model.h"

#include "qsig/constants.h"
#include "qsig/fast_math.h"
#include "qsig/moment_method.h"
#include "qsig/numerical_em_solvers.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <string>
#include <thread>
#include <vector>

namespace qsig::app
{
namespace
{
using Type = AntennaElement::Type;
using cplx = std::complex<double>;
constexpr double kPi = qsig::constants::pi;
constexpr double kDeg = kPi / 180.0;

double finiteOr(double v, double fallback)
{
    return std::isfinite(v) ? v : fallback;
}

// Orthonormal frame of an element: u along its axis, e2 and e3 across it (turned by the roll angle).
struct Frame
{
    Vec3 u, e2, e3;
};

Frame frameOf(const AntennaElement &e)
{
    Frame f;
    f.u = antennaAxis(e);
    const Vec3 reference = std::abs(f.u.z) < 0.9 ? Vec3{0.0, 0.0, 1.0} : Vec3{1.0, 0.0, 0.0};
    const Vec3 a = normalized(cross(reference, f.u)); // horizontal when possible
    const Vec3 b = cross(f.u, a);
    const double r = finiteOr(e.rollDeg, 0.0) * kDeg;
    f.e2 = a * std::cos(r) + b * std::sin(r);
    f.e3 = cross(f.u, f.e2);
    return f;
}

// A parabola z = s^2 / (4 f) from s = 0 to s = 'half', as points (s, z) about 'step' apart along the curve
// (four at least): the profile of a paraboloid, half that of a parabolic cylinder.
std::vector<std::pair<double, double>> parabolaProfile(double half, double focal, double step)
{
    const double f = std::max(1e-9, focal);
    // Arc length from the vertex, s(1 + x^2)^(1/2) integrated with x = s / (2 f): in closed form.
    auto arc = [f](double x) { const double t = x / (2.0 * f); return f * (t * std::sqrt(1.0 + t * t) + std::asinh(t)); };
    const double total = arc(half);
    const int pieces = std::clamp(static_cast<int>(std::ceil(total / std::max(step, 1e-12) - 1e-9)), 4, 400);
    std::vector<std::pair<double, double>> profile;
    for (int q = 0; q <= pieces; ++q)
    {
        // The point at that arc length, by bisection on s.
        const double target = total * q / pieces;
        double lo = 0.0, hi = half;
        for (int i = 0; i < 60; ++i)
        {
            const double mid = 0.5 * (lo + hi);
            (arc(mid) < target ? lo : hi) = mid;
        }
        const double sq = q == 0 ? 0.0 : q == pieces ? half : 0.5 * (lo + hi);
        profile.push_back({sq, sq * sq / (4.0 * f)});
    }
    return profile;
}

// The point (s, s^2 / 4f) of the parabola nearest to (x, z), for s in [lo, hi].
double nearestOnParabola(double x, double z, double focal, double lo, double hi)
{
    const double f = std::max(1e-9, focal);
    auto distance2 = [&](double sq) { const double dz = sq * sq / (4.0 * f) - z; return (sq - x) * (sq - x) + dz * dz; };
    // A coarse search, then Newton on the derivative of the squared distance.
    double best = std::clamp(x, lo, hi);
    for (int q = 0; q <= 64; ++q)
    {
        const double sq = lo + (hi - lo) * q / 64.0;
        if (distance2(sq) < distance2(best)) best = sq;
    }
    for (int i = 0; i < 30; ++i)
    {
        const double g = best - x + (best * best / (4.0 * f) - z) * best / (2.0 * f);
        const double dg = 1.0 + best * best / (4.0 * f * f) + (best * best / (4.0 * f) - z) / (2.0 * f);
        if (!(dg > 1e-12)) break;
        const double next = std::clamp(best - g / dg, lo, hi);
        if (std::abs(next - best) < 1e-15 * std::max(1.0, std::abs(best))) break;
        best = next;
    }
    return best;
}

// A curve as one polyline with an exact vertex at 'fraction' of its length (0 and 1: its ends). The
// index of that vertex.
int vertexAtFraction(std::vector<Vec3> &line, double fraction)
{
    if (line.size() < 2) return 0;
    const double t = std::clamp(fraction, 0.0, 1.0);
    if (t < 0.02) return 0;
    if (t > 0.98) return static_cast<int>(line.size()) - 1;
    double total = 0.0;
    for (std::size_t k = 0; k + 1 < line.size(); ++k) total += norm(line[k + 1] - line[k]);
    double walked = 0.0;
    const double target = t * total;
    for (std::size_t k = 0; k + 1 < line.size(); ++k)
    {
        const double piece = norm(line[k + 1] - line[k]);
        if (walked + piece >= target)
        {
            const double u = piece > 0.0 ? (target - walked) / piece : 0.0;
            if (u < 1e-6) return static_cast<int>(k);
            if (u > 1.0 - 1e-6) return static_cast<int>(k) + 1;
            line.insert(line.begin() + static_cast<std::ptrdiff_t>(k) + 1, line[k] + (line[k + 1] - line[k]) * u);
            return static_cast<int>(k) + 1;
        }
        walked += piece;
    }
    return static_cast<int>(line.size()) - 1;
}

// Points of the wire of an element, as polylines, with the index of the feed vertex (polyline, vertex).
struct Path
{
    std::vector<std::vector<Vec3>> polylines;
    int feedLine = -1, feedVertex = -1;
    // Vertices, on the first polyline, of the further sources of a plain wire (moreFeeds, in order).
    std::vector<int> moreVertices;
};

Path pathOf(const AntennaElement &e)
{
    Path path;
    const Frame f = frameOf(e);
    const Vec3 c = e.position;
    const double length = std::max(1e-6, finiteOr(e.length, 0.5));
    const bool fed = e.fed != 0;
    switch (e.type)
    {
    case Type::Wire:
    {
        // Below 0.02 and above 0.98 a source is at the end itself: the foot of a wire on a surface. The
        // wire has a vertex at each of its sources: the first one, then those of 'moreFeeds'.
        struct Cut
        {
            double t;
            std::size_t feed;
        };
        std::vector<Cut> cuts;
        auto place = [&cuts](double fraction)
        {
            const double t = std::clamp(finiteOr(fraction, 0.5), 0.0, 1.0);
            cuts.push_back({t < 0.02 ? 0.0 : t > 0.98 ? 1.0 : t, cuts.size()});
        };
        if (fed)
        {
            place(e.feedFraction);
            for (const AntennaFeed &more : e.moreFeeds) place(more.fraction);
        }
        std::vector<int> vertexOf(cuts.size(), -1);
        std::stable_sort(cuts.begin(), cuts.end(), [](const Cut &a, const Cut &b) { return a.t < b.t; });
        std::vector<Vec3> line{c - 0.5 * length * f.u};
        double last = 0.0;
        for (const Cut &cut : cuts)
        {
            if (cut.t <= 0.0)
                vertexOf[cut.feed] = 0;
            else if (cut.t >= 1.0)
                vertexOf[cut.feed] = -2; // the last vertex, known once the line is complete
            else
            {
                // Two sources at the same place share their vertex: refused when the scene is prepared.
                if (line.size() == 1 || cut.t - last > 1e-9) line.push_back(c + (cut.t - 0.5) * length * f.u);
                last = cut.t;
                vertexOf[cut.feed] = static_cast<int>(line.size()) - 1;
            }
        }
        line.push_back(c + 0.5 * length * f.u);
        for (int &vertex : vertexOf)
            if (vertex == -2) vertex = static_cast<int>(line.size()) - 1;
        path.polylines.push_back(line);
        if (fed)
        {
            path.feedLine = 0;
            path.feedVertex = vertexOf[0];
            path.moreVertices.assign(vertexOf.begin() + 1, vertexOf.end());
        }
        break;
    }
    case Type::Dipole:
    {
        // Arms tilted by (180 - apex) / 2 towards e2: a V opening along e2, straight at 180 degrees.
        const double beta = 0.5 * (180.0 - std::clamp(finiteOr(e.apexDeg, 180.0), 10.0, 180.0)) * kDeg;
        const Vec3 d1 = std::cos(beta) * f.u + std::sin(beta) * f.e2;
        const Vec3 d2 = -std::cos(beta) * f.u + std::sin(beta) * f.e2;
        path.polylines.push_back({c + 0.5 * length * d2, c, c + 0.5 * length * d1});
        path.feedLine = 0, path.feedVertex = 1;
        break;
    }
    case Type::FoldedDipole:
    {
        const Vec3 s = std::max(1e-6, finiteOr(e.width, 0.02)) * f.e2;
        const Vec3 a = c - 0.5 * length * f.u, b = c + 0.5 * length * f.u;
        path.polylines.push_back({c, b, b + s, a + s, a, c});
        path.feedLine = 0, path.feedVertex = 0;
        break;
    }
    case Type::CircularLoop:
    {
        const int sides = std::clamp(e.sides, 3, 200);
        const double r = std::max(1e-6, finiteOr(e.radius, 0.16));
        std::vector<Vec3> line;
        for (int k = 0; k <= sides; ++k)
        {
            const double phi = 2.0 * kPi * double(k % sides) / double(sides);
            line.push_back(c + r * (std::cos(phi) * f.e2 + std::sin(phi) * f.e3));
        }
        path.polylines.push_back(line);
        path.feedLine = 0, path.feedVertex = 0;
        break;
    }
    case Type::RectangularLoop:
    {
        // In the plane across the axis: width along e2, height (length) along e3; fed at the middle of
        // the side at -e3.
        const double w = 0.5 * std::max(1e-6, finiteOr(e.width, 0.2)), h = 0.5 * length;
        const Vec3 m = c - h * f.e3;
        path.polylines.push_back({m, m + w * f.e2, c + h * f.e3 + w * f.e2, c + h * f.e3 - w * f.e2, m - w * f.e2, m});
        path.feedLine = 0, path.feedVertex = 0;
        break;
    }
    case Type::Helix:
    {
        const int perTurn = std::clamp(e.sides, 6, 64);
        const double turns = std::clamp(finiteOr(e.turns, 5.0), 0.25, 60.0);
        const double r = std::max(1e-6, finiteOr(e.radius, 0.16)), pitch = std::max(0.0, finiteOr(e.pitch, 0.22));
        const int count = std::max(2, static_cast<int>(std::lround(turns * perTurn)));
        const double height = turns * pitch;
        std::vector<Vec3> line;
        for (int k = 0; k <= count; ++k)
        {
            const double t = 2.0 * kPi * turns * double(k) / double(count);
            line.push_back(c + (t / (2.0 * kPi) * pitch - 0.5 * height) * f.u + r * (std::cos(t) * f.e2 + std::sin(t) * f.e3));
        }
        path.polylines.push_back(line);
        // Fed at the middle of the wire by default (normal mode, like a coiled dipole); at 0, at its
        // foot: the axial mode, the foot standing on a ground plane.
        const double t = std::clamp(finiteOr(e.feedFraction, 0.5), 0.0, 1.0);
        path.feedLine = 0, path.feedVertex = std::clamp(static_cast<int>(std::floor(t * count + 1e-9)), 0, count);
        break;
    }
    case Type::Plate:
    {
        // Across the axis, as the rectangular loop: width along e2, height (length) along e3.
        const Vec3 w = 0.5 * std::max(1e-6, finiteOr(e.width, 0.2)) * f.e2, h = 0.5 * length * f.e3;
        path.polylines.push_back({c - w - h, c + w - h, c + w + h, c - w + h, c - w - h});
        break;
    }
    case Type::Disc:
    {
        const double r = std::max(1e-6, finiteOr(e.radius, 0.16));
        std::vector<Vec3> line;
        for (int k = 0; k <= 48; ++k)
        {
            const double phi = 2.0 * kPi * double(k % 48) / 48.0;
            line.push_back(c + r * (std::cos(phi) * f.e2 + std::sin(phi) * f.e3));
        }
        path.polylines.push_back(line);
        break;
    }
    case Type::Box:
    {
        // Its base across the axis as a plate (width along e2, length along e3), its height along the axis:
        // the two ends, then the four edges between them.
        const Vec3 w = 0.5 * std::max(1e-6, finiteOr(e.width, 0.2)) * f.e2, l = 0.5 * length * f.e3,
                   h = 0.5 * std::max(1e-6, finiteOr(e.height, 0.2)) * f.u;
        for (const double side : {-1.0, 1.0})
            path.polylines.push_back({c - w - l + side * h, c + w - l + side * h, c + w + l + side * h, c - w + l + side * h, c - w - l + side * h});
        for (const Vec3 &corner : {-w - l, w - l, w + l, l - w}) path.polylines.push_back({c + corner - h, c + corner + h});
        break;
    }
    case Type::Cylinder:
    case Type::Cone:
    {
        // The circle of each flat end, and four lines along the side (to the apex for a cone).
        const double r = std::max(1e-6, finiteOr(e.radius, 0.16));
        const Vec3 h = 0.5 * std::max(1e-6, finiteOr(e.height, 0.2)) * f.u;
        auto circle = [&](const Vec3 &middle)
        {
            std::vector<Vec3> line;
            for (int k = 0; k <= 48; ++k)
            {
                const double phi = 2.0 * kPi * double(k % 48) / 48.0;
                line.push_back(middle + r * (std::cos(phi) * f.e2 + std::sin(phi) * f.e3));
            }
            return line;
        };
        path.polylines.push_back(circle(c - h));
        if (e.type == Type::Cylinder) path.polylines.push_back(circle(c + h));
        for (int k = 0; k < 4; ++k)
        {
            const Vec3 radial = r * (std::cos(0.5 * kPi * k) * f.e2 + std::sin(0.5 * kPi * k) * f.e3);
            path.polylines.push_back({c - h + radial, e.type == Type::Cylinder ? c + h + radial : c + h});
        }
        break;
    }
    case Type::Paraboloid:
    {
        // The rim and a circle halfway out, and four meridians from the vertex.
        const double r = std::max(1e-6, finiteOr(e.radius, 0.5)), focal = std::max(1e-6, finiteOr(e.focalLength, 0.5));
        for (const double rho : {r, 0.5 * r})
        {
            std::vector<Vec3> line;
            for (int k = 0; k <= 48; ++k)
            {
                const double phi = 2.0 * kPi * double(k % 48) / 48.0;
                line.push_back(c + f.u * (rho * rho / (4.0 * focal)) + rho * (std::cos(phi) * f.e2 + std::sin(phi) * f.e3));
            }
            path.polylines.push_back(line);
        }
        for (int k = 0; k < 4; ++k)
        {
            const Vec3 radial = std::cos(0.5 * kPi * k) * f.e2 + std::sin(0.5 * kPi * k) * f.e3;
            std::vector<Vec3> line;
            for (int q = 0; q <= 16; ++q)
            {
                const double rho = r * q / 16.0;
                line.push_back(c + f.u * (rho * rho / (4.0 * focal)) + radial * rho);
            }
            path.polylines.push_back(line);
        }
        break;
    }
    case Type::ParabolicCylinder:
    {
        // The parabola at both ends, the two edges and the line of the vertex.
        const double half = 0.5 * std::max(1e-6, finiteOr(e.width, 1.0)), focal = std::max(1e-6, finiteOr(e.focalLength, 0.5));
        const Vec3 l = 0.5 * length * f.e3;
        for (const double side : {-1.0, 1.0})
        {
            std::vector<Vec3> line;
            for (int q = -16; q <= 16; ++q)
            {
                const double sq = half * q / 16.0;
                line.push_back(c + side * l + f.u * (sq * sq / (4.0 * focal)) + f.e2 * sq);
            }
            path.polylines.push_back(line);
        }
        for (const double sq : {-half, 0.0, half})
        {
            const Vec3 at = c + f.u * (sq * sq / (4.0 * focal)) + f.e2 * sq;
            path.polylines.push_back({at - l, at + l});
        }
        break;
    }
    case Type::Arc:
    {
        // From -sweep/2 to +sweep/2 about e2, in the plane across the axis; its source anywhere along it.
        const double r = std::max(1e-6, finiteOr(e.radius, 0.25));
        const double sweep = std::clamp(finiteOr(e.sweepDeg, 180.0), 1.0, 359.0) * kDeg;
        const int count = std::max(4, static_cast<int>(std::ceil(std::clamp(e.sides, 6, 200) * sweep / (2.0 * kPi))));
        std::vector<Vec3> line;
        for (int k = 0; k <= count; ++k)
        {
            const double phi = -0.5 * sweep + sweep * k / count;
            line.push_back(c + r * (std::cos(phi) * f.e2 + std::sin(phi) * f.e3));
        }
        const int vertex = fed ? vertexAtFraction(line, finiteOr(e.feedFraction, 0.5)) : -1;
        path.polylines.push_back(line);
        if (fed) path.feedLine = 0, path.feedVertex = vertex;
        break;
    }
    case Type::Spiral:
    {
        // Two arms r = r0 + a phi, the second the first turned by half a turn, joined across the centre
        // where the source is. Points about 'radius' x 2 pi / sides apart, twelve per turn at least.
        const double outer = std::max(1e-6, finiteOr(e.radius, 0.2));
        const double inner = std::clamp(finiteOr(e.innerRadius, 0.01), 1e-7, 0.9 * outer);
        const double turns = std::clamp(finiteOr(e.turns, 3.0), 0.25, 30.0);
        const double rate = (outer - inner) / (2.0 * kPi * turns);
        const double spacing = 2.0 * kPi * outer / std::clamp(e.sides, 8, 400);
        std::vector<Vec3> arm;
        const double end = 2.0 * kPi * turns;
        for (double phi = 0.0;;)
        {
            const double rho = inner + rate * phi;
            arm.push_back(rho * (std::cos(phi) * f.e2 + std::sin(phi) * f.e3));
            if (phi >= end) break;
            // No stub at the end: the last step takes what is left when less than half a step would remain.
            const double stepAngle = std::min(2.0 * kPi / 12.0, spacing / rho);
            phi = end - (phi + stepAngle) < 0.5 * stepAngle ? end : phi + stepAngle;
        }
        std::vector<Vec3> line;
        for (auto it = arm.rbegin(); it != arm.rend(); ++it) line.push_back(c - *it);
        line.push_back(c);
        for (const Vec3 &q : arm) line.push_back(c + q);
        path.polylines.push_back(line);
        if (fed) path.feedLine = 0, path.feedVertex = static_cast<int>(arm.size());
        break;
    }
    case Type::Sphere:
    {
        // Three great circles: across the axis, and two through it.
        const double r = std::max(1e-6, finiteOr(e.radius, 0.16));
        const Vec3 pairs[3][2] = {{f.e2, f.e3}, {f.e3, f.u}, {f.u, f.e2}};
        for (const auto &pair : pairs)
        {
            std::vector<Vec3> line;
            for (int k = 0; k <= 48; ++k)
            {
                const double phi = 2.0 * kPi * double(k % 48) / 48.0;
                line.push_back(c + r * (std::cos(phi) * pair[0] + std::sin(phi) * pair[1]));
            }
            path.polylines.push_back(line);
        }
        break;
    }
    }
    if (!fed || elementIsSurface(e.type))
    {
        path.feedLine = path.feedVertex = -1;
        path.moreVertices.clear();
    }
    return path;
}

// A surface in its own frame: u and v in its plane.
struct SurfaceGeometry
{
    Vec3 centre, u, v, normal;
    double halfU = 0.0, halfV = 0.0, radius = 0.0;
    bool disc = false;
};

SurfaceGeometry surfaceGeometry(const AntennaElement &e)
{
    const Frame f = frameOf(e);
    SurfaceGeometry g;
    g.centre = e.position;
    g.u = f.e2;
    g.v = f.e3;
    g.normal = f.u;
    g.disc = e.type == Type::Disc;
    g.radius = std::max(1e-6, finiteOr(e.radius, 0.16));
    g.halfU = 0.5 * std::max(1e-6, finiteOr(e.width, 0.2));
    g.halfV = 0.5 * std::max(1e-6, finiteOr(e.length, 0.5));
    return g;
}

// The flat faces of the skin of an element: a plate, a disc, the six faces of a box. None for a wire,
// nor for a curved solid (see onCurvedSkin).
std::vector<SurfaceGeometry> flatFaces(const AntennaElement &e)
{
    std::vector<SurfaceGeometry> faces;
    if (e.type == Type::Plate || e.type == Type::Disc)
        faces.push_back(surfaceGeometry(e));
    else if (e.type == Type::Box)
    {
        const Frame f = frameOf(e);
        const double w = 0.5 * std::max(1e-6, finiteOr(e.width, 0.2)), l = 0.5 * std::max(1e-6, finiteOr(e.length, 0.5)),
                     h = 0.5 * std::max(1e-6, finiteOr(e.height, 0.2));
        auto face = [&](const Vec3 &normal, double offset, const Vec3 &u, double halfU, const Vec3 &v, double halfV)
        {
            SurfaceGeometry g;
            g.centre = e.position + normal * offset;
            g.u = u;
            g.v = v;
            g.normal = normal;
            g.halfU = halfU;
            g.halfV = halfV;
            faces.push_back(g);
        };
        for (const double side : {-1.0, 1.0})
        {
            face(f.u * side, h, f.e2, w, f.e3, l);
            face(f.e2 * side, w, f.e3, l, f.u, h);
            face(f.e3 * side, l, f.u, h, f.e2, w);
        }
    }
    return faces;
}

// Curved solids are surfaces of revolution about their axis. Their profile, as points (radius, height
// along the axis from the centre): the corners for a cylinder and a cone; for a sphere its meridian
// from pole to pole, in arcs of 'step' at most (four at least).
std::vector<std::pair<double, double>> revolutionProfile(const AntennaElement &e, double step)
{
    const double r = std::max(1e-6, finiteOr(e.radius, 0.16)), h = 0.5 * std::max(1e-6, finiteOr(e.height, 0.2));
    if (e.type == Type::Cylinder) return {{0.0, -h}, {r, -h}, {r, h}, {0.0, h}};
    if (e.type == Type::Paraboloid) return parabolaProfile(r, finiteOr(e.focalLength, 0.5), step > 0.0 ? step : r / 200.0);
    if (e.type == Type::Cone) return {{0.0, -h}, {r, -h}, {0.0, h}};
    if (e.type != Type::Sphere) return {};
    const int bands = std::clamp(static_cast<int>(std::ceil(kPi * r / std::max(step, 1e-9) - 1e-9)), 4, 64);
    std::vector<std::pair<double, double>> profile;
    for (int q = 0; q <= bands; ++q)
        profile.push_back({q == 0 || q == bands ? 0.0 : r * std::sin(kPi * q / bands), -r * std::cos(kPi * q / bands)});
    return profile;
}

// The point of the profile nearest to (rho, z), in the half-plane of the axis.
std::pair<double, double> nearestOnProfile(const std::vector<std::pair<double, double>> &profile, double rho, double z)
{
    std::pair<double, double> nearest{rho, z};
    double best = std::numeric_limits<double>::infinity();
    for (std::size_t k = 0; k + 1 < profile.size(); ++k)
    {
        const double ar = profile[k].first, az = profile[k].second, dr = profile[k + 1].first - ar, dz = profile[k + 1].second - az;
        const double span = dr * dr + dz * dz;
        const double t = span > 0.0 ? std::clamp(((rho - ar) * dr + (z - az) * dz) / span, 0.0, 1.0) : 0.0;
        const double pr = ar + t * dr, pz = az + t * dz, distance = std::hypot(rho - pr, z - pz);
        if (distance < best)
        {
            best = distance;
            nearest = {pr, pz};
        }
    }
    return nearest;
}

// Whether 'p' lies on the skin of a curved solid (the exact surface, not its triangles).
bool onCurvedSkin(const AntennaElement &e, const Vec3 &p, double tolerance)
{
    const Vec3 axis = antennaAxis(e), rel = p - e.position;
    // The parabolic surfaces, exactly: the distance to the parabola, within the aperture.
    if (e.type == Type::Paraboloid || e.type == Type::ParabolicCylinder)
    {
        const Frame f = frameOf(e);
        const double focal = std::max(1e-6, finiteOr(e.focalLength, 0.5)), z = dot(rel, f.u);
        double across = 0.0, half = 0.0;
        if (e.type == Type::Paraboloid)
        {
            across = norm(rel - f.u * z);
            half = std::max(1e-6, finiteOr(e.radius, 0.5));
        }
        else
        {
            across = std::abs(dot(rel, f.e2));
            half = 0.5 * std::max(1e-6, finiteOr(e.width, 1.0));
            if (std::abs(dot(rel, f.e3)) > 0.5 * std::max(1e-6, finiteOr(e.length, 0.5)) + tolerance) return false;
        }
        if (across > half + tolerance) return false;
        const double sq = nearestOnParabola(across, z, focal, 0.0, half);
        return std::hypot(across - sq, z - sq * sq / (4.0 * focal)) <= tolerance;
    }
    if (e.type != Type::Cylinder && e.type != Type::Cone && e.type != Type::Sphere) return false;
    if (e.type == Type::Sphere) return std::abs(norm(rel) - std::max(1e-6, finiteOr(e.radius, 0.16))) <= tolerance;
    const double z = dot(rel, axis), rho = norm(rel - axis * z);
    const auto nearest = nearestOnProfile(revolutionProfile(e, 0.0), rho, z);
    return std::hypot(rho - nearest.first, z - nearest.second) <= tolerance;
}

bool liesOn(const SurfaceGeometry &g, const Vec3 &p, double tolerance)
{
    const Vec3 rel = p - g.centre;
    if (std::abs(dot(rel, g.normal)) > tolerance) return false;
    const double a = dot(rel, g.u), b = dot(rel, g.v);
    return g.disc ? std::hypot(a, b) <= g.radius + tolerance
                  : std::abs(a) <= g.halfU + tolerance && std::abs(b) <= g.halfV + tolerance;
}

Vec3 direction(double thetaDeg, double phiDeg)
{
    const double t = thetaDeg * kDeg, p = phiDeg * kDeg;
    return {std::sin(t) * std::cos(p), std::sin(t) * std::sin(p), std::cos(t)};
}

double toDb(double linear)
{
    return 10.0 * std::log10(std::max(linear, 1e-30));
}

struct Prepared
{
    NumericalEM::WireNetworkMomInput input;
    std::vector<int> feedElement;
    std::vector<int> feedSource; // index of each source on its element
    std::string error;
    double segmentsPerWavelength = 0.0;
};

Prepared prepare(const std::vector<AntennaElement> &elements, const AntennaSettings &settings, double frequency)
{
    std::string sameplace; // name of an element with two sources at the same place
    Prepared p;
    auto &in = p.input;
    in.frequencyHz = frequency;
    in.currentBasisTreatment = NumericalEM::WireCurrentBasisTreatment::LinearRooftopCharge;
    in.computeFarField = false;
    in.maxUnknowns = 1500;
    // The mesh follows the wavelength; segments never shorter than two radii (thin-wire kernel).
    const double lambda = qsig::constants::c0 / frequency;
    double maxRadius = 0.0;
    for (const auto &e : elements)
        if (!elementIsSurface(e.type)) maxRadius = std::max(maxRadius, finiteOr(e.wireRadius, 1e-3));
    double spw = std::clamp(finiteOr(settings.segmentsPerWavelength, 100.0), 10.0, 240.0);
    if (maxRadius > 0.0) spw = std::min(spw, lambda / (2.0 * maxRadius));
    p.segmentsPerWavelength = spw;
    in.segmentsPerWavelength = std::max(4, static_cast<int>(std::floor(spw)));
    in.maxSegmentLengthRadiusFactor = 50.0;
    for (std::size_t i = 0; i < elements.size(); ++i)
    {
        const AntennaElement &e = elements[i];
        if (elementIsSurface(e.type)) continue;
        const Path path = pathOf(e);
        const double a = std::max(1e-7, finiteOr(e.wireRadius, 1e-3));
        for (std::size_t l = 0; l < path.polylines.size(); ++l)
        {
            const auto &line = path.polylines[l];
            for (std::size_t k = 0; k + 1 < line.size(); ++k)
                if (norm(line[k + 1] - line[k]) > 1e-9) in.wires.push_back({std::to_string(i), line[k], line[k + 1], a});
        }
        if (path.feedLine >= 0)
        {
            NumericalEM::WireFeed3D feed;
            feed.name = e.name;
            feed.positionM = path.polylines[static_cast<std::size_t>(path.feedLine)][static_cast<std::size_t>(path.feedVertex)];
            feed.voltageV = std::polar(finiteOr(e.strength, 1.0), finiteOr(e.phaseDeg, 0.0) * kDeg);
            feed.referenceOhm = std::max(1e-3, finiteOr(settings.referenceOhm, 50.0));
            in.feeds.push_back(feed);
            p.feedElement.push_back(static_cast<int>(i));
            p.feedSource.push_back(0);
            // Further sources of a plain wire, each at its own vertex.
            std::vector<int> taken{path.feedVertex};
            for (std::size_t k = 0; k < path.moreVertices.size() && k < e.moreFeeds.size(); ++k)
            {
                const AntennaFeed &more = e.moreFeeds[k];
                const int vertex = path.moreVertices[k];
                if (std::find(taken.begin(), taken.end(), vertex) != taken.end())
                {
                    sameplace = e.name.empty() ? std::string("un fil") : e.name;
                    continue;
                }
                taken.push_back(vertex);
                NumericalEM::WireFeed3D further = feed;
                further.name = more.name.empty() ? e.name + " (" + std::to_string(k + 2) + ")" : more.name;
                further.positionM = path.polylines[0][static_cast<std::size_t>(vertex)];
                further.voltageV = std::polar(finiteOr(more.strength, 1.0), finiteOr(more.phaseDeg, 0.0) * kDeg);
                in.feeds.push_back(further);
                p.feedElement.push_back(static_cast<int>(i));
                p.feedSource.push_back(static_cast<int>(k) + 1);
            }
        }
    }
    if (in.wires.empty())
        p.error = "La scène ne contient aucun fil.";
    else if (in.feeds.empty())
        p.error = "Aucune source : alimentez au moins un élément.";
    else if (!sameplace.empty())
        p.error = "Deux sources sont au même endroit sur « " + sameplace + " » : écartez-les.";
    return p;
}

// Element of a mesh segment: its span is named after the element index.
int elementOfSegment(const NumericalEM::WireNetworkMomInput &in, const NumericalEM::WireNetworkMeshSegment &s)
{
    if (s.sourceWireIndex < 0 || s.sourceWireIndex >= static_cast<int>(in.wires.size())) return -1;
    return std::stoi(in.wires[static_cast<std::size_t>(s.sourceWireIndex)].name);
}
} // namespace

bool elementIsSurface(Type type)
{
    return type == Type::Plate || type == Type::Disc || type == Type::Paraboloid || type == Type::ParabolicCylinder ||
           elementIsSolid(type);
}

bool elementIsSolid(Type type)
{
    return type == Type::Box || type == Type::Cylinder || type == Type::Cone || type == Type::Sphere;
}

bool elementIsDielectric(const AntennaElement &element)
{
    return elementIsSolid(element.type) && element.medium == 1;
}

int surfaceAt(const std::vector<AntennaElement> &elements, const Vec3 &point, double tolerance)
{
    for (std::size_t i = 0; i < elements.size(); ++i)
    {
        // A conducting skin: a wire is not attached to a dielectric, no source stands on it.
        if (!elementIsSurface(elements[i].type) || elementIsDielectric(elements[i])) continue;
        if (onCurvedSkin(elements[i], point, tolerance)) return static_cast<int>(i);
        for (const SurfaceGeometry &g : flatFaces(elements[i]))
            if (liesOn(g, point, tolerance)) return static_cast<int>(i);
    }
    return -1;
}

Vec3 projectOnSurface(const AntennaElement &surface, const Vec3 &point)
{
    const Vec3 normal = antennaAxis(surface);
    if (surface.type == Type::Paraboloid || surface.type == Type::ParabolicCylinder)
    {
        // The nearest point of the parabola, in the plane of the axis that holds the point.
        const Frame f = frameOf(surface);
        const Vec3 rel = point - surface.position;
        const double focal = std::max(1e-6, finiteOr(surface.focalLength, 0.5)), z = dot(rel, f.u);
        if (surface.type == Type::Paraboloid)
        {
            const Vec3 acrossVector = rel - f.u * z;
            const double across = norm(acrossVector);
            const Vec3 radial = across > 1e-12 ? acrossVector * (1.0 / across) : f.e2;
            const double sq = nearestOnParabola(across, z, focal, 0.0, std::max(1e-6, finiteOr(surface.radius, 0.5)));
            return surface.position + f.u * (sq * sq / (4.0 * focal)) + radial * sq;
        }
        const double half = 0.5 * std::max(1e-6, finiteOr(surface.width, 1.0)), along = 0.5 * std::max(1e-6, finiteOr(surface.length, 0.5));
        const double sq = nearestOnParabola(dot(rel, f.e2), z, focal, -half, half);
        return surface.position + f.u * (sq * sq / (4.0 * focal)) + f.e2 * sq + f.e3 * std::clamp(dot(rel, f.e3), -along, along);
    }
    if (!elementIsSolid(surface.type)) return point - normal * dot(point - surface.position, normal);
    const Frame f = frameOf(surface);
    const Vec3 rel = point - surface.position;
    const double r = std::max(1e-6, finiteOr(surface.radius, 0.16));
    if (surface.type == Type::Sphere)
    {
        const double distance = norm(rel);
        return surface.position + (distance > 1e-12 ? rel * (r / distance) : f.u * r);
    }
    if (surface.type == Type::Box)
    {
        // Outside: each coordinate brought back into the box. Inside: out through the nearest face.
        const Vec3 axes[3] = {f.e2, f.e3, f.u};
        const double halves[3] = {0.5 * std::max(1e-6, finiteOr(surface.width, 0.2)), 0.5 * std::max(1e-6, finiteOr(surface.length, 0.5)),
                                  0.5 * std::max(1e-6, finiteOr(surface.height, 0.2))};
        double local[3];
        bool inside = true;
        int nearest = 0;
        double gap = std::numeric_limits<double>::infinity();
        for (int k = 0; k < 3; ++k)
        {
            local[k] = dot(rel, axes[k]);
            if (std::abs(local[k]) > halves[k]) inside = false;
            if (halves[k] - std::abs(local[k]) < gap)
            {
                gap = halves[k] - std::abs(local[k]);
                nearest = k;
            }
            local[k] = std::clamp(local[k], -halves[k], halves[k]);
        }
        if (inside) local[nearest] = local[nearest] < 0.0 ? -halves[nearest] : halves[nearest];
        return surface.position + axes[0] * local[0] + axes[1] * local[1] + axes[2] * local[2];
    }
    // Cylinder, cone: in the half-plane of the axis that holds the point, the nearest point of the profile.
    const double z = dot(rel, f.u);
    const Vec3 across = rel - f.u * z;
    const double rho = norm(across);
    const Vec3 radial = rho > 1e-12 ? across * (1.0 / rho) : f.e2;
    const auto nearest = nearestOnProfile(revolutionProfile(surface, 0.0), rho, z);
    return surface.position + f.u * nearest.second + radial * nearest.first;
}

Vec3 antennaAxis(const AntennaElement &e)
{
    const double az = finiteOr(e.azimuthDeg, 0.0) * kDeg, el = finiteOr(e.elevationDeg, 90.0) * kDeg;
    return normalized({std::cos(el) * std::cos(az), std::cos(el) * std::sin(az), std::sin(el)});
}

void wireEndpoints(const AntennaElement &wire, Vec3 &a, Vec3 &b)
{
    const Vec3 half = antennaAxis(wire) * (0.5 * std::max(finiteOr(wire.length, 0.0), 0.0));
    a = wire.position - half;
    b = wire.position + half;
}

bool setWireEndpoints(AntennaElement &wire, const Vec3 &a, const Vec3 &b)
{
    const Vec3 d = b - a;
    const double length = norm(d);
    if (!std::isfinite(length) || !(length > 10.0 * std::max(wire.wireRadius, 0.0)) || !(length > 0.0)) return false;
    wire.position = (a + b) * 0.5;
    wire.length = length;
    wire.elevationDeg = std::atan2(d.z, std::hypot(d.x, d.y)) / kDeg;
    // A vertical wire has no azimuth: 0 by convention.
    wire.azimuthDeg = std::hypot(d.x, d.y) > 1e-12 * length ? std::atan2(d.y, d.x) / kDeg : 0.0;
    return true;
}

double wireFraction(const AntennaElement &wire, const Vec3 &p)
{
    Vec3 a, b;
    wireEndpoints(wire, a, b);
    const Vec3 d = b - a;
    const double length2 = dot(d, d);
    return length2 > 0.0 ? std::clamp(dot(p - a, d) / length2, 0.0, 1.0) : 0.5;
}

bool feedSlides(Type type)
{
    return type == Type::Wire || type == Type::Arc;
}

double curveFraction(const AntennaElement &element, const Vec3 &p)
{
    if (element.type == Type::Wire) return wireFraction(element, p);
    if (element.type != Type::Arc) return 0.5;
    // The angle of the point about the axis, from -sweep/2 (0) to +sweep/2 (1).
    const Frame f = frameOf(element);
    const Vec3 rel = p - element.position;
    const double sweep = std::clamp(finiteOr(element.sweepDeg, 180.0), 1.0, 359.0) * kDeg;
    double phi = std::atan2(dot(rel, f.e3), dot(rel, f.e2));
    // Past an end, the nearer end (angles round the circle).
    if (std::abs(phi) > 0.5 * sweep)
    {
        auto apart = [](double a, double b)
        {
            const double d = std::fmod(std::abs(a - b), 2.0 * kPi);
            return std::min(d, 2.0 * kPi - d);
        };
        phi = apart(phi, 0.5 * sweep) <= apart(phi, -0.5 * sweep) ? 0.5 * sweep : -0.5 * sweep;
    }
    return std::clamp((phi + 0.5 * sweep) / sweep, 0.0, 1.0);
}

std::vector<Vec3> antennaAnchors(const std::vector<AntennaElement> &elements, int exclude, bool feeds)
{
    std::vector<Vec3> anchors;
    for (std::size_t i = 0; i < elements.size(); ++i)
    {
        if (static_cast<int>(i) == exclude || elementIsSurface(elements[i].type)) continue;
        const AntennaShape shape = antennaShape(elements[i]);
        for (const auto &line : shape.polylines)
        {
            if (line.empty()) continue;
            anchors.push_back(line.front());
            if (norm(line.back() - line.front()) > 0.0) anchors.push_back(line.back()); // not twice for a closed loop
        }
        if (feeds)
            for (const auto &feed : shape.feeds) anchors.push_back(feed);
    }
    return anchors;
}

namespace
{
// An angle in degrees without the dust of the trigonometry: 90 and not 89.99999999999999.
double tidyDegrees(double degrees)
{
    const double rounded = std::round(degrees * 1e9) / 1e9;
    return rounded == 0.0 ? 0.0 : rounded;
}

// Orientation angles of an element from its axis 'u' and its transverse direction 'e2' (see frameOf).
void setFrame(AntennaElement &e, const Vec3 &u, const Vec3 &e2)
{
    const Vec3 axis = normalized(u);
    // atan2 and not asin: near the vertical, asin turns one bit of z into a microdegree.
    e.elevationDeg = tidyDegrees(std::atan2(axis.z, std::hypot(axis.x, axis.y)) / kDeg);
    // A vertical axis has no azimuth: 0 by convention.
    e.azimuthDeg = std::hypot(axis.x, axis.y) > 1e-12 ? tidyDegrees(std::atan2(axis.y, axis.x) / kDeg) : 0.0;
    // The roll is counted from the transverse axes frameOf() builds for this axis.
    e.rollDeg = 0.0;
    const Frame f = frameOf(e);
    e.rollDeg = tidyDegrees(std::atan2(dot(e2, f.e3), dot(e2, f.e2)) / kDeg);
}

// Rodrigues: v turned by 'angle' about the unit vector k.
Vec3 turned(const Vec3 &v, const Vec3 &k, double angle)
{
    const double c = std::cos(angle), s = std::sin(angle);
    return v * c + cross(k, v) * s + k * (dot(k, v) * (1.0 - c));
}
} // namespace

void rotateElement(AntennaElement &element, const Vec3 &pivot, const Vec3 &axis, double angleDeg)
{
    const double length = norm(axis);
    if (!(length > 0.0) || !std::isfinite(angleDeg)) return;
    const Vec3 k = axis / length;
    const double angle = angleDeg * kDeg;
    const Frame f = frameOf(element);
    element.position = pivot + turned(element.position - pivot, k, angle);
    setFrame(element, turned(f.u, k, angle), turned(f.e2, k, angle));
}

void mirrorElement(AntennaElement &element, const Vec3 &point, const Vec3 &normal)
{
    const double length = norm(normal);
    if (!(length > 0.0)) return;
    const Vec3 n = normal / length;
    auto image = [&n](const Vec3 &v) { return v - n * (2.0 * dot(v, n)); };
    const Frame f = frameOf(element);
    element.position = point + image(element.position - point);
    // A mirror turns a right-handed frame into a left-handed one, and e3 = u x e2 is not free. Elements
    // drawn with u and e2 (wires, dipoles) take the images of both. Those drawn with e2 and e3 around
    // the axis (loops, helix) take the image of e2 and the opposite of the image of u, which makes
    // u x e2 the image of e3: the loop and its source are the exact images, the helix keeps its sense.
    // A cone and a parabolic surface open along their axis: they take its image, as a wire does (their
    // shape is the same on both sides of e2 and e3). Arcs and spirals are drawn around the axis.
    const bool alongAxis = element.type == Type::Cone || element.type == Type::Paraboloid || element.type == Type::ParabolicCylinder;
    const bool aroundAxis = !alongAxis && (element.type == Type::CircularLoop || element.type == Type::RectangularLoop ||
                                           element.type == Type::Helix || element.type == Type::Arc || element.type == Type::Spiral ||
                                           elementIsSurface(element.type));
    setFrame(element, aroundAxis ? -image(f.u) : image(f.u), image(f.e2));
    // The axis of the helix has turned over: its source stays at the end it was at.
    if (element.type == Type::Helix) element.feedFraction = 1.0 - std::clamp(finiteOr(element.feedFraction, 0.5), 0.0, 1.0);
}

Vec3 elementsCentre(const std::vector<AntennaElement> &elements, const std::vector<int> &rows)
{
    double lo[3] = {1e300, 1e300, 1e300}, hi[3] = {-1e300, -1e300, -1e300};
    bool any = false;
    for (const int row : rows)
    {
        if (row < 0 || row >= static_cast<int>(elements.size())) continue;
        for (const auto &line : pathOf(elements[static_cast<std::size_t>(row)]).polylines)
            for (const Vec3 &q : line)
            {
                const double v[3] = {q.x, q.y, q.z};
                for (int k = 0; k < 3; ++k)
                {
                    lo[k] = std::min(lo[k], v[k]);
                    hi[k] = std::max(hi[k], v[k]);
                }
                any = true;
            }
    }
    if (!any) return {};
    return {0.5 * (lo[0] + hi[0]), 0.5 * (lo[1] + hi[1]), 0.5 * (lo[2] + hi[2])};
}

// ---- Constraints ----

namespace
{
using Constraint = AntennaConstraint;
constexpr double kLengthTolerance = 1e-9;  // [m] positions, lengths, distances
constexpr double kAngleTolerance = 1e-7;   // [rad] directions

const AntennaElement *elementById(const std::vector<AntennaElement> &elements, int id)
{
    if (id <= 0) return nullptr;
    for (const auto &e : elements)
        if (e.id == id) return &e;
    return nullptr;
}

Vec3 anyPerpendicular(const Vec3 &u)
{
    const Vec3 other = std::abs(u.z) < 0.9 ? Vec3{0.0, 0.0, 1.0} : Vec3{1.0, 0.0, 0.0};
    return normalized(cross(u, other));
}

// Turns the element about its centre, by the shortest way, until its axis is 'target' (a unit vector).
bool turnAxisTo(AntennaElement &e, const Vec3 &target)
{
    const Vec3 u = antennaAxis(e);
    const Vec3 k = cross(u, target);
    const double sine = norm(k), cosine = dot(u, target);
    const double angle = std::atan2(sine, cosine);
    if (!(angle > 0.5 * kAngleTolerance)) return false;
    rotateElement(e, e.position, sine > 1e-12 ? k : anyPerpendicular(u), angle / kDeg);
    return true;
}

// Direction at 'angle' from 'reference', in the plane that holds 'current' (any plane when they are parallel).
Vec3 directionAt(const Vec3 &reference, const Vec3 &current, double angle)
{
    Vec3 across = current - reference * dot(current, reference);
    across = norm(across) > 1e-9 ? normalized(across) : anyPerpendicular(reference);
    return reference * std::cos(angle) + across * std::sin(angle);
}

// Moves, turns or resizes 'e' to meet one constraint. True when something changed.
bool enforce(const std::vector<AntennaElement> &elements, AntennaElement &e, const Constraint &c)
{
    const AntennaElement *reference = elementById(elements, c.reference);
    if (constraintNeedsReference(c.type) && (!reference || reference == &e)) return false;
    switch (c.type)
    {
    case Constraint::Type::FixPosition:
    {
        Vec3 p = e.position;
        if (c.axes & 1) p.x = c.fixed.x;
        if (c.axes & 2) p.y = c.fixed.y;
        if (c.axes & 4) p.z = c.fixed.z;
        if (!(norm(p - e.position) > 0.5 * kLengthTolerance)) return false;
        e.position = p;
        return true;
    }
    case Constraint::Type::Length:
    case Constraint::Type::EqualLength:
    {
        if (!elementHasLength(e.type)) return false;
        if (c.type == Constraint::Type::EqualLength && !elementHasLength(reference->type)) return false;
        const double length = c.type == Constraint::Type::Length ? c.value : reference->length;
        if (!(length > 0.0) || !(std::abs(length - e.length) > 0.5 * kLengthTolerance)) return false;
        e.length = length;
        return true;
    }
    case Constraint::Type::Parallel:
    {
        // The sense the element already has: a wire is not turned round to become parallel.
        const Vec3 axis = antennaAxis(*reference);
        return turnAxisTo(e, dot(antennaAxis(e), axis) >= 0.0 ? axis : -axis);
    }
    case Constraint::Type::Perpendicular:
        return turnAxisTo(e, directionAt(antennaAxis(*reference), antennaAxis(e), 0.5 * kPi));
    case Constraint::Type::Angle:
        return turnAxisTo(e, directionAt(antennaAxis(*reference), antennaAxis(e), std::clamp(c.value, 0.0, 180.0) * kDeg));
    case Constraint::Type::Coincident:
    case Constraint::Type::Distance:
    {
        const Vec3 from = elementAnchor(*reference, c.referenceAnchor), to = elementAnchor(e, c.anchor);
        Vec3 shift = from - to;
        if (c.type == Constraint::Type::Distance)
        {
            // Along the line that joins the two points (along x when they are at the same place).
            const double distance = norm(to - from);
            const Vec3 away = distance > 1e-12 ? (to - from) / distance : Vec3{1.0, 0.0, 0.0};
            shift = away * (std::max(c.value, 0.0) - distance);
        }
        if (!(norm(shift) > 0.5 * kLengthTolerance)) return false;
        e.position += shift;
        return true;
    }
    }
    return false;
}
} // namespace

bool elementHasLength(Type type)
{
    return type == Type::Wire || type == Type::Dipole || type == Type::FoldedDipole || type == Type::RectangularLoop;
}

bool constraintNeedsReference(AntennaConstraint::Type type)
{
    return type != Constraint::Type::FixPosition && type != Constraint::Type::Length;
}

void ensureElementIds(std::vector<AntennaElement> &elements)
{
    int last = 0;
    for (const auto &e : elements) last = std::max(last, e.id);
    std::vector<int> seen;
    seen.reserve(elements.size());
    for (auto &e : elements)
    {
        const bool taken = e.id > 0 && std::find(seen.begin(), seen.end(), e.id) != seen.end();
        if (taken)
        {
            e.constraints.clear();
            e.group.clear();
        }
        if (e.id <= 0 || taken) e.id = ++last;
        seen.push_back(e.id);
    }
}

void pruneConstraints(std::vector<AntennaElement> &elements)
{
    for (auto &e : elements)
        std::erase_if(e.constraints,
                      [&](const Constraint &c)
                      {
                          if (!constraintNeedsReference(c.type)) return false;
                          const AntennaElement *reference = elementById(elements, c.reference);
                          return !reference || reference == &e;
                      });
}

Vec3 elementAnchor(const AntennaElement &element, int anchor)
{
    if ((anchor == 1 || anchor == 2) && !elementIsSurface(element.type))
    {
        const Path path = pathOf(element);
        if (!path.polylines.empty() && !path.polylines.front().empty() && !path.polylines.back().empty())
            return anchor == 1 ? path.polylines.front().front() : path.polylines.back().back();
    }
    return element.position;
}

double constraintError(const std::vector<AntennaElement> &elements, const AntennaElement &e, const AntennaConstraint &c)
{
    const AntennaElement *reference = elementById(elements, c.reference);
    if (constraintNeedsReference(c.type) && (!reference || reference == &e)) return 0.0;
    switch (c.type)
    {
    case Constraint::Type::FixPosition:
    {
        double error = 0.0;
        if (c.axes & 1) error = std::max(error, std::abs(e.position.x - c.fixed.x));
        if (c.axes & 2) error = std::max(error, std::abs(e.position.y - c.fixed.y));
        if (c.axes & 4) error = std::max(error, std::abs(e.position.z - c.fixed.z));
        return error;
    }
    case Constraint::Type::Length:
        return elementHasLength(e.type) ? std::abs(e.length - c.value) : 0.0;
    case Constraint::Type::EqualLength:
        return elementHasLength(e.type) && elementHasLength(reference->type) ? std::abs(e.length - reference->length) : 0.0;
    case Constraint::Type::Parallel:
        return std::asin(std::min(1.0, norm(cross(antennaAxis(e), antennaAxis(*reference)))));
    case Constraint::Type::Perpendicular:
        return std::asin(std::min(1.0, std::abs(dot(antennaAxis(e), antennaAxis(*reference)))));
    case Constraint::Type::Angle:
    {
        const Vec3 u = antennaAxis(e), v = antennaAxis(*reference);
        return std::abs(std::atan2(norm(cross(u, v)), dot(u, v)) - std::clamp(c.value, 0.0, 180.0) * kDeg);
    }
    case Constraint::Type::Coincident:
        return norm(elementAnchor(e, c.anchor) - elementAnchor(*reference, c.referenceAnchor));
    case Constraint::Type::Distance:
        return std::abs(norm(elementAnchor(e, c.anchor) - elementAnchor(*reference, c.referenceAnchor)) - std::max(c.value, 0.0));
    }
    return 0.0;
}

bool constraintMet(const std::vector<AntennaElement> &elements, const AntennaElement &element, const AntennaConstraint &c)
{
    const bool direction = c.type == Constraint::Type::Parallel || c.type == Constraint::Type::Perpendicular ||
                           c.type == Constraint::Type::Angle;
    // A little above what the enforcement leaves: angles are stored to a billionth of a degree.
    return constraintError(elements, element, c) <= (direction ? kAngleTolerance : 10.0 * kLengthTolerance);
}

int applyConstraints(std::vector<AntennaElement> &elements)
{
    for (int pass = 0; pass < 16; ++pass)
    {
        bool changed = false;
        for (auto &e : elements)
        {
            // A copy: enforcing changes the element, not its list of constraints, but stay clear of aliasing.
            const std::vector<Constraint> constraints = e.constraints;
            for (const Constraint &c : constraints)
                if (c.enabled && enforce(elements, e, c)) changed = true;
        }
        if (!changed) break;
    }
    int unmet = 0;
    for (const auto &e : elements)
        for (const Constraint &c : e.constraints)
            if (c.enabled && !constraintMet(elements, e, c)) ++unmet;
    return unmet;
}

bool splitWire(const AntennaElement &wire, double fraction, AntennaElement &first, AntennaElement &second)
{
    if (wire.type != Type::Wire || !(fraction > 0.0) || !(fraction < 1.0)) return false;
    Vec3 a, b;
    wireEndpoints(wire, a, b);
    const Vec3 p = a + (b - a) * fraction;
    AntennaElement head = wire, tail = wire;
    if (!setWireEndpoints(head, a, p) || !setWireEndpoints(tail, p, b)) return false;
    // Both parts keep the direction of the wire to the last bit.
    head.azimuthDeg = tail.azimuthDeg = wire.azimuthDeg;
    head.elevationDeg = tail.elevationDeg = wire.elevationDeg;
    if (wire.fed != 0)
    {
        // The source stays where it is, on the part that holds it (a source right at the cut goes to
        // the second part, as near to the junction as a source can be).
        const double t = std::clamp(finiteOr(wire.feedFraction, 0.5), 0.0, 1.0);
        AntennaElement &bare = t < fraction ? tail : head;
        bare.fed = 0;
        bare.feedFraction = 0.5;
        // A source at an end of the wire stays at that end.
        if (t < fraction)
            head.feedFraction = t < 0.02 ? 0.0 : t / fraction;
        else
            tail.feedFraction = t > 0.98 ? 1.0 : (t - fraction) / (1.0 - fraction);
    }
    // The further sources go to the part that holds them; a part left with further sources only gets
    // the first of them as its own.
    head.moreFeeds.clear();
    tail.moreFeeds.clear();
    if (wire.fed != 0)
        for (AntennaFeed more : wire.moreFeeds)
        {
            const double t = std::clamp(finiteOr(more.fraction, 0.5), 0.0, 1.0);
            AntennaElement &part = t < fraction ? head : tail;
            more.fraction = t < fraction ? (t < 0.02 ? 0.0 : t / fraction) : (t > 0.98 ? 1.0 : (t - fraction) / (1.0 - fraction));
            if (part.fed != 0)
                part.moreFeeds.push_back(more);
            else
            {
                part.fed = 1;
                part.feedFraction = more.fraction;
                part.strength = more.strength;
                part.phaseDeg = more.phaseDeg;
            }
        }
    first = head;
    second = tail;
    return true;
}

int feedCount(const AntennaElement &element)
{
    if (element.fed == 0 || elementIsSurface(element.type)) return 0;
    return element.type == Type::Wire ? 1 + static_cast<int>(element.moreFeeds.size()) : 1;
}

AntennaFeed feedAt(const AntennaElement &element, int index)
{
    if (index > 0 && index < feedCount(element)) return element.moreFeeds[static_cast<std::size_t>(index) - 1];
    AntennaFeed first;
    first.fraction = element.feedFraction;
    first.strength = element.strength;
    first.phaseDeg = element.phaseDeg;
    return first;
}

bool canAddFeed(const AntennaElement &element)
{
    return !elementIsSurface(element.type) && (element.fed == 0 || element.type == Type::Wire);
}

bool setFeedAt(AntennaElement &element, int index, const AntennaFeed &source)
{
    if (index < 0 || index >= feedCount(element)) return false;
    if (index > 0)
    {
        element.moreFeeds[static_cast<std::size_t>(index) - 1] = source;
        return true;
    }
    element.feedFraction = source.fraction;
    element.strength = source.strength;
    element.phaseDeg = source.phaseDeg;
    return true;
}

int addFeed(AntennaElement &element, const AntennaFeed &source)
{
    if (!canAddFeed(element)) return -1;
    if (element.fed == 0)
    {
        element.fed = 1;
        element.moreFeeds.clear();
        setFeedAt(element, 0, source);
        return 0;
    }
    element.moreFeeds.push_back(source);
    return static_cast<int>(element.moreFeeds.size());
}

bool removeFeed(AntennaElement &element, int index)
{
    const int count = feedCount(element);
    if (index < 0 || index >= count) return false;
    if (index > 0)
    {
        element.moreFeeds.erase(element.moreFeeds.begin() + (index - 1));
        return true;
    }
    if (count > 1)
    {
        // The next source becomes the first one (it keeps its place and its voltage, not its own name).
        const AntennaFeed next = element.moreFeeds.front();
        element.moreFeeds.erase(element.moreFeeds.begin());
        setFeedAt(element, 0, next);
        return true;
    }
    element.fed = 0;
    element.moreFeeds.clear();
    return true;
}

AntennaShape antennaShape(const AntennaElement &element)
{
    const Path path = pathOf(element);
    AntennaShape shape;
    shape.polylines = path.polylines;
    shape.surface = elementIsSurface(element.type);
    // The skin as closed polygons.
    if (element.type == Type::Plate || element.type == Type::Disc)
        shape.faces.push_back(path.polylines.front());
    else if (element.type == Type::Box)
    {
        for (const SurfaceGeometry &g : flatFaces(element))
        {
            const Vec3 a = g.u * g.halfU, b = g.v * g.halfV;
            shape.faces.push_back({g.centre - a - b, g.centre + a - b, g.centre + a + b, g.centre - a + b, g.centre - a - b});
        }
    }
    else if (element.type == Type::ParabolicCylinder)
    {
        // Bands of the parabola along the length; the focal line marked.
        const Frame f = frameOf(element);
        const double half = 0.5 * std::max(1e-6, finiteOr(element.width, 1.0)), focal = std::max(1e-6, finiteOr(element.focalLength, 0.5));
        const Vec3 l = f.e3 * (0.5 * std::max(1e-6, finiteOr(element.length, 0.5)));
        auto at = [&](double sq) { return element.position + f.u * (sq * sq / (4.0 * focal)) + f.e2 * sq; };
        for (int q = -8; q < 8; ++q)
        {
            const Vec3 a = at(half * q / 8.0), b = at(half * (q + 1) / 8.0);
            shape.faces.push_back({a - l, b - l, b + l, a + l, a - l});
        }
        const Vec3 focus = element.position + f.u * focal;
        shape.marks = {focus - l, focus, focus + l};
    }
    else if (elementIsSolid(element.type) || element.type == Type::Paraboloid)
    {
        // A curved solid in sixteen sectors: a polygon for each flat end, a band of quadrilaterals for
        // each stretch of its profile (six bands from pole to pole for a sphere).
        const Frame f = frameOf(element);
        const double r = std::max(1e-6, finiteOr(element.radius, 0.16));
        const auto profile = revolutionProfile(element, kPi * r / 6.0);
        const int sectors = 16;
        auto at = [&](const std::pair<double, double> &ring, int q)
        {
            const double angle = 2.0 * kPi * (q % sectors) / sectors;
            return element.position + f.u * ring.second + (f.e2 * std::cos(angle) + f.e3 * std::sin(angle)) * ring.first;
        };
        for (std::size_t k = 0; k + 1 < profile.size(); ++k)
        {
            const auto &lower = profile[k], &upper = profile[k + 1];
            if (lower.second == upper.second)
            {
                std::vector<Vec3> end;
                for (int q = 0; q <= sectors; ++q) end.push_back(at(lower.first > upper.first ? lower : upper, q));
                shape.faces.push_back(end);
                continue;
            }
            for (int q = 0; q < sectors; ++q) shape.faces.push_back({at(lower, q), at(lower, q + 1), at(upper, q + 1), at(upper, q), at(lower, q)});
        }
        if (element.type == Type::Paraboloid) shape.marks = {element.position + f.u * std::max(1e-6, finiteOr(element.focalLength, 0.5))};
    }
    if (path.feedLine >= 0)
        shape.feeds.push_back(path.polylines[static_cast<std::size_t>(path.feedLine)][static_cast<std::size_t>(path.feedVertex)]);
    for (const int vertex : path.moreVertices) shape.feeds.push_back(path.polylines[0][static_cast<std::size_t>(vertex)]);
    return shape;
}

namespace
{
// Scene with surfaces: wires and triangles in one system (qsig::mom). The surfaces are meshed with a
// vertex under every wire end that stands on them: the wire is attached there.
// Line along which two plates touch, when it is parallel to a side of each of them: its two ends.
// Parallel planes (coplanar plates too) have none: side by side, their meshes meet by their vertices.
bool contactLine(const SurfaceGeometry &a, const SurfaceGeometry &b, double tolerance, Vec3 &from, Vec3 &to)
{
    if (a.disc || b.disc) return false;
    const Vec3 across = cross(a.normal, b.normal);
    const double sine = norm(across);
    if (sine < 1e-6) return false;
    const Vec3 along = across * (1.0 / sine);
    auto alongSide = [&](const SurfaceGeometry &g)
    {
        return std::abs(std::abs(dot(along, g.u)) - 1.0) < 1e-6 || std::abs(std::abs(dot(along, g.v)) - 1.0) < 1e-6;
    };
    if (!alongSide(a) || !alongSide(b)) return false;
    // A point of both planes (n . x = h), then the part of the line inside both rectangles.
    const double ha = dot(a.centre, a.normal), hb = dot(b.centre, b.normal);
    const Vec3 origin = (cross(b.normal, across) * ha + cross(across, a.normal) * hb) * (1.0 / (sine * sine));
    // The plates touch within the tolerance; the ends of the line are those of the plates themselves,
    // so that two contact lines that meet at a corner end on the same point.
    double lo = -1e300, hi = 1e300, exactLo = -1e300, exactHi = 1e300;
    auto clip = [&](const SurfaceGeometry &g)
    {
        const Vec3 rel = origin - g.centre;
        const Vec3 axes[2] = {g.u, g.v};
        const double halves[2] = {g.halfU, g.halfV};
        for (int k = 0; k < 2; ++k)
        {
            const double slope = dot(along, axes[k]), offset = dot(rel, axes[k]), half = halves[k] + tolerance;
            if (std::abs(slope) < 1e-9)
            {
                if (std::abs(offset) > half) lo = 1e300; // the line misses this plate
                continue;
            }
            const double t0 = (-half - offset) / slope, t1 = (half - offset) / slope;
            lo = std::max(lo, std::min(t0, t1));
            hi = std::min(hi, std::max(t0, t1));
            const double e0 = (-halves[k] - offset) / slope, e1 = (halves[k] - offset) / slope;
            exactLo = std::max(exactLo, std::min(e0, e1));
            exactHi = std::min(exactHi, std::max(e0, e1));
        }
    };
    clip(a);
    clip(b);
    if (!(hi - lo > 100.0 * tolerance)) return false;
    from = origin + along * std::clamp(exactLo, lo, hi);
    to = origin + along * std::clamp(exactHi, lo, hi);
    return true;
}

// Whether 'p' is inside a solid, farther than 'margin' from its skin.
bool insideSolid(const AntennaElement &e, const Vec3 &p, double margin)
{
    const Frame f = frameOf(e);
    const Vec3 rel = p - e.position;
    const double r = std::max(1e-6, finiteOr(e.radius, 0.16)), h = 0.5 * std::max(1e-6, finiteOr(e.height, 0.2));
    const double z = dot(rel, f.u);
    switch (e.type)
    {
    case Type::Box:
        return std::abs(dot(rel, f.e2)) < 0.5 * std::max(1e-6, finiteOr(e.width, 0.2)) - margin &&
               std::abs(dot(rel, f.e3)) < 0.5 * std::max(1e-6, finiteOr(e.length, 0.5)) - margin && std::abs(z) < h - margin;
    case Type::Sphere:
        return norm(rel) < r - margin;
    case Type::Cylinder:
        return norm(rel - f.u * z) < r - margin && std::abs(z) < h - margin;
    case Type::Cone:
        // The radius of the cone at this height, from the base to the apex.
        return z > -h + margin && z < h - margin && norm(rel - f.u * z) < r * (h - z) / (2.0 * h) - margin;
    default:
        return false;
    }
}

bool solveWithSurfaces(const std::vector<AntennaElement> &elements, const AntennaSettings &settings, const Prepared &p,
                       AntennaResult &out, std::vector<qsig::mom::Moment> &moments, std::vector<qsig::mom::Moment> &magneticMoments)
{
    namespace mom = qsig::mom;
    mom::Input in;
    in.frequency = out.frequency;
    in.segmentsPerWavelength = p.input.segmentsPerWavelength;
    in.maxUnknowns = 2000;
    for (const auto &w : p.input.wires) in.wires.push_back({w.aM, w.bM, w.radiusM, std::stoi(w.name)});
    // Dielectric solids: one body each, of permittivity eps (1 - j tan delta). Their skins carry an
    // electric and a magnetic current: twice the unknowns, so a higher limit.
    std::vector<int> bodyOf(elements.size(), -1);
    for (std::size_t i = 0; i < elements.size(); ++i)
    {
        if (!elementIsDielectric(elements[i])) continue;
        const double epsilon = std::clamp(finiteOr(elements[i].permittivity, 4.0), 1.0, 1000.0);
        const double tangent = std::clamp(finiteOr(elements[i].lossTangent, 0.0), 0.0, 10.0);
        bodyOf[i] = static_cast<int>(in.dielectrics.size());
        in.dielectrics.push_back({cplx(epsilon, -epsilon * tangent)});
    }
    if (!in.dielectrics.empty()) in.maxUnknowns = 3000;
    // A conductor that touches a dielectric, or goes through it, is not calculated yet: the wires first.
    for (const auto &w : in.wires)
        for (std::size_t i = 0; i < elements.size(); ++i)
        {
            if (bodyOf[i] < 0) continue;
            for (const double t : {0.0, 0.25, 0.5, 0.75, 1.0})
                if (insideSolid(elements[i], w.a + (w.b - w.a) * t, 10.0 * in.tolerance))
                {
                    out.error = "Un fil traverse le diélectrique « " + elements[i].name + " » : ce n'est pas encore calculé.";
                    return false;
                }
        }
    for (const auto &f : p.input.feeds) in.feeds.push_back({f.positionM, f.voltageV, false});

    std::vector<Vec3> ends;
    for (const AntennaElement &e : elements)
    {
        if (elementIsSurface(e.type)) continue;
        for (const auto &line : pathOf(e).polylines)
        {
            if (line.empty()) continue;
            ends.push_back(line.front());
            ends.push_back(line.back());
        }
    }
    const double step = out.wavelength / std::clamp(finiteOr(settings.facetsPerWavelength, 10.0), 4.0, 40.0);
    // The surfaces, each with the points its mesh must go through: the ends of the wires lying on it.
    struct Sheet
    {
        std::size_t element = 0;
        SurfaceGeometry g;
        std::vector<Vec3> points;
        std::vector<mom::Triangle> mesh;
        int body = -1;     // dielectric body of the solid it is a face of
        double step = 0.0; // size of its triangles: finer on a dielectric, where the wave is shorter
    };
    // The same number of triangles per wavelength in a dielectric, where the wavelength is that of the medium.
    auto stepOf = [&](std::size_t element)
    {
        return bodyOf[element] < 0 ? step : step / std::sqrt(in.dielectrics[static_cast<std::size_t>(bodyOf[element])].relativePermittivity.real());
    };
    std::vector<Sheet> sheets;
    for (std::size_t i = 0; i < elements.size(); ++i)
    {
        if (!elementIsSurface(elements[i].type)) continue;
        // A plate, a disc, or each of the six faces of a box (joined along their sides as plates are).
        for (const SurfaceGeometry &g : flatFaces(elements[i]))
        {
            Sheet sheet;
            sheet.element = i;
            sheet.g = g;
            sheet.body = bodyOf[i];
            sheet.step = stepOf(i);
            // A wire lands on a conductor only.
            for (const Vec3 &end : ends)
                if (sheet.body < 0 && liesOn(sheet.g, end, in.tolerance)) sheet.points.push_back(end - sheet.g.normal * dot(end - sheet.g.centre, sheet.g.normal));
            sheets.push_back(sheet);
        }
    }
    auto touching = [&](std::size_t first, std::size_t second)
    {
        const bool both = bodyOf[first] >= 0 && bodyOf[second] >= 0;
        return (both ? std::string("Deux diélectriques se touchent (« ") : std::string("Un conducteur touche un diélectrique (« ")) + elements[first].name +
               " » et « " + elements[second].name + " ») : ce n'est pas encore calculé.";
    };
    // A conducting plate or disc that lies on a dielectric solid, or cuts through it: its centre, its
    // corners or the middles of its sides are on the skin of the solid, or inside.
    for (const Sheet &sheet : sheets)
    {
        if (sheet.body >= 0) continue;
        const SurfaceGeometry &g = sheet.g;
        std::vector<Vec3> probes{g.centre};
        for (int q = 0; q < 8; ++q)
        {
            const double angle = 0.25 * kPi * q;
            // On the rim of a disc; on the outline of a plate (corners and middles of the sides).
            const double a = std::cos(angle), b = std::sin(angle), reach = g.disc ? 1.0 : 1.0 / std::max(std::abs(a), std::abs(b));
            probes.push_back(g.centre + g.u * (a * reach * (g.disc ? g.radius : g.halfU)) + g.v * (b * reach * (g.disc ? g.radius : g.halfV)));
        }
        for (std::size_t i = 0; i < elements.size(); ++i)
        {
            if (bodyOf[i] < 0 || i == sheet.element) continue;
            const auto faces = flatFaces(elements[i]);
            for (const Vec3 &probe : probes)
            {
                const bool onSkin = onCurvedSkin(elements[i], probe, in.tolerance) ||
                                    std::any_of(faces.begin(), faces.end(), [&](const SurfaceGeometry &face) { return liesOn(face, probe, in.tolerance); });
                if (!onSkin && !insideSolid(elements[i], probe, 10.0 * in.tolerance)) continue;
                out.error = touching(sheet.element, i);
                return false;
            }
        }
    }
    auto meshOf = [&](const Sheet &sheet)
    {
        const SurfaceGeometry &g = sheet.g;
        const int tag = static_cast<int>(sheet.element);
        // A disc has two rings at least: its centre alone could not carry a current towards the rim.
        return g.disc ? mom::meshDisc(g.centre, g.u, g.v, g.radius, std::min(sheet.step, 0.5 * g.radius), sheet.points, tag)
                      : mom::meshRectangle(g.centre, g.u, g.v, 2.0 * g.halfU, 2.0 * g.halfV, sheet.step, sheet.points, tag);
    };
    // Plates that touch along a line parallel to a side of each of them are joined (corner, T, cross,
    // box): their meshes must share the vertices of that line, the solver then lets the current pass.
    // First the two ends of each contact line...
    for (std::size_t a = 0; a < sheets.size(); ++a)
        for (std::size_t b = a + 1; b < sheets.size(); ++b)
        {
            Vec3 from, to;
            if (!contactLine(sheets[a].g, sheets[b].g, in.tolerance, from, to)) continue;
            if (sheets[a].body != sheets[b].body)
            {
                out.error = touching(sheets[a].element, sheets[b].element);
                return false;
            }
            for (Sheet *sheet : {&sheets[a], &sheets[b]})
                for (const Vec3 &end : {from, to})
                {
                    auto same = [&](const Vec3 &q) { return norm(q - end) <= 10.0 * in.tolerance; };
                    if (std::none_of(sheet->points.begin(), sheet->points.end(), same)) sheet->points.push_back(end);
                }
        }
    // ... then the grid lines. The mesh of a plate is a grid: a point it must go through (the landing of
    // a wire, the end of a contact line) draws a line across the whole plate in each direction. Where
    // such a line meets another plate (at its end on a side they share, or where it crosses the line on
    // which the other plate stands), that plate must go through the meeting point too, and so on around
    // a box. Only these points are exchanged: between them every plate cuts its lines evenly, in the same
    // way on both sides of a contact, so the meshes meet vertex for vertex without each being cut as
    // fine as all its neighbours together.
    for (int pass = 0; pass < 32; ++pass)
    {
        bool changed = false;
        for (std::size_t a = 0; a < sheets.size(); ++a)
        {
            const SurfaceGeometry &g = sheets[a].g;
            if (g.disc) continue;
            // The two grid lines of each point, as segments across the plate. Its four sides are grid
            // lines too (those of two opposite corners): plates side by side in one plane, which have no
            // contact line, meet by the corners that one has on the side of the other.
            std::vector<Vec3> through{g.centre - g.u * g.halfU - g.v * g.halfV, g.centre + g.u * g.halfU + g.v * g.halfV};
            through.insert(through.end(), sheets[a].points.begin(), sheets[a].points.end());
            std::vector<std::pair<Vec3, Vec3>> gridLines;
            for (const Vec3 &q : through)
            {
                const double s = dot(q - g.centre, g.u), t = dot(q - g.centre, g.v);
                gridLines.push_back({g.centre + g.u * s - g.v * g.halfV, g.centre + g.u * s + g.v * g.halfV});
                gridLines.push_back({g.centre - g.u * g.halfU + g.v * t, g.centre + g.u * g.halfU + g.v * t});
            }
            for (std::size_t b = 0; b < sheets.size(); ++b)
            {
                const SurfaceGeometry &other = sheets[b].g;
                if (b == a || other.disc || sheets[b].body != sheets[a].body) continue;
                auto offer = [&](const Vec3 &meeting)
                {
                    if (!liesOn(other, meeting, in.tolerance)) return;
                    const Vec3 onPlate = meeting - other.normal * dot(meeting - other.centre, other.normal);
                    auto same = [&](const Vec3 &q) { return norm(q - onPlate) <= 10.0 * in.tolerance; };
                    if (std::any_of(sheets[b].points.begin(), sheets[b].points.end(), same)) return;
                    sheets[b].points.push_back(onPlate);
                    changed = true;
                };
                for (const auto &[from, to] : gridLines)
                {
                    const double d0 = dot(from - other.centre, other.normal), d1 = dot(to - other.centre, other.normal);
                    if (std::abs(d0) <= in.tolerance && std::abs(d1) <= in.tolerance)
                    {
                        // In the plane of the other plate (a plate in pieces, a box standing on a plate):
                        // the ends of the line, and those of its part inside the other plate when it runs
                        // along one of its sides, so that where the two overlap they are cut alike.
                        offer(from);
                        offer(to);
                        const Vec3 axes[2] = {other.u, other.v};
                        const double halves[2] = {other.halfU, other.halfV};
                        for (int k = 0; k < 2; ++k)
                        {
                            const double a0 = dot(from - other.centre, axes[k]), a1 = dot(to - other.centre, axes[k]);
                            const double b0 = dot(from - other.centre, axes[1 - k]), b1 = dot(to - other.centre, axes[1 - k]);
                            if (std::abs(b1 - b0) > in.tolerance || std::abs(b0) > halves[1 - k] + in.tolerance) continue;
                            const double lo = std::max(std::min(a0, a1), -halves[k]), hi = std::min(std::max(a0, a1), halves[k]);
                            if (!(hi - lo > 100.0 * in.tolerance)) continue;
                            const double across = std::clamp(b0, -halves[1 - k], halves[1 - k]);
                            offer(other.centre + axes[k] * lo + axes[1 - k] * across);
                            offer(other.centre + axes[k] * hi + axes[1 - k] * across);
                        }
                    }
                    else if ((d0 <= in.tolerance && d1 >= -in.tolerance) || (d0 >= -in.tolerance && d1 <= in.tolerance))
                        offer(from + (to - from) * (d0 / (d0 - d1)));
                }
            }
        }
        if (!changed) break;
    }
    for (Sheet &sheet : sheets) sheet.mesh = meshOf(sheet);
    // Where two plates lie on each other in one plane (a box standing on a ground plate, two plates that
    // overlap), the conductor is there once: the later one loses the cells that the earlier one covers.
    // Cut alike, they stay joined along the edge of what is left.
    for (std::size_t b = 1; b < sheets.size(); ++b)
        for (std::size_t a = 0; a < b; ++a)
        {
            const SurfaceGeometry &under = sheets[a].g, &over = sheets[b].g;
            if (under.disc || over.disc) continue;
            if (std::abs(std::abs(dot(under.normal, over.normal)) - 1.0) > 1e-9 || std::abs(dot(over.centre - under.centre, under.normal)) > in.tolerance) continue;
            auto covered = [&](const mom::Triangle &t) { return liesOn(under, (t.a + t.b + t.c) * (1.0 / 3.0), in.tolerance); };
            if (sheets[a].body != sheets[b].body)
            {
                // A plate lying on a face of a dielectric, two dielectrics face to face.
                if (std::none_of(sheets[b].mesh.begin(), sheets[b].mesh.end(), covered)) continue;
                out.error = touching(sheets[a].element, sheets[b].element);
                return false;
            }
            sheets[b].mesh.erase(std::remove_if(sheets[b].mesh.begin(), sheets[b].mesh.end(), covered), sheets[b].mesh.end());
        }
    for (Sheet &sheet : sheets)
    {
        for (mom::Triangle &t : sheet.mesh) t.body = sheet.body;
        in.triangles.insert(in.triangles.end(), sheet.mesh.begin(), sheet.mesh.end());
    }
    // The curved solids: closed skins of revolution about their axis, the nearest vertex moved under
    // each wire that lands on them. They are not joined to the plates or the solids they touch.
    for (std::size_t i = 0; i < elements.size(); ++i)
    {
        const AntennaElement &e = elements[i];
        if (e.type != Type::Cylinder && e.type != Type::Cone && e.type != Type::Sphere && e.type != Type::Paraboloid &&
            e.type != Type::ParabolicCylinder)
            continue;
        // A wire lands on a conductor only.
        std::vector<Vec3> landings;
        for (const Vec3 &end : ends)
            if (bodyOf[i] < 0 && onCurvedSkin(e, end, in.tolerance)) landings.push_back(end);
        const Frame f = frameOf(e);
        const double fine = stepOf(i);
        std::vector<mom::Triangle> skin;
        if (e.type == Type::ParabolicCylinder)
        {
            // The parabola across, swept along the focal line.
            const double half = 0.5 * std::max(1e-6, finiteOr(e.width, 1.0)), focal = std::max(1e-6, finiteOr(e.focalLength, 0.5));
            const auto halfProfile = parabolaProfile(half, focal, fine);
            std::vector<Vec3> profile;
            for (auto it = halfProfile.rbegin(); it != halfProfile.rend(); ++it) profile.push_back(e.position + f.u * it->second - f.e2 * it->first);
            for (std::size_t q = 1; q < halfProfile.size(); ++q) profile.push_back(e.position + f.u * halfProfile[q].second + f.e2 * halfProfile[q].first);
            skin = mom::meshSweep(profile, f.e3, std::max(1e-6, finiteOr(e.length, 0.5)), fine, landings, static_cast<int>(i));
        }
        else
            skin = mom::meshRevolution(e.position, f.e2, f.e3, f.u, revolutionProfile(e, fine), fine, landings, static_cast<int>(i));
        for (mom::Triangle &t : skin) t.body = bodyOf[i];
        in.triangles.insert(in.triangles.end(), skin.begin(), skin.end());
    }

    const mom::Result r = mom::solve(in);
    if (!r.valid)
    {
        out.error = r.error;
        return false;
    }
    out.valid = true;
    out.unknowns = r.unknowns;
    for (const mom::Segment &s : r.segments)
    {
        out.segments.push_back({s.p0, s.p1, s.i0, s.i1, s.tag});
        out.maxCurrent = std::max({out.maxCurrent, std::abs(s.i0), std::abs(s.i1)});
    }
    auto modulus = [](const mom::ComplexVec3 &j) { return std::sqrt(std::norm(j.x) + std::norm(j.y) + std::norm(j.z)); };
    for (const mom::Facet &f : r.facets)
    {
        const AntennaFacet facet{f.a, f.b, f.c, modulus(f.ja), modulus(f.jb), modulus(f.jc), f.tag};
        out.maxSurfaceCurrent = std::max({out.maxSurfaceCurrent, facet.ja, facet.jb, facet.jc});
        out.facets.push_back(facet);
    }
    const double z0 = std::max(1e-3, finiteOr(settings.referenceOhm, 50.0));
    for (std::size_t f = 0; f < r.feeds.size(); ++f)
    {
        const mom::FeedResult &src = r.feeds[f];
        AntennaFeedResult feed;
        feed.name = f < p.input.feeds.size() ? p.input.feeds[f].name : std::string();
        feed.element = f < p.feedElement.size() ? p.feedElement[f] : -1;
        feed.feedIndex = f < p.feedSource.size() ? p.feedSource[f] : 0;
        feed.voltage = src.voltage;
        feed.current = src.current;
        feed.impedance = src.impedance;
        const cplx sum = src.impedance + z0;
        if (std::abs(sum) > 1e-18) feed.reflection = (src.impedance - z0) / sum;
        const double gamma = std::abs(feed.reflection);
        feed.returnLossDb = gamma > 1e-15 ? -20.0 * std::log10(gamma) : 300.0;
        feed.vswr = gamma < 1.0 ? (1.0 + gamma) / std::max(1.0 - gamma, 1e-15) : std::numeric_limits<double>::infinity();
        feed.powerW = 0.5 * std::real(src.voltage * std::conj(src.current));
        out.acceptedPowerW += feed.powerW;
        out.feeds.push_back(feed);
    }
    moments = r.moments;
    magneticMoments = r.magneticMoments;
    return true;
}
} // namespace

AntennaResult solveAntenna(const std::vector<AntennaElement> &elements, const AntennaSettings &settings,
                           const AntennaSolveOptions &options)
{
    const auto begin = std::chrono::steady_clock::now();
    AntennaResult out;
    const double frequency = std::max(1.0, finiteOr(settings.frequency, 300e6));
    out.frequency = frequency;
    out.wavelength = qsig::constants::c0 / frequency;
    const Prepared p = prepare(elements, settings, frequency);
    out.segmentsPerWavelength = p.segmentsPerWavelength;
    if (!p.error.empty())
    {
        out.error = p.error;
        return out;
    }
    std::vector<qsig::mom::Moment> surfaceMoments, magneticMoments;
    if (std::any_of(elements.begin(), elements.end(), [](const AntennaElement &e) { return elementIsSurface(e.type); }))
    {
        if (!solveWithSurfaces(elements, settings, p, out, surfaceMoments, magneticMoments)) return out;
    }
    else
    {
        const auto wire = NumericalEM::solveWireNetworkMom(p.input);
        if (!wire.valid)
        {
            out.error = wire.error;
            return out;
        }
        out.valid = true;
        out.note = wire.note;
        out.unknowns = wire.solvedWireDofCount;
        for (const auto &s : wire.meshSegments)
        {
            AntennaSegment seg;
            seg.p0 = s.p0M;
            seg.p1 = s.p1M;
            seg.i0 = s.currentAtP0A;
            seg.i1 = s.currentAtP1A;
            seg.element = elementOfSegment(p.input, s);
            out.maxCurrent = std::max({out.maxCurrent, std::abs(seg.i0), std::abs(seg.i1)});
            out.segments.push_back(seg);
        }
        for (std::size_t f = 0; f < wire.feeds.size(); ++f)
        {
            const auto &src = wire.feeds[f];
            AntennaFeedResult r;
            r.name = src.name;
            r.element = f < p.feedElement.size() ? p.feedElement[f] : -1;
            r.feedIndex = f < p.feedSource.size() ? p.feedSource[f] : 0;
            r.voltage = src.voltageV;
            r.current = src.currentA;
            r.impedance = src.activeImpedanceOhm;
            r.reflection = src.reflectionCoefficient;
            r.returnLossDb = src.returnLossDb;
            r.vswr = src.vswr;
            r.powerW = 0.5 * std::real(src.voltageV * std::conj(src.currentA));
            out.acceptedPowerW += r.powerW;
            out.feeds.push_back(r);
        }
    }

    if (options.pattern)
    {
        const double k = 2.0 * kPi / out.wavelength;
        qsig::mom::Radiator radiator(k);
        for (const AntennaSegment &s : out.segments) radiator.addSegment(s.p0, s.p1, s.i0, s.i1);
        for (const qsig::mom::Moment &m : surfaceMoments) radiator.addMoment(m.position, m.moment);
        for (const qsig::mom::Moment &m : magneticMoments) radiator.addMagneticMoment(m.position, m.moment);
        // Grid on cell centres: the midpoint rule integrates the power over the sphere.
        const double step = std::clamp(options.gridStepDeg, 0.5, 15.0);
        const int nt = std::max(6, static_cast<int>(std::lround(180.0 / step)));
        const int np = 2 * nt;
        const double dt = 180.0 / nt, dp = 360.0 / np;
        std::vector<double> intensity(static_cast<std::size_t>(nt * np));
        // The grid rows are independent: on several threads when the grid is large (the 3-degree
        // pattern of an array), alone otherwise (sweeps and the optimiser already fill the cores).
        auto rows = [&](int first, int last)
        {
            for (int t = first; t < last; ++t)
                for (int q = 0; q < np; ++q)
                    intensity[static_cast<std::size_t>(t * np + q)] =
                        radiator.intensity(direction((t + 0.5) * dt, (q + 0.5) * dp));
        };
        const double work = double(nt) * double(np) * double(radiator.points());
        const int threads =
            work > 2e6 ? std::clamp(static_cast<int>(std::thread::hardware_concurrency()), 1, std::min(8, nt)) : 1;
        if (threads == 1)
            rows(0, nt);
        else
        {
            std::vector<std::thread> pool;
            for (int i = 0; i < threads; ++i) pool.emplace_back(rows, nt * i / threads, nt * (i + 1) / threads);
            for (auto &th : pool) th.join();
        }
        // Integral and maximum in a fixed order: the result does not depend on the threads.
        double integral = 0.0, peak = 0.0;
        for (int t = 0; t < nt; ++t)
        {
            const double theta = (t + 0.5) * dt;
            out.thetaDeg.push_back(theta);
            for (int q = 0; q < np; ++q)
            {
                const double phi = (q + 0.5) * dp;
                if (t == 0) out.phiDeg.push_back(phi);
                const double v = intensity[static_cast<std::size_t>(t * np + q)];
                integral += v * std::sin(theta * kDeg) * (dt * kDeg) * (dp * kDeg);
                if (v > peak)
                {
                    peak = v;
                    out.mainThetaDeg = theta;
                    out.mainPhiDeg = phi;
                }
            }
        }
        if (integral > 0.0)
        {
            out.hasPattern = true;
            // Refine the direction of the maximum around the best grid cell.
            for (double span = dt; span > 0.05; span *= 0.5)
            {
                double bestT = out.mainThetaDeg, bestP = out.mainPhiDeg;
                for (int a = -1; a <= 1; ++a)
                    for (int b = -1; b <= 1; ++b)
                    {
                        const double theta = std::clamp(out.mainThetaDeg + a * span, 0.0, 180.0);
                        const double phi = out.mainPhiDeg + b * span;
                        const double v = radiator.intensity(direction(theta, phi));
                        if (v > peak)
                        {
                            peak = v;
                            bestT = theta;
                            bestP = phi;
                        }
                    }
                out.mainThetaDeg = bestT;
                out.mainPhiDeg = std::fmod(bestP + 360.0, 360.0);
            }
            const double scale = 4.0 * kPi / integral; // D = 4 pi U / P
            out.directivityDbi = toDb(scale * peak);
            out.radiatedPowerW = radiator.power(integral);
            out.gainDbi.reserve(intensity.size());
            for (double v : intensity) out.gainDbi.push_back(toDb(scale * v));
            const Vec3 front = direction(out.mainThetaDeg, out.mainPhiDeg);
            out.frontToBackDb = toDb(peak) - toDb(radiator.intensity(-1.0 * front));
            {
                // Circular components of the field of the main lobe: right-hand is (theta^ - j phi^) / sqrt 2
                // for a wave that leaves along r^ = theta^ x phi^ (IEEE convention, exp(j w t)).
                const double t = out.mainThetaDeg * kDeg, q = out.mainPhiDeg * kDeg;
                const Vec3 thetaHat{std::cos(t) * std::cos(q), std::cos(t) * std::sin(q), -std::sin(t)};
                const Vec3 phiHat{-std::sin(q), std::cos(q), 0.0};
                const qsig::mom::ComplexVec3 f = radiator.field(front);
                const cplx eTheta = thetaHat.x * f.x + thetaHat.y * f.y + thetaHat.z * f.z;
                const cplx ePhi = phiHat.x * f.x + phiHat.y * f.y + phiHat.z * f.z;
                const double right = std::abs(eTheta + cplx(0.0, 1.0) * ePhi), left = std::abs(eTheta - cplx(0.0, 1.0) * ePhi);
                const double minor = std::abs(right - left), major = right + left;
                out.axialRatioDb = minor > 1e-5 * major ? 20.0 * std::log10(major / minor) : 100.0;
                out.polarisationSense = out.axialRatioDb >= 100.0 ? 0 : right > left ? 1 : -1;
            }

            const double cut = std::clamp(options.cutStepDeg, 0.25, 10.0);
            const int nc = static_cast<int>(std::lround(360.0 / cut));
            for (int i = 0; i <= nc; ++i)
            {
                const double angle = 360.0 * i / nc;
                out.horizontal.angleDeg.push_back(angle);
                out.horizontal.gainDbi.push_back(toDb(scale * radiator.intensity(direction(90.0, angle))));
                // Vertical planes: theta over the full circle (sin theta < 0 is the opposite half-plane).
                out.vertical.angleDeg.push_back(angle);
                out.vertical.gainDbi.push_back(toDb(scale * radiator.intensity(direction(angle, out.mainPhiDeg))));
                out.vertical90.angleDeg.push_back(angle);
                out.vertical90.gainDbi.push_back(
                    toDb(scale * radiator.intensity(direction(angle, out.mainPhiDeg + 90.0))));
            }
        }
    }
    out.milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
    return out;
}

SweepPoint solveSweepPoint(const std::vector<AntennaElement> &elements, const AntennaSettings &settings,
                           double frequency)
{
    AntennaSettings at = settings;
    at.frequency = frequency;
    AntennaSolveOptions options;
    options.gridStepDeg = 6.0;
    options.cutStepDeg = 10.0;
    const AntennaResult r = solveAntenna(elements, at, options);
    SweepPoint point;
    point.frequency = frequency;
    if (!r.valid || r.feeds.empty()) return point;
    point.valid = true;
    point.impedance = r.feeds.front().impedance;
    point.reflection = r.feeds.front().reflection;
    point.returnLossDb = r.feeds.front().returnLossDb;
    point.vswr = r.feeds.front().vswr;
    point.gainDbi = r.directivityDbi;
    point.frontToBackDb = r.frontToBackDb;
    return point;
}

std::vector<double> sweepFrequencies(const AntennaSettings &settings)
{
    const int n = std::clamp(settings.sweepPoints, 2, 401);
    const double a = std::max(1.0, finiteOr(settings.sweepStart, 200e6));
    const double b = std::max(a * 1.000001, finiteOr(settings.sweepStop, 400e6));
    std::vector<double> f;
    for (int i = 0; i < n; ++i) f.push_back(a + (b - a) * i / (n - 1));
    return f;
}

std::vector<std::vector<std::complex<double>>> admittanceMatrix(const std::vector<AntennaElement> &elements,
                                                                const AntennaSettings &settings)
{
    std::vector<std::size_t> fed;
    for (std::size_t i = 0; i < elements.size(); ++i)
        if (elements[i].fed != 0) fed.push_back(i);
    std::vector<std::vector<cplx>> y(fed.size(), std::vector<cplx>(fed.size()));
    AntennaSolveOptions options;
    options.pattern = false;
    for (std::size_t j = 0; j < fed.size(); ++j)
    {
        std::vector<AntennaElement> driven = elements;
        for (std::size_t i = 0; i < fed.size(); ++i)
        {
            driven[fed[i]].strength = i == j ? 1.0 : 0.0;
            driven[fed[i]].phaseDeg = 0.0;
        }
        const AntennaResult r = solveAntenna(driven, settings, options);
        if (!r.valid || r.feeds.size() != fed.size()) return {};
        for (std::size_t i = 0; i < fed.size(); ++i) y[i][j] = r.feeds[i].current;
    }
    return y;
}

// ---- Presets ----

const std::vector<AntennaPresetInfo> &antennaPresets()
{
    static const std::vector<AntennaPresetInfo> presets = {
        {"dipole", "Dipôle λ/2"},
        {"v_dipole", "Dipôle en V (120°)"},
        {"folded_dipole", "Dipôle replié"},
        {"quad_loop", "Cadre carré à la résonance (≈ λ)"},
        {"circular_loop", "Boucle circulaire à la résonance (≈ λ)"},
        {"small_loop", "Petite boucle (dipôle magnétique)"},
        {"helix", "Hélice en mode normal (antenne ressort)"},
        {"yagi3", "Yagi 3 éléments"},
        {"yagi6", "Yagi 6 éléments"},
        {"endfire_pair", "Deux dipôles en quadrature (cardioïde)"},
        {"broadside4", "Réseau de 4 dipôles en phase"},
        {"monopole_plate", "Monopôle λ/4 sur plan de masse"},
        {"dipole_reflector", "Dipôle devant un réflecteur plan"},
        {"corner_reflector", "Dipôle dans un réflecteur en coin (90°)"},
        {"patch_air", "Antenne patch dans l'air (sonde)"},
        {"monopole_box", "Monopôle sur un boîtier (pavé conducteur)"},
        {"monopole_sphere", "Monopôle sur une sphère conductrice"},
        {"dipole_dielectric", "Dipôle près d'une sphère diélectrique"},
        {"parabola_dipole", "Parabole avec un dipôle au foyer"},
        {"parabolic_cylinder", "Cylindre parabolique avec un dipôle sur la ligne focale"},
        {"spiral", "Spirale d'Archimède à deux bras"},
        {"helix_axial", "Hélice en mode axial sur plan de masse"},
    };
    return presets;
}

std::vector<AntennaElement> antennaPreset(const std::string &key, double frequency)
{
    const double lambda = qsig::constants::c0 / std::max(1.0, frequency);
    auto dipole = [&](const char *name, double x, double y, double lengthLambda)
    {
        AntennaElement e;
        e.name = name;
        e.type = Type::Dipole;
        e.position = {x * lambda, y * lambda, 0.0};
        e.length = lengthLambda * lambda;
        e.wireRadius = 1e-3 * lambda;
        return e;
    };
    auto parasite = [&](const char *name, double x, double lengthLambda)
    {
        AntennaElement e = dipole(name, x, 0.0, lengthLambda);
        e.type = Type::Wire;
        e.fed = 0;
        return e;
    };
    if (key == "dipole") return {dipole("Dipôle", 0.0, 0.0, 0.475)};
    if (key == "v_dipole")
    {
        AntennaElement v = dipole("Dipôle en V", 0.0, 0.0, 0.5);
        v.apexDeg = 120.0;
        return {v};
    }
    if (key == "folded_dipole")
    {
        AntennaElement f = dipole("Dipôle replié", 0.0, 0.0, 0.47);
        f.type = Type::FoldedDipole;
        f.width = 0.02 * lambda;
        return {f};
    }
    if (key == "quad_loop")
    {
        AntennaElement q;
        q.name = "Cadre";
        q.type = Type::RectangularLoop;
        q.azimuthDeg = 0.0; // axis along x: the loop stands in the yz plane, fed at the bottom
        q.elevationDeg = 0.0;
        q.length = q.width = 0.275 * lambda; // perimeter 1.10 lambda: resonance of this wire (132 ohm)
        q.wireRadius = 1e-3 * lambda;
        return {q};
    }
    if (key == "circular_loop" || key == "small_loop")
    {
        AntennaElement l;
        l.name = key == "small_loop" ? "Petite boucle" : "Boucle";
        l.type = Type::CircularLoop;
        // Circumference 0.1 lambda, or 1.07 lambda: resonance of the full-wave loop (143 ohm).
        l.radius = (key == "small_loop" ? 0.1 : 1.07) * lambda / (2.0 * kPi);
        l.sides = key == "small_loop" ? 16 : 24;
        l.wireRadius = (key == "small_loop" ? 2e-4 : 1e-3) * lambda;
        return {l};
    }
    if (key == "helix")
    {
        // Normal mode: diameter and height small against the wavelength, fed at the middle. A coiled
        // dipole: broadside pattern (D = 1.77 dBi), first resonance at 4.28 turns, i.e. 0.085 lambda high
        // for 0.54 lambda of wire, R = 2.9 ohm. (Longer helices resonate again on modes with reversed
        // current lobes along the axis: not a normal-mode antenna any more.)
        AntennaElement h;
        h.name = "Hélice";
        h.type = Type::Helix;
        h.radius = 0.02 * lambda;
        h.pitch = 0.02 * lambda;
        h.turns = 4.28;
        h.sides = 16;
        h.wireRadius = 5e-4 * lambda;
        return {h};
    }
    if (key == "yagi3")
        return {parasite("Réflecteur", -0.2, 0.495), dipole("Radiateur", 0.0, 0.0, 0.47),
                parasite("Directeur 1", 0.2, 0.44)};
    if (key == "yagi6")
        return {parasite("Réflecteur", -0.2, 0.49), dipole("Radiateur", 0.0, 0.0, 0.47),
                parasite("Directeur 1", 0.15, 0.44),  parasite("Directeur 2", 0.40, 0.435),
                parasite("Directeur 3", 0.65, 0.43),  parasite("Directeur 4", 0.90, 0.425)};
    if (key == "endfire_pair")
    {
        // Spaced by lambda/4, the CURRENT of the second one lagging by 90 degrees: the waves add towards +x
        // and cancel towards -x. Coupling between the dipoles makes equal voltages give unequal currents,
        // so the voltages come from the impedance matrix: V = Z I with I = (1, -j).
        AntennaElement a = dipole("Dipôle A", 0.0, 0.0, 0.475), b = dipole("Dipôle B", 0.25, 0.0, 0.475);
        AntennaSettings settings;
        settings.frequency = frequency;
        const auto y = admittanceMatrix({a, b}, settings);
        if (y.size() == 2)
        {
            const cplx det = y[0][0] * y[1][1] - y[0][1] * y[1][0];
            if (std::abs(det) > 0.0)
            {
                const cplx z11 = y[1][1] / det, z12 = -y[0][1] / det, z21 = -y[1][0] / det, z22 = y[0][0] / det;
                const cplx i1{1.0, 0.0}, i2{0.0, -1.0};
                const cplx v1 = z11 * i1 + z12 * i2, v2 = z21 * i1 + z22 * i2;
                const double scale = 1.0 / std::abs(v1);
                a.strength = 1.0;
                a.phaseDeg = 0.0;
                b.strength = std::abs(v2) * scale;
                b.phaseDeg = std::arg(v2 / v1) / kDeg;
                return {a, b};
            }
        }
        b.phaseDeg = -90.0;
        return {a, b};
    }
    if (key == "monopole_plate")
    {
        // A wire standing on a square plate of one wavelength, fed at its foot between wire and plate.
        // 0.24 lambda: resonance on this plate (39.5 ohm). The finite plane lifts the main lobe to about
        // 45 degrees above the horizon; on an endless plane it would lie along the ground.
        AntennaElement plane;
        plane.name = "Plan de masse";
        plane.type = Type::Plate;
        plane.fed = 0;
        plane.width = plane.length = lambda;
        AntennaElement m;
        m.name = "Monopôle";
        m.type = Type::Wire;
        m.length = 0.24 * lambda;
        m.position = {0.0, 0.0, 0.5 * m.length};
        m.wireRadius = 1e-3 * lambda;
        m.feedFraction = 0.0;
        return {plane, m};
    }
    if (key == "dipole_reflector")
    {
        // A dipole at 0.2 lambda of a plate of one wavelength, the plate across x: the wave of the image
        // adds in front (8.5 dBi), the plate shadows the back.
        AntennaElement plate;
        plate.name = "Réflecteur";
        plate.type = Type::Plate;
        plate.fed = 0;
        plate.azimuthDeg = 0.0;
        plate.elevationDeg = 0.0;
        plate.width = plate.length = lambda;
        return {plate, dipole("Dipôle", 0.2, 0.0, 0.456)};
    }
    if (key == "corner_reflector")
    {
        // A dipole at 0.5 lambda of the apex of two plates at 90 degrees, joined along z. The three images
        // of a corner with infinite sides give 11.8 dBi; with sides and a height of one wavelength, 10.6 dBi
        // and an impedance within a few ohms of that of the images.
        auto panel = [&](const char *name, double azimuthDeg, double ySign)
        {
            AntennaElement e;
            e.name = name;
            e.type = Type::Plate;
            e.fed = 0;
            e.azimuthDeg = azimuthDeg;
            e.elevationDeg = 0.0;
            e.width = e.length = lambda;
            const double offset = 0.5 * lambda * std::sqrt(0.5);
            e.position = {offset, ySign * offset, 0.0};
            return e;
        };
        return {panel("Panneau gauche", 135.0, 1.0), panel("Panneau droit", 45.0, -1.0), dipole("Dipôle", 0.5, 0.0, 0.47)};
    }
    if (key == "patch_air")
    {
        // A plate of 0.46 x 0.5 lambda at 0.03 lambda above a ground plate of one wavelength, fed by a probe
        // between the two at 0.12 lambda of the centre. The half-wave resonance of the plate along x is just
        // below (R greatest near 0.445 lambda, with the +50 ohm of the probe); at 0.46 the reactance of the
        // probe is cancelled: about 50 ohm, 9.8 dBi straight above the patch.
        AntennaElement ground;
        ground.name = "Plan de masse";
        ground.type = Type::Plate;
        ground.fed = 0;
        ground.width = ground.length = lambda;
        AntennaElement top;
        top.name = "Patch";
        top.type = Type::Plate;
        top.fed = 0;
        top.width = 0.5 * lambda;
        top.length = 0.46 * lambda;
        top.position = {0.0, 0.0, 0.03 * lambda};
        AntennaElement probe;
        probe.name = "Sonde";
        probe.type = Type::Wire;
        probe.length = 0.03 * lambda;
        probe.position = {0.12 * lambda, 0.0, 0.015 * lambda};
        probe.wireRadius = 1e-3 * lambda;
        probe.feedFraction = 0.0;
        return {ground, top, probe};
    }
    if (key == "monopole_box" || key == "monopole_sphere")
    {
        // A quarter-wave monopole fed at its foot on a conducting solid: a box of 0.4 x 0.4 x 0.2 lambda
        // (a handset, a case), or a sphere of 0.2 lambda of radius. The solid is its whole ground: the
        // currents run down its sides and it radiates with the wire.
        const bool box = key == "monopole_box";
        AntennaElement body;
        body.name = box ? "Boîtier" : "Sphère";
        body.type = box ? Type::Box : Type::Sphere;
        body.fed = 0;
        body.width = body.length = 0.4 * lambda;
        body.height = 0.2 * lambda;
        body.radius = 0.2 * lambda;
        const double top = box ? 0.1 * lambda : 0.2 * lambda;
        AntennaElement monopole;
        monopole.name = "Monopôle";
        monopole.type = Type::Wire;
        monopole.length = 0.24 * lambda;
        monopole.position = {0.0, 0.0, top + 0.12 * lambda};
        monopole.wireRadius = 1e-3 * lambda;
        monopole.feedFraction = 0.0;
        return {body, monopole};
    }
    if (key == "parabola_dipole" || key == "parabolic_cylinder")
    {
        // A dish of 2 lambda across (f / D = 0.4), or a parabolic cylinder 2 lambda wide and 1 long
        // (f = 0.5 lambda), opening upwards; a half-wave dipole along x at the focus, or on the focal line.
        // The dish: about 12 dBi (physical optics: 12.0); the cylinder: about 9.4 dBi.
        const bool dish = key == "parabola_dipole";
        AntennaElement reflector;
        reflector.name = dish ? "Parabole" : "Cylindre parabolique";
        reflector.type = dish ? Type::Paraboloid : Type::ParabolicCylinder;
        reflector.fed = 0;
        reflector.radius = 1.0 * lambda;
        reflector.width = 2.0 * lambda;
        reflector.length = 1.0 * lambda;
        reflector.focalLength = (dish ? 0.8 : 0.5) * lambda;
        AntennaElement feed = dipole("Dipôle", 0.0, 0.0, 0.47);
        feed.azimuthDeg = 0.0;
        feed.elevationDeg = 0.0; // along x: across the dish; along the focal line of the cylinder
        feed.position = {0.0, 0.0, reflector.focalLength};
        return {reflector, feed};
    }
    if (key == "spiral")
    {
        // Two arms of 4 turns from 0.02 to 0.25 lambda, fed at the centre: circular polarisation along
        // the axis (axial ratio 0.2 dB here), of opposite senses on the two sides, about 3.9 dBi.
        AntennaElement spiral;
        spiral.name = "Spirale";
        spiral.type = Type::Spiral;
        spiral.radius = 0.25 * lambda;
        spiral.innerRadius = 0.02 * lambda;
        spiral.turns = 4.0;
        spiral.sides = 48;
        spiral.wireRadius = 5e-4 * lambda;
        return {spiral};
    }
    if (key == "dipole_dielectric")
    {
        // A half-wave dipole at 0.3 lambda of the centre of a sphere of permittivity 4 and 0.16 lambda of
        // radius. The field goes through the sphere, slower than around it: it bends the wave towards
        // itself, as a lens (about 4 dBi on its side, against 2.1 for the dipole alone).
        AntennaElement sphere;
        sphere.name = "Sphère diélectrique";
        sphere.type = Type::Sphere;
        sphere.fed = 0;
        sphere.radius = 0.16 * lambda;
        sphere.medium = 1;
        sphere.permittivity = 4.0;
        sphere.position = {0.3 * lambda, 0.0, 0.0};
        return {dipole("Dipôle", 0.0, 0.0, 0.47), sphere};
    }
    if (key == "helix_axial")
    {
        // Kraus helix: circumference of one wavelength, pitch angle 13 degrees, 5 turns, its foot fed on
        // a disc of 0.5 lambda of radius. Beam along the axis, circular polarisation, about 140 ohm.
        AntennaElement disc;
        disc.name = "Plan de masse";
        disc.type = Type::Disc;
        disc.fed = 0;
        disc.radius = 0.5 * lambda;
        AntennaElement h;
        h.name = "Hélice";
        h.type = Type::Helix;
        h.radius = lambda / (2.0 * kPi);
        h.pitch = lambda * std::tan(13.0 * kDeg);
        h.turns = 5.0;
        h.sides = 16;
        h.wireRadius = 2e-3 * lambda;
        h.feedFraction = 0.0;
        h.position = {0.0, 0.0, 0.5 * h.turns * h.pitch};
        return {disc, h};
    }
    if (key == "broadside4")
        return {dipole("Dipôle 1", 0.0, -0.75, 0.475), dipole("Dipôle 2", 0.0, -0.25, 0.475),
                dipole("Dipôle 3", 0.0, 0.25, 0.475), dipole("Dipôle 4", 0.0, 0.75, 0.475)};
    return {};
}
} // namespace qsig::app
