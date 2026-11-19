#pragma once

// Scenarios shared by two programs:
//  - tools/baseline/legacy_baseline.cpp, compiled against the QTsignalApp 6.14 sources,
//    which records reference values into tests/fixtures/legacy_baseline.inc;
//  - tests/test_parity.cpp, compiled against the new core, which replays them.
// Field and enum names are identical in both code bases, so the same templates serve both.
// The including file must include the solver headers first.

#include <array>
#include <cstddef>
#include <vector>

namespace scenarios
{
using P3 = std::array<double, 3>;

inline const std::vector<P3> &probePoints()
{
    static const std::vector<P3> points = {
        {0.37, 0.21, 0.05},  {-0.8, 0.45, -0.3}, {1.6, -1.1, 0.7},  {0.05, -0.02, 0.4},
        {0.11, -0.04, 0.03}, {2.5, 2.5, 2.5},    {-0.15, 0.6, 0.0}, {0.3, 0.1, -0.12},
    };
    return points;
}

// One source of every type, plus variants that exercise rotations and hollow geometries.
template <class S> std::vector<S> electrostaticSources()
{
    using T = typename S::Type;
    std::vector<S> out;
    auto make = [&out](T type) -> S &
    {
        S s;
        s.type = type;
        s.position = {0.1, -0.05, 0.0};
        out.push_back(s);
        return out.back();
    };
    make(T::PointCharge).strength = 2.5e-9;
    make(T::SolidSphere).radius = 0.3;
    make(T::SphericalShell).radius = 0.35;
    make(T::InfiniteCylinderZ).strength = 4e-10;
    make(T::FiniteLine).angleDeg = 25.0;
    make(T::RectangularPlate);
    make(T::ThickSphericalShell);
    make(T::InfiniteHollowCylinderZ).strength = -3e-10;
    make(T::InfiniteLine).strength = 1e-10;
    make(T::CircularPlate).radius = 0.4;
    make(T::AnnularPlate).angleDeg = 15.0;
    make(T::TriangularPlate).angleDeg = -40.0;
    make(T::InfinitePlane).strength = 2e-11;
    make(T::RectangularVolume).angleDeg = 10.0;
    S &rotated = make(T::RectangularPlate);
    rotated.angleDeg = 33.0;
    rotated.width = 0.8;
    rotated.height = 0.2;
    rotated.position = {-0.2, 0.3, 0.1};
    return out;
}

template <class S> std::vector<S> magnetostaticSources()
{
    using T = typename S::Type;
    std::vector<S> out;
    auto make = [&out](T type) -> S &
    {
        S s;
        s.type = type;
        s.position = {-0.05, 0.08, 0.0};
        out.push_back(s);
        return out.back();
    };
    make(T::InfiniteWireZ).strength = 3.0;
    S &wire = make(T::FiniteWire);
    wire.azimuthDeg = 30.0;
    wire.elevationDeg = 20.0;
    make(T::CircularLoop).radius = 0.4;
    S &solenoid = make(T::Solenoid);
    solenoid.turns = 250;
    solenoid.length = 1.2;
    solenoid.radius = 0.3;
    make(T::MagneticDipole).strength = 0.8;
    make(T::SolidCurrentCylinderZ);
    make(T::HollowCurrentCylinderZ);
    S &rect = make(T::RectangularLoop);
    rect.azimuthDeg = 45.0;
    rect.elevationDeg = 60.0;
    make(T::TriangularLoop);
    make(T::HelmholtzPair).length = 0.5;
    S &sheet = make(T::InfiniteCurrentSheet);
    sheet.azimuthDeg = 30.0;
    sheet.strength = 2.0;
    return out;
}

// FDTD cases, applied to a freshly constructed FDTD::Solver2D.
struct FdtdCase
{
    FDTD::BoundaryCondition boundary;
    FDTD::Polarization polarization;
    FDTD::SourceWaveform waveform;
    int steps;
};

inline const std::vector<FdtdCase> &fdtdCases()
{
    static const std::vector<FdtdCase> cases = {
        {FDTD::BoundaryCondition::Cpml, FDTD::Polarization::TMz, FDTD::SourceWaveform::GaussianModulatedSine, 400},
        {FDTD::BoundaryCondition::MurFirstOrder, FDTD::Polarization::TMz, FDTD::SourceWaveform::ContinuousSine, 300},
        {FDTD::BoundaryCondition::Pec, FDTD::Polarization::TEz, FDTD::SourceWaveform::Ricker, 300},
        {FDTD::BoundaryCondition::Cpml, FDTD::Polarization::TEz, FDTD::SourceWaveform::GaussianModulatedSine, 350},
    };
    return cases;
}

inline const std::vector<std::array<int, 2>> &fdtdProbes()
{
    static const std::vector<std::array<int, 2>> probes = {{60, 50}, {95, 50}, {80, 20}, {30, 80}};
    return probes;
}

inline void setupFdtd(FDTD::Solver2D &solver, const FdtdCase &c)
{
    solver.configure(120, 100, 0.005, 0.005, 0.95, c.boundary);
    solver.setPolarization(c.polarization);
    FDTD::Source source;
    source.name = "S1";
    source.x = 40;
    source.y = 50;
    source.waveform = c.waveform;
    source.frequencyHz = 6e9;
    source.amplitude = 1.0;
    solver.setSource(0, source);
    FDTD::Material dielectric;
    dielectric.epsilonR = 4.0;
    dielectric.conductivitySPerM = 0.002;
    solver.setMaterialRect(70, 30, 90, 70, dielectric);
    FDTD::Material metal;
    metal.pec = true;
    solver.setMaterialRect(100, 10, 103, 40, metal);
}

// Wire-network MoM cases (center-fed dipole, dipole + parasitic reflector, a bent V).
inline std::vector<NumericalEM::WireNetworkMomInput> wireCases()
{
    std::vector<NumericalEM::WireNetworkMomInput> cases;
    // The 6.14 baseline was recorded with the historical pulse formulation (its default then).
    NumericalEM::WireNetworkMomInput dipole;
    dipole.currentBasisTreatment = NumericalEM::WireCurrentBasisTreatment::PulsePointMatching;
    dipole.frequencyHz = 100e6;
    dipole.wires.push_back({"D", {0.0, 0.0, -0.7}, {0.0, 0.0, 0.7}, 0.001});
    dipole.feeds.push_back({"F", {0.0, 0.0, 0.0}, {1.0, 0.0}, 50.0});
    cases.push_back(dipole);

    // Thicker wires keep the adaptive mesh (segment <= 3.5 radius) under maxUnknowns.
    NumericalEM::WireNetworkMomInput yagi = dipole;
    yagi.wires.front().radiusM = 0.004;
    yagi.wires.push_back({"R", {-0.6, 0.0, -0.76}, {-0.6, 0.0, 0.76}, 0.004});
    cases.push_back(yagi);

    NumericalEM::WireNetworkMomInput vee;
    vee.currentBasisTreatment = NumericalEM::WireCurrentBasisTreatment::PulsePointMatching;
    vee.frequencyHz = 145e6;
    vee.wires.push_back({"A", {0.0, 0.0, 0.0}, {0.42, 0.0, 0.25}, 0.0015});
    vee.wires.push_back({"B", {0.0, 0.0, 0.0}, {-0.42, 0.0, 0.25}, 0.0015});
    vee.feeds.push_back({"F", {0.0, 0.0, 0.0}, {1.0, 0.0}, 50.0});
    cases.push_back(vee);
    return cases;
}

inline const std::vector<const char *> &calculatorExpressions()
{
    static const std::vector<const char *> expressions = {
        "2*pi*sqrt(3)", "c0/(2*1e8)", "1/(4*pi*epsilon0)", "mu0*c0", "kb*300", "sin(pi/6)+log10(1000)",
    };
    return expressions;
}
} // namespace scenarios
