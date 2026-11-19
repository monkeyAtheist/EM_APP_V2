#pragma once

// Electrostatic field of a set of uniformly charged sources, by superposition.
// Qt-free port of the legacy ElectrostaticModel kernels (QTsignalApp 6.14):
// same formulas, same discretisations, CODATA 2022 constants. The Qt object that
// stores a scene and emits change notifications belongs to the application layer.

#include "qsig/constants.h"
#include "qsig/vec3.h"

#include <string>
#include <vector>

namespace Electrostatics
{
using Vec3 = qsig::Vec3;
using qsig::cross;
using qsig::dot;
using qsig::norm;
using qsig::normalized;

struct FieldResult
{
    Vec3 electricField;     // V/m
    double potential = 0.0; // V, referenced to infinity
    bool potentialDefined = true;
    bool singular = false;
    std::string note;

    // Potential that stays usable when a source extends to infinity (equipotentials, differences of
    // potential). Equal to 'potential' for finite sources. An infinite line or cylinder contributes
    // -lambda/(2 pi eps0) ln(rho / rho0) with V = 0 at rho0 = kInfiniteReferenceRadius from its axis;
    // an infinite plane contributes -sigma |d| / (2 eps0), with V = 0 on the plane.
    double relativePotential = 0.0;
    bool relativePotentialDefined = true;
    bool potentialReferenced = false; // an infinite source fixed the reference of relativePotential
};

inline constexpr double kInfiniteReferenceRadius = 1.0; // m

struct Source
{
    // Enum values are persisted by index: append only, never reorder.
    enum class Type
    {
        PointCharge,
        SolidSphere,
        SphericalShell,
        InfiniteCylinderZ,
        FiniteLine,
        RectangularPlate,
        ThickSphericalShell,
        InfiniteHollowCylinderZ,
        InfiniteLine,
        CircularPlate,
        AnnularPlate,
        TriangularPlate,
        InfinitePlane,
        RectangularVolume
    };

    std::string name;
    Type type = Type::PointCharge;
    Vec3 position;

    // Point / sphere / finite line / finite plate or volume: total charge Q [C].
    // Infinite line and cylinders: linear density lambda [C/m].
    // Infinite plane: surface density sigma [C/m^2].
    double strength = 1.0e-9;

    double radius = 0.25;      // outer sphere / cylinder / disk radius [m]
    double innerRadius = 0.12; // hollow core / annulus inner radius [m]
    double length = 1.0;       // finite line [m]
    double width = 1.0;        // rectangular / triangular plate width [m]
    double height = 0.6;       // rectangular / triangular plate height [m]
    double thickness = 0.1;    // finite-thickness rectangular slab [m]
    double angleDeg = 0.0;     // rotation of line / plate in the XY plane
};

// Line, cylinders and plane: their charge extends to infinity, so V cannot be referenced to infinity.
bool extendsToInfinity(Source::Type type);

FieldResult fieldFromSource(const Source &source, const Vec3 &point);
FieldResult fieldAt(const std::vector<Source> &sources, const Vec3 &point);
Vec3 forceAt(const std::vector<Source> &sources, const Vec3 &point, double testChargeC);

std::string typeName(Source::Type type);
std::string strengthSymbol(Source::Type type);
std::string strengthUnit(Source::Type type);
} // namespace Electrostatics
