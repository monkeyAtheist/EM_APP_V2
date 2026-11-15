#include "qsig/electrostatics.h"

#include <algorithm>
#include <limits>

namespace Electrostatics
{

namespace
{
constexpr double SingularDistance = 1.0e-9;
constexpr double Pi = qsig::constants::pi;

FieldResult pointContribution(double charge, const Vec3 &sourcePosition, const Vec3 &point)
{
    FieldResult result;
    const Vec3 r = point - sourcePosition;
    const double r2 = r.normSquared();
    if (r2 <= SingularDistance * SingularDistance)
    {
        result.singular = true;
        result.potentialDefined = false;
        result.note = std::string("Measurement point coincides with a charge element.");
        return result;
    }

    const double distance = std::sqrt(r2);
    result.electricField = r * (qsig::constants::coulombK * charge / (r2 * distance));
    result.potential = qsig::constants::coulombK * charge / distance;
    return result;
}

Vec3 rotateXY(double x, double y, double angleRad)
{
    const double c = std::cos(angleRad);
    const double s = std::sin(angleRad);
    return {x * c - y * s, x * s + y * c, 0.0};
}

FieldResult solidSphereContribution(double totalCharge, double radius, const Vec3 &center, const Vec3 &point)
{
    FieldResult result;
    radius = std::max(radius, SingularDistance);
    const Vec3 r = point - center;
    const double distance = r.norm();
    if (distance >= radius) return pointContribution(totalCharge, center, point);

    result.electricField = r * (qsig::constants::coulombK * totalCharge / (radius * radius * radius));
    const double ratio = distance / radius;
    result.potential = qsig::constants::coulombK * totalCharge * (3.0 - ratio * ratio) / (2.0 * radius);
    return result;
}

Vec3 infiniteSolidCylinderField(double lambda, double radius, double dx, double dy)
{
    const double rho2 = dx * dx + dy * dy;
    const double rho = std::sqrt(rho2);
    radius = std::max(radius, SingularDistance);
    if (rho <= SingularDistance) return {};
    if (rho < radius)
    {
        const double factor = lambda / (2.0 * Pi * qsig::constants::epsilon0 * radius * radius);
        return {factor * dx, factor * dy, 0.0};
    }
    const double factor = lambda / (2.0 * Pi * qsig::constants::epsilon0 * rho2);
    return {factor * dx, factor * dy, 0.0};
}

void accumulate(FieldResult &dst, const FieldResult &src)
{
    dst.electricField += src.electricField;
    if (dst.potentialDefined && src.potentialDefined)
        dst.potential += src.potential;
    else
        dst.potentialDefined = false;
    dst.singular = dst.singular || src.singular;
    if (!src.note.empty())
    {
        if (!dst.note.empty()) dst.note += std::string(" | ");
        dst.note += src.note;
    }
}

FieldResult integrateRectangularPlate(const Source &source, const Vec3 &point)
{
    FieldResult result;
    const double width = std::max(source.width, SingularDistance);
    const double height = std::max(source.height, SingularDistance);
    const double aspect = width / height;
    const int nx = std::clamp(int(std::round(24.0 * std::sqrt(aspect))), 12, 56);
    const int ny = std::clamp(int(std::round(24.0 / std::sqrt(aspect))), 12, 56);
    const int count = nx * ny;
    const double dq = source.strength / double(count);
    const double angle = qsig::constants::degToRad(source.angleDeg);

    for (int ix = 0; ix < nx; ++ix)
    {
        const double x = (-0.5 + (double(ix) + 0.5) / double(nx)) * width;
        for (int iy = 0; iy < ny; ++iy)
        {
            const double y = (-0.5 + (double(iy) + 0.5) / double(ny)) * height;
            const Vec3 offset = rotateXY(x, y, angle);
            const Vec3 elementPos{source.position.x + offset.x, source.position.y + offset.y, source.position.z};
            accumulate(result, pointContribution(dq, elementPos, point));
        }
    }
    if (result.singular)
        result.note = std::string("Measurement point lies on the numerical rectangular-plate mesh; move it slightly "
                                  "away from the source plane.");
    return result;
}

FieldResult integrateRectangularVolume(const Source &source, const Vec3 &point)
{
    FieldResult result;
    const double width = std::max(source.width, SingularDistance);
    const double height = std::max(source.height, SingularDistance);
    const double thickness = std::max(source.thickness, SingularDistance);
    const double angle = qsig::constants::degToRad(source.angleDeg);

    // Tensor-product 6-point Gauss-Legendre integration over a uniformly charged
    // rectangular parallelepiped. Because rho=Q/(W H t), mapping [-1,1]^3 onto
    // the physical volume gives dq = Q * wx*wy*wz / 8.
    // Even-order nodes avoid placing a quadrature point at the geometric centre.
    static constexpr double node[6] = {-0.9324695142031521, -0.6612093864662645, -0.2386191860831969,
                                       0.2386191860831969,  0.6612093864662645,  0.9324695142031521};
    static constexpr double weight[6] = {0.1713244923791704, 0.3607615730481386, 0.4679139345726910,
                                         0.4679139345726910, 0.3607615730481386, 0.1713244923791704};

    for (int ix = 0; ix < 6; ++ix)
    {
        const double x = 0.5 * width * node[ix];
        for (int iy = 0; iy < 6; ++iy)
        {
            const double y = 0.5 * height * node[iy];
            const Vec3 xy = rotateXY(x, y, angle);
            for (int iz = 0; iz < 6; ++iz)
            {
                const double z = 0.5 * thickness * node[iz];
                const double dq = source.strength * weight[ix] * weight[iy] * weight[iz] / 8.0;
                const Vec3 elementPos{source.position.x + xy.x, source.position.y + xy.y, source.position.z + z};
                accumulate(result, pointContribution(dq, elementPos, point));
            }
        }
    }
    if (result.singular)
        result.note = std::string("Measurement point coincides with a rectangular-volume quadrature node; move it "
                                  "slightly or change the dimensions.");
    else
        result.note =
            std::string("Finite-thickness rectangular slab evaluated by 6x6x6 Gauss-Legendre volume integration.");
    return result;
}

FieldResult integrateCircularPlate(const Source &source, const Vec3 &point, bool annular)
{
    FieldResult result;
    const double outer = std::max(source.radius, SingularDistance);
    const double inner = annular ? std::clamp(source.innerRadius, 0.0, outer * (1.0 - 1e-9)) : 0.0;
    const double area = Pi * (outer * outer - inner * inner);
    if (area <= 0.0) return result;

    // Equal-area radial bins: q is identical in every polar cell, which avoids
    // overweighting the centre and is stable for educational off-axis studies.
    const int nr = 26;
    const int nphi = 72;
    const double dq = source.strength / double(nr * nphi);
    const double angle = qsig::constants::degToRad(source.angleDeg);
    const double in2 = inner * inner;
    const double out2 = outer * outer;
    for (int ir = 0; ir < nr; ++ir)
    {
        const double f = (double(ir) + 0.5) / double(nr);
        const double r = std::sqrt(in2 + f * (out2 - in2));
        for (int ip = 0; ip < nphi; ++ip)
        {
            const double phi = 2.0 * Pi * (double(ip) + 0.5) / double(nphi);
            const Vec3 offset = rotateXY(r * std::cos(phi), r * std::sin(phi), angle);
            const Vec3 elementPos{source.position.x + offset.x, source.position.y + offset.y, source.position.z};
            accumulate(result, pointContribution(dq, elementPos, point));
        }
    }
    if (result.singular)
        result.note = std::string("Measurement point lies on the numerical circular-plate mesh; move it slightly away "
                                  "from the source plane.");
    return result;
}

FieldResult integrateTriangularPlate(const Source &source, const Vec3 &point)
{
    FieldResult result;
    const double width = std::max(source.width, SingularDistance);
    const double height = std::max(source.height, SingularDistance);
    const double angle = qsig::constants::degToRad(source.angleDeg);
    const int nx = 64;
    const int ny = 56;

    struct Sample
    {
        double x;
        double y;
    };
    std::vector<Sample> samples;
    samples.reserve(nx * ny / 2);
    // Isosceles triangle centred at its centroid: base y=-h/3, apex y=+2h/3.
    const double yBase = -height / 3.0;
    for (int iy = 0; iy < ny; ++iy)
    {
        const double y = yBase + (double(iy) + 0.5) * height / double(ny);
        const double t = std::clamp((y - yBase) / height, 0.0, 1.0);
        const double halfWidth = 0.5 * width * (1.0 - t);
        for (int ix = 0; ix < nx; ++ix)
        {
            const double x = (-0.5 + (double(ix) + 0.5) / double(nx)) * width;
            if (std::abs(x) <= halfWidth) samples.push_back({x, y});
        }
    }
    if (samples.empty()) return result;
    const double dq = source.strength / double(samples.size());
    for (const auto &sample : samples)
    {
        const Vec3 offset = rotateXY(sample.x, sample.y, angle);
        const Vec3 elementPos{source.position.x + offset.x, source.position.y + offset.y, source.position.z};
        accumulate(result, pointContribution(dq, elementPos, point));
    }
    if (result.singular)
        result.note = std::string("Measurement point lies on the numerical triangular-plate mesh; move it slightly "
                                  "away from the source plane.");
    return result;
}
} // namespace

FieldResult fieldAt(const std::vector<Source> &sources, const Vec3 &point)
{
    FieldResult total;
    for (const auto &source : sources) accumulate(total, fieldFromSource(source, point));
    return total;
}

Vec3 forceAt(const std::vector<Source> &sources, const Vec3 &point, double testChargeC)
{
    return fieldAt(sources, point).electricField * testChargeC;
}

std::string typeName(Source::Type type)
{
    using Type = Source::Type;
    switch (type)
    {
    case Type::PointCharge:
        return std::string("Point charge");
    case Type::SolidSphere:
        return std::string("Uniform solid sphere");
    case Type::SphericalShell:
        return std::string("Charged spherical shell (surface)");
    case Type::InfiniteCylinderZ:
        return std::string("Infinite solid cylinder (Z axis)");
    case Type::FiniteLine:
        return std::string("Finite charged wire");
    case Type::RectangularPlate:
        return std::string("Rectangular charged plate");
    case Type::ThickSphericalShell:
        return std::string("Thick charged sphere with hollow core");
    case Type::InfiniteHollowCylinderZ:
        return std::string("Infinite hollow charged cylinder (Z axis)");
    case Type::InfiniteLine:
        return std::string("Infinite line charge (Z axis)");
    case Type::CircularPlate:
        return std::string("Circular charged plate / disk");
    case Type::AnnularPlate:
        return std::string("Annular charged plate");
    case Type::TriangularPlate:
        return std::string("Triangular charged plate");
    case Type::InfinitePlane:
        return std::string("Infinite charged plane (XY)");
    case Type::RectangularVolume:
        return std::string("Rectangular charged plate (finite thickness)");
    }
    return std::string("Unknown");
}

std::string strengthSymbol(Source::Type type)
{
    using Type = Source::Type;
    if (type == Type::InfiniteCylinderZ || type == Type::InfiniteHollowCylinderZ || type == Type::InfiniteLine)
        return std::string("lambda");
    if (type == Type::InfinitePlane) return std::string("sigma");
    return std::string("Q");
}

std::string strengthUnit(Source::Type type)
{
    using Type = Source::Type;
    if (type == Type::InfiniteCylinderZ || type == Type::InfiniteHollowCylinderZ || type == Type::InfiniteLine)
        return std::string("C/m");
    if (type == Type::InfinitePlane) return std::string("C/m²");
    return std::string("C");
}

FieldResult fieldFromSource(const Source &source, const Vec3 &point)
{
    using Type = Source::Type;

    if (source.type == Type::PointCharge) return pointContribution(source.strength, source.position, point);

    if (source.type == Type::SolidSphere)
        return solidSphereContribution(source.strength, source.radius, source.position, point);

    if (source.type == Type::SphericalShell)
    {
        FieldResult result;
        const Vec3 r = point - source.position;
        const double distance = r.norm();
        const double radius = std::max(source.radius, SingularDistance);
        if (distance >= radius) return pointContribution(source.strength, source.position, point);
        result.electricField = {};
        result.potential = qsig::constants::coulombK * source.strength / radius;
        return result;
    }

    if (source.type == Type::ThickSphericalShell)
    {
        const double outer = std::max(source.radius, SingularDistance);
        const double inner = std::clamp(source.innerRadius, 0.0, outer * (1.0 - 1e-9));
        if (inner <= SingularDistance) return solidSphereContribution(source.strength, outer, source.position, point);
        const double volumeFactor = outer * outer * outer - inner * inner * inner;
        const double outerEquivalentQ = source.strength * (outer * outer * outer) / volumeFactor;
        const double innerEquivalentQ = source.strength * (inner * inner * inner) / volumeFactor;
        FieldResult result = solidSphereContribution(outerEquivalentQ, outer, source.position, point);
        const auto cavity = solidSphereContribution(-innerEquivalentQ, inner, source.position, point);
        accumulate(result, cavity);
        return result;
    }

    if (source.type == Type::InfiniteCylinderZ)
    {
        FieldResult result;
        const double dx = point.x - source.position.x;
        const double dy = point.y - source.position.y;
        result.electricField = infiniteSolidCylinderField(source.strength, source.radius, dx, dy);
        result.potentialDefined = false;
        result.note = std::string("Infinite-cylinder potential requires a reference radius; only E is reported.");
        return result;
    }

    if (source.type == Type::InfiniteHollowCylinderZ)
    {
        FieldResult result;
        const double outer = std::max(source.radius, SingularDistance);
        const double inner = std::clamp(source.innerRadius, 0.0, outer * (1.0 - 1e-9));
        const double dx = point.x - source.position.x;
        const double dy = point.y - source.position.y;
        if (inner <= SingularDistance)
            result.electricField = infiniteSolidCylinderField(source.strength, outer, dx, dy);
        else
        {
            const double areaFactor = outer * outer - inner * inner;
            const double outerLambda = source.strength * outer * outer / areaFactor;
            const double innerLambda = source.strength * inner * inner / areaFactor;
            result.electricField = infiniteSolidCylinderField(outerLambda, outer, dx, dy) -
                                   infiniteSolidCylinderField(innerLambda, inner, dx, dy);
        }
        result.potentialDefined = false;
        result.note =
            std::string("Infinite hollow-cylinder potential requires a reference radius; only E is reported.");
        return result;
    }

    if (source.type == Type::InfiniteLine)
    {
        FieldResult result;
        const double dx = point.x - source.position.x;
        const double dy = point.y - source.position.y;
        const double rho2 = dx * dx + dy * dy;
        if (rho2 <= SingularDistance * SingularDistance)
        {
            result.singular = true;
            result.potentialDefined = false;
            result.note = std::string("Measurement point lies on the infinite line charge.");
            return result;
        }
        const double factor = source.strength / (2.0 * Pi * qsig::constants::epsilon0 * rho2);
        result.electricField = {factor * dx, factor * dy, 0.0};
        result.potentialDefined = false;
        result.note = std::string("Infinite-line potential requires a reference radius; only E is reported.");
        return result;
    }

    if (source.type == Type::InfinitePlane)
    {
        FieldResult result;
        const double dz = point.z - source.position.z;
        if (std::abs(dz) <= SingularDistance)
        {
            result.singular = true;
            result.potentialDefined = false;
            result.note = std::string("Measurement point lies on the ideal infinite charged plane.");
            return result;
        }
        const double sign = dz > 0.0 ? 1.0 : -1.0;
        result.electricField = {0.0, 0.0, sign * source.strength / (2.0 * qsig::constants::epsilon0)};
        result.potentialDefined = false;
        result.note =
            std::string("The absolute potential of an infinite plane is reference-dependent; only E is reported.");
        return result;
    }

    if (source.type == Type::FiniteLine)
    {
        // Exact field of a uniformly charged finite line. source.strength is the TOTAL charge Q.
        FieldResult result;
        const double length = std::max(source.length, SingularDistance);
        const double halfLength = 0.5 * length;
        const double lambda = source.strength / length;
        const double angle = qsig::constants::degToRad(source.angleDeg);
        const Vec3 axis{std::cos(angle), std::sin(angle), 0.0};
        const Vec3 r = point - source.position;
        const double s = r.x * axis.x + r.y * axis.y + r.z * axis.z;
        const Vec3 rhoVector = r - axis * s;
        const double rho2 = rhoVector.normSquared();
        const double tNear = s - halfLength;
        const double tFar = s + halfLength;
        const double rNear = std::sqrt(rho2 + tNear * tNear);
        const double rFar = std::sqrt(rho2 + tFar * tFar);

        if (rho2 <= SingularDistance * SingularDistance)
        {
            if (std::abs(s) <= halfLength + SingularDistance)
            {
                result.singular = true;
                result.potentialDefined = false;
                result.note = std::string("Measurement point lies on the finite charged wire.");
                return result;
            }
            const double axialFactor = qsig::constants::coulombK * lambda * (1.0 / rNear - 1.0 / rFar);
            result.electricField = axis * axialFactor;
            const double absS = std::abs(s);
            result.potential = qsig::constants::coulombK * lambda * std::log((absS + halfLength) / (absS - halfLength));
            return result;
        }

        const double rho = std::sqrt(rho2);
        const double axialFactor = 1.0 / rNear - 1.0 / rFar;
        const double transverseFactor = (tFar / rFar - tNear / rNear) / rho2;
        result.electricField =
            (axis * axialFactor + rhoVector * transverseFactor) * (qsig::constants::coulombK * lambda);
        result.potential = qsig::constants::coulombK * lambda * (std::asinh(tFar / rho) - std::asinh(tNear / rho));
        return result;
    }

    if (source.type == Type::RectangularPlate) return integrateRectangularPlate(source, point);
    if (source.type == Type::RectangularVolume) return integrateRectangularVolume(source, point);
    if (source.type == Type::CircularPlate) return integrateCircularPlate(source, point, false);
    if (source.type == Type::AnnularPlate) return integrateCircularPlate(source, point, true);
    if (source.type == Type::TriangularPlate) return integrateTriangularPlate(source, point);

    return {};
}
} // namespace Electrostatics
