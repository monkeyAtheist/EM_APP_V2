#include "qsig/app/source_shapes.h"

#include "qsig/constants.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <utility>

namespace qsig::app
{
namespace
{
using qsig::Vec3;
using Path = SourceShape3D::Path;
constexpr double kPi = qsig::constants::pi;

// Closed circle of radius r around c in the plane (u, v), turning from u to v.
Path circle(const Vec3 &c, const Vec3 &u, const Vec3 &v, double r, int points = 64)
{
    Path path;
    path.reserve(static_cast<std::size_t>(points) + 1);
    for (int i = 0; i <= points; ++i)
    {
        const double a = 2.0 * kPi * i / points;
        path.push_back(c + u * (r * std::cos(a)) + v * (r * std::sin(a)));
    }
    return path;
}

Path closed(Path path)
{
    if (!path.empty()) path.push_back(path.front());
    return path;
}

// Arrows along a path at the given fractions of its length, pointing along it (sense > 0) or against.
void addArrows(SourceShape3D &shape, const Path &path, std::initializer_list<double> fractions, double sense)
{
    double total = 0.0;
    for (std::size_t i = 0; i + 1 < path.size(); ++i) total += norm(path[i + 1] - path[i]);
    if (!(total > 0.0)) return;
    for (const double fraction : fractions)
    {
        const double target = std::clamp(fraction, 0.0, 1.0) * total;
        double run = 0.0;
        for (std::size_t i = 0; i + 1 < path.size(); ++i)
        {
            const Vec3 d = path[i + 1] - path[i];
            const double length = norm(d);
            if (length > 0.0 && run + length >= target)
            {
                shape.arrows.push_back({path[i] + d * ((target - run) / length), d * (sense / length), false});
                break;
            }
            run += length;
        }
    }
}

void addThreeCircles(SourceShape3D &shape, const Vec3 &c, double r)
{
    const Vec3 ex{1.0, 0.0, 0.0}, ey{0.0, 1.0, 0.0}, ez{0.0, 0.0, 1.0};
    shape.lines.push_back(circle(c, ex, ey, r));
    shape.lines.push_back(circle(c, ex, ez, r));
    shape.lines.push_back(circle(c, ey, ez, r));
}

// Cylinder along z through (x, y), drawn from zLow to zHigh: three circles and eight generators.
void addCylinderZ(SourceShape3D &shape, double x, double y, double radius, double zLow, double zHigh)
{
    const Vec3 ex{1.0, 0.0, 0.0}, ey{0.0, 1.0, 0.0};
    for (const double z : {zLow, 0.5 * (zLow + zHigh), zHigh}) shape.lines.push_back(circle({x, y, z}, ex, ey, radius));
    for (int i = 0; i < 8; ++i)
    {
        const double a = 2.0 * kPi * i / 8.0;
        const double px = x + radius * std::cos(a), py = y + radius * std::sin(a);
        shape.lines.push_back({{px, py, zLow}, {px, py, zHigh}});
    }
}

int signOf(double strength)
{
    return strength > 0.0 ? 1 : strength < 0.0 ? -1 : 0;
}
} // namespace

SourceShape3D sourceShape(const Electrostatics::Source &s, const Vec3 &viewCentre, double extent)
{
    using Type = Electrostatics::Source::Type;
    SourceShape3D shape;
    shape.centre = s.position;
    shape.sign = signOf(s.strength);
    const double angle = qsig::constants::degToRad(s.angleDeg);
    const double ca = std::cos(angle), sa = std::sin(angle);
    // Local frame of lines, plates and volumes: turned by angleDeg about z.
    auto rotated = [&](double x, double y, double z)
    { return Vec3{s.position.x + x * ca - y * sa, s.position.y + x * sa + y * ca, s.position.z + z}; };
    const Vec3 ex{1.0, 0.0, 0.0}, ey{0.0, 1.0, 0.0};
    const double zLow = viewCentre.z - extent, zHigh = viewCentre.z + extent;
    switch (s.type)
    {
    case Type::PointCharge:
        shape.point = true;
        break;
    case Type::SolidSphere:
    case Type::SphericalShell:
    case Type::ThickSphericalShell:
        shape.sphereRadius = std::max(s.radius, 0.0);
        addThreeCircles(shape, s.position, shape.sphereRadius);
        if (s.type == Type::ThickSphericalShell && s.innerRadius > 0.0 && s.innerRadius < s.radius)
            addThreeCircles(shape, s.position, s.innerRadius);
        break;
    case Type::InfiniteLine:
        shape.infinite = true;
        shape.lines.push_back({{s.position.x, s.position.y, zLow}, {s.position.x, s.position.y, zHigh}});
        break;
    case Type::InfiniteCylinderZ:
    case Type::InfiniteHollowCylinderZ:
        shape.infinite = true;
        addCylinderZ(shape, s.position.x, s.position.y, std::max(s.radius, 0.0), zLow, zHigh);
        if (s.type == Type::InfiniteHollowCylinderZ && s.innerRadius > 0.0 && s.innerRadius < s.radius)
            for (const double z : {zLow, zHigh})
                shape.lines.push_back(circle({s.position.x, s.position.y, z}, ex, ey, s.innerRadius));
        break;
    case Type::FiniteLine:
        shape.lines.push_back({rotated(-0.5 * s.length, 0.0, 0.0), rotated(0.5 * s.length, 0.0, 0.0)});
        break;
    case Type::RectangularPlate:
    case Type::RectangularVolume:
    {
        const double a = 0.5 * s.width, b = 0.5 * s.height;
        const double h = s.type == Type::RectangularVolume ? 0.5 * std::max(s.thickness, 0.0) : 0.0;
        for (const double z : {-h, h})
        {
            const Path face{rotated(-a, -b, z), rotated(a, -b, z), rotated(a, b, z), rotated(-a, b, z)};
            shape.faces.push_back(face);
            shape.lines.push_back(closed(face));
            if (h == 0.0) break;
        }
        if (h > 0.0)
            for (const auto &[x, y] : {std::pair{-a, -b}, std::pair{a, -b}, std::pair{a, b}, std::pair{-a, b}})
                shape.lines.push_back({rotated(x, y, -h), rotated(x, y, h)});
        break;
    }
    case Type::TriangularPlate:
    {
        const Path face{rotated(-0.5 * s.width, -s.height / 3.0, 0.0), rotated(0.5 * s.width, -s.height / 3.0, 0.0),
                        rotated(0.0, 2.0 * s.height / 3.0, 0.0)};
        shape.faces.push_back(face);
        shape.lines.push_back(closed(face));
        break;
    }
    case Type::CircularPlate:
    case Type::AnnularPlate:
    {
        Path rim = circle(s.position, ex, ey, std::max(s.radius, 0.0));
        shape.lines.push_back(rim);
        rim.pop_back();
        shape.faces.push_back(std::move(rim));
        if (s.type == Type::AnnularPlate && s.innerRadius > 0.0)
            shape.lines.push_back(circle(s.position, ex, ey, s.innerRadius));
        break;
    }
    case Type::InfinitePlane:
    {
        shape.infinite = true;
        const double z = s.position.z;
        const Path face{{viewCentre.x - extent, viewCentre.y - extent, z},
                        {viewCentre.x + extent, viewCentre.y - extent, z},
                        {viewCentre.x + extent, viewCentre.y + extent, z},
                        {viewCentre.x - extent, viewCentre.y + extent, z}};
        shape.faces.push_back(face);
        shape.lines.push_back(closed(face));
        break;
    }
    }
    return shape;
}

SourceShape3D sourceShape(const Magnetostatics::Source &s, const Vec3 &viewCentre, double extent)
{
    using Type = Magnetostatics::Source::Type;
    SourceShape3D shape;
    shape.centre = s.position;
    shape.sign = signOf(s.strength);
    const double sense = s.strength >= 0.0 ? 1.0 : -1.0;
    const Vec3 axis = Magnetostatics::axisUnit(s);
    Vec3 u, v;
    Magnetostatics::perpendicularBasis(axis, u, v);
    const double zLow = viewCentre.z - extent, zHigh = viewCentre.z + extent;
    switch (s.type)
    {
    case Type::InfiniteWireZ:
    {
        shape.infinite = true;
        const Path wire{{s.position.x, s.position.y, zLow}, {s.position.x, s.position.y, zHigh}};
        shape.lines.push_back(wire);
        addArrows(shape, wire, {0.3, 0.7}, sense);
        break;
    }
    case Type::SolidCurrentCylinderZ:
    case Type::HollowCurrentCylinderZ:
    {
        shape.infinite = true;
        addCylinderZ(shape, s.position.x, s.position.y, std::max(s.radius, 0.0), zLow, zHigh);
        if (s.type == Type::HollowCurrentCylinderZ && s.innerRadius > 0.0 && s.innerRadius < s.radius)
            for (const double z : {zLow, zHigh})
                shape.lines.push_back(circle({s.position.x, s.position.y, z}, {1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, s.innerRadius));
        // The current runs along the generators.
        for (std::size_t i = 3; i < shape.lines.size() && i < 11; i += 2) addArrows(shape, shape.lines[i], {0.5}, sense);
        break;
    }
    case Type::FiniteWire:
    {
        const double half = 0.5 * std::max(s.length, 1e-9);
        const Path wire{s.position - axis * half, s.position + axis * half};
        shape.lines.push_back(wire);
        addArrows(shape, wire, {0.5}, sense);
        break;
    }
    case Type::CircularLoop:
    {
        const Path loop = circle(s.position, u, v, std::max(s.radius, 0.0));
        shape.lines.push_back(loop);
        addArrows(shape, loop, {0.125, 0.375, 0.625, 0.875}, sense);
        break;
    }
    case Type::HelmholtzPair:
    {
        const double half = 0.5 * (s.length > 1e-9 ? s.length : s.radius);
        for (const double side : {-1.0, 1.0})
        {
            const Path loop = circle(s.position + axis * (side * half), u, v, std::max(s.radius, 0.0));
            shape.lines.push_back(loop);
            addArrows(shape, loop, {0.125, 0.625}, sense);
        }
        break;
    }
    case Type::Solenoid:
    {
        // A helix of at most 30 drawn turns between the two end circles.
        const double length = std::max(s.length, 1e-9), radius = std::max(s.radius, 0.0);
        const int turns = std::clamp(s.turns, 1, 30);
        constexpr int perTurn = 28;
        Path helix;
        helix.reserve(static_cast<std::size_t>(turns * perTurn) + 1);
        for (int i = 0; i <= turns * perTurn; ++i)
        {
            const double t = static_cast<double>(i) / (turns * perTurn);
            const double a = 2.0 * kPi * turns * t;
            helix.push_back(s.position + axis * ((t - 0.5) * length) + u * (radius * std::cos(a)) + v * (radius * std::sin(a)));
        }
        addArrows(shape, helix, {0.5 / turns, 1.0 - 0.5 / turns}, sense);
        shape.lines.push_back(std::move(helix));
        for (const double side : {-0.5, 0.5}) shape.lines.push_back(circle(s.position + axis * (side * length), u, v, radius));
        break;
    }
    case Type::RectangularLoop:
    case Type::TriangularLoop:
    {
        const double w = std::max(s.width, 1e-9), h = std::max(s.height, 1e-9);
        const Vec3 &c = s.position;
        Path loop;
        if (s.type == Type::RectangularLoop)
        {
            loop = {c - u * (0.5 * w) - v * (0.5 * h), c + u * (0.5 * w) - v * (0.5 * h), c + u * (0.5 * w) + v * (0.5 * h),
                    c - u * (0.5 * w) + v * (0.5 * h), c - u * (0.5 * w) - v * (0.5 * h)};
            addArrows(shape, loop, {0.125, 0.375, 0.625, 0.875}, sense);
        }
        else
        {
            loop = {c - u * (0.5 * w) - v * (h / 3.0), c + u * (0.5 * w) - v * (h / 3.0), c + v * (2.0 * h / 3.0),
                    c - u * (0.5 * w) - v * (h / 3.0)};
            addArrows(shape, loop, {1.0 / 6.0, 0.5, 5.0 / 6.0}, sense);
        }
        shape.lines.push_back(std::move(loop));
        break;
    }
    case Type::MagneticDipole:
        shape.point = true;
        shape.arrows.push_back({s.position, axis * sense, true});
        break;
    case Type::InfiniteCurrentSheet:
    {
        shape.infinite = true;
        const double z = s.position.z;
        const Path face{{viewCentre.x - extent, viewCentre.y - extent, z},
                        {viewCentre.x + extent, viewCentre.y - extent, z},
                        {viewCentre.x + extent, viewCentre.y + extent, z},
                        {viewCentre.x - extent, viewCentre.y + extent, z}};
        shape.faces.push_back(face);
        shape.lines.push_back(closed(face));
        const double az = qsig::constants::degToRad(s.azimuthDeg);
        const Vec3 k{std::cos(az) * sense, std::sin(az) * sense, 0.0};
        for (const double dx : {-0.5, 0.5})
            for (const double dy : {-0.5, 0.5})
                shape.arrows.push_back({{viewCentre.x + dx * extent, viewCentre.y + dy * extent, z}, k, true});
        break;
    }
    }
    return shape;
}

double sourceReach(const Electrostatics::Source &s)
{
    using Type = Electrostatics::Source::Type;
    switch (s.type)
    {
    case Type::PointCharge:
    case Type::InfiniteLine:
    case Type::InfinitePlane:
        return 0.0;
    case Type::SolidSphere:
    case Type::SphericalShell:
    case Type::ThickSphericalShell:
    case Type::InfiniteCylinderZ:
    case Type::InfiniteHollowCylinderZ:
    case Type::CircularPlate:
    case Type::AnnularPlate:
        return std::max(s.radius, 0.0);
    case Type::FiniteLine:
        return 0.5 * std::abs(s.length);
    case Type::RectangularPlate:
        return 0.5 * std::hypot(s.width, s.height);
    case Type::RectangularVolume:
        return 0.5 * std::hypot(s.width, s.height, s.thickness);
    case Type::TriangularPlate:
        return std::hypot(0.5 * s.width, 2.0 * s.height / 3.0);
    }
    return 0.0;
}

double sourceReach(const Magnetostatics::Source &s)
{
    using Type = Magnetostatics::Source::Type;
    switch (s.type)
    {
    case Type::InfiniteWireZ:
    case Type::MagneticDipole:
    case Type::InfiniteCurrentSheet:
        return 0.0;
    case Type::SolidCurrentCylinderZ:
    case Type::HollowCurrentCylinderZ:
    case Type::CircularLoop:
        return std::max(s.radius, 0.0);
    case Type::FiniteWire:
        return 0.5 * std::abs(s.length);
    case Type::Solenoid:
        return std::hypot(s.radius, 0.5 * s.length);
    case Type::HelmholtzPair:
        return std::hypot(s.radius, 0.5 * (s.length > 1e-9 ? s.length : s.radius));
    case Type::RectangularLoop:
        return 0.5 * std::hypot(s.width, s.height);
    case Type::TriangularLoop:
        return std::hypot(0.5 * s.width, 2.0 * s.height / 3.0);
    }
    return 0.0;
}
} // namespace qsig::app
