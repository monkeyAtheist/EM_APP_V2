#include "qsig/magnetostatics.h"

#include <algorithm>
#include <cmath>

namespace Magnetostatics
{

namespace
{
constexpr double SingularDistance = 1e-8;
constexpr double Pi = qsig::constants::pi;

void perpendicularBasis(const Vec3 &axis, Vec3 &u, Vec3 &v)
{
    const Vec3 a = normalized(axis);
    const Vec3 reference = std::abs(a.z) < 0.85 ? Vec3{0.0, 0.0, 1.0} : Vec3{1.0, 0.0, 0.0};
    u = normalized(cross(a, reference));
    v = normalized(cross(a, u));
}

Vec3 finiteSegmentField(const Vec3 &p0, const Vec3 &p1, double current, const Vec3 &point, bool *singular)
{
    const Vec3 delta = p1 - p0;
    const double length = delta.norm();
    if (length <= SingularDistance) return {};
    const Vec3 axis = delta / length;
    const Vec3 center = (p0 + p1) * 0.5;
    const double halfLength = 0.5 * length;
    const Vec3 r = point - center;
    const double s = dot(r, axis);
    const Vec3 rhoVector = r - axis * s;
    const double rho2 = rhoVector.normSquared();
    if (rho2 <= SingularDistance * SingularDistance)
    {
        if (std::abs(s) <= halfLength + SingularDistance && singular) *singular = true;
        return {};
    }
    const double tNear = s - halfLength;
    const double tFar = s + halfLength;
    const double rNear = std::sqrt(rho2 + tNear * tNear);
    const double rFar = std::sqrt(rho2 + tFar * tFar);
    const double geometry = (tFar / rFar - tNear / rNear) / rho2;
    return cross(axis, rhoVector) * (qsig::constants::biotSavartK * current * geometry);
}

Vec3 currentCylinderField(double totalCurrent, double innerRadius, double outerRadius, double dx, double dy)
{
    outerRadius = std::max(outerRadius, SingularDistance);
    innerRadius = std::clamp(innerRadius, 0.0, outerRadius * (1.0 - 1e-9));
    const double rho2 = dx * dx + dy * dy;
    const double rho = std::sqrt(rho2);
    if (rho <= SingularDistance) return {};

    double enclosed = totalCurrent;
    if (rho < innerRadius)
        enclosed = 0.0;
    else if (rho < outerRadius)
    {
        if (innerRadius <= SingularDistance)
            enclosed = totalCurrent * rho2 / (outerRadius * outerRadius);
        else
            enclosed = totalCurrent * (rho2 - innerRadius * innerRadius) /
                       (outerRadius * outerRadius - innerRadius * innerRadius);
    }
    const double factor = qsig::constants::mu0 * enclosed / (2.0 * Pi * rho2);
    return {-factor * dy, factor * dx, 0.0};
}
} // namespace

Vec3 axisUnit(const Source &source)
{
    const double az = qsig::constants::degToRad(source.azimuthDeg);
    const double el = qsig::constants::degToRad(source.elevationDeg);
    return normalized({std::cos(el) * std::cos(az), std::cos(el) * std::sin(az), std::sin(el)});
}

FieldResult fieldAt(const std::vector<Source> &sources, const Vec3 &point)
{
    FieldResult total;
    for (const auto &source : sources)
    {
        const FieldResult contribution = fieldFromSource(source, point);
        total.magneticFluxDensity += contribution.magneticFluxDensity;
        total.singular = total.singular || contribution.singular;
        if (!contribution.note.empty())
        {
            if (!total.note.empty()) total.note += std::string(" | ");
            total.note += contribution.note;
        }
    }
    total.magneticField = total.magneticFluxDensity / qsig::constants::mu0;
    return total;
}

Vec3 lorentzForceAt(const std::vector<Source> &sources, const Vec3 &point, double testChargeC, const Vec3 &velocity)
{
    const auto field = fieldAt(sources, point);
    return cross(velocity, field.magneticFluxDensity) * testChargeC;
}

static Vec3 loopField(const Vec3 &center, const Vec3 &axis, double radius, double effectiveCurrent, const Vec3 &point,
                      int segments, bool *singular)
{
    Vec3 u, v;
    perpendicularBasis(axis, u, v);
    Vec3 result;
    const int n = std::max(24, segments);

    for (int i = 0; i < n; ++i)
    {
        const double a0 = 2.0 * Pi * double(i) / double(n);
        const double a1 = 2.0 * Pi * double(i + 1) / double(n);
        const Vec3 p0 = center + (u * std::cos(a0) + v * std::sin(a0)) * radius;
        const Vec3 p1 = center + (u * std::cos(a1) + v * std::sin(a1)) * radius;
        result += finiteSegmentField(p0, p1, effectiveCurrent, point, singular);
    }
    return result;
}

FieldResult fieldFromSource(const Source &source, const Vec3 &point)
{
    FieldResult out;
    using Type = Source::Type;

    if (source.type == Type::InfiniteWireZ)
    {
        const double dx = point.x - source.position.x;
        const double dy = point.y - source.position.y;
        const double rho2 = dx * dx + dy * dy;
        if (rho2 < SingularDistance * SingularDistance)
        {
            out.singular = true;
            out.note = std::string("Measurement point lies on an infinite wire.");
            return out;
        }
        const double factor = qsig::constants::mu0 * source.strength / (2.0 * Pi * rho2);
        out.magneticFluxDensity = {-factor * dy, factor * dx, 0.0};
    }
    else if (source.type == Type::SolidCurrentCylinderZ || source.type == Type::HollowCurrentCylinderZ)
    {
        const double dx = point.x - source.position.x;
        const double dy = point.y - source.position.y;
        const double inner = source.type == Type::HollowCurrentCylinderZ ? source.innerRadius : 0.0;
        out.magneticFluxDensity = currentCylinderField(source.strength, inner, source.radius, dx, dy);
    }
    else if (source.type == Type::FiniteWire)
    {
        const Vec3 axis = axisUnit(source);
        const double length = std::max(source.length, SingularDistance);
        const Vec3 p0 = source.position - axis * (0.5 * length);
        const Vec3 p1 = source.position + axis * (0.5 * length);
        out.magneticFluxDensity = finiteSegmentField(p0, p1, source.strength, point, &out.singular);
        if (out.singular) out.note = std::string("Measurement point lies on the finite current wire.");
    }
    else if (source.type == Type::CircularLoop)
    {
        out.magneticFluxDensity = loopField(source.position, axisUnit(source), std::max(source.radius, 1e-9),
                                            source.strength, point, 320, &out.singular);
    }
    else if (source.type == Type::RectangularLoop || source.type == Type::TriangularLoop)
    {
        Vec3 u, v;
        perpendicularBasis(axisUnit(source), u, v);
        const double w = std::max(source.width, SingularDistance);
        const double h = std::max(source.height, SingularDistance);
        if (source.type == Type::RectangularLoop)
        {
            const Vec3 p0 = source.position - u * (0.5 * w) - v * (0.5 * h);
            const Vec3 p1 = source.position + u * (0.5 * w) - v * (0.5 * h);
            const Vec3 p2 = source.position + u * (0.5 * w) + v * (0.5 * h);
            const Vec3 p3 = source.position - u * (0.5 * w) + v * (0.5 * h);
            out.magneticFluxDensity += finiteSegmentField(p0, p1, source.strength, point, &out.singular);
            out.magneticFluxDensity += finiteSegmentField(p1, p2, source.strength, point, &out.singular);
            out.magneticFluxDensity += finiteSegmentField(p2, p3, source.strength, point, &out.singular);
            out.magneticFluxDensity += finiteSegmentField(p3, p0, source.strength, point, &out.singular);
        }
        else
        {
            // Isosceles triangular loop centred at its centroid.
            const Vec3 p0 = source.position - u * (0.5 * w) - v * (h / 3.0);
            const Vec3 p1 = source.position + u * (0.5 * w) - v * (h / 3.0);
            const Vec3 p2 = source.position + v * (2.0 * h / 3.0);
            out.magneticFluxDensity += finiteSegmentField(p0, p1, source.strength, point, &out.singular);
            out.magneticFluxDensity += finiteSegmentField(p1, p2, source.strength, point, &out.singular);
            out.magneticFluxDensity += finiteSegmentField(p2, p0, source.strength, point, &out.singular);
        }
        if (out.singular) out.note = std::string("Measurement point lies on a current-loop filament.");
    }
    else if (source.type == Type::Solenoid)
    {
        const Vec3 axis = axisUnit(source);
        const int virtualLoops = std::clamp(source.turns, 1, 28);
        const double effectiveCurrent = source.strength * double(std::max(source.turns, 1)) / double(virtualLoops);
        const double length = std::max(source.length, 1e-9);
        for (int i = 0; i < virtualLoops; ++i)
        {
            const double s =
                virtualLoops == 1 ? 0.0 : -0.5 * length + (double(i) + 0.5) * length / double(virtualLoops);
            const Vec3 center = source.position + axis * s;
            out.magneticFluxDensity +=
                loopField(center, axis, std::max(source.radius, 1e-9), effectiveCurrent, point, 88, &out.singular);
        }
    }
    else if (source.type == Type::HelmholtzPair)
    {
        const Vec3 axis = axisUnit(source);
        const double separation = source.length > SingularDistance ? source.length : std::max(source.radius, 1e-9);
        out.magneticFluxDensity += loopField(source.position - axis * (0.5 * separation), axis,
                                             std::max(source.radius, 1e-9), source.strength, point, 220, &out.singular);
        out.magneticFluxDensity += loopField(source.position + axis * (0.5 * separation), axis,
                                             std::max(source.radius, 1e-9), source.strength, point, 220, &out.singular);
    }
    else if (source.type == Type::InfiniteCurrentSheet)
    {
        const double dz = point.z - source.position.z;
        if (std::abs(dz) <= SingularDistance)
        {
            out.singular = true;
            out.note = std::string("Measurement point lies on the ideal infinite current sheet.");
            return out;
        }
        const double az = qsig::constants::degToRad(source.azimuthDeg);
        const Vec3 kdir{std::cos(az), std::sin(az), 0.0};
        const Vec3 normal{0.0, 0.0, 1.0};
        const double sign = dz > 0.0 ? 1.0 : -1.0;
        out.magneticFluxDensity = cross(kdir, normal) * (0.5 * qsig::constants::mu0 * source.strength * sign);
    }
    else // Magnetic dipole
    {
        const Vec3 r = point - source.position;
        const double rn = r.norm();
        if (rn < SingularDistance)
        {
            out.singular = true;
            out.note = std::string("Measurement point lies on a magnetic dipole.");
            return out;
        }
        const Vec3 rhat = r / rn;
        const Vec3 m = axisUnit(source) * source.strength;
        out.magneticFluxDensity = (rhat * (3.0 * dot(m, rhat)) - m) * (qsig::constants::biotSavartK / (rn * rn * rn));
    }

    out.magneticField = out.magneticFluxDensity / qsig::constants::mu0;
    return out;
}

std::string typeName(Source::Type type)
{
    using Type = Source::Type;
    switch (type)
    {
    case Type::InfiniteWireZ:
        return std::string("Infinite wire (Z)");
    case Type::FiniteWire:
        return std::string("Finite straight wire");
    case Type::CircularLoop:
        return std::string("Circular loop");
    case Type::Solenoid:
        return std::string("Finite solenoid");
    case Type::MagneticDipole:
        return std::string("Magnetic dipole");
    case Type::SolidCurrentCylinderZ:
        return std::string("Infinite solid current cylinder (Z)");
    case Type::HollowCurrentCylinderZ:
        return std::string("Infinite hollow current cylinder (Z)");
    case Type::RectangularLoop:
        return std::string("Rectangular current loop");
    case Type::TriangularLoop:
        return std::string("Triangular current loop");
    case Type::HelmholtzPair:
        return std::string("Helmholtz coil pair");
    case Type::InfiniteCurrentSheet:
        return std::string("Infinite current sheet (XY)");
    }
    return std::string("Magnetic source");
}

std::string strengthSymbol(Source::Type type)
{
    if (type == Source::Type::MagneticDipole) return std::string("m");
    if (type == Source::Type::InfiniteCurrentSheet) return std::string("K");
    return std::string("I");
}

std::string strengthUnit(Source::Type type)
{
    if (type == Source::Type::MagneticDipole) return std::string("A.m²");
    if (type == Source::Type::InfiniteCurrentSheet) return std::string("A/m");
    return std::string("A");
}
} // namespace Magnetostatics
