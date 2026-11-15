#pragma once

// Magnetostatic field of a set of current sources (Biot-Savart superposition).
// Qt-free port of the legacy MagnetostaticModel kernels (QTsignalApp 6.14):
// same formulas, same discretisations, CODATA 2022 constants. The Qt object that
// stores a scene and emits change notifications belongs to the application layer.

#include "qsig/constants.h"
#include "qsig/vec3.h"

#include <string>
#include <vector>

namespace Magnetostatics
{
using Vec3 = qsig::Vec3;
using qsig::cross;
using qsig::dot;
using qsig::norm;
using qsig::normalized;

struct FieldResult
{
    Vec3 magneticFluxDensity; // B [T]
    Vec3 magneticField;       // H [A/m], vacuum: B / mu0
    bool singular = false;
    std::string note;
};

struct Source
{
    // Enum values are persisted by index: append only, never reorder.
    enum class Type
    {
        InfiniteWireZ,
        FiniteWire,
        CircularLoop,
        Solenoid,
        MagneticDipole,
        SolidCurrentCylinderZ,
        HollowCurrentCylinderZ,
        RectangularLoop,
        TriangularLoop,
        HelmholtzPair,
        InfiniteCurrentSheet
    };

    std::string name;
    Type type = Type::InfiniteWireZ;
    Vec3 position;

    // Current [A] for wires, loops, coils and current cylinders; magnetic moment [A m^2]
    // for the dipole; surface current density K [A/m] for the infinite sheet.
    double strength = 1.0;
    double radius = 0.5;                   // loop / solenoid / cylinder outer radius [m]
    double innerRadius = 0.25;             // hollow cylinder inner radius [m]
    double length = 2.0;                   // finite wire / solenoid, or Helmholtz separation [m]
    double width = 1.0;                    // rectangular / triangular loop width [m]
    double height = 0.7;                   // rectangular / triangular loop height [m]
    int turns = 100;                       // solenoid turns
    double wireDiameter = 0.0008;          // winding wire diameter [m]
    double radialBuild = 0.0008;           // winding radial depth [m]
    double coreRelativePermeability = 1.0; // effective mu_r for derived L / B estimates only
    double azimuthDeg = 0.0;               // source axis azimuth in XY; current direction for the sheet
    double elevationDeg = 90.0;            // source axis elevation from XY
};

FieldResult fieldFromSource(const Source &source, const Vec3 &point);
FieldResult fieldAt(const std::vector<Source> &sources, const Vec3 &point);
Vec3 lorentzForceAt(const std::vector<Source> &sources, const Vec3 &point, double testChargeC,
                    const Vec3 &velocityMetersPerSecond);
Vec3 axisUnit(const Source &source);

std::string typeName(Source::Type type);
std::string strengthSymbol(Source::Type type);
std::string strengthUnit(Source::Type type);
} // namespace Magnetostatics
