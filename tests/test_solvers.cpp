// FDTD, BEM, loop coupling and thin-wire MoM.

#include "qsig_test.h"
#include "reference_fields.h"

#include "qsig/antenna_validation_bench.h"
#include "qsig/fdtd_solver.h"
#include "qsig/numerical_em_solvers.h"

#include <algorithm>

namespace k = qsig::constants;

// ---------------------------------------------------------------- FDTD

TEST_CASE("fdtd: time step honours the 2D Courant limit", "[fdtd]")
{
    FDTD::Solver2D s;
    REQUIRE(s.configure(100, 80, 0.004, 0.003, 0.9, FDTD::BoundaryCondition::Cpml));
    const double limit = 1.0 / (k::c0 * std::sqrt(1.0 / (0.004 * 0.004) + 1.0 / (0.003 * 0.003)));
    CHECK_REL(s.dtS(), 0.9 * limit, 1e-12);
}

namespace
{
FDTD::Solver2D pulseInBox(FDTD::BoundaryCondition boundary)
{
    FDTD::Solver2D s;
    s.configure(160, 160, 0.003, 0.003, 0.95, boundary);
    FDTD::Source src;
    src.x = 80;
    src.y = 80;
    src.waveform = FDTD::SourceWaveform::Ricker;
    src.frequencyHz = 4e9;
    s.setSource(0, src);
    return s;
}
} // namespace

TEST_CASE("fdtd: CPML absorbs an outgoing pulse", "[fdtd]")
{
    auto s = pulseInBox(FDTD::BoundaryCondition::Cpml);
    double peak = 0.0;
    for (int i = 0; i < 6; ++i)
    {
        s.step(50);
        peak = std::max(peak, s.totalEnergyPerMeterJ());
    }
    s.step(1500);
    CHECK(s.totalEnergyPerMeterJ() < 1e-3 * peak);
}

TEST_CASE("fdtd: a PEC cavity keeps its energy once the source has stopped", "[fdtd]")
{
    auto s = pulseInBox(FDTD::BoundaryCondition::Pec);
    s.step(600);
    const double e1 = s.totalEnergyPerMeterJ();
    s.step(1200);
    const double e2 = s.totalEnergyPerMeterJ();
    CHECK(e1 > 0.0);
    CHECK_REL(e2, e1, 0.05);
}

TEST_CASE("fdtd: a pulse travels at c", "[fdtd]")
{
    // Peak time of the Ricker pulse at two probes 40 cells apart on the same ray.
    auto s = pulseInBox(FDTD::BoundaryCondition::Cpml);
    double peakA = 0.0, peakB = 0.0, tA = 0.0, tB = 0.0;
    for (int n = 0; n < 1000; ++n)
    {
        s.step(1);
        const double a = std::abs(s.sample(95, 80).ezVPerM), b = std::abs(s.sample(135, 80).ezVPerM);
        if (a > peakA)
        {
            peakA = a;
            tA = s.timeS();
        }
        if (b > peakB)
        {
            peakB = b;
            tB = s.timeS();
        }
    }
    REQUIRE(peakA > 0.0);
    REQUIRE(peakB > 0.0);
    CHECK_REL(tB - tA, 40 * 0.003 / k::c0, 0.05);
}

// ---------------------------------------------------------------- BEM, loops

TEST_CASE("bem: two-wire line capacitance matches the closed form", "[mom]")
{
    NumericalEM::BemInput2D in;
    in.conductorA.center = {-0.02, 0.0};
    in.conductorB.center = {0.02, 0.0};
    in.conductorA.radiusM = in.conductorB.radiusM = 0.006;
    in.conductorA.potentialV = 0.5;
    in.conductorB.potentialV = -0.5;
    in.panelsPerConductor = 96;
    const auto r = NumericalEM::solveTwoConductorBem2D(in);
    REQUIRE(r.valid);
    CHECK_REL(r.capacitancePerLengthFpm, NumericalEM::analyticTwoWireCapacitancePerLength(0.006, 0.04), 0.01);
    CHECK_REL(NumericalEM::analyticTwoWireCapacitancePerLength(0.006, 0.04),
              k::pi * k::epsilon0 / std::acosh(0.04 / 0.012), 1e-12);
}

TEST_CASE("loops: coaxial mutual inductance matches Maxwell's formula", "[mom]")
{
    NumericalEM::LoopCouplingInput in;
    in.primary.radiusM = 0.05;
    in.secondary.radiusM = 0.04;
    in.secondary.center = {0.0, 0.0, 0.03};
    in.integrationSegments = 360;
    const auto r = NumericalEM::solveCircularLoopCoupling(in);
    REQUIRE(r.valid);
    CHECK_REL(r.mutualInductanceH, reference::coaxialLoopsMutual(0.05, 0.04, 0.03), 5e-3);
}

// ---------------------------------------------------------------- thin-wire MoM

TEST_CASE("pocklington: non-converged results are now refused (UI defaults)", "[mom]")
{
    NumericalEM::ThinWireMomInput in; // tab defaults: 100 MHz, 1.409 m, 15 mm, 41 segments
    in.lengthM = 1.409;
    in.radiusM = 0.015;
    const auto r = NumericalEM::solveThinWireDipolePocklington(in);
    CHECK(!r.valid);
    CHECK(r.convergenceRelative > NumericalEM::kPocklingtonConvergenceTolerance);
    CHECK(r.error.find("Not converged") != std::string::npos);
}

TEST_CASE("pocklington: the 1 mm wire that returned j605 kOhm is refused", "[mom]")
{
    NumericalEM::ThinWireMomInput in;
    in.lengthM = 1.409;
    in.radiusM = 0.001;
    in.segments = 41;
    const auto r = NumericalEM::solveThinWireDipolePocklington(in);
    CHECK(!r.valid);
}

namespace
{
NumericalEM::WireNetworkMomResult halfWaveDipole()
{
    const auto &ref = reference::kHalfWaveDipole;
    const double lambda = k::c0 / 100e6;
    NumericalEM::WireNetworkMomInput in;
    in.frequencyHz = 100e6;
    const double h = 0.5 * ref.lengthLambda * lambda;
    in.wires.push_back({"D", {0.0, 0.0, -h}, {0.0, 0.0, h}, ref.radiusLambda * lambda});
    in.feeds.push_back({"F", {0.0, 0.0, 0.0}, {1.0, 0.0}, 50.0});
    return NumericalEM::solveWireNetworkMom(in);
}
} // namespace

TEST_CASE("wire MoM: half-wave dipole resistance within 10 % of Hallen", "[mom]")
{
    const auto r = halfWaveDipole();
    REQUIRE(r.valid);
    REQUIRE(!r.feeds.empty());
    CHECK_REL(r.feeds.front().activeImpedanceOhm.real(), reference::kHalfWaveDipole.resistanceOhm, 0.10);
    CHECK_REL(r.directivityDbi, 2.15, 0.03);
}

TEST_CASE("wire MoM: half-wave dipole impedance (audit: X 25 ohm vs ~50)", "[mom][defect]")
{
    // To be settled against NEC-2 in phase 3; the band absorbs the delta-gap model spread.
    const auto r = halfWaveDipole();
    REQUIRE(r.valid);
    const auto z = r.feeds.front().activeImpedanceOhm;
    CHECK_REL(z.real(), reference::kHalfWaveDipole.resistanceOhm, 0.05);
    CHECK_NEAR(z.imag(), reference::kHalfWaveDipole.reactanceOhm, 0.0, reference::kHalfWaveDipole.reactanceBandOhm);
}

TEST_CASE("validation bench: a PASS must mean the impedance is close to its reference", "[validation][slow][defect]")
{
    // Today the dipole is declared PASS with a 23.5 % impedance error (threshold 35 %).
    const auto report = AntennaValidation::runProductionCase(AntennaValidation::CaseKind::HalfWaveDipole, 100e6);
    REQUIRE(!report.rows.empty());
    const auto &row = report.rows.front();
    CHECK(row.verdict != AntennaValidation::Verdict::Pass || row.impedanceReferenceErrorPercent < 5.0);
}

TEST_CASE("validation bench: patch impedance is passive", "[validation][slow]")
{
    // A passive antenna cannot have Re(Zin) < 0. With FMA contraction (GCC on ARM64), the legacy
    // patch path silently returned -0.43 - j8.3 ohm instead of 9.51 - j42.4 ohm.
    const auto report = AntennaValidation::runProductionCase(AntennaValidation::CaseKind::RectangularPatch, 100e6);
    REQUIRE(!report.rows.empty());
    CHECK(report.rows.front().inputImpedanceOhm.real() > 0.0);
}
