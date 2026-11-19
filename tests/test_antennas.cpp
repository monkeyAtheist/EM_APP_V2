// "Antennes" module, computation side (qsig/app/antenna_model.h, antenna_optimizer.h, antenna_bench.h):
// geometry of the elements, far field of the solved currents, presets, sweeps, the Yagi optimiser and
// the validation bench.
#include "qsig_test.h"

#include "qsig/app/antenna_bench.h"
#include "qsig/app/antenna_model.h"
#include "qsig/app/antenna_optimizer.h"
#include "qsig/constants.h"
#include "qsig/numerical_em_solvers.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <vector>

using qsig::Vec3;
using qsig::app::AntennaElement;
using qsig::app::AntennaSettings;
using qsig::app::AntennaSolveOptions;

namespace
{
using Type = AntennaElement::Type;
constexpr double kF = 300e6;
const double kLambda = qsig::constants::c0 / kF;

double polylineLength(const std::vector<qsig::Vec3> &line)
{
    double sum = 0.0;
    for (std::size_t k = 0; k + 1 < line.size(); ++k) sum += qsig::norm(line[k + 1] - line[k]);
    return sum;
}

double maxOf(const std::vector<double> &v)
{
    return *std::max_element(v.begin(), v.end());
}
} // namespace

TEST_CASE("antennas: elements expand into closed or open wires with their feed", "[antennas]")
{
    AntennaElement dipole;
    dipole.type = Type::Dipole;
    dipole.length = 0.5;
    dipole.position = {1.0, 2.0, 3.0};
    auto shape = qsig::app::antennaShape(dipole);
    REQUIRE(shape.polylines.size() == 1);
    CHECK_REL(polylineLength(shape.polylines[0]), 0.5, 1e-12);
    REQUIRE(shape.feeds.size() == 1);
    CHECK(qsig::norm(shape.feeds[0] - dipole.position) < 1e-12); // fed at the centre
    // Vertical by default: elevation 90 degrees.
    CHECK_NEAR(shape.polylines[0].front().z, 3.0 - 0.25, 0.0, 1e-12);

    // A V of 120 degrees keeps its total length; its arms open by 60 degrees from the axis.
    dipole.apexDeg = 120.0;
    shape = qsig::app::antennaShape(dipole);
    CHECK_REL(polylineLength(shape.polylines[0]), 0.5, 1e-12);
    const qsig::Vec3 arm = shape.polylines[0].back() - dipole.position;
    CHECK_NEAR(std::acos(arm.z / qsig::norm(arm)) * 180.0 / qsig::constants::pi, 30.0, 0.0, 1e-9);

    // Loops are closed polygons; a passive wire has no feed.
    AntennaElement loop;
    loop.type = Type::CircularLoop;
    loop.radius = 0.1;
    loop.sides = 12;
    shape = qsig::app::antennaShape(loop);
    CHECK(qsig::norm(shape.polylines[0].front() - shape.polylines[0].back()) < 1e-12);
    CHECK_REL(polylineLength(shape.polylines[0]), 12 * 2 * 0.1 * std::sin(qsig::constants::pi / 12), 1e-12);
    AntennaElement folded;
    folded.type = Type::FoldedDipole;
    folded.length = 0.5;
    folded.width = 0.01;
    CHECK_REL(polylineLength(qsig::app::antennaShape(folded).polylines[0]), 2 * 0.5 + 2 * 0.01, 1e-12);
    AntennaElement parasite;
    parasite.type = Type::Wire;
    parasite.fed = 0;
    CHECK(qsig::app::antennaShape(parasite).feeds.empty());
    CHECK(qsig::app::antennaShape(parasite).polylines[0].size() == 2);

    // A helix: as many turns as asked, at the given pitch.
    AntennaElement helix;
    helix.type = Type::Helix;
    helix.radius = 0.05;
    helix.pitch = 0.02;
    helix.turns = 4;
    helix.sides = 16;
    shape = qsig::app::antennaShape(helix);
    CHECK(shape.polylines[0].size() == 4 * 16 + 1);
    CHECK_NEAR(shape.polylines[0].back().z - shape.polylines[0].front().z, 4 * 0.02, 0.0, 1e-12);
}

TEST_CASE("antennas: far field of the solved currents against the solver and against energy", "[antennas]")
{
    // Directivity from our pattern equals the solver's own far field (vertical lambda/2 dipole).
    const auto dipole = qsig::app::antennaPreset("dipole", kF);
    AntennaSettings settings;
    settings.frequency = kF;
    const auto r = qsig::app::solveAntenna(dipole, settings);
    REQUIRE(r.valid);
    REQUIRE(r.hasPattern);
    NumericalEM::WireNetworkMomInput in;
    in.frequencyHz = kF;
    in.segmentsPerWavelength = 100;
    in.maxSegmentLengthRadiusFactor = 50.0;
    in.wires.push_back({"D", {0, 0, -0.2375 * kLambda}, {0, 0, 0.2375 * kLambda}, 1e-3 * kLambda});
    in.feeds.push_back({"F", {0, 0, 0}, {1, 0}, 50});
    const auto reference = NumericalEM::solveWireNetworkMom(in);
    REQUIRE(reference.valid);
    CHECK_NEAR(r.directivityDbi, reference.directivityDbi, 0.0, 0.02);
    CHECK_REL(r.feeds[0].impedance.real(), reference.feeds[0].activeImpedanceOhm.real(), 1e-9);
    CHECK_REL(r.radiatedPowerW, r.acceptedPowerW, 2e-3);
    // Omnidirectional in the horizontal plane, a figure eight in a vertical one (nulls along the axis).
    const auto &h = r.horizontal.gainDbi;
    CHECK(maxOf(h) - *std::min_element(h.begin(), h.end()) < 0.01);
    CHECK_NEAR(maxOf(r.vertical.gainDbi), r.directivityDbi, 0.0, 0.01);
    CHECK(r.vertical.gainDbi.front() < r.directivityDbi - 30.0); // theta = 0: along the wire
    CHECK_NEAR(r.mainThetaDeg, 90.0, 0.0, 0.5);
    CHECK_NEAR(r.frontToBackDb, 0.0, 0.0, 0.01);
    // The grid reaches the same maximum.
    CHECK_NEAR(maxOf(r.gainDbi), r.directivityDbi, 0.0, 0.05);

    // Energy for every preset: the power radiated through the sphere is the power fed.
    for (const auto &p : qsig::app::antennaPresets())
    {
        const auto elements = qsig::app::antennaPreset(p.key, kF);
        REQUIRE(!elements.empty());
        AntennaSolveOptions coarse;
        coarse.gridStepDeg = 3.0;
        const auto s = qsig::app::solveAntenna(elements, settings, coarse);
        REQUIRE(s.valid);
        CHECK_REL(s.radiatedPowerW, s.acceptedPowerW, 3e-3);
    }
}

TEST_CASE("antennas: Yagi and phased pair point their beam along +x", "[antennas]")
{
    AntennaSettings settings;
    settings.frequency = kF;
    const auto yagi = qsig::app::solveAntenna(qsig::app::antennaPreset("yagi3", kF), settings);
    REQUIRE(yagi.valid);
    CHECK_NEAR(yagi.mainThetaDeg, 90.0, 0.0, 2.0);
    CHECK(std::min(yagi.mainPhiDeg, 360.0 - yagi.mainPhiDeg) < 2.0);
    CHECK(yagi.directivityDbi > 7.5);
    CHECK(yagi.frontToBackDb > 12.0);

    // Pair a quarter wavelength apart: the voltages of the preset impose currents in quadrature.
    const auto pairElements = qsig::app::antennaPreset("endfire_pair", kF);
    const auto pair = qsig::app::solveAntenna(pairElements, settings);
    REQUIRE(pair.valid);
    REQUIRE(pair.feeds.size() == 2);
    const auto ratio = pair.feeds[1].current / pair.feeds[0].current;
    CHECK_NEAR(std::abs(ratio), 1.0, 0.0, 1e-6);
    CHECK_NEAR(std::arg(ratio) * 180.0 / qsig::constants::pi, -90.0, 0.0, 1e-4);
    CHECK(pair.frontToBackDb > 20.0);
    CHECK(std::min(pair.mainPhiDeg, 360.0 - pair.mainPhiDeg) < 2.0);

    // The coupling matrix is reciprocal.
    const auto y = qsig::app::admittanceMatrix(pairElements, settings);
    REQUIRE(y.size() == 2);
    CHECK(std::abs(y[0][1] - y[1][0]) < 1e-6 * std::abs(y[0][1]));
}

TEST_CASE("antennas: sweeps, mesh guard and errors", "[antennas]")
{
    AntennaSettings settings;
    settings.frequency = kF;
    settings.sweepStart = 250e6;
    settings.sweepStop = 350e6;
    settings.sweepPoints = 11;
    const auto f = qsig::app::sweepFrequencies(settings);
    REQUIRE(f.size() == 11);
    CHECK(f.front() == 250e6);
    CHECK_REL(f.back(), 350e6, 1e-12);

    // A sweep point is the same solve as at that frequency.
    const auto dipole = qsig::app::antennaPreset("dipole", kF);
    const auto point = qsig::app::solveSweepPoint(dipole, settings, 280e6);
    AntennaSettings at = settings;
    at.frequency = 280e6;
    AntennaSolveOptions fast;
    fast.pattern = false;
    const auto direct = qsig::app::solveAntenna(dipole, at, fast);
    REQUIRE(point.valid);
    CHECK(std::abs(point.impedance - direct.feeds[0].impedance) < 1e-9 * std::abs(direct.feeds[0].impedance));
    // Below resonance the dipole is capacitive, above it inductive.
    CHECK(qsig::app::solveSweepPoint(dipole, settings, 250e6).impedance.imag() < 0.0);
    CHECK(qsig::app::solveSweepPoint(dipole, settings, 350e6).impedance.imag() > 0.0);

    // Segments never shorter than two radii: a thick wire lowers the mesh density.
    auto thick = dipole;
    thick[0].wireRadius = 0.02 * kLambda;
    const auto r = qsig::app::solveAntenna(thick, settings, fast);
    REQUIRE(r.valid);
    CHECK(r.segmentsPerWavelength <= 25.0 + 1e-9);

    // Nothing to solve.
    CHECK(!qsig::app::solveAntenna({}, settings).valid);
    auto passive = dipole;
    passive[0].fed = 0;
    const auto none = qsig::app::solveAntenna(passive, settings);
    CHECK(!none.valid);
    CHECK(!none.error.empty());
}

TEST_CASE("antennas: the Yagi optimiser improves its score and keeps the layout", "[antennas]")
{
    const auto yagi = qsig::app::antennaPreset("yagi3", kF);
    AntennaSettings settings;
    qsig::app::YagiOptimizerOptions options;
    options.goal = qsig::app::yagiGoal(qsig::app::YagiGoal::Kind::Balanced, true);
    options.maxEvaluations = 40;
    int calls = 0;
    const auto r = qsig::app::optimiseYagi(yagi, settings, options,
                                           [&](int evaluation, int budget, const qsig::app::SweepPoint &)
                                           {
                                               ++calls;
                                               CHECK(evaluation <= budget);
                                               return true;
                                           });
    REQUIRE(r.valid);
    CHECK(!r.cancelled);
    CHECK(r.variables == 5); // three lengths, two spacings
    CHECK(r.evaluations == calls);
    CHECK(r.evaluations <= 40);
    CHECK(r.improved);
    CHECK(r.scoreAfter > r.scoreBefore + 0.5);
    CHECK_NEAR(r.scoreBefore, qsig::app::yagiScore(qsig::app::solveSweepPoint(yagi, settings, kF), options.goal), 0.0,
               1e-9);
    // The radiator stays in place, the order along the boom too, every length within its bounds.
    REQUIRE(r.elements.size() == 3);
    CHECK(r.elements[1].position.x == yagi[1].position.x);
    CHECK(r.elements[0].position.x < r.elements[1].position.x);
    CHECK(r.elements[1].position.x < r.elements[2].position.x);
    for (const auto &e : r.elements)
    {
        CHECK(e.length >= 0.30 * kLambda - 1e-12);
        CHECK(e.length <= 0.60 * kLambda + 1e-12);
        CHECK(e.position.y == 0.0);
        CHECK(e.position.z == 0.0);
    }
    // What is reported is what the scene will show.
    const auto check = qsig::app::solveSweepPoint(r.elements, settings, kF);
    CHECK_NEAR(r.after.gainDbi, check.gainDbi, 0.0, 1e-9);

    // Stopped by the progress callback: the evaluations end there.
    const auto stopped = qsig::app::optimiseYagi(yagi, settings, options,
                                                 [](int evaluation, int, const qsig::app::SweepPoint &)
                                                 { return evaluation < 4; });
    CHECK(stopped.cancelled);
    CHECK(stopped.evaluations == 4);
    CHECK(stopped.valid);

    // Nothing to optimise.
    CHECK(!qsig::app::optimiseYagi(qsig::app::antennaPreset("dipole", kF), settings, options).valid);
    auto unfed = yagi;
    unfed[1].fed = 0;
    CHECK(!qsig::app::optimiseYagi(unfed, settings, options).error.empty());
    // Goals: weights from the kind; the match term only when asked.
    CHECK(qsig::app::yagiGoal(qsig::app::YagiGoal::Kind::FrontToBack, false).matchWeight == 0.0);
    CHECK(qsig::app::yagiGoal(qsig::app::YagiGoal::Kind::Gain, true).frontToBackWeight <
          qsig::app::yagiGoal(qsig::app::YagiGoal::Kind::Balanced, true).frontToBackWeight);
}

TEST_CASE("antennas: the validation bench passes at the default mesh and flags a coarse one", "[antennas]")
{
    int steps = 0;
    const auto report = qsig::app::runAntennaBench(AntennaSettings{}, [&](int done, int total)
                                                   {
                                                       CHECK(done == steps + 1);
                                                       CHECK(total == qsig::app::antennaBenchSteps());
                                                       steps = done;
                                                   });
    CHECK(steps == qsig::app::antennaBenchSteps());
    REQUIRE(report.cases.size() == 14);
    CHECK(report.passed == 14);
    CHECK(report.passed + report.warnings + report.failed == static_cast<int>(report.cases.size()));
    for (const auto &c : report.cases)
    {
        CHECK(!c.source.empty());
        CHECK(std::isfinite(c.deviation));
        CHECK(c.passLimit < c.warnLimit);
        // A PASS is a deviation within its threshold, nothing else.
        CHECK((c.verdict == qsig::app::AntennaBenchCase::Verdict::Pass) == (c.deviation <= c.passLimit));
    }
    // 40 segments per wavelength: the half-wave dipole resistance drifts past 3 % of Hallen.
    AntennaSettings coarse;
    coarse.segmentsPerWavelength = 40.0;
    const auto rough = qsig::app::runAntennaBench(coarse);
    CHECK(rough.warnings + rough.failed >= 1);
    CHECK(rough.cases.front().verdict != qsig::app::AntennaBenchCase::Verdict::Pass);
}

TEST_CASE("antennas: wires drawn between two points, and joined where their ends meet", "[antennas]")
{
    using qsig::app::setWireEndpoints;
    using qsig::app::wireEndpoints;
    AntennaElement wire;
    wire.type = AntennaElement::Type::Wire;
    wire.wireRadius = 1e-3;
    // Any direction: the element lies exactly between the two points.
    const Vec3 pairs[][2] = {{{0.1, 0.2, 0.3}, {0.4, -0.3, 0.9}}, {{0.0, 0.0, -0.2}, {0.0, 0.0, 0.5}}, {{1.0, 1.0, 1.0}, {0.2, 1.0, 1.0}},
                            {{0.0, 0.0, 0.5}, {0.0, 0.0, -0.2}}};
    for (const auto &pair : pairs)
    {
        REQUIRE(setWireEndpoints(wire, pair[0], pair[1]));
        Vec3 a, b;
        wireEndpoints(wire, a, b);
        CHECK_NEAR(qsig::norm(a - pair[0]), 0.0, 0.0, 1e-15);
        CHECK_NEAR(qsig::norm(b - pair[1]), 0.0, 0.0, 1e-15);
        CHECK_NEAR(wire.length, qsig::norm(pair[1] - pair[0]), 0.0, 1e-15);
        const auto shape = antennaShape(wire);
        REQUIRE(shape.polylines.size() == 1);
        CHECK_NEAR(qsig::norm(shape.polylines[0].front() - pair[0]), 0.0, 0.0, 1e-15);
        CHECK_NEAR(qsig::norm(shape.polylines[0].back() - pair[1]), 0.0, 0.0, 1e-15);
    }
    // Too short for its radius: refused, nothing changed.
    const AntennaElement before = wire;
    CHECK(!setWireEndpoints(wire, {0.0, 0.0, 0.0}, {0.0, 0.0, 0.005}));
    CHECK(wire == before);
    // Position along the wire of the nearest point, clamped to its ends.
    REQUIRE(setWireEndpoints(wire, {0.0, 0.0, 0.0}, {2.0, 0.0, 0.0}));
    CHECK_NEAR(qsig::app::wireFraction(wire, {0.5, 0.3, -0.1}), 0.25, 0.0, 1e-15);
    CHECK(qsig::app::wireFraction(wire, {-1.0, 0.0, 0.0}) == 0.0);
    CHECK(qsig::app::wireFraction(wire, {5.0, 0.0, 0.0}) == 1.0);

    // A dipole drawn as two wires meeting at an end: the same antenna as the one-piece dipole.
    const double length = 0.475 * kLambda;
    AntennaElement dipole;
    dipole.type = AntennaElement::Type::Dipole;
    dipole.length = length;
    dipole.wireRadius = 1e-3 * kLambda;
    AntennaElement lower, upper;
    lower.type = upper.type = AntennaElement::Type::Wire;
    lower.wireRadius = upper.wireRadius = dipole.wireRadius;
    REQUIRE(setWireEndpoints(lower, {0.0, 0.0, -0.5 * length}, {0.0, 0.0, 0.1 * length}));
    REQUIRE(setWireEndpoints(upper, {0.0, 0.0, 0.1 * length}, {0.0, 0.0, 0.5 * length}));
    lower.fed = 1;
    lower.feedFraction = qsig::app::wireFraction(lower, {0.0, 0.0, 0.0});
    upper.fed = 0;
    AntennaSolveOptions noPattern;
    noPattern.pattern = false;
    const auto whole = solveAntenna({dipole}, AntennaSettings{}, noPattern);
    const auto joined = solveAntenna({lower, upper}, AntennaSettings{}, noPattern);
    REQUIRE(whole.valid);
    REQUIRE(joined.valid);
    CHECK_NEAR(std::abs(joined.feeds[0].impedance - whole.feeds[0].impedance), 0.0, 0.0, 0.05);
    // Pulled 1 cm apart, the two wires are no longer one antenna.
    REQUIRE(setWireEndpoints(upper, {0.0, 0.0, 0.1 * length + 0.01}, {0.0, 0.0, 0.5 * length + 0.01}));
    const auto apart = solveAntenna({lower, upper}, AntennaSettings{}, noPattern);
    REQUIRE(apart.valid);
    CHECK(std::abs(apart.feeds[0].impedance - whole.feeds[0].impedance) > 50.0);

    // Anchors: both ends of each wire and the feed point.
    const auto anchors = qsig::app::antennaAnchors({lower, upper});
    CHECK(anchors.size() == 5);
    const auto loopAnchors = qsig::app::antennaAnchors(qsig::app::antennaPreset("quad_loop", kF));
    CHECK(loopAnchors.size() == 2); // closed loop: its start once, and the feed
}

namespace
{
// One element of each type, off the origin and askew, with every parameter that shapes it.
std::vector<AntennaElement> oneOfEach()
{
    std::vector<AntennaElement> elements;
    for (const Type type : {Type::Wire, Type::Dipole, Type::FoldedDipole, Type::CircularLoop, Type::RectangularLoop, Type::Helix})
    {
        AntennaElement e;
        e.type = type;
        e.position = {0.3, -0.2, 0.15};
        e.azimuthDeg = 30.0;
        e.elevationDeg = 20.0;
        e.rollDeg = 40.0;
        e.length = 0.4;
        e.width = 0.1;
        e.radius = 0.12;
        e.apexDeg = 120.0;
        e.pitch = 0.05;
        e.turns = 3.0;
        e.sides = 12;
        e.feedFraction = 0.3;
        elements.push_back(e);
    }
    return elements;
}

// Largest distance between the points of a shape and the images of the points of another one, taken
// in the same order (polylines and sources).
template <typename Map> double shapeGap(const AntennaElement &result, const AntennaElement &original, Map map)
{
    const auto a = antennaShape(result), b = antennaShape(original);
    if (a.polylines.size() != b.polylines.size() || a.feeds.size() != b.feeds.size()) return 1e300;
    double gap = 0.0;
    for (std::size_t i = 0; i < a.polylines.size(); ++i)
    {
        if (a.polylines[i].size() != b.polylines[i].size()) return 1e300;
        for (std::size_t k = 0; k < a.polylines[i].size(); ++k)
            gap = std::max(gap, qsig::norm(a.polylines[i][k] - map(b.polylines[i][k])));
    }
    for (std::size_t i = 0; i < a.feeds.size(); ++i) gap = std::max(gap, qsig::norm(a.feeds[i] - map(b.feeds[i])));
    return gap;
}
} // namespace

TEST_CASE("antennas: designer rotations and mirrors carry every point of an element", "[antennas]")
{
    using qsig::app::mirrorElement;
    using qsig::app::rotateElement;
    // Rotation about any axis through any pivot: each point of the wire and the source turn with it.
    const Vec3 pivot{-0.4, 0.25, 0.1};
    const Vec3 axis = qsig::normalized(Vec3{1.0, -2.0, 0.5});
    const double angle = 73.0 * qsig::constants::pi / 180.0;
    auto turn = [&](const Vec3 &q)
    {
        const Vec3 v = q - pivot;
        return pivot + v * std::cos(angle) + qsig::cross(axis, v) * std::sin(angle) +
               axis * (qsig::dot(axis, v) * (1.0 - std::cos(angle)));
    };
    for (const AntennaElement &original : oneOfEach())
    {
        AntennaElement e = original;
        rotateElement(e, pivot, axis * 3.0, 73.0); // the axis need not be a unit vector
        CHECK(shapeGap(e, original, turn) < 1e-9);
        // And back.
        rotateElement(e, pivot, axis, -73.0);
        CHECK(shapeGap(e, original, [](const Vec3 &q) { return q; }) < 1e-9);
    }
    // Quarter turns give clean angles: a horizontal wire along x becomes a wire along y, a vertical
    // dipole stays vertical while it goes round the z axis.
    AntennaElement wire;
    wire.type = Type::Wire;
    wire.elevationDeg = 0.0;
    wire.position = {0.3, 0.0, 0.0};
    rotateElement(wire, {}, {0.0, 0.0, 1.0}, 90.0);
    CHECK(wire.azimuthDeg == 90.0);
    CHECK(wire.elevationDeg == 0.0);
    CHECK_NEAR(qsig::norm(wire.position - Vec3{0.0, 0.3, 0.0}), 0.0, 0.0, 1e-15);
    AntennaElement dipole;
    dipole.position = {0.3, 0.0, 0.0};
    rotateElement(dipole, {}, {0.0, 0.0, 1.0}, 90.0);
    CHECK(dipole.elevationDeg == 90.0);
    CHECK(dipole.azimuthDeg == 0.0);
    // A degenerate axis changes nothing.
    const AntennaElement before = dipole;
    rotateElement(dipole, {}, {0.0, 0.0, 0.0}, 45.0);
    CHECK(dipole == before);

    // Mirror in any plane: each point and the source go to their images, in the same order along the
    // wire (the polarity of the source is kept). The helix keeps its winding sense: its centre and
    // its axis are mirrored, and its points stay on the mirrored cylinder.
    const Vec3 point{0.1, 0.05, -0.2};
    const Vec3 normal = qsig::normalized(Vec3{0.3, 1.0, -0.4});
    auto image = [&](const Vec3 &q) { return q - normal * (2.0 * qsig::dot(q - point, normal)); };
    for (const AntennaElement &original : oneOfEach())
    {
        AntennaElement e = original;
        mirrorElement(e, point, normal * 2.0);
        CHECK_NEAR(qsig::norm(e.position - image(original.position)), 0.0, 0.0, 1e-12);
        if (original.type != Type::Helix)
        {
            CHECK(shapeGap(e, original, image) < 1e-9);
        }
        else
        {
            const Vec3 mirroredAxis = image(original.position + antennaAxis(original)) - image(original.position);
            CHECK_NEAR(std::abs(qsig::dot(antennaAxis(e), mirroredAxis)), 1.0, 0.0, 1e-9);
            const auto shape = antennaShape(e); // kept alive: polylines[0] is a reference into it
            for (const Vec3 &q : shape.polylines[0])
            {
                const Vec3 r = q - e.position;
                const Vec3 across = r - antennaAxis(e) * qsig::dot(r, antennaAxis(e));
                CHECK_NEAR(qsig::norm(across), original.radius, 0.0, 1e-9);
            }
        }
        // Mirrored twice: the element itself.
        mirrorElement(e, point, normal);
        CHECK(shapeGap(e, original, [](const Vec3 &q) { return q; }) < 1e-9);
    }

    // Pivot of a selection: the centre of the box that holds its wires.
    AntennaElement first, second;
    first.type = second.type = Type::Wire;
    REQUIRE(qsig::app::setWireEndpoints(first, {0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}));
    REQUIRE(qsig::app::setWireEndpoints(second, {0.0, 2.0, -1.0}, {0.0, 2.0, 3.0}));
    const Vec3 centre = qsig::app::elementsCentre({first, second}, {0, 1});
    CHECK_NEAR(qsig::norm(centre - Vec3{0.5, 1.0, 1.0}), 0.0, 0.0, 1e-12);
    CHECK_NEAR(qsig::norm(qsig::app::elementsCentre({first, second}, {1}) - Vec3{0.0, 2.0, 1.0}), 0.0, 0.0, 1e-12);
}

TEST_CASE("antennas: a cut wire is the same antenna, and a branch on a wire is joined (T junction)", "[antennas]")
{
    using qsig::app::setWireEndpoints;
    using qsig::app::splitWire;
    using qsig::app::wireEndpoints;
    const double length = 0.475 * kLambda;
    AntennaElement wire;
    wire.type = Type::Wire;
    wire.wireRadius = 1e-3 * kLambda;
    REQUIRE(setWireEndpoints(wire, {0.0, 0.0, -0.5 * length}, {0.0, 0.0, 0.5 * length}));
    wire.fed = 1;
    wire.feedFraction = 0.5;

    // Cut at a quarter of its length: two wires end to end, the source on the part that held it.
    AntennaElement head, tail;
    REQUIRE(splitWire(wire, 0.25, head, tail));
    Vec3 a, b, c, d;
    wireEndpoints(head, a, b);
    wireEndpoints(tail, c, d);
    CHECK_NEAR(qsig::norm(a - Vec3{0.0, 0.0, -0.5 * length}), 0.0, 0.0, 1e-15);
    CHECK_NEAR(qsig::norm(b - c), 0.0, 0.0, 1e-15);
    CHECK_NEAR(qsig::norm(d - Vec3{0.0, 0.0, 0.5 * length}), 0.0, 0.0, 1e-15);
    CHECK_NEAR(b.z, -0.25 * length, 0.0, 1e-15);
    CHECK(head.fed == 0);
    CHECK(tail.fed == 1);
    CHECK_NEAR(tail.feedFraction, 1.0 / 3.0, 0.0, 1e-12); // the middle of the whole wire
    AntennaSolveOptions noPattern;
    noPattern.pattern = false;
    const auto whole = solveAntenna({wire}, AntennaSettings{}, noPattern);
    const auto cut = solveAntenna({head, tail}, AntennaSettings{}, noPattern);
    REQUIRE(whole.valid);
    REQUIRE(cut.valid);
    CHECK_NEAR(std::abs(cut.feeds[0].impedance - whole.feeds[0].impedance), 0.0, 0.0, 0.05);
    // A source before the cut stays on the first part.
    wire.feedFraction = 0.1;
    REQUIRE(splitWire(wire, 0.25, head, tail));
    CHECK(head.fed == 1);
    CHECK(tail.fed == 0);
    CHECK_NEAR(head.feedFraction, 0.4, 0.0, 1e-12);
    wire.feedFraction = 0.5;
    // Refused: at an end, too close to one for the part to stand, or not a plain wire.
    CHECK(!splitWire(wire, 0.0, head, tail));
    CHECK(!splitWire(wire, 1.0, head, tail));
    CHECK(!splitWire(wire, 0.001, head, tail));
    AntennaElement dipole;
    CHECK(!splitWire(dipole, 0.5, head, tail));

    // A branch ending on the body of the wire, at 0.7 of its length: the solver joins it there (T).
    // Current flows into the branch and the currents of the node balance (Kirchhoff). With the wire
    // cut at that point the antenna is the same.
    const Vec3 node{0.0, 0.0, 0.2 * length};
    AntennaElement branch;
    branch.type = Type::Wire;
    branch.wireRadius = wire.wireRadius;
    branch.fed = 0;
    REQUIRE(setWireEndpoints(branch, node, node + Vec3{0.2 * kLambda, 0.0, 0.0}));
    REQUIRE(splitWire(wire, 0.7, head, tail));
    auto intoBranch = [&node](const qsig::app::AntennaResult &r, int element, std::complex<double> &balance)
    {
        // Currents at the node: leaving it along a segment that starts there, entering it otherwise.
        std::complex<double> branchCurrent = 0.0;
        balance = 0.0;
        for (const auto &s : r.segments)
        {
            const bool starts = qsig::norm(s.p0 - node) < 1e-9, ends = qsig::norm(s.p1 - node) < 1e-9;
            if (!starts && !ends) continue;
            const std::complex<double> leaving = starts ? s.i0 : -s.i1;
            balance += leaving;
            if (s.element == element) branchCurrent = leaving;
        }
        return std::abs(branchCurrent);
    };
    std::complex<double> balance;
    const auto tee = solveAntenna({wire, branch}, AntennaSettings{}, noPattern);
    REQUIRE(tee.valid);
    CHECK(intoBranch(tee, 1, balance) > 0.02 * tee.maxCurrent);
    CHECK(std::abs(balance) < 1e-9 * tee.maxCurrent);
    const auto cutTee = solveAntenna({head, tail, branch}, AntennaSettings{}, noPattern);
    REQUIRE(cutTee.valid);
    CHECK(intoBranch(cutTee, 2, balance) > 0.02 * cutTee.maxCurrent);
    CHECK_NEAR(std::abs(cutTee.feeds[0].impedance - tee.feeds[0].impedance), 0.0, 0.0, 0.05);
    // Pulled a millimetre away from the wire, the branch is on its own: no current at its end.
    REQUIRE(setWireEndpoints(branch, node + Vec3{1e-3, 0.0, 0.0}, node + Vec3{0.2 * kLambda, 0.0, 0.0}));
    const auto loose = solveAntenna({wire, branch}, AntennaSettings{}, noPattern);
    REQUIRE(loose.valid);
    double endCurrent = 1.0;
    for (const auto &s : loose.segments)
        if (s.element == 1 && qsig::norm(s.p0 - node - Vec3{1e-3, 0.0, 0.0}) < 1e-9) endCurrent = std::abs(s.i0);
    CHECK(endCurrent < 1e-9 * loose.maxCurrent);

    // A wire must not end on a source (the solver refuses a source on a branched node): the anchors
    // offered to a wire leave the sources out; those of the measuring tool keep them.
    const auto forWires = qsig::app::antennaAnchors({wire}, -1, false);
    const auto forMeasures = qsig::app::antennaAnchors({wire});
    CHECK(forWires.size() == 2);
    CHECK(forMeasures.size() == 3);
    CHECK(qsig::app::antennaAnchors({wire, branch}, 0, false).size() == 2); // the wire itself left out
    AntennaElement onSource;
    onSource.type = Type::Wire;
    onSource.wireRadius = wire.wireRadius;
    onSource.fed = 0;
    REQUIRE(setWireEndpoints(onSource, {0.0, 0.0, 0.0}, {0.2 * kLambda, 0.0, 0.0}));
    CHECK(!solveAntenna({wire, onSource}, AntennaSettings{}, noPattern).valid);
}

TEST_CASE("antennas: designer constraints move, turn and resize the element that holds them", "[antennas]")
{
    using qsig::app::AntennaConstraint;
    using qsig::app::applyConstraints;
    using qsig::app::constraintError;
    using qsig::app::elementAnchor;
    using qsig::app::setWireEndpoints;
    using CType = AntennaConstraint::Type;
    auto wire = [](const Vec3 &a, const Vec3 &b)
    {
        AntennaElement e;
        e.type = Type::Wire;
        setWireEndpoints(e, a, b);
        return e;
    };
    auto constraint = [](CType type, int reference)
    {
        AntennaConstraint c;
        c.type = type;
        c.reference = reference;
        return c;
    };
    auto angleBetween = [](const AntennaElement &a, const AntennaElement &b)
    {
        const Vec3 u = antennaAxis(a), v = antennaAxis(b);
        return std::atan2(qsig::norm(qsig::cross(u, v)), qsig::dot(u, v)) * 180.0 / qsig::constants::pi;
    };

    // Ids: given once, kept; a copy (same id) gets its own and neither the constraints nor the group.
    std::vector<AntennaElement> scene{wire({0.0, 0.0, 0.0}, {0.0, 0.0, 0.5}), wire({0.2, 0.1, 0.0}, {0.5, 0.3, 0.2})};
    qsig::app::ensureElementIds(scene);
    CHECK(scene[0].id == 1);
    CHECK(scene[1].id == 2);
    scene[1].group = "G";
    scene[1].constraints.push_back(constraint(CType::Parallel, 1));
    scene.push_back(scene[1]);
    qsig::app::ensureElementIds(scene);
    CHECK(scene[1].id == 2);
    CHECK(scene[2].id == 3);
    CHECK(scene[2].constraints.empty());
    CHECK(scene[2].group.empty());
    CHECK(scene[1].constraints.size() == 1);
    scene.pop_back();
    scene[1].constraints.clear();
    AntennaElement &a = scene[0], &b = scene[1];

    // Anchors: centre and the two ends.
    CHECK_NEAR(qsig::norm(elementAnchor(a, 0) - Vec3{0.0, 0.0, 0.25}), 0.0, 0.0, 1e-15);
    CHECK_NEAR(qsig::norm(elementAnchor(a, 1) - Vec3{0.0, 0.0, 0.0}), 0.0, 0.0, 1e-15);
    CHECK_NEAR(qsig::norm(elementAnchor(a, 2) - Vec3{0.0, 0.0, 0.5}), 0.0, 0.0, 1e-15);

    // Parallel: b turns about its centre, without being turned round; its length is kept.
    const Vec3 centre = b.position;
    const double length = b.length;
    b.constraints = {constraint(CType::Parallel, 1)};
    CHECK(constraintError(scene, b, b.constraints[0]) > 0.5);
    CHECK(applyConstraints(scene) == 0);
    CHECK_NEAR(angleBetween(a, b), 0.0, 0.0, 1e-6);
    CHECK_NEAR(qsig::norm(b.position - centre), 0.0, 0.0, 1e-15);
    CHECK_NEAR(b.length, length, 0.0, 1e-15);
    // Perpendicular, then 60 degrees.
    b.constraints = {constraint(CType::Perpendicular, 1)};
    CHECK(applyConstraints(scene) == 0);
    CHECK_NEAR(angleBetween(a, b), 90.0, 0.0, 1e-6);
    b.constraints = {constraint(CType::Angle, 1)};
    b.constraints[0].value = 60.0;
    CHECK(applyConstraints(scene) == 0);
    CHECK_NEAR(angleBetween(a, b), 60.0, 0.0, 1e-6);

    // Coincident with a direction: the first end of b on the second end of a, b at a right angle.
    // When a moves or turns, b follows.
    AntennaConstraint join = constraint(CType::Coincident, 1);
    join.anchor = 1;
    join.referenceAnchor = 2;
    b.constraints = {constraint(CType::Perpendicular, 1), join};
    CHECK(applyConstraints(scene) == 0);
    CHECK_NEAR(qsig::norm(elementAnchor(b, 1) - Vec3{0.0, 0.0, 0.5}), 0.0, 0.0, 1e-9);
    CHECK_NEAR(angleBetween(a, b), 90.0, 0.0, 1e-6);
    a.position += Vec3{0.1, -0.2, 0.05};
    qsig::app::rotateElement(a, a.position, {1.0, 0.0, 0.0}, 35.0);
    CHECK(applyConstraints(scene) == 0);
    CHECK_NEAR(qsig::norm(elementAnchor(b, 1) - elementAnchor(a, 2)), 0.0, 0.0, 1e-9);
    CHECK_NEAR(angleBetween(a, b), 90.0, 0.0, 1e-6);

    // Lengths: imposed, then that of the reference.
    b.constraints = {constraint(CType::Length, 0)};
    b.constraints[0].value = 0.123;
    CHECK(applyConstraints(scene) == 0);
    CHECK(b.length == 0.123);
    b.constraints = {constraint(CType::EqualLength, 1)};
    CHECK(applyConstraints(scene) == 0);
    CHECK(b.length == a.length);
    // A loop has no length: the constraint does nothing and is not counted as unmet.
    AntennaElement loop;
    loop.type = Type::CircularLoop;
    loop.id = 7;
    loop.constraints = {constraint(CType::EqualLength, 1)};
    scene.push_back(loop);
    CHECK(applyConstraints(scene) == 0);
    CHECK(scene.back() == loop);
    scene.pop_back();

    // Distance between centres, along the line that joins them.
    AntennaConstraint apart = constraint(CType::Distance, 1);
    apart.value = 0.3;
    scene[1].constraints = {apart};
    const Vec3 direction = qsig::normalized(scene[1].position - scene[0].position);
    CHECK(applyConstraints(scene) == 0);
    CHECK_NEAR(qsig::norm(scene[1].position - scene[0].position), 0.3, 0.0, 1e-9);
    CHECK_NEAR(qsig::norm(qsig::normalized(scene[1].position - scene[0].position) - direction), 0.0, 0.0, 1e-9);

    // Position fixed on x and z: y stays free.
    AntennaConstraint fix = constraint(CType::FixPosition, 0);
    fix.axes = 1 | 4;
    fix.fixed = {1.0, 2.0, 3.0};
    scene[1].constraints = {fix};
    scene[1].position = {0.4, 0.5, 0.6};
    CHECK(applyConstraints(scene) == 0);
    CHECK_NEAR(qsig::norm(scene[1].position - Vec3{1.0, 0.5, 3.0}), 0.0, 0.0, 0.0);

    // A disabled constraint is ignored. Two that contradict each other: reported, and the call ends.
    AntennaConstraint centred = constraint(CType::Coincident, 1);
    centred.enabled = false;
    fix.axes = 7;
    scene[1].constraints = {fix, centred};
    CHECK(applyConstraints(scene) == 0);
    CHECK_NEAR(qsig::norm(scene[1].position - Vec3{1.0, 2.0, 3.0}), 0.0, 0.0, 0.0);
    scene[1].constraints[1].enabled = true;
    CHECK(applyConstraints(scene) >= 1);

    // A chain listed against its order: c follows b, b follows a. One call settles it.
    std::vector<AntennaElement> chain{wire({0.9, 0.0, 0.0}, {0.9, 0.0, 0.4}), wire({0.5, 0.0, 0.0}, {0.5, 0.2, 0.3}),
                                      wire({0.0, 0.0, 0.0}, {0.3, 0.0, 0.0})};
    chain[0].id = 3;
    chain[1].id = 2;
    chain[2].id = 1;
    AntennaConstraint link = constraint(CType::Coincident, 2);
    link.anchor = 1;
    link.referenceAnchor = 2;
    chain[0].constraints = {link, constraint(CType::Parallel, 1)};
    link.reference = 1;
    chain[1].constraints = {link};
    CHECK(applyConstraints(chain) == 0);
    CHECK_NEAR(qsig::norm(elementAnchor(chain[1], 1) - Vec3{0.3, 0.0, 0.0}), 0.0, 0.0, 1e-9);
    CHECK_NEAR(qsig::norm(elementAnchor(chain[0], 1) - elementAnchor(chain[1], 2)), 0.0, 0.0, 1e-9);
    CHECK_NEAR(angleBetween(chain[0], chain[2]), 0.0, 0.0, 1e-6);

    // The reference is removed: its constraints go with it; those without a reference stay.
    chain[1].constraints.push_back(constraint(CType::Length, 0));
    chain[1].constraints.back().value = 0.2;
    chain.pop_back();
    qsig::app::pruneConstraints(chain);
    CHECK(chain[1].constraints.size() == 1);
    CHECK(chain[1].constraints[0].type == CType::Length);
    CHECK(chain[0].constraints.size() == 1); // its link to element 2 stays, its parallel to element 1 goes
    CHECK(chain[0].constraints[0].type == CType::Coincident);
}

TEST_CASE("antennas: surfaces - a monopole on its ground plane, a dipole and its reflector, the axial helix", "[antennas]")
{
    AntennaSettings settings;
    settings.frequency = kF;

    // Monopole of 0.24 lambda on a plate of one wavelength, fed at its foot: resonance, lobe lifted by
    // the finite plane, all the delivered power radiated.
    const auto monopoleScene = qsig::app::antennaPreset("monopole_plate", kF);
    REQUIRE(monopoleScene.size() == 2);
    CHECK(qsig::app::elementIsSurface(monopoleScene[0].type));
    const auto monopole = qsig::app::solveAntenna(monopoleScene, settings);
    REQUIRE(monopole.valid);
    REQUIRE(monopole.feeds.size() == 1);
    CHECK(monopole.feeds[0].element == 1);
    CHECK_NEAR(monopole.feeds[0].impedance.real(), 39.5, 0.0, 1.0);
    CHECK_NEAR(monopole.feeds[0].impedance.imag(), 1.2, 0.0, 1.5);
    CHECK(monopole.feeds[0].vswr < 1.35);
    CHECK_NEAR(monopole.directivityDbi, 3.87, 0.0, 0.15);
    CHECK_NEAR(monopole.mainThetaDeg, 44.5, 0.0, 3.0);
    CHECK_REL(monopole.radiatedPowerW, monopole.acceptedPowerW, 3e-3);
    CHECK(monopole.facets.size() == 200); // 10 x 10 cells
    CHECK(monopole.maxSurfaceCurrent > 0.0);
    for (const auto &facet : monopole.facets) CHECK(facet.element == 0);
    for (const auto &segment : monopole.segments) CHECK(segment.element == 1);
    CHECK(monopole.polarisationSense == 0); // linear
    // Around the foot, the current of the wire spreads over the plate: all of it crosses every circle
    // centred on the foot, |J| = I / (2 pi rho) (the junction current is part of what is shown).
    for (const double ring : {0.15, 0.25})
    {
        double sum = 0.0;
        int count = 0;
        for (const auto &facet : monopole.facets)
        {
            const Vec3 c = (facet.a + facet.b + facet.c) / 3.0;
            if (std::abs(std::hypot(c.x, c.y) - ring * kLambda) > 0.05 * kLambda) continue;
            sum += (facet.ja + facet.jb + facet.jc) / 3.0;
            ++count;
        }
        REQUIRE(count > 8);
        CHECK_REL(sum / count, std::abs(monopole.feeds[0].current) / (2.0 * qsig::constants::pi * ring * kLambda), 0.08);
    }

    // The same wire upside down in the list of its ends (fed at fraction 1, that end on the plate): the same antenna.
    auto reversed = monopoleScene;
    reversed[1].elevationDeg = -90.0;
    reversed[1].feedFraction = 1.0;
    const auto same = qsig::app::solveAntenna(reversed, settings);
    REQUIRE(same.valid);
    CHECK(std::abs(same.feeds[0].impedance - monopole.feeds[0].impedance) < 1e-6);

    // A finer mesh of the plate moves the impedance by less than an ohm.
    AntennaSettings fine = settings;
    fine.facetsPerWavelength = 14;
    const auto refined = qsig::app::solveAntenna(monopoleScene, fine);
    REQUIRE(refined.valid);
    CHECK(refined.facets.size() == 392);
    CHECK(std::abs(refined.feeds[0].impedance - monopole.feeds[0].impedance) < 1.0);

    // The foot lifted off the plate: the source is at a free end, and the result says so.
    auto lifted = monopoleScene;
    lifted[1].position.z += 0.01;
    const auto broken = qsig::app::solveAntenna(lifted, settings);
    CHECK(!broken.valid);
    CHECK(broken.error.find("bout libre") != std::string::npos);

    // Dipole at 0.2 lambda of a plate: the beam leaves the plate (+x), matched to 50 ohm.
    const auto reflector = qsig::app::solveAntenna(qsig::app::antennaPreset("dipole_reflector", kF), settings);
    REQUIRE(reflector.valid);
    CHECK(reflector.directivityDbi > 8.2);
    CHECK(reflector.directivityDbi < 9.0);
    CHECK(reflector.frontToBackDb > 15.0);
    CHECK(reflector.feeds[0].vswr < 1.4);
    CHECK_NEAR(reflector.mainThetaDeg, 90.0, 0.0, 1.0);
    CHECK(std::cos(reflector.mainPhiDeg * qsig::constants::pi / 180.0) > 0.99);
    CHECK_REL(reflector.radiatedPowerW, reflector.acceptedPowerW, 3e-3);

    // Kraus helix on a disc: beam along the axis, right-hand circular (a right-handed winding), about
    // 140 ohm for a circumference of one wavelength, some 10 dBi for 5 turns.
    const auto helixScene = qsig::app::antennaPreset("helix_axial", kF);
    REQUIRE(helixScene.size() == 2);
    const auto helix = qsig::app::solveAntenna(helixScene, settings);
    REQUIRE(helix.valid);
    CHECK(helix.mainThetaDeg < 3.0);
    CHECK_NEAR(helix.feeds[0].impedance.real(), 140.0, 0.0, 12.0);
    CHECK(helix.directivityDbi > 9.6);
    CHECK(helix.directivityDbi < 11.0);
    CHECK(helix.polarisationSense == 1);
    CHECK(helix.axialRatioDb < 1.5);
    CHECK_REL(helix.radiatedPowerW, helix.acceptedPowerW, 3e-3);
    CHECK(helix.facets.size() == 150);
    // Its mirror image winds the other way... the model has right-handed helices only: a mirror keeps
    // the sense, and the polarisation with it.
    auto mirrored = helixScene;
    for (auto &e : mirrored) qsig::app::mirrorElement(e, {0.0, 0.0, 0.0}, {1.0, 0.0, 0.0});
    AntennaSolveOptions coarse;
    coarse.gridStepDeg = 6.0;
    const auto image = qsig::app::solveAntenna(mirrored, settings, coarse);
    REQUIRE(image.valid);
    CHECK(image.polarisationSense == 1);
    CHECK_NEAR(image.feeds[0].impedance.real(), helix.feeds[0].impedance.real(), 0.0, 1.0);

    // Without its surfaces the scene goes to the wire solver, which refuses the source at a free end.
    const auto alone = qsig::app::solveAntenna({monopoleScene[1]}, settings);
    CHECK(!alone.valid);
    CHECK(alone.facets.empty());
}

TEST_CASE("antennas: surfaces in the designer - outline, landing points, rotation and mirror", "[antennas]")
{
    AntennaElement plate;
    plate.type = Type::Plate;
    plate.fed = 0;
    plate.position = {0.1, -0.2, 0.3};
    plate.width = 0.6;
    plate.length = 0.4;
    const auto shape = qsig::app::antennaShape(plate);
    CHECK(shape.surface);
    CHECK(shape.feeds.empty());
    REQUIRE(shape.polylines.size() == 1);
    REQUIRE(shape.polylines[0].size() == 5); // closed outline
    CHECK_REL(polylineLength(shape.polylines[0]), 2.0, 1e-12);
    for (const auto &corner : shape.polylines[0]) CHECK_NEAR(corner.z, 0.3, 0.0, 1e-12); // horizontal by default

    // Points on the plate, beside it and above it.
    const std::vector<AntennaElement> scene{plate};
    CHECK(qsig::app::surfaceAt(scene, {0.1, -0.2, 0.3}) == 0);
    CHECK(qsig::app::surfaceAt(scene, shape.polylines[0][0]) == 0); // a corner
    CHECK(qsig::app::surfaceAt(scene, {0.1, -0.2, 0.3001}) == -1);
    CHECK(qsig::app::surfaceAt(scene, {0.9, -0.2, 0.3}) == -1);
    const Vec3 above{0.2, -0.1, 0.9};
    const Vec3 landing = qsig::app::projectOnSurface(plate, above);
    CHECK(qsig::norm(landing - Vec3{0.2, -0.1, 0.3}) < 1e-12);
    CHECK(qsig::app::surfaceAt(scene, landing) == 0);
    // A surface offers no end to snap a wire to, and holds no length.
    CHECK(qsig::app::antennaAnchors(scene).empty());
    CHECK(!qsig::app::elementHasLength(Type::Plate));
    CHECK(qsig::norm(qsig::app::elementAnchor(plate, 1) - plate.position) < 1e-12);
    CHECK(qsig::norm(qsig::app::elementsCentre(scene, {0}) - plate.position) < 1e-12);

    // A disc: its rim, and the same tests of position.
    AntennaElement disc;
    disc.type = Type::Disc;
    disc.fed = 0;
    disc.radius = 0.25;
    disc.elevationDeg = 0.0; // normal along x: the disc stands in the yz plane
    const auto rim = qsig::app::antennaShape(disc);
    REQUIRE(rim.polylines.size() == 1);
    for (const auto &q : rim.polylines[0])
    {
        CHECK_NEAR(q.x, 0.0, 0.0, 1e-12);
        CHECK_REL(std::hypot(q.y, q.z), 0.25, 1e-12);
    }
    CHECK(qsig::app::surfaceAt({disc}, {0.0, 0.1, 0.2}) == 0);
    CHECK(qsig::app::surfaceAt({disc}, {0.0, 0.2, 0.2}) == -1);

    // Rotation and mirror carry the corners of the plate (as a set: a mirror runs the outline backwards).
    auto sameCorners = [](const AntennaElement &element, const std::vector<Vec3> &expected)
    {
        const auto outline = qsig::app::antennaShape(element).polylines[0];
        for (const Vec3 &corner : expected)
            if (std::none_of(outline.begin(), outline.end(), [&](const Vec3 &q) { return qsig::norm(q - corner) < 1e-9; })) return false;
        return true;
    };
    AntennaElement turned = plate;
    turned.rollDeg = 20.0;
    const auto before = qsig::app::antennaShape(turned).polylines[0];
    qsig::app::rotateElement(turned, {0.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, 90.0); // about y: x -> -z, z -> x
    std::vector<Vec3> expected;
    for (const Vec3 &q : before) expected.push_back({q.z, q.y, -q.x});
    CHECK(sameCorners(turned, expected));
    AntennaElement image = plate;
    image.rollDeg = 20.0;
    image.elevationDeg = 40.0;
    const auto original = qsig::app::antennaShape(image).polylines[0];
    qsig::app::mirrorElement(image, {0.5, 0.0, 0.0}, {1.0, 0.0, 0.0});
    expected.clear();
    for (const Vec3 &q : original) expected.push_back({1.0 - q.x, q.y, q.z});
    CHECK(sameCorners(image, expected));

    // A wire fed at its foot stays fed at its foot when it is cut; the other part is bare.
    AntennaElement wire;
    wire.type = Type::Wire;
    wire.length = 0.4;
    wire.feedFraction = 0.0;
    AntennaElement first, second;
    REQUIRE(qsig::app::splitWire(wire, 0.5, first, second));
    CHECK(first.fed == 1);
    CHECK(first.feedFraction == 0.0);
    CHECK(second.fed == 0);
    REQUIRE(qsig::app::antennaShape(first).feeds.size() == 1);
    CHECK(qsig::norm(qsig::app::antennaShape(first).feeds[0] - qsig::app::antennaShape(wire).polylines[0].front()) < 1e-12);
    // A helix: fed at its middle by default, at its foot with the fraction 0.
    AntennaElement helix;
    helix.type = Type::Helix;
    const auto coil = qsig::app::antennaShape(helix);
    const std::size_t middle = (coil.polylines[0].size() - 1) / 2;
    CHECK(qsig::norm(coil.feeds[0] - coil.polylines[0][middle]) < 1e-12);
    helix.feedFraction = 0.0;
    CHECK(qsig::norm(qsig::app::antennaShape(helix).feeds[0] - coil.polylines[0].front()) < 1e-12);
}

TEST_CASE("antennas: plates that touch are joined - a plate in pieces, a corner reflector against its images", "[antennas]")
{
    AntennaSettings settings;
    settings.frequency = kF;
    auto plate = [](double x, double y, double z, double width, double length, double azimuthDeg)
    {
        AntennaElement e;
        e.type = Type::Plate;
        e.fed = 0;
        e.position = {x * kLambda, y * kLambda, z * kLambda};
        e.width = width * kLambda;
        e.length = length * kLambda;
        e.azimuthDeg = azimuthDeg;
        e.elevationDeg = 0.0;
        return e;
    };
    auto dipole = [](double x, double y, double length, double phaseDeg)
    {
        AntennaElement e;
        e.type = Type::Dipole;
        e.position = {x * kLambda, y * kLambda, 0.0};
        e.length = length * kLambda;
        e.wireRadius = 1e-3 * kLambda;
        e.phaseDeg = phaseDeg;
        return e;
    };

    // A reflector of one wavelength in one piece, then in three pieces whose sides do not cut their
    // common lines alike (0.45 and 0.55 lambda under 1 lambda): joined, they are the same reflector.
    // One piece alone is another antenna.
    const auto whole = qsig::app::solveAntenna({plate(0, 0, 0, 1.0, 1.0, 0), dipole(0.2, 0, 0.456, 0)}, settings);
    const auto pieces = qsig::app::solveAntenna({plate(0, 0, 0.25, 1.0, 0.5, 0), plate(0, -0.275, -0.25, 0.45, 0.5, 0),
                                                 plate(0, 0.225, -0.25, 0.55, 0.5, 0), dipole(0.2, 0, 0.456, 0)},
                                                settings);
    const auto half = qsig::app::solveAntenna({plate(0, 0, 0.25, 1.0, 0.5, 0), dipole(0.2, 0, 0.456, 0)}, settings);
    REQUIRE(whole.valid);
    REQUIRE(pieces.valid);
    REQUIRE(half.valid);
    CHECK(std::abs(pieces.feeds[0].impedance - whole.feeds[0].impedance) < 0.3);
    CHECK_NEAR(pieces.directivityDbi, whole.directivityDbi, 0.0, 0.05);
    CHECK_NEAR(pieces.frontToBackDb, whole.frontToBackDb, 0.0, 0.3);
    CHECK(std::abs(half.feeds[0].impedance - whole.feeds[0].impedance) > 10.0);

    // Corner reflector of the examples: two plates at 90 degrees joined along z, a dipole at 0.5 lambda of
    // the apex. With infinite sides it is the dipole and its three images (currents + - + -): same
    // impedance, four times the directivity of the four dipoles.
    const auto cornerScene = qsig::app::antennaPreset("corner_reflector", kF);
    REQUIRE(cornerScene.size() == 3);
    const auto corner = qsig::app::solveAntenna(cornerScene, settings);
    const double length = cornerScene[2].length / kLambda;
    const auto images = qsig::app::solveAntenna({dipole(0.5, 0, length, 0), dipole(0, 0.5, length, 180), dipole(-0.5, 0, length, 0),
                                                 dipole(0, -0.5, length, 180)},
                                                settings);
    REQUIRE(corner.valid);
    REQUIRE(images.valid);
    REQUIRE(corner.feeds.size() == 1);
    CHECK(std::abs(corner.feeds[0].impedance - images.feeds[0].impedance) < 8.0);
    CHECK_NEAR(corner.feeds[0].impedance.real(), 127.4, 0.0, 2.0);
    CHECK_NEAR(corner.feeds[0].impedance.imag(), -2.8, 0.0, 2.5);
    CHECK_NEAR(corner.directivityDbi, 10.56, 0.0, 0.2);
    CHECK(corner.directivityDbi > images.directivityDbi + 6.02 - 1.6); // finite sides: within 1.6 dB of the infinite corner
    CHECK(corner.frontToBackDb > 18.0);
    CHECK_NEAR(corner.mainPhiDeg, 0.0, 0.0, 2.0);
    CHECK_NEAR(corner.radiatedPowerW / corner.acceptedPowerW, 1.0, 2e-3, 0.0);

    // Taller plates and longer sides bring the impedance to that of the images within an ohm or two.
    auto larger = cornerScene;
    const double offset = 0.75 * kLambda * std::sqrt(0.5);
    for (int k = 0; k < 2; ++k)
    {
        larger[static_cast<std::size_t>(k)].width = 1.5 * kLambda;
        larger[static_cast<std::size_t>(k)].length = 1.5 * kLambda;
        larger[static_cast<std::size_t>(k)].position = {offset, (k == 0 ? 1.0 : -1.0) * offset, 0.0};
    }
    const auto large = qsig::app::solveAntenna(larger, settings);
    REQUIRE(large.valid);
    CHECK(std::abs(large.feeds[0].impedance - images.feeds[0].impedance) < 2.5);
    CHECK(large.directivityDbi > corner.directivityDbi + 1.5);
}

TEST_CASE("antennas: air patch - a plate close above its ground plate, fed by a probe", "[antennas]")
{
    AntennaSettings settings;
    settings.frequency = kF;
    // The gap (0.03 lambda) is three times smaller than the mesh: the solver must hold all the same.
    const auto scene = qsig::app::antennaPreset("patch_air", kF);
    REQUIRE(scene.size() == 3);
    const auto patch = qsig::app::solveAntenna(scene, settings);
    REQUIRE(patch.valid);
    REQUIRE(patch.feeds.size() == 1);
    CHECK(patch.feeds[0].element == 2);
    CHECK_NEAR(patch.feeds[0].impedance.real(), 50.0, 0.0, 3.0);
    CHECK_NEAR(patch.feeds[0].impedance.imag(), -1.2, 0.0, 3.0);
    CHECK(patch.feeds[0].vswr < 1.15);
    // Broadside beam of an air patch on a ground plate of one wavelength, all the delivered power radiated.
    CHECK_NEAR(patch.directivityDbi, 9.76, 0.0, 0.2);
    CHECK(patch.mainThetaDeg < 6.0);
    CHECK_NEAR(patch.radiatedPowerW / patch.acceptedPowerW, 1.0, 2e-3, 0.0);

    // A finer mesh moves the impedance by a few ohms only.
    AntennaSettings fine = settings;
    fine.facetsPerWavelength = 14;
    const auto finer = qsig::app::solveAntenna(scene, fine);
    REQUIRE(finer.valid);
    CHECK(std::abs(finer.feeds[0].impedance - patch.feeds[0].impedance) < 6.0);

    // The half-wave resonance of the plate: R is greatest for a length near 0.445 lambda (fringing fields
    // lengthen the plate), where the probe adds its inductance, about +50 ohm for this probe.
    auto withLength = [&](double length)
    {
        auto changed = scene;
        changed[1].length = length * kLambda;
        return qsig::app::solveAntenna(changed, settings);
    };
    const auto shorter = withLength(0.42), resonant = withLength(0.445), longer = withLength(0.48);
    REQUIRE(shorter.valid);
    REQUIRE(resonant.valid);
    REQUIRE(longer.valid);
    CHECK(resonant.feeds[0].impedance.real() > 2.5 * shorter.feeds[0].impedance.real());
    CHECK(resonant.feeds[0].impedance.real() > 2.5 * longer.feeds[0].impedance.real());
    CHECK(resonant.feeds[0].impedance.real() > 95.0);
    CHECK(resonant.feeds[0].impedance.real() < 135.0);
    CHECK_NEAR(resonant.feeds[0].impedance.imag(), 50.0, 0.0, 25.0);
}

TEST_CASE("antennas: sources are entities on their wire - several on one wire, added, removed, cut with it", "[antennas]")
{
    AntennaSettings settings;
    settings.frequency = kF;
    auto wire = [](double x, double length)
    {
        AntennaElement e;
        e.name = "Fil";
        e.type = Type::Wire;
        e.position = {x * kLambda, 0.0, 0.0};
        e.length = length * kLambda;
        e.wireRadius = 1e-3 * kLambda;
        e.azimuthDeg = 0.0;
        e.elevationDeg = 0.0; // along x
        e.fed = 0;
        return e;
    };
    using qsig::app::AntennaFeed;

    // One wire of one wavelength with two sources, at a quarter and at three quarters: the same antenna as
    // two half-wave wires end to end (joined where they touch), each with its source in the middle.
    AntennaElement both = wire(0.0, 1.0);
    CHECK(qsig::app::feedCount(both) == 0);
    CHECK(qsig::app::canAddFeed(both));
    AntennaFeed left, right;
    left.fraction = 0.25;
    right.fraction = 0.75;
    right.name = "Seconde";
    CHECK(qsig::app::addFeed(both, left) == 0);
    CHECK(qsig::app::addFeed(both, right) == 1);
    CHECK(qsig::app::feedCount(both) == 2);
    CHECK(qsig::app::feedAt(both, 1).name == "Seconde");
    CHECK(qsig::app::antennaShape(both).feeds.size() == 2);
    AntennaElement a = wire(-0.25, 0.5), b = wire(0.25, 0.5);
    a.fed = b.fed = 1;
    const auto one = qsig::app::solveAntenna({both}, settings);
    const auto two = qsig::app::solveAntenna({a, b}, settings);
    REQUIRE(one.valid);
    REQUIRE(two.valid);
    REQUIRE(one.feeds.size() == 2);
    REQUIRE(two.feeds.size() == 2);
    CHECK(one.feeds[0].element == 0);
    CHECK(one.feeds[1].element == 0);
    CHECK(one.feeds[0].feedIndex == 0);
    CHECK(one.feeds[1].feedIndex == 1);
    CHECK(one.feeds[1].name == "Seconde");
    for (std::size_t k = 0; k < 2; ++k) CHECK(std::abs(one.feeds[k].impedance - two.feeds[k].impedance) < 1e-6);
    CHECK_NEAR(one.directivityDbi, two.directivityDbi, 0.0, 1e-6);
    CHECK_NEAR(one.acceptedPowerW, one.feeds[0].powerW + one.feeds[1].powerW, 1e-12, 0.0);
    // By symmetry the two sources see the same impedance.
    CHECK(std::abs(one.feeds[0].impedance - one.feeds[1].impedance) < 1e-6);

    // Reciprocity between the two sources of the wire: each one driven alone (the other at 0 V is a plain
    // piece of wire), the current through the other is the same.
    AntennaElement uneven = wire(0.0, 0.9);
    AntennaFeed first, second;
    first.fraction = 0.2;
    second.fraction = 0.65;
    qsig::app::addFeed(uneven, first);
    qsig::app::addFeed(uneven, second);
    AntennaElement drive1 = uneven, drive2 = uneven;
    second.strength = 0.0;
    qsig::app::setFeedAt(drive1, 1, second);
    first.strength = 0.0;
    qsig::app::setFeedAt(drive2, 0, first);
    const auto r1 = qsig::app::solveAntenna({drive1}, settings), r2 = qsig::app::solveAntenna({drive2}, settings);
    REQUIRE(r1.valid);
    REQUIRE(r2.valid);
    CHECK(std::abs(r1.feeds[1].current - r2.feeds[0].current) < 1e-9 * std::abs(r1.feeds[0].current));
    CHECK(std::abs(r1.feeds[1].current) > 1e-3 * std::abs(r1.feeds[0].current));

    // Two sources at the same place are refused, with the name of the wire.
    AntennaElement clash = wire(0.0, 0.5);
    qsig::app::addFeed(clash, AntennaFeed{});
    qsig::app::addFeed(clash, AntennaFeed{});
    const auto refused = qsig::app::solveAntenna({clash}, settings);
    CHECK(!refused.valid);
    CHECK(refused.error.find("Fil") != std::string::npos);

    // Removing the first source: the next one takes its place; removing the last one: a passive wire.
    AntennaElement edited = both;
    CHECK(qsig::app::removeFeed(edited, 0));
    CHECK(qsig::app::feedCount(edited) == 1);
    CHECK_NEAR(qsig::app::feedAt(edited, 0).fraction, 0.75, 0.0, 1e-12);
    CHECK(qsig::app::removeFeed(edited, 0));
    CHECK(qsig::app::feedCount(edited) == 0);
    CHECK(edited.fed == 0);
    CHECK(!qsig::app::removeFeed(edited, 0));

    // Cut in the middle, each part keeps the source it holds, at the same place in space.
    AntennaElement head, tail;
    REQUIRE(qsig::app::splitWire(both, 0.5, head, tail));
    CHECK(qsig::app::feedCount(head) == 1);
    CHECK(qsig::app::feedCount(tail) == 1);
    CHECK_NEAR(qsig::app::feedAt(head, 0).fraction, 0.5, 0.0, 1e-12);
    CHECK_NEAR(qsig::app::feedAt(tail, 0).fraction, 0.5, 0.0, 1e-12);
    const auto cut = qsig::app::solveAntenna({head, tail}, settings);
    REQUIRE(cut.valid);
    REQUIRE(cut.feeds.size() == 2);
    CHECK(std::abs(cut.feeds[0].impedance - one.feeds[0].impedance) < 1e-6);

    // The other wire elements take one source, at the place their type gives it; a surface takes none.
    AntennaElement dipole;
    dipole.type = Type::Dipole;
    CHECK(qsig::app::feedCount(dipole) == 1);
    CHECK(!qsig::app::canAddFeed(dipole));
    CHECK(qsig::app::removeFeed(dipole, 0));
    CHECK(qsig::app::canAddFeed(dipole));
    AntennaElement plate;
    plate.type = Type::Plate;
    CHECK(qsig::app::feedCount(plate) == 0);
    CHECK(!qsig::app::canAddFeed(plate));
}

TEST_CASE("antennas: a closed box of six joined plates - the meshes meet, cut no finer than needed", "[antennas]")
{
    // First step towards the solids: a box of 0.4 x 0.4 x 0.2 wavelength made of six plates joined along
    // their sides, a monopole of 0.24 wavelength standing in the middle of its top, fed at its foot.
    auto plate = [](double x, double y, double z, double width, double length, double azimuthDeg, double elevationDeg)
    {
        AntennaElement e;
        e.type = Type::Plate;
        e.fed = 0;
        e.position = {x * kLambda, y * kLambda, z * kLambda};
        e.width = width * kLambda;
        e.length = length * kLambda;
        e.azimuthDeg = azimuthDeg;
        e.elevationDeg = elevationDeg;
        return e;
    };
    const double a = 0.4, b = 0.4, h = 0.2;
    AntennaElement monopole;
    monopole.type = Type::Wire;
    monopole.length = 0.24 * kLambda;
    monopole.position = {0.0, 0.0, (0.5 * h + 0.12) * kLambda};
    monopole.wireRadius = 1e-3 * kLambda;
    monopole.feedFraction = 0.0;
    const std::vector<AntennaElement> top{plate(0, 0, 0.5 * h, b, a, 0, 90), monopole};
    const std::vector<AntennaElement> box{plate(0, 0, 0.5 * h, b, a, 0, 90),  plate(0, 0, -0.5 * h, b, a, 0, 90),
                                          plate(0.5 * a, 0, 0, b, h, 0, 0),   plate(-0.5 * a, 0, 0, b, h, 0, 0),
                                          plate(0, 0.5 * b, 0, a, h, 90, 0),  plate(0, -0.5 * b, 0, a, h, 90, 0),
                                          monopole};
    AntennaSettings coarse, fine;
    coarse.frequency = fine.frequency = kF;
    coarse.facetsPerWavelength = 10.0;
    fine.facetsPerWavelength = 16.0;
    const auto alone = qsig::app::solveAntenna(top, coarse);
    const auto box10 = qsig::app::solveAntenna(box, coarse);
    const auto box16 = qsig::app::solveAntenna(box, fine);
    REQUIRE(alone.valid);
    REQUIRE(box10.valid);
    REQUIRE(box16.valid);
    // The line that the foot of the monopole draws across the top goes round the box, and nothing more:
    // 8 x 8 cells on the top and the bottom, 8 x 4 on each side (512 triangles, 768 edges). Exchanging
    // every vertex between neighbours gave 1872 unknowns here.
    CHECK(box16.facets.size() == 512);
    CHECK(box16.unknowns < 900);
    // The same antenna at both densities, and not the monopole on its top plate alone.
    CHECK(std::abs(box16.feeds[0].impedance - box10.feeds[0].impedance) < 0.5);
    CHECK_NEAR(box16.directivityDbi, box10.directivityDbi, 0.0, 0.1);
    CHECK(std::abs(box10.feeds[0].impedance - alone.feeds[0].impedance) > 5.0);
    // The currents close round the edges of the box: the power fed is the power radiated.
    CHECK_NEAR(box10.radiatedPowerW / box10.acceptedPowerW, 1.0, 0.0, 2e-3);
    CHECK_NEAR(box16.radiatedPowerW / box16.acceptedPowerW, 1.0, 0.0, 2e-3);
}

TEST_CASE("antennas: conducting solids - a box is its six plates, a tube is a wire, a wire lands on any face", "[antennas]")
{
    AntennaSettings coarse, fine;
    coarse.frequency = fine.frequency = kF;
    coarse.facetsPerWavelength = 10.0;
    fine.facetsPerWavelength = 16.0;
    auto solid = [](Type type, double radius, double height)
    {
        AntennaElement e;
        e.type = type;
        e.fed = 0;
        e.radius = radius * kLambda;
        e.height = height * kLambda;
        e.width = e.length = 0.4 * kLambda;
        return e;
    };
    auto monopole = [](double x, double y, double z, double azimuthDeg, double elevationDeg)
    {
        // 0.24 lambda from the point (x, y, z), where its foot is fed, along the direction given.
        AntennaElement e;
        e.type = Type::Wire;
        e.length = 0.24 * kLambda;
        e.wireRadius = 1e-3 * kLambda;
        e.azimuthDeg = azimuthDeg;
        e.elevationDeg = elevationDeg;
        e.feedFraction = 0.0;
        e.position = Vec3{x, y, z} * kLambda + qsig::app::antennaAxis(e) * (0.5 * e.length);
        return e;
    };
    auto balanced = [](const qsig::app::AntennaResult &r) { return std::abs(r.radiatedPowerW / r.acceptedPowerW - 1.0) < 2e-3; };
    auto apart = [](const qsig::app::AntennaResult &a, const qsig::app::AntennaResult &b) { return std::abs(a.feeds[0].impedance - b.feeds[0].impedance); };

    // What a solid is: a surface for everything said of surfaces, and a closed skin.
    for (const Type type : {Type::Box, Type::Cylinder, Type::Cone, Type::Sphere})
    {
        CHECK(qsig::app::elementIsSolid(type));
        CHECK(qsig::app::elementIsSurface(type));
        const AntennaElement body = solid(type, 0.2, 0.2);
        CHECK(qsig::app::feedCount(body) == 0);
        CHECK(!qsig::app::canAddFeed(body));
        const auto shape = qsig::app::antennaShape(body);
        CHECK(shape.surface);
        CHECK(shape.feeds.empty());
        CHECK(shape.faces.size() == (type == Type::Box ? 6u : type == Type::Cylinder ? 18u : type == Type::Cone ? 17u : 96u));
        // Any point is brought to the skin: from outside, from inside, from the centre itself.
        for (const Vec3 &from : {Vec3{0.7, 0.3, 0.5}, Vec3{0.03, -0.02, 0.05}, Vec3{0.0, 0.0, 0.0}, Vec3{0.0, 0.0, -0.9}})
        {
            const Vec3 landing = qsig::app::projectOnSurface(body, from * kLambda);
            CHECK(qsig::app::surfaceAt({body}, landing) == 0);
        }
        CHECK(qsig::app::surfaceAt({body}, Vec3{0.0, 0.0, 0.0}) == -1);
    }
    CHECK(!qsig::app::elementIsSolid(Type::Plate));

    // A box is its six faces joined: the element and six plates give the same antenna (monopole in the
    // middle of the top of a box of 0.4 x 0.4 x 0.2 lambda).
    auto plate = [](double x, double y, double z, double width, double length, double azimuthDeg, double elevationDeg)
    {
        AntennaElement e;
        e.type = Type::Plate;
        e.fed = 0;
        e.position = {x * kLambda, y * kLambda, z * kLambda};
        e.width = width * kLambda;
        e.length = length * kLambda;
        e.azimuthDeg = azimuthDeg;
        e.elevationDeg = elevationDeg;
        return e;
    };
    const AntennaElement box = solid(Type::Box, 0.2, 0.2);
    const auto onBox = qsig::app::solveAntenna({box, monopole(0, 0, 0.1, 0, 90)}, coarse);
    const auto onPlates = qsig::app::solveAntenna({plate(0, 0, 0.1, 0.4, 0.4, 0, 90), plate(0, 0, -0.1, 0.4, 0.4, 0, 90), plate(0.2, 0, 0, 0.4, 0.2, 0, 0),
                                                   plate(-0.2, 0, 0, 0.4, 0.2, 0, 0), plate(0, 0.2, 0, 0.4, 0.2, 90, 0), plate(0, -0.2, 0, 0.4, 0.2, 90, 0),
                                                   monopole(0, 0, 0.1, 0, 90)},
                                                  coarse);
    REQUIRE(onBox.valid);
    REQUIRE(onPlates.valid);
    CHECK(onBox.facets.size() == onPlates.facets.size());
    CHECK(apart(onBox, onPlates) < 0.05);
    CHECK(balanced(onBox));
    for (const auto &facet : onBox.facets) CHECK(facet.element == 0);
    // The wire lands anywhere on a face, and on any face: off the centre of the top, on a side. The
    // currents close (power balance) and the two mesh densities give the same antenna.
    for (const AntennaElement &wire : {monopole(0.1, 0.05, 0.1, 0, 90), monopole(0.2, 0.03, 0.02, 0, 0)})
    {
        const auto r10 = qsig::app::solveAntenna({box, wire}, coarse), r16 = qsig::app::solveAntenna({box, wire}, fine);
        REQUIRE(r10.valid);
        REQUIRE(r16.valid);
        CHECK(balanced(r10));
        CHECK(balanced(r16));
        CHECK(apart(r10, r16) < 0.6);
        CHECK(apart(r10, onBox) > 3.0); // not the monopole in the middle of the top
    }

    // Conductors that lie on each other in one plane are there once. Two plates overlapping by 0.2 lambda
    // are the plate of their union (a reflector of one wavelength behind a dipole); a box standing on a
    // ground plate is the same antenna whether the plate or the box comes first.
    AntennaElement driven;
    driven.type = Type::Dipole;
    driven.position = {0.2 * kLambda, 0.0, 0.0};
    driven.length = 0.456 * kLambda;
    driven.wireRadius = 1e-3 * kLambda;
    const auto whole = qsig::app::solveAntenna({plate(0, 0, 0, 1.0, 1.0, 0, 0), driven}, coarse);
    const auto overlapping = qsig::app::solveAntenna({plate(0, -0.2, 0, 0.6, 1.0, 0, 0), plate(0, 0.2, 0, 0.6, 1.0, 0, 0), driven}, coarse);
    REQUIRE(whole.valid);
    REQUIRE(overlapping.valid);
    CHECK(overlapping.facets.size() == whole.facets.size());
    CHECK(apart(overlapping, whole) < 0.05);
    CHECK_NEAR(overlapping.frontToBackDb, whole.frontToBackDb, 0.0, 0.05);
    AntennaElement standing = box;
    standing.position = {0.0, 0.0, 0.1 * kLambda};
    const AntennaElement ground = plate(0, 0, 0, 1.0, 1.0, 0, 90), onTop = monopole(0, 0, 0.2, 0, 90);
    const auto plateFirst = qsig::app::solveAntenna({ground, standing, onTop}, coarse);
    const auto boxFirst = qsig::app::solveAntenna({standing, ground, onTop}, coarse);
    REQUIRE(plateFirst.valid);
    REQUIRE(boxFirst.valid);
    CHECK(plateFirst.facets.size() == boxFirst.facets.size());
    CHECK(apart(plateFirst, boxFirst) < 0.1);
    CHECK(balanced(plateFirst));
    CHECK(apart(plateFirst, onBox) > 10.0); // the ground plate under the box changes the antenna

    // A closed tube is a wire: a dipole with a parasitic rod of 0.005 lambda of radius at 0.15 lambda,
    // the rod as a wire (thin-wire kernel), then as a closed cylinder (triangles): the same dipole
    // impedance, far from that of the dipole alone.
    AntennaSettings twelve = coarse;
    twelve.facetsPerWavelength = 12.0;
    AntennaElement dipole;
    dipole.type = Type::Dipole;
    dipole.length = 0.47 * kLambda;
    dipole.wireRadius = 1e-3 * kLambda;
    AntennaElement rod;
    rod.type = Type::Wire;
    rod.fed = 0;
    rod.length = 0.5 * kLambda;
    rod.wireRadius = 0.005 * kLambda;
    rod.position = {0.15 * kLambda, 0.0, 0.0};
    AntennaElement tube = solid(Type::Cylinder, 0.005, 0.5);
    tube.position = rod.position;
    const auto alone = qsig::app::solveAntenna({dipole}, twelve);
    const auto withRod = qsig::app::solveAntenna({dipole, rod}, twelve);
    const auto withTube = qsig::app::solveAntenna({dipole, tube}, twelve);
    REQUIRE(alone.valid);
    REQUIRE(withRod.valid);
    REQUIRE(withTube.valid);
    CHECK(apart(withTube, withRod) < 0.3);
    CHECK_NEAR(withTube.directivityDbi, withRod.directivityDbi, 0.0, 0.05);
    CHECK(apart(withRod, alone) > 20.0);
    CHECK(balanced(withTube));

    // Curved solids: the monopole on a sphere, on the end and on the wall of a cylinder, on the base of
    // a cone. The wire is attached (a vertex of the skin is under its foot), the power balance holds,
    // the two densities agree: within 0.8 ohm, 3.5 ohm for the sphere whose facets shrink it.
    AntennaElement cone = solid(Type::Cone, 0.2, 0.3);
    cone.elevationDeg = -90.0; // apex down, its base on top
    struct Case
    {
        AntennaElement body, wire;
        double tolerance;
    };
    const std::vector<Case> cases{{solid(Type::Sphere, 0.2, 0.2), monopole(0, 0, 0.2, 0, 90), 3.5},
                                  {solid(Type::Cylinder, 0.2, 0.2), monopole(0, 0, 0.1, 0, 90), 0.8},
                                  {solid(Type::Cylinder, 0.2, 0.2), monopole(0.2, 0, 0.03, 0, 0), 1.6},
                                  {cone, monopole(0, 0, 0.15, 0, 90), 0.8}};
    for (const Case &c : cases)
    {
        Vec3 foot, tip;
        qsig::app::wireEndpoints(c.wire, foot, tip);
        CHECK(qsig::app::surfaceAt({c.body, c.wire}, foot) == 0);
        const auto r10 = qsig::app::solveAntenna({c.body, c.wire}, coarse), r16 = qsig::app::solveAntenna({c.body, c.wire}, fine);
        REQUIRE(r10.valid);
        REQUIRE(r16.valid);
        CHECK(balanced(r10));
        CHECK(balanced(r16));
        CHECK(apart(r10, r16) < c.tolerance);
        CHECK(r16.facets.size() > r10.facets.size());
        // Attached: without the solid the foot is in the air, and a source at a free end is refused.
        CHECK(!qsig::app::solveAntenna({c.wire}, coarse).valid);
    }

    // The two examples.
    for (const std::string key : {"monopole_box", "monopole_sphere"})
    {
        const auto elements = qsig::app::antennaPreset(key, kF);
        REQUIRE(elements.size() == 2);
        CHECK(qsig::app::elementIsSolid(elements[0].type));
        AntennaSettings settings;
        settings.frequency = kF;
        const auto r = qsig::app::solveAntenna(elements, settings);
        REQUIRE(r.valid);
        CHECK(balanced(r));
        CHECK(std::abs(r.feeds[0].impedance - (key == "monopole_box" ? std::complex<double>(27.27, -4.30) : std::complex<double>(51.01, -9.14))) < 0.3);
    }
}

TEST_CASE("antennas: dielectric solids - the field goes through them, the power balance holds, contacts are refused", "[antennas]")
{
    AntennaSettings settings;
    settings.frequency = kF;
    auto solid = [](Type type, double x, double permittivity, double lossTangent)
    {
        // Permittivity 0: a perfect conductor.
        AntennaElement e;
        e.name = "Bloc";
        e.type = type;
        e.fed = 0;
        e.position = {x * kLambda, 0.0, 0.0};
        e.radius = 0.16 * kLambda;
        e.width = e.length = e.height = 0.3 * kLambda;
        e.medium = permittivity > 0.0 ? 1 : 0;
        e.permittivity = permittivity;
        e.lossTangent = lossTangent;
        return e;
    };
    AntennaElement dipole;
    dipole.name = "Dipôle";
    dipole.type = Type::Dipole;
    dipole.length = 0.47 * kLambda;
    dipole.wireRadius = 1e-3 * kLambda;
    auto balance = [](const qsig::app::AntennaResult &r) { return r.radiatedPowerW / r.acceptedPowerW; };
    auto apart = [](const qsig::app::AntennaResult &a, const qsig::app::AntennaResult &b) { return std::abs(a.feeds[0].impedance - b.feeds[0].impedance); };

    // What a dielectric solid is: a solid whose medium is not the perfect conductor. A wire is not
    // attached to it (no source at a foot standing on it).
    const AntennaElement sphere = solid(Type::Sphere, 0.3, 4.0, 0.0);
    CHECK(qsig::app::elementIsDielectric(sphere));
    CHECK(!qsig::app::elementIsDielectric(solid(Type::Sphere, 0.3, 0.0, 0.0)));
    AntennaElement plateOfDielectric = sphere;
    plateOfDielectric.type = Type::Plate; // only a solid has an inside
    CHECK(!qsig::app::elementIsDielectric(plateOfDielectric));
    CHECK(qsig::app::surfaceAt({sphere}, sphere.position + Vec3{0.16 * kLambda, 0.0, 0.0}) == -1);
    CHECK(qsig::app::surfaceAt({solid(Type::Sphere, 0.3, 0.0, 0.0)}, sphere.position + Vec3{0.16 * kLambda, 0.0, 0.0}) == 0);

    // A dipole at 0.3 lambda of a sphere of permittivity 4: its skin carries electric and magnetic
    // currents (three unknowns per edge counted with the wire), the dipole sees it, all the power fed is
    // radiated, and it is not the conducting sphere.
    const auto alone = qsig::app::solveAntenna({dipole}, settings);
    const auto beside = qsig::app::solveAntenna({dipole, sphere}, settings);
    const auto metal = qsig::app::solveAntenna({dipole, solid(Type::Sphere, 0.3, 0.0, 0.0)}, settings);
    REQUIRE(alone.valid);
    REQUIRE(beside.valid);
    REQUIRE(metal.valid);
    CHECK(std::abs(beside.feeds[0].impedance - std::complex<double>(65.05, -5.46)) < 0.3);
    CHECK_NEAR(balance(beside), 1.0, 0.0, 2e-3);
    CHECK(apart(beside, alone) > 3.0);
    CHECK(apart(beside, metal) > 10.0);
    CHECK(beside.directivityDbi > alone.directivityDbi + 1.0); // it draws the wave to its side
    CHECK(beside.unknowns > 4 * metal.unknowns);               // finer triangles (the wave is shorter inside), twice the currents

    // A box of the permittivity of air is not there; with permittivity 4 and tan delta 0.1 it takes
    // some of the power (about 6 %).
    const auto transparent = qsig::app::solveAntenna({dipole, solid(Type::Box, 0.3, 1.0001, 0.0)}, settings);
    REQUIRE(transparent.valid);
    CHECK(apart(transparent, alone) < 0.2);
    CHECK_NEAR(transparent.directivityDbi, alone.directivityDbi, 0.0, 0.02);
    const auto lossy = qsig::app::solveAntenna({dipole, solid(Type::Box, 0.3, 4.0, 0.1)}, settings);
    REQUIRE(lossy.valid);
    CHECK(balance(lossy) < 0.97);
    CHECK(balance(lossy) > 0.90);

    // Not calculated yet, and said so: a conductor on the dielectric, a wire through it, two
    // dielectrics that touch. A plate above it, in the air, is fine.
    AntennaElement plate;
    plate.name = "Plaque";
    plate.type = Type::Plate;
    plate.fed = 0;
    plate.width = plate.length = 0.2 * kLambda;
    plate.position = {0.3 * kLambda, 0.0, 0.15 * kLambda};
    const AntennaElement box = solid(Type::Box, 0.3, 4.0, 0.0);
    const auto lying = qsig::app::solveAntenna({dipole, box, plate}, settings);
    CHECK(!lying.valid);
    CHECK(lying.error.find("touche") != std::string::npos);
    CHECK(lying.error.find("Plaque") != std::string::npos);
    AntennaElement through = dipole;
    through.position = box.position;
    const auto pierced = qsig::app::solveAntenna({through, box}, settings);
    CHECK(!pierced.valid);
    CHECK(pierced.error.find("traverse") != std::string::npos);
    const auto twin = qsig::app::solveAntenna({dipole, box, solid(Type::Box, 0.6, 2.0, 0.0)}, settings);
    CHECK(!twin.valid);
    CHECK(twin.error.find("touchent") != std::string::npos);
    plate.position.z = 0.25 * kLambda;
    const auto above = qsig::app::solveAntenna({dipole, box, plate}, settings);
    REQUIRE(above.valid);
    CHECK_NEAR(balance(above), 1.0, 0.0, 2e-3);

    // The example.
    const auto example = qsig::app::antennaPreset("dipole_dielectric", kF);
    REQUIRE(example.size() == 2);
    CHECK(qsig::app::elementIsDielectric(example[1]));
    const auto solved = qsig::app::solveAntenna(example, settings);
    REQUIRE(solved.valid);
    CHECK(std::abs(solved.feeds[0].impedance - beside.feeds[0].impedance) < 1.5); // its dipole is a little thicker
}

TEST_CASE("antennas: curved shapes - dish and parabolic cylinder against flat sheets and physical optics, arc, spiral", "[antennas]")
{
    AntennaSettings coarse, fine;
    coarse.frequency = fine.frequency = kF;
    coarse.facetsPerWavelength = 10.0;
    fine.facetsPerWavelength = 13.0;
    auto apart = [](const qsig::app::AntennaResult &a, const qsig::app::AntennaResult &b) { return std::abs(a.feeds[0].impedance - b.feeds[0].impedance); };
    auto balanced = [](const qsig::app::AntennaResult &r) { return std::abs(r.radiatedPowerW / r.acceptedPowerW - 1.0) < 2e-3; };
    AntennaElement dipole;
    dipole.type = Type::Dipole;
    dipole.length = 0.47 * kLambda;
    dipole.wireRadius = 1e-3 * kLambda;
    AntennaElement across = dipole; // along x
    across.azimuthDeg = 0.0;
    across.elevationDeg = 0.0;

    // What the new shapes are: open conducting surfaces, and wires.
    for (const Type type : {Type::Paraboloid, Type::ParabolicCylinder})
    {
        CHECK(qsig::app::elementIsSurface(type));
        CHECK(!qsig::app::elementIsSolid(type));
    }
    CHECK(!qsig::app::elementIsSurface(Type::Arc));
    CHECK(!qsig::app::elementIsSurface(Type::Spiral));

    // A dish of a very long focal length is a disc, a parabolic cylinder of a very long focal length a
    // plate: a dipole a quarter wave above them sees the same thing.
    AntennaElement disc;
    disc.type = Type::Disc;
    disc.fed = 0;
    disc.radius = 0.5 * kLambda;
    AntennaElement flatDish;
    flatDish.type = Type::Paraboloid;
    flatDish.fed = 0;
    flatDish.radius = 0.5 * kLambda;
    flatDish.focalLength = 200.0 * kLambda;
    AntennaElement plate;
    plate.type = Type::Plate;
    plate.fed = 0;
    plate.width = plate.length = 1.0 * kLambda;
    AntennaElement flatCylinder;
    flatCylinder.type = Type::ParabolicCylinder;
    flatCylinder.fed = 0;
    flatCylinder.width = flatCylinder.length = 1.0 * kLambda;
    flatCylinder.focalLength = 200.0 * kLambda;
    AntennaElement above = across;
    above.position = {0.0, 0.0, 0.25 * kLambda};
    const auto onDisc = qsig::app::solveAntenna({disc, above}, coarse), onFlatDish = qsig::app::solveAntenna({flatDish, above}, coarse);
    const auto onPlate = qsig::app::solveAntenna({plate, above}, coarse), onFlatCylinder = qsig::app::solveAntenna({flatCylinder, above}, coarse);
    REQUIRE(onDisc.valid);
    REQUIRE(onFlatDish.valid);
    REQUIRE(onPlate.valid);
    REQUIRE(onFlatCylinder.valid);
    CHECK(apart(onFlatDish, onDisc) < 0.2);
    CHECK_NEAR(onFlatDish.directivityDbi, onDisc.directivityDbi, 0.0, 0.02);
    CHECK(apart(onFlatCylinder, onPlate) < 0.1);
    CHECK_NEAR(onFlatCylinder.directivityDbi, onPlate.directivityDbi, 0.0, 0.02);

    // The example: a dish of 2 lambda across, f = 0.8 lambda, a half-wave dipole along x at the focus.
    // Physical optics (the incident field of the dipole on the dish, its currents 2 n x H, computed apart
    // with numpy) gives 12.03 dBi: the moments, which also see the edges, within 0.4 dB. The mesh is
    // fine enough (10 and 13 triangles per wavelength agree), the triangles hold the area of the dish.
    const auto example = qsig::app::antennaPreset("parabola_dipole", kF);
    REQUIRE(example.size() == 2);
    const auto dish10 = qsig::app::solveAntenna(example, coarse), dish13 = qsig::app::solveAntenna(example, fine);
    REQUIRE(dish10.valid);
    REQUIRE(dish13.valid);
    CHECK_NEAR(dish10.directivityDbi, 12.03, 0.0, 0.4);
    CHECK_NEAR(dish13.directivityDbi, dish10.directivityDbi, 0.0, 0.1);
    CHECK(apart(dish13, dish10) < 0.3);
    CHECK(balanced(dish10));
    CHECK(dish10.mainThetaDeg < 2.0); // the beam along the axis of the dish
    CHECK(dish10.frontToBackDb > 15.0);
    double dishArea = 0.0;
    for (const auto &facet : dish10.facets) dishArea += 0.5 * qsig::norm(qsig::cross(facet.b - facet.a, facet.c - facet.a));
    const double h = 1.0 / 3.2; // depth R^2 / (4 f), lambdas
    const double exactArea = qsig::constants::pi * 1.0 / (6.0 * h * h) * (std::pow(1.0 + 4.0 * h * h, 1.5) - 1.0) * kLambda * kLambda;
    CHECK_REL(dishArea, exactArea, 0.02);
    // The focus is marked in the view.
    const auto marks = qsig::app::antennaShape(example[0]).marks;
    REQUIRE(marks.size() == 1);
    CHECK(qsig::norm(marks[0] - example[1].position) < 1e-12);

    // The parabolic cylinder of the example: converged, all the power radiated, the beam along its axis.
    const auto cylinder = qsig::app::antennaPreset("parabolic_cylinder", kF);
    const auto cyl10 = qsig::app::solveAntenna(cylinder, coarse), cyl13 = qsig::app::solveAntenna(cylinder, fine);
    REQUIRE(cyl10.valid);
    REQUIRE(cyl13.valid);
    CHECK(apart(cyl13, cyl10) < 0.3);
    CHECK_NEAR(cyl13.directivityDbi, cyl10.directivityDbi, 0.0, 0.1);
    CHECK(cyl10.directivityDbi > 9.0);
    CHECK(balanced(cyl10));

    // A wire whose foot is on the dish is attached to it: the foot is on its skin, any point is brought
    // to the skin, and a monopole standing at the vertex is solved.
    const AntennaElement &dish = example[0];
    CHECK(qsig::app::surfaceAt({dish}, dish.position + Vec3{0.3, 0.0, 0.09 / 3.2} * kLambda) == 0);
    for (const Vec3 &from : {Vec3{0.4, 0.2, 0.5}, Vec3{0.0, 0.0, -0.3}, Vec3{1.5, 0.0, 0.1}})
        CHECK(qsig::app::surfaceAt({dish}, qsig::app::projectOnSurface(dish, from * kLambda)) == 0);
    AntennaElement monopole;
    monopole.type = Type::Wire;
    monopole.length = 0.24 * kLambda;
    monopole.wireRadius = 1e-3 * kLambda;
    monopole.feedFraction = 0.0;
    monopole.position = {0.3 * kLambda, 0.0, 0.09 / 3.2 * kLambda + 0.12 * kLambda};
    const auto standing = qsig::app::solveAntenna({dish, monopole}, coarse);
    REQUIRE(standing.valid);
    CHECK(balanced(standing));

    // An arc of 5 lambda of radius and 0.47 lambda long is the dipole; a source slides along an arc, and
    // by symmetry it sees the same at a quarter and at three quarters.
    AntennaElement arc;
    arc.type = Type::Arc;
    arc.radius = 5.0 * kLambda;
    arc.sweepDeg = 0.47 / 5.0 * 180.0 / qsig::constants::pi;
    arc.sides = 2000;
    arc.wireRadius = 1e-3 * kLambda;
    arc.feedFraction = 0.5;
    CHECK(qsig::app::feedSlides(Type::Arc));
    CHECK(!qsig::app::feedSlides(Type::Spiral));
    const auto straight = qsig::app::solveAntenna({dipole}, coarse), bent = qsig::app::solveAntenna({arc}, coarse);
    REQUIRE(straight.valid);
    REQUIRE(bent.valid);
    CHECK(apart(bent, straight) < 0.1);
    AntennaElement half = arc;
    half.radius = 0.25 * kLambda;
    half.sweepDeg = 180.0;
    half.sides = 48;
    half.feedFraction = 0.25;
    AntennaElement mirrored = half;
    mirrored.feedFraction = 0.75;
    const auto quarter = qsig::app::solveAntenna({half}, coarse), threeQuarters = qsig::app::solveAntenna({mirrored}, coarse);
    REQUIRE(quarter.valid);
    REQUIRE(threeQuarters.valid);
    CHECK(apart(quarter, threeQuarters) < 1e-6 * std::abs(quarter.feeds[0].impedance));
    // The point of an arc at an angle, and past its ends (the nearer end).
    for (const double t : {0.0, 0.25, 0.5, 0.9, 1.0})
    {
        const double phi = (-90.0 + 180.0 * t) * qsig::constants::pi / 180.0;
        AntennaElement probe = half;
        const Vec3 e2{0.0, -1.0, 0.0}, e3{1.0, 0.0, 0.0}; // across the vertical axis
        CHECK_NEAR(qsig::app::curveFraction(probe, (e2 * std::cos(phi) + e3 * std::sin(phi)) * (0.25 * kLambda)), t, 0.0, 1e-9);
    }
    CHECK(qsig::app::curveFraction(half, Vec3{-0.1, 0.25, 0.0} * kLambda) == 0.0);

    // The two-arm spiral of the example: circular polarisation along its axis (axial ratio below 0.5 dB),
    // the same with 60 and 100 segments per wavelength, all the power radiated; its source at the centre.
    const auto spiral = qsig::app::antennaPreset("spiral", kF);
    REQUIRE(spiral.size() == 1);
    CHECK(qsig::app::feedCount(spiral[0]) == 1);
    CHECK(qsig::norm(qsig::app::antennaShape(spiral[0]).feeds.at(0) - spiral[0].position) < 1e-12);
    AntennaSettings sixty = coarse;
    sixty.segmentsPerWavelength = 60.0;
    const auto spiral100 = qsig::app::solveAntenna(spiral, coarse), spiral60 = qsig::app::solveAntenna(spiral, sixty);
    REQUIRE(spiral100.valid);
    REQUIRE(spiral60.valid);
    CHECK(spiral100.axialRatioDb < 0.5);
    CHECK(apart(spiral100, spiral60) < 1.0);
    CHECK(balanced(spiral100));
    CHECK(std::min(spiral100.mainThetaDeg, 180.0 - spiral100.mainThetaDeg) < 2.0);

    // A mirror leaves a cone and a dish opening towards the image of their axis.
    AntennaElement cone;
    cone.type = Type::Cone;
    cone.fed = 0;
    cone.radius = 0.2 * kLambda;
    cone.height = 0.3 * kLambda;
    cone.elevationDeg = 60.0;
    cone.azimuthDeg = 20.0;
    for (const AntennaElement &original : {cone, dish})
    {
        AntennaElement image = original;
        qsig::app::mirrorElement(image, Vec3{0.1, 0.0, 0.0}, Vec3{1.0, 0.0, 0.0});
        const Vec3 axis = qsig::app::antennaAxis(original), turned = qsig::app::antennaAxis(image);
        CHECK_NEAR(turned.x, -axis.x, 0.0, 1e-12);
        CHECK_NEAR(turned.y, axis.y, 0.0, 1e-12);
        CHECK_NEAR(turned.z, axis.z, 0.0, 1e-12);
    }
}

