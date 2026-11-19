// "Ondes" module: the FDTD simulation of a scene (qsig/app/wave_simulation.h) against physics.
#include "qsig_test.h"

#include "qsig/app/wave_simulation.h"
#include "qsig/constants.h"

#include <algorithm>
#include <cmath>
#include <vector>

using qsig::app::WaveGrid;
using qsig::app::WaveObject;
using qsig::app::WaveSettings;
using qsig::app::WaveSimulation;
using qsig::app::waveGrid;

namespace
{
using Type = WaveObject::Type;

WaveObject make(Type type, double x, double y)
{
    WaveObject o;
    o.type = type;
    o.position = {x, y, 0.0};
    return o;
}

// A vertical line source crossing the whole area: a plane wave travelling along x.
WaveObject planeWave(double x, int waveform)
{
    WaveObject o = make(Type::LineSource, x, 0.0);
    o.angleDeg = 90.0;
    o.length = 2.0;
    o.waveform = waveform;
    return o;
}
} // namespace

TEST_CASE("waves: the mesh follows the source frequency, the boundary and the cell budget", "[waves]")
{
    WaveSettings s; // 3 m x 2 m, 20 cells per wavelength, CPML
    const WaveGrid g = waveGrid(s, {make(Type::PointSource, 0.0, 0.0)});
    CHECK(g.areaNx == 200);
    CHECK(g.areaNy == 133);
    CHECK(g.pad == qsig::app::kWaveCpmlCells);
    CHECK(g.nx == 200 + 2 * g.pad);
    CHECK_REL(g.dx, 0.015, 1e-12);
    CHECK_REL(g.dy * g.areaNy, 2.0, 1e-12);
    // The cells of the area tile it exactly: the first one starts on its left edge.
    CHECK_NEAR(g.x0 + (g.pad - 0.5) * g.dx, -1.5, 0.0, 1e-12);
    CHECK_REL(g.cellsPerWavelength, qsig::constants::c0 / 1e9 / std::max(g.dx, g.dy), 1e-12);
    CHECK(!g.limited);

    // Pulses are meshed above their carrier: a Ricker wavelet up to twice its peak frequency.
    WaveObject ricker = make(Type::PointSource, 0.0, 0.0);
    ricker.waveform = 2;
    CHECK_REL(waveGrid(s, {ricker}).frequency, 2e9, 1e-12);
    // Without any source the mesh still exists (1 GHz).
    CHECK_REL(waveGrid(s, {}).frequency, 1e9, 1e-12);

    // A mesh finer than the budget is coarsened.
    WaveObject fast = make(Type::PointSource, 0.0, 0.0);
    fast.frequency = 30e9;
    const WaveGrid capped = waveGrid(s, {fast});
    CHECK(capped.limited);
    CHECK(capped.nx * capped.ny <= qsig::app::kWaveMaxCells);
    CHECK(capped.nx * capped.ny > qsig::app::kWaveMaxCells / 2);
    // Closed or simply absorbing boundary: one cell around the area.
    s.boundary = 2;
    CHECK(waveGrid(s, {make(Type::PointSource, 0.0, 0.0)}).pad == 1);
    // Very low frequency: still at least 24 cells across the smaller side.
    WaveObject slow = make(Type::PointSource, 0.0, 0.0);
    slow.frequency = 1e6;
    CHECK(waveGrid(s, {slow}).areaNy >= qsig::app::kWaveMinAreaCells);
}

TEST_CASE("waves: a point source radiates a cylindrical wave decaying as 1/sqrt(r)", "[waves]")
{
    WaveSimulation sim;
    sim.update(WaveSettings{}, {make(Type::PointSource, -0.9, 0.0)});
    sim.advance(600); // about 20 ns: steady state up to 1.5 m
    const double near = sim.amplitudeAt(-0.3, 0.0), far = sim.amplitudeAt(0.3, 0.0);
    REQUIRE(near > 0.0);
    // 2D Green function: amplitude ~ 1/sqrt(kr) for kr >> 1 (kr = 12.6 and 25 here).
    CHECK_REL(near / far, std::sqrt(2.0), 0.015);
    // Isotropy of the scheme at 20 cells per wavelength: same distance along y.
    CHECK_REL(sim.amplitudeAt(-0.9, 0.6), near, 0.01);
    // The pictures cover the area, one pixel per cell.
    const auto frame = sim.frame();
    REQUIRE(frame.picture != nullptr);
    CHECK(frame.picture->images[1].width() == 200);
    CHECK(frame.picture->images[1].height() == 133);
    CHECK(frame.picture->scale[0] > 0.0);
    CHECK(frame.picture->scale[1] > 0.0);
    CHECK(frame.energy > 0.0);
    CHECK(frame.steps == 600);
}

TEST_CASE("waves: a pulse is twice slower in a medium of index 2 and enters it with t = 2/(1+n)", "[waves]")
{
    // Time centroid of the energy of the pulse at two points 0.7 m apart, inside the medium.
    const double probes[2] = {-0.2, 0.5};
    double delay[2] = {0.0, 0.0}, peak[2] = {0.0, 0.0};
    for (int medium = 0; medium < 2; ++medium)
    {
        WaveSettings s;
        s.width = 2.4;
        s.height = 0.6;
        std::vector<WaveObject> objects{planeWave(-1.0, 1)};
        if (medium == 1)
        {
            WaveObject glass = make(Type::DielectricBlock, 0.55, 0.0); // from x = -0.4 to beyond the area
            glass.width = 1.9;
            glass.height = 2.0;
            glass.epsilonR = 4.0;
            objects.push_back(glass);
        }
        WaveSimulation sim;
        sim.update(s, objects);
        const int steps = static_cast<int>(24e-9 / sim.solver().dtS());
        double weight[2] = {0.0, 0.0}, moment[2] = {0.0, 0.0};
        for (int n = 0; n < steps; ++n)
        {
            sim.advance(1);
            const double t = sim.solver().timeS();
            for (int k = 0; k < 2; ++k)
            {
                const double v = sim.primaryAt(probes[k], 0.0);
                weight[k] += v * v;
                moment[k] += t * v * v;
            }
            peak[medium] = std::max(peak[medium], std::abs(sim.primaryAt(probes[0], 0.0)));
        }
        delay[medium] = moment[1] / weight[1] - moment[0] / weight[0];
    }
    CHECK_REL(delay[0], 0.7 / qsig::constants::c0, 0.01);
    CHECK_REL(delay[1] / delay[0], 2.0, 0.04); // numerical dispersion: 10 cells per wavelength at 1.5 GHz
    CHECK_REL(peak[1] / peak[0], 2.0 / 3.0, 0.03);
}

TEST_CASE("waves: conductors, closed cavity and polarisation", "[waves]")
{
    // Closed cavity: once the wavelet has left its source, the energy stays.
    WaveSettings s;
    s.width = 1.0;
    s.height = 0.7;
    s.boundary = 2;
    WaveObject pulse = make(Type::PointSource, -0.21, 0.13);
    pulse.waveform = 2;
    WaveObject post = make(Type::ConductorDisk, 0.2, -0.1);
    post.radius = 0.08;
    WaveSimulation cavity;
    cavity.update(s, {pulse, post});
    cavity.advance(400);
    const double before = cavity.energy();
    cavity.advance(1200);
    REQUIRE(before > 0.0);
    // Leapfrog energy (E and H half a step apart, boundary nodes outside the sum): about 2 %.
    CHECK_REL(cavity.energy(), before, 0.05);
    CHECK(cavity.conductorAt(0.2, -0.1));
    CHECK(cavity.primaryAt(0.2, -0.1) == 0.0);
    CHECK(!cavity.conductorAt(0.0, 0.0));

    // TEz: the source drives Hz; Ez stays zero everywhere.
    WaveSettings te;
    te.polarization = 1;
    WaveSimulation tez;
    tez.update(te, {make(Type::PointSource, 0.0, 0.0)});
    tez.advance(200);
    CHECK(tez.amplitudeAt(0.3, 0.0) > 0.0);
    double ez = 0.0;
    for (double v : tez.solver().ez()) ez = std::max(ez, std::abs(v));
    CHECK(ez == 0.0);
    // A change of polarisation restarts the fields.
    CHECK(tez.update(WaveSettings{}, {make(Type::PointSource, 0.0, 0.0)}));
    CHECK(tez.solver().stepCount() == 0);
}

TEST_CASE("waves: rasterised walls and line sources, edits under the running waves", "[waves]")
{
    WaveSettings s;
    WaveObject wall = make(Type::SlitWall, -0.8, 0.0);
    wall.angleDeg = 90.0;
    wall.length = 2.6;
    wall.thickness = 0.04;
    wall.slits = 2;
    wall.slitWidth = 0.06;
    wall.slitSpacing = 0.6;
    WaveSimulation sim;
    sim.update(s, {make(Type::PointSource, -1.2, 0.0), wall});
    CHECK(sim.conductorAt(-0.8, 0.0));  // between the slits
    CHECK(!sim.conductorAt(-0.8, 0.3)); // slit centres at y = +-0.3
    CHECK(!sim.conductorAt(-0.8, -0.3));
    CHECK(sim.conductorAt(-0.8, 0.6));
    CHECK(!sim.conductorAt(-0.7, 0.0)); // beside the wall

    // A line source drives every node along it, once.
    WaveObject line = make(Type::LineSource, 0.0, 0.0);
    line.angleDeg = 90.0;
    line.length = 1.0;
    sim.update(s, {line});
    const int expected = static_cast<int>(std::lround(1.0 / sim.grid().dy)) + 1;
    CHECK(std::abs(static_cast<int>(sim.sources().size()) - expected) <= 1);
    line.angleDeg = 30.0; // slanted: at least one node per cell along x (0.87 m), no duplicates
    sim.update(s, {line});
    const auto &nodes = sim.sources();
    bool duplicates = false;
    for (std::size_t a = 0; a < nodes.size(); ++a)
        for (std::size_t b = a + 1; b < nodes.size(); ++b)
            duplicates = duplicates || (nodes[a].x == nodes[b].x && nodes[a].y == nodes[b].y);
    CHECK(!duplicates);
    CHECK(nodes.size() >= static_cast<std::size_t>(0.866 / sim.grid().dx));

    // Obstacles move under the running waves; a new frequency means a new mesh, from t = 0.
    sim.advance(50);
    CHECK(!sim.update(s, {line, make(Type::DielectricDisk, 0.5, 0.0)}));
    CHECK(sim.solver().stepCount() == 50);
    line.frequency = 2e9;
    CHECK(sim.update(s, {line}));
    CHECK(sim.solver().stepCount() == 0);

    // The probe records the primary field at every step.
    sim.setProbe(0.2, 0.0);
    sim.advance(120);
    const auto frame = sim.frame();
    CHECK(frame.probe.valid);
    CHECK(!frame.probe.trace.empty());
    CHECK_NEAR(frame.probe.trace.back(), sim.primaryAt(0.2, 0.0), 1e-6, 1e-9); // float storage
    CHECK(frame.probe.traceStep > 0.0);
    sim.setProbe(5.0, 0.0); // outside the area
    CHECK(!sim.frame().probe.valid);
    // An empty scene is silent.
    sim.update(s, {});
    sim.advance(20);
    CHECK(sim.energy() == 0.0);
}

TEST_CASE("waves: freehand drawings are painted and erased in order, and move with their position", "[waves]")
{
    WaveSettings s;
    WaveObject drawing = make(Type::ConductorDrawing, 0.0, 0.0);
    qsig::app::WaveStroke line;
    line.points = {-0.5, 0.0, 0.5, 0.0};
    line.width = 0.04;
    qsig::app::WaveStroke dot;
    dot.points = {0.0, 0.6};
    dot.width = 0.1;
    qsig::app::WaveStroke gap;
    gap.points = {0.0, -0.1, 0.0, 0.1};
    gap.width = 0.1;
    gap.erase = true;
    drawing.strokes = {line, dot, gap};
    WaveSimulation sim;
    sim.update(s, {drawing});
    CHECK(sim.conductorAt(0.3, 0.0));
    CHECK(sim.conductorAt(-0.45, 0.0));
    CHECK(!sim.conductorAt(0.3, 0.1));  // the line is 4 cm wide
    CHECK(!sim.conductorAt(0.0, 0.0));  // erased by the later stroke
    CHECK(!sim.conductorAt(0.03, 0.0)); // the gap is 10 cm wide
    CHECK(sim.conductorAt(0.0, 0.6));   // a single point is a dot
    CHECK(sim.conductorAt(0.03, 0.62));
    CHECK(!sim.conductorAt(0.0, 0.7));

    // A thin stroke still makes a continuous wall: no node gap along a slanted line.
    WaveObject thin = make(Type::ConductorDrawing, 0.0, 0.0);
    qsig::app::WaveStroke slanted;
    slanted.points = {-0.6, -0.4, 0.6, 0.5};
    slanted.width = 0.001;
    thin.strokes = {slanted};
    sim.update(s, {thin});
    bool continuous = true;
    for (double t = 0.05; t < 0.95; t += 0.01)
        continuous = continuous && sim.conductorAt(-0.6 + 1.2 * t, -0.4 + 0.9 * t);
    CHECK(continuous);

    // Moving the drawing moves every stroke; a dielectric drawing sets its material.
    WaveObject glass = drawing;
    glass.type = Type::DielectricDrawing;
    glass.epsilonR = 3.0;
    glass.position = {0.0, -0.5, 0.0};
    sim.update(s, {glass});
    CHECK(!sim.conductorAt(0.3, -0.5));
    int i = 0, j = 0;
    REQUIRE(sim.nodeAt(0.3, -0.5, i, j));
    CHECK(sim.solver().materialAt(i, j).epsilonR == 3.0);
    REQUIRE(sim.nodeAt(0.3, 0.0, i, j));
    CHECK(sim.solver().materialAt(i, j).epsilonR == 1.0);
}

TEST_CASE("waves: point B gives the amplitude ratio, the phase difference and the delay", "[waves]")
{
    // A plane wave along x: B, 10 cm after A, sees the same amplitude a third of a period later.
    WaveSettings s;
    s.width = 2.4;
    s.height = 0.6;
    WaveSimulation sim;
    sim.update(s, {planeWave(-1.0, 0)});
    sim.setProbes(-0.2, 0.0, true, -0.1, 0.0);
    sim.advance(900); // 30 ns
    const auto frame = sim.frame();
    REQUIRE(frame.probe.valid);
    REQUIRE(frame.hasProbeB);
    REQUIRE(frame.probeB.valid);
    CHECK_REL(frame.phaseFrequency, 1e9, 1e-12);
    CHECK_REL(frame.probeB.amplitude / frame.probe.amplitude, 1.0, 0.01);
    CHECK_REL(std::abs(frame.probeB.phasor) / std::abs(frame.probe.phasor), 1.0, 0.01);
    CHECK_REL(std::abs(frame.probe.phasor), frame.probe.amplitude, 0.01);
    const double phase = std::arg(frame.probeB.phasor / frame.probe.phasor); // B lags: negative
    const double delay = -phase / (2.0 * qsig::constants::pi * 1e9);
    // The points snap to the nearest nodes: 10.5 cm apart here.
    const double distance = (frame.probeB.i - frame.probe.i) * sim.grid().dx;
    CHECK_REL(distance, 0.105, 1e-9);
    CHECK_REL(delay, distance / qsig::constants::c0, 0.01);
    // Both traces cover the same steps.
    CHECK(frame.probe.trace.size() == frame.probeB.trace.size());
    CHECK(!frame.probeB.trace.empty());
    // Pulses only: no phase frequency.
    WaveSimulation pulse;
    pulse.update(s, {planeWave(-1.0, 1)});
    CHECK(pulse.frame().phaseFrequency == 0.0);
}
