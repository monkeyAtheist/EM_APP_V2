// FDTD, BEM, loop coupling and thin-wire MoM.

#include "qsig_test.h"
#include "reference_fields.h"

#include "qsig/antenna_validation_bench.h"
#include "qsig/fdtd_solver.h"
#include "qsig/numerical_em_solvers.h"

#include <algorithm>
#include <complex>
#include <cmath>
#include <vector>

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

TEST_CASE("fdtd: the CPML reflects less than -80 dB (against a box three times larger)", "[fdtd]")
{
    // Field at a probe 20 cells from a Ricker source, in the 160 x 160 test box and in a 400 x 400 box
    // whose boundary is too far to answer within the window: the difference is what the CPML sends back.
    // 6.14 profile (H on the E nodes): -38 dB. Corrected: about -95 dB.
    for (const auto polarization : {FDTD::Polarization::TMz, FDTD::Polarization::TEz})
    {
        auto record = [polarization](int n, int offset)
        {
            FDTD::Solver2D s;
            s.setPolarization(polarization);
            s.configure(n, n, 0.003, 0.003, 0.95, FDTD::BoundaryCondition::Cpml);
            FDTD::Source src;
            src.x = 80 + offset;
            src.y = 80 + offset;
            src.waveform = FDTD::SourceWaveform::Ricker;
            src.frequencyHz = 4e9;
            s.setSources({src});
            std::vector<double> trace;
            while (s.timeS() < 3.5e-9)
            {
                s.step(1);
                trace.push_back(s.primaryScalarField()[static_cast<std::size_t>((80 + offset) * n + 100 + offset)]);
            }
            return trace;
        };
        const auto box = record(160, 0), reference = record(400, 120);
        REQUIRE(box.size() == reference.size());
        double peak = 0.0, returned = 0.0;
        for (std::size_t k = 0; k < box.size(); ++k)
        {
            peak = std::max(peak, std::abs(reference[k]));
            returned = std::max(returned, std::abs(box[k] - reference[k]));
        }
        REQUIRE(peak > 0.0);
        CHECK(returned < 1e-4 * peak);
    }
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

TEST_CASE("fdtd: whole-grid materials and source lists give the same fields as the per-cell setup", "[fdtd]")
{
    // Dielectric, magnetic lossy and conducting regions: every precomputed coefficient is involved.
    constexpr int nx = 80, ny = 60;
    FDTD::Material glass, ferrite, metal;
    glass.epsilonR = 4.0;
    ferrite.muR = 3.0;
    ferrite.conductivitySPerM = 0.5;
    metal.pec = true;
    auto materialOf = [&](int x, int y) -> const FDTD::Material *
    {
        if (x > 40 && x < 55 && y > 10 && y < 50) return &glass;
        if (x > 58 && x < 66 && y > 20 && y < 40) return &ferrite;
        if (x > 30 && x < 34 && y > 25 && y < 35) return &metal;
        return nullptr;
    };
    for (const auto polarization : {FDTD::Polarization::TMz, FDTD::Polarization::TEz})
    {
        FDTD::Solver2D perCell, whole;
        std::vector<FDTD::Material> grid(static_cast<std::size_t>(nx * ny));
        for (FDTD::Solver2D *s : {&perCell, &whole})
        {
            s->setPolarization(polarization);
            s->configure(nx, ny, 0.01, 0.01);
        }
        for (int y = 0; y < ny; ++y)
            for (int x = 0; x < nx; ++x)
                if (const FDTD::Material *m = materialOf(x, y))
                {
                    perCell.setMaterialCell(x, y, *m);
                    grid[static_cast<std::size_t>(y * nx + x)] = *m;
                }
        whole.setMaterials(grid);
        FDTD::Source source;
        source.x = 20;
        source.y = 30;
        source.waveform = FDTD::SourceWaveform::Ricker;
        source.frequencyHz = 3e9;
        perCell.setSource(0, source);
        whole.setSources({source});
        perCell.step(160);
        whole.step(160);
        CHECK(perCell.maxAbsPrimaryField() > 0.0);
        CHECK(perCell.ez() == whole.ez());
        CHECK(perCell.hz() == whole.hz());
        CHECK(perCell.hx() == whole.hx());
        CHECK(perCell.ey() == whole.ey());
    }
    // An empty source list leaves the grid silent (clearSources() would restore the default source).
    FDTD::Solver2D silent;
    silent.configure(40, 40, 0.01, 0.01);
    silent.setSources({});
    silent.step(50);
    CHECK(silent.maxAbsPrimaryField() == 0.0);
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

TEST_CASE("wire MoM: half-wave dipole impedance and directivity against Hallen", "[mom]")
{
    // Audit: the historical pulse formulation gave 82 + j25 ohm (reactance about half of it); the
    // Galerkin rooftop formulation, default since, gives 87 + j47. The reactance band absorbs the
    // delta-gap feed model (Hallen itself: 63 -> 51 ohm from N = 41 to 161).
    const auto r = halfWaveDipole();
    REQUIRE(r.valid);
    REQUIRE(!r.feeds.empty());
    REQUIRE(r.linearRooftopBasisUsed);
    const auto z = r.feeds.front().activeImpedanceOhm;
    CHECK_REL(z.real(), reference::kHalfWaveDipole.resistanceOhm, 0.03);
    CHECK_NEAR(z.imag(), reference::kHalfWaveDipole.reactanceOhm, 0.0, 8.0);
    CHECK_NEAR(r.directivityDbi, 2.15, 0.0, 0.05);
}

namespace
{
NumericalEM::WireNetworkMomResult thinDipole(double lengthLambda, double radiusLambda, bool farField = true,
                                             double radiusFactor = 3.5)
{
    const double lambda = k::c0 / 100e6;
    NumericalEM::WireNetworkMomInput in;
    in.frequencyHz = 100e6;
    in.maxUnknowns = 1200;
    in.maxSegmentLengthRadiusFactor = radiusFactor;
    in.computeFarField = farField;
    const double h = 0.5 * lengthLambda * lambda;
    in.wires.push_back({"D", {0.0, 0.0, -h}, {0.0, 0.0, h}, radiusLambda * lambda});
    in.feeds.push_back({"F", {0.0, 0.0, 0.0}, {1.0, 0.0}, 50.0});
    return NumericalEM::solveWireNetworkMom(in);
}
} // namespace

TEST_CASE("wire MoM: resonant length and resistance of a thin dipole against Hallen", "[mom]")
{
    double lo = 0.46, hi = 0.49;
    for (int i = 0; i < 18; ++i) // 0.03 lambda / 2^18: far below the tolerance
    {
        const double mid = 0.5 * (lo + hi);
        (thinDipole(mid, 1e-3, false).feeds.front().activeImpedanceOhm.imag() > 0.0 ? hi : lo) = mid;
    }
    const double length = 0.5 * (lo + hi);
    CHECK_REL(length, reference::kThinDipoleResonantLengthLambda, 0.01);
    CHECK_REL(thinDipole(length, 1e-3, false).feeds.front().activeImpedanceOhm.real(),
              reference::kThinDipoleResonantResistanceOhm, 0.02);
}

TEST_CASE("wire MoM: the impedance converges with the mesh", "[mom]")
{
    // Default mesh (segments of 3.5 radii) against a finer one (2.5 radii): within 1 %.
    const auto z = thinDipole(0.5, 1e-3, false).feeds.front().activeImpedanceOhm;
    const auto fine = thinDipole(0.5, 1e-3, false, 2.5).feeds.front().activeImpedanceOhm;
    CHECK(std::abs(z - fine) < 0.01 * std::abs(fine));
}

TEST_CASE("wire MoM: radiated power, current moment, reciprocity", "[mom]")
{
    // Energy: the power integrated over the far-field sphere equals the power delivered at the feed.
    const auto dipole = thinDipole(0.5, 1e-3);
    CHECK_REL(dipole.radiatedPowerW, dipole.acceptedPowerW, 1e-3);

    // Short dipole: radiation of its current moment p = integral of I dl, P = eta k^2 |p|^2 / (12 pi),
    // and the directivity of a Hertzian dipole (1.76 dBi). Both independent of the feed model.
    const auto shortDipole = thinDipole(0.05, 2e-4);
    REQUIRE(shortDipole.valid);
    std::complex<double> moment{0.0, 0.0};
    for (const auto &s : shortDipole.meshSegments) moment += 0.5 * (s.currentAtP0A + s.currentAtP1A) * s.lengthM * s.tangent.z;
    const double kk = 2.0 * k::pi * 100e6 / k::c0;
    CHECK_REL(shortDipole.radiatedPowerW, k::eta0 * kk * kk * std::norm(moment) / (12.0 * k::pi), 0.005);
    CHECK_NEAR(shortDipole.directivityDbi, 1.76, 0.0, 0.02);
    CHECK_REL(shortDipole.radiatedPowerW, shortDipole.acceptedPowerW, 1e-3);

    // Two parallel dipoles, each with its own port: the Galerkin system is reciprocal (Y12 = Y21).
    const double lambda = k::c0 / 100e6;
    NumericalEM::WireNetworkMomInput pair;
    pair.frequencyHz = 100e6;
    pair.maxUnknowns = 1200;
    pair.computeFarField = false;
    pair.wires.push_back({"1", {0.0, 0.0, -0.25 * lambda}, {0.0, 0.0, 0.25 * lambda}, 5e-4 * lambda});
    pair.wires.push_back({"2", {0.4 * lambda, 0.0, -0.2 * lambda}, {0.4 * lambda, 0.0, 0.2 * lambda}, 5e-4 * lambda});
    pair.feeds.push_back({"P1", {0.0, 0.0, 0.0}, {1.0, 0.0}, 50.0});
    pair.feeds.push_back({"P2", {0.4 * lambda, 0.0, 0.0}, {0.0, 0.0}, 50.0});
    const auto first = NumericalEM::solveWireNetworkMom(pair);
    pair.feeds[0].voltageV = {0.0, 0.0};
    pair.feeds[1].voltageV = {1.0, 0.0};
    const auto second = NumericalEM::solveWireNetworkMom(pair);
    REQUIRE(first.valid);
    REQUIRE(second.valid);
    const auto y21 = first.feeds[1].currentA, y12 = second.feeds[0].currentA;
    CHECK(std::abs(y21 - y12) < 1e-6 * std::abs(y21));
    CHECK(std::abs(y21) > 1e-4); // the coupling is real
}

TEST_CASE("validation bench: a PASS must mean the impedance is close to its reference", "[validation][slow]")
{
    // Audit: the 6.14 bench declared the dipole PASS with a 23.5 % impedance error (threshold 35 %,
    // against the induced-EMF value of a vanishing radius). Fixed: production path = rooftop basis,
    // reference = Hallen for the same wire, PASS within 3 % on R.
    const auto report = AntennaValidation::runProductionCase(AntennaValidation::CaseKind::HalfWaveDipole, 100e6);
    REQUIRE(!report.rows.empty());
    const auto &row = report.rows.front();
    CHECK(row.productionPath);
    CHECK(row.verdict != AntennaValidation::Verdict::Pass || row.impedanceReferenceErrorPercent < 5.0);
    CHECK(row.verdict == AntennaValidation::Verdict::Pass);
    CHECK(row.impedanceReferenceErrorPercent < 3.0);
}

TEST_CASE("validation bench: patch impedance is passive", "[validation][slow]")
{
    // A passive antenna cannot have Re(Zin) < 0. With FMA contraction (GCC on ARM64), the legacy
    // patch path silently returned -0.43 - j8.3 ohm instead of 9.51 - j42.4 ohm.
    const auto report = AntennaValidation::runProductionCase(AntennaValidation::CaseKind::RectangularPatch, 100e6);
    REQUIRE(!report.rows.empty());
    CHECK(report.rows.front().inputImpedanceOhm.real() > 0.0);
}
