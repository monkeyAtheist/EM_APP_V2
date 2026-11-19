#include "qsig/magnetostatics.h"
#include "qsig/sheet_integrals.h"
#include "qsig/special_functions.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace Magnetostatics
{

void perpendicularBasis(const Vec3 &axis, Vec3 &u, Vec3 &v)
{
    const Vec3 a = normalized(axis);
    const Vec3 reference = std::abs(a.z) < 0.85 ? Vec3{0.0, 0.0, 1.0} : Vec3{1.0, 0.0, 0.0};
    u = normalized(cross(a, reference));
    v = normalized(cross(a, u));
}

namespace
{
constexpr double SingularDistance = 1e-8;
constexpr double Pi = qsig::constants::pi;

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

namespace
{
// Point in the frame of a source with axis n centred at 'center': z along the axis, rho away from it.
struct AxialFrame
{
    double z = 0.0;
    double rho = 0.0;
    Vec3 axis;
    Vec3 radialUnit; // zero on the axis
};

AxialFrame axialFrame(const Vec3 &center, const Vec3 &axis, const Vec3 &point)
{
    AxialFrame f;
    f.axis = normalized(axis);
    const Vec3 r = point - center;
    f.z = dot(r, f.axis);
    const Vec3 radial = r - f.axis * f.z;
    f.rho = radial.norm();
    f.radialUnit = f.rho > 0.0 ? radial / f.rho : Vec3{};
    return f;
}

// Exact field of a circular filament (complete elliptic integrals K and E); the current turns
// counterclockwise about 'axis'. Replaces the 6.14 polygon of 320 straight segments.
Vec3 exactLoopField(const Vec3 &center, const Vec3 &axis, double a, double current, const Vec3 &point, bool *singular)
{
    const AxialFrame f = axialFrame(center, axis, point);
    const double rho = f.rho, z = f.z;
    const double mu0I = qsig::constants::mu0 * current;
    const double q = (a + rho) * (a + rho) + z * z;
    const double d = (a - rho) * (a - rho) + z * z; // squared distance to the wire in the meridian plane
    if (d <= SingularDistance * SingularDistance)
    {
        if (singular) *singular = true;
        return {};
    }
    const double k2 = 4.0 * a * rho / q;
    if (k2 < 1e-8)
    {
        // Near the axis: on-axis field plus the first-order radial term (from div B = 0).
        const double s = a * a + z * z;
        const double bz = mu0I * a * a / (2.0 * s * std::sqrt(s));
        const double brho = 3.0 * mu0I * a * a * z * rho / (4.0 * s * s * std::sqrt(s));
        return f.axis * bz + f.radialUnit * brho;
    }
    double K = 0.0, E = 0.0;
    qsig::sheet::ellipticKEc(std::sqrt(k2), std::sqrt(d / q), K, E); // exact complementary modulus near the wire
    const double c = mu0I / (2.0 * Pi * std::sqrt(q));
    const double bz = c * (K + (a * a - rho * rho - z * z) / d * E);
    const double brho = c * z / rho * (-K + (a * a + rho * rho + z * z) / d * E);
    return f.axis * bz + f.radialUnit * brho;
}

// Exact field of a finite solenoid modelled as a uniform cylindrical current sheet (n = N / L turns per
// metre), after Derby and Olbert, Am. J. Phys. 78, 229 (2010). Singular only on the end rings.
Vec3 solenoidSheetField(const Vec3 &center, const Vec3 &axis, double a, double length, double turnsPerMetre,
                        double current, const Vec3 &point, bool *singular)
{
    const AxialFrame f = axialFrame(center, axis, point);
    const double rho = f.rho;
    const double b0 = qsig::constants::mu0 * turnsPerMetre * current / Pi;
    const double gamma = (a - rho) / (a + rho);
    double brho = 0.0, bz = 0.0;
    for (const double side : {1.0, -1.0})
    {
        const double zs = f.z + side * 0.5 * length; // z+ then z-
        const double far = std::sqrt(zs * zs + (rho + a) * (rho + a));
        const double kc = std::sqrt(zs * zs + (a - rho) * (a - rho)) / far;
        if (kc < 1e-12)
        {
            if (singular) *singular = true;
            return {};
        }
        const double alpha = a / far;
        const double beta = zs / far;
        brho += side * alpha * qsig::special::bulirschCel(kc, 1.0, 1.0, -1.0);
        bz += side * beta * qsig::special::bulirschCel(kc, gamma * gamma, 1.0, gamma);
    }
    return f.axis * (b0 * a / (a + rho) * bz) + f.radialUnit * (b0 * brho);
}
} // namespace

// Up to this many turns the solenoid is drawn and computed turn by turn (exact loops); above, as the
// ideal current sheet, which differs from the real winding only within about one pitch of the wire.
constexpr int kDiscreteSolenoidTurns = 24;

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
        out.magneticFluxDensity = exactLoopField(source.position, axisUnit(source), std::max(source.radius, 1e-9),
                                                 source.strength, point, &out.singular);
        if (out.singular) out.note = std::string("Measurement point lies on the current loop.");
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
        const int turns = std::max(source.turns, 1);
        const double length = std::max(source.length, 1e-9);
        const double radius = std::max(source.radius, 1e-9);
        if (turns <= kDiscreteSolenoidTurns)
        {
            for (int i = 0; i < turns; ++i)
            {
                const double s = turns == 1 ? 0.0 : -0.5 * length + (double(i) + 0.5) * length / double(turns);
                out.magneticFluxDensity +=
                    exactLoopField(source.position + axis * s, axis, radius, source.strength, point, &out.singular);
            }
        }
        else
        {
            out.magneticFluxDensity = solenoidSheetField(source.position, axis, radius, length, double(turns) / length,
                                                         source.strength, point, &out.singular);
        }
        if (out.singular) out.note = std::string("Measurement point lies on the solenoid winding.");
    }
    else if (source.type == Type::HelmholtzPair)
    {
        const Vec3 axis = axisUnit(source);
        const double separation = source.length > SingularDistance ? source.length : std::max(source.radius, 1e-9);
        out.magneticFluxDensity += exactLoopField(source.position - axis * (0.5 * separation), axis,
                                                  std::max(source.radius, 1e-9), source.strength, point, &out.singular);
        out.magneticFluxDensity += exactLoopField(source.position + axis * (0.5 * separation), axis,
                                                  std::max(source.radius, 1e-9), source.strength, point, &out.singular);
        if (out.singular) out.note = std::string("Measurement point lies on a Helmholtz coil.");
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

double magnetomotiveForce(const std::vector<Source> &sources, const Vec3 &a, const Vec3 &b, int panels)
{
    constexpr double gx[4] = {-0.8611363115940526, -0.3399810435848563, 0.3399810435848563, 0.8611363115940526};
    constexpr double gw[4] = {0.3478548451374538, 0.6521451548625461, 0.6521451548625461, 0.3478548451374538};
    const Vec3 d = b - a;
    const int n = std::max(1, panels);
    double sum = 0.0;
    for (int i = 0; i < n; ++i)
        for (int g = 0; g < 4; ++g)
        {
            const double t = (i + 0.5 * (gx[g] + 1.0)) / n;
            const FieldResult f = fieldAt(sources, a + d * t);
            if (f.singular) return std::numeric_limits<double>::quiet_NaN();
            sum += gw[g] * dot(f.magneticField, d);
        }
    return 0.5 * sum / n;
}

std::string strengthUnit(Source::Type type)
{
    if (type == Source::Type::MagneticDipole) return std::string("A.m²");
    if (type == Source::Type::InfiniteCurrentSheet) return std::string("A/m");
    return std::string("A");
}
} // namespace Magnetostatics
