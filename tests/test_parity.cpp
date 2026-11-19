// Parity with the QTsignalApp 6.14 code: replays tests/fixtures/scenarios.h on the new core
// and compares with tests/fixtures/legacy_baseline.inc (recorded from the old sources).
// The only expected differences come from the CODATA 2018 -> 2022 constants (~1e-9).

#include "qsig_test.h"

#include "qsig/antenna_validation_bench.h"
#include "qsig/electrostatics.h"
#include "qsig/em_calculator_model.h"
#include "qsig/fdtd_solver.h"
#include "qsig/magnetostatics.h"
#include "qsig/numerical_em_solvers.h"

#include "fixtures/legacy_baseline.inc"
#include "fixtures/scenarios.h"

#include <iterator>

namespace
{
constexpr double kRel = 1e-7;

// Compares a value with the next baseline entry; absolute floor scales with the case magnitude.
struct Cursor
{
    const double *data;
    std::size_t size;
    std::size_t index = 0;
    double next() { return index < size ? data[index++] : std::nan(""); }
};

template <std::size_t N> Cursor cursor(const double (&array)[N])
{
    return {array, N};
}
} // namespace

TEST_CASE("parity: electrostatic kernels", "[parity]")
{
    auto c = cursor(kLegacyElectrostatic);
    const auto sources = scenarios::electrostaticSources<Electrostatics::Source>();
    auto compare = [&c](const Electrostatics::FieldResult &r)
    {
        const double ex = c.next(), ey = c.next(), ez = c.next(), v = c.next();
        const double floor = 1e-9 * (std::abs(ex) + std::abs(ey) + std::abs(ez)) + 1e-12; // V/m
        CHECK_NEAR(r.electricField.x, ex, kRel, floor);
        CHECK_NEAR(r.electricField.y, ey, kRel, floor);
        CHECK_NEAR(r.electricField.z, ez, kRel, floor);
        CHECK_NEAR(r.potential, v, kRel, 1e-9 * std::abs(v) + 1e-12);
        CHECK((r.potentialDefined ? 1.0 : 0.0) == c.next());
        CHECK((r.singular ? 1.0 : 0.0) == c.next());
    };
    // Plates and slab were replaced by exact kernels in phase 1 (the 6.14 point-charge grids were
    // the defect): their baseline entries are skipped, as is the whole-scene sum that includes them.
    using T = Electrostatics::Source::Type;
    auto replaced = [](T t)
    {
        return t == T::RectangularPlate || t == T::CircularPlate || t == T::AnnularPlate || t == T::TriangularPlate ||
               t == T::RectangularVolume;
    };
    for (const auto &s : sources)
        for (const auto &p : scenarios::probePoints())
        {
            if (replaced(s.type))
            {
                c.index += 6;
                continue;
            }
            compare(Electrostatics::fieldAt({s}, {p[0], p[1], p[2]}));
        }
    c.index += 6 * scenarios::probePoints().size();
    CHECK(c.index == c.size);
}

TEST_CASE("parity: magnetostatic kernels", "[parity]")
{
    auto c = cursor(kLegacyMagnetostatic);
    const auto sources = scenarios::magnetostaticSources<Magnetostatics::Source>();
    auto compare = [&c](const Magnetostatics::FieldResult &r)
    {
        const double bx = c.next(), by = c.next(), bz = c.next();
        const double floor = 1e-9 * (std::abs(bx) + std::abs(by) + std::abs(bz)) + 1e-21; // T
        CHECK_NEAR(r.magneticFluxDensity.x, bx, kRel, floor);
        CHECK_NEAR(r.magneticFluxDensity.y, by, kRel, floor);
        CHECK_NEAR(r.magneticFluxDensity.z, bz, kRel, floor);
        CHECK((r.singular ? 1.0 : 0.0) == c.next());
    };
    // Loops, solenoid and Helmholtz pair became exact in phase 2 (the 6.14 polygons of 88 to 320 segments
    // and the 28 "virtual" loops of the solenoid were the defect): their entries are skipped, as is the
    // whole-scene sum that includes them.
    using T = Magnetostatics::Source::Type;
    auto replaced = [](T t) { return t == T::CircularLoop || t == T::Solenoid || t == T::HelmholtzPair; };
    for (const auto &s : sources)
        for (const auto &p : scenarios::probePoints())
        {
            if (replaced(s.type))
            {
                c.index += 4;
                continue;
            }
            compare(Magnetostatics::fieldAt({s}, {p[0], p[1], p[2]}));
        }
    c.index += 4 * scenarios::probePoints().size();
    CHECK(c.index == c.size);
}

TEST_CASE("parity: FDTD time stepping (Mur, PEC; TMz and TEz)", "[parity]")
{
    // The CPML was corrected after the audit (its magnetic profile sat on the electric nodes, half a cell
    // off: -38 dB of reflection instead of -95 dB): the CPML cases are skipped, the CPML tests of the
    // [fdtd] group measure the absorber against a reference box instead.
    auto c = cursor(kLegacyFdtd);
    for (const auto &fc : scenarios::fdtdCases())
    {
        if (fc.boundary == FDTD::BoundaryCondition::Cpml)
        {
            c.index += 4 * scenarios::fdtdProbes().size() + 1;
            continue;
        }
        FDTD::Solver2D solver;
        scenarios::setupFdtd(solver, fc);
        solver.step(fc.steps);
        const double scale = solver.maxAbsPrimaryField();
        for (const auto &pr : scenarios::fdtdProbes())
        {
            const auto s = solver.sample(pr[0], pr[1]);
            CHECK_NEAR(s.ezVPerM, c.next(), 1e-6, 1e-7 * scale);
            CHECK_NEAR(s.hzAPerM, c.next(), 1e-6, 1e-7 * scale / qsig::constants::eta0);
            CHECK_NEAR(s.exVPerM, c.next(), 1e-6, 1e-7 * scale);
            CHECK_NEAR(s.hxAPerM, c.next(), 1e-6, 1e-7 * scale / qsig::constants::eta0);
        }
        CHECK_REL(solver.totalEnergyPerMeterJ(), c.next(), 1e-6);
    }
}

TEST_CASE("parity: wire-network MoM, BEM, loop coupling, Pocklington", "[parity]")
{
    auto w = cursor(kLegacyWireNetwork);
    for (const auto &in : scenarios::wireCases())
    {
        const auto r = NumericalEM::solveWireNetworkMom(in);
        CHECK((r.valid ? 1.0 : 0.0) == w.next());
        REQUIRE(!r.feeds.empty());
        CHECK_REL(r.feeds.front().activeImpedanceOhm.real(), w.next(), kRel);
        CHECK_REL(r.feeds.front().activeImpedanceOhm.imag(), w.next(), kRel);
        CHECK_REL(r.directivityDbi, w.next(), kRel);
    }

    auto b = cursor(kLegacyBemLoop);
    NumericalEM::BemInput2D bem;
    bem.conductorA.center = {-0.02, 0.0};
    bem.conductorB.center = {0.025, 0.004};
    bem.conductorB.shape = NumericalEM::ConductorShape::Rectangle;
    bem.conductorA.potentialV = 0.5;
    bem.conductorB.potentialV = -0.5;
    bem.epsilonR = 2.2;
    const auto br = NumericalEM::solveTwoConductorBem2D(bem);
    CHECK((br.valid ? 1.0 : 0.0) == b.next());
    CHECK_REL(br.capacitancePerLengthFpm, b.next(), kRel);
    NumericalEM::LoopCouplingInput lc;
    lc.secondary.center = {0.0, 0.03, 0.04};
    lc.secondary.axis = {0.0, 0.3, 1.0};
    lc.secondary.turns = 3;
    const auto l = NumericalEM::solveCircularLoopCoupling(lc);
    CHECK((l.valid ? 1.0 : 0.0) == b.next());
    CHECK_REL(l.mutualInductanceH, b.next(), kRel);
    CHECK_REL(l.primarySelfInductanceH, b.next(), kRel);
    CHECK_REL(l.secondarySelfInductanceH, b.next(), kRel);
    CHECK_REL(l.couplingCoefficient, b.next(), kRel);

    // The first-resolution impedance is unchanged; only the new convergence check differs.
    auto p = cursor(kLegacyPocklington);
    for (double radius : {0.015, 0.001})
    {
        NumericalEM::ThinWireMomInput in;
        in.lengthM = 1.409;
        in.radiusM = radius;
        in.segments = 41;
        const auto r = NumericalEM::solveThinWireDipolePocklington(in);
        CHECK_REL(r.inputImpedanceOhm.real(), p.next(), kRel);
        CHECK_REL(r.inputImpedanceOhm.imag(), p.next(), kRel);
    }
}

TEST_CASE("parity: calculator expressions", "[parity]")
{
    auto c = cursor(kLegacyCalculator);
    for (const char *expression : scenarios::calculatorExpressions())
        // epsilon0 is now derived as 1/(mu0 c^2) instead of the rounded CODATA literal: ~1e-12.
        CHECK_REL(EmCalculator::evaluateExpression(expression).value, c.next(), 1e-11);
}

TEST_CASE("parity: production validation bench (loop, monopole)", "[parity][slow]")
{
    // The patch case is excluded: its production path makes a discrete choice that flips on
    // last-bit rounding (see "validation bench: patch impedance is passive").
    // The dipole is skipped: its production path (pulse -> rooftop basis) and its reference were
    // changed on purpose when the bench was fixed (a PASS at 23.5 % error, see test_solvers.cpp).
    auto c = cursor(kLegacyValidation);
    for (auto kind : {AntennaValidation::CaseKind::HalfWaveDipole, AntennaValidation::CaseKind::SmallCircularLoop,
                      AntennaValidation::CaseKind::QuarterWaveMonopole})
    {
        if (kind == AntennaValidation::CaseKind::HalfWaveDipole)
        {
            c.index += 4;
            continue;
        }
        const auto report = AntennaValidation::runProductionCase(kind, 100e6);
        REQUIRE(!report.rows.empty());
        const auto &row = report.rows.front();
        CHECK_REL(row.inputImpedanceOhm.real(), c.next(), 1e-6);
        CHECK_REL(row.inputImpedanceOhm.imag(), c.next(), 1e-6);
        CHECK_NEAR(row.directivityDbi, c.next(), 1e-6, 1e-9);
        CHECK(static_cast<double>(row.verdict) == c.next());
    }
}
