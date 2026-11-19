#include "qsig/app/wave_simulation.h"

#include "qsig/app/field_map.h"
#include "qsig/constants.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace qsig::app
{
namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kCourant = 0.95;
constexpr std::size_t kTraceCapacity = 4096;
constexpr std::size_t kTraceShown = 900;   // samples of the trace window
constexpr std::size_t kTracePoints = 300;  // after decimation
constexpr int kLutSize = 1024;

double clampPositive(double v, double minimum)
{
    return std::isfinite(v) ? std::max(v, minimum) : minimum;
}

// Squared distance from (px, py) to the segment [a, b].
double segmentDistance2(double px, double py, double ax, double ay, double bx, double by)
{
    const double ux = bx - ax, uy = by - ay, len2 = ux * ux + uy * uy;
    double t = len2 > 0.0 ? ((px - ax) * ux + (py - ay) * uy) / len2 : 0.0;
    t = std::clamp(t, 0.0, 1.0);
    const double dx = px - (ax + t * ux), dy = py - (ay + t * uy);
    return dx * dx + dy * dy;
}

struct Stop
{
    double t;
    int r, g, b;
};

QColor interpolate(const Stop *stops, int n, double t)
{
    t = std::clamp(std::isfinite(t) ? t : 0.0, 0.0, 1.0);
    for (int k = 1; k < n; ++k)
        if (t <= stops[k].t)
        {
            const Stop &a = stops[k - 1], &b = stops[k];
            const double u = (t - a.t) / (b.t - a.t);
            auto mix = [u](int x, int y) { return static_cast<int>(std::lround(x + u * (y - x))); };
            return QColor(mix(a.r, b.r), mix(a.g, b.g), mix(a.b, b.b));
        }
    return QColor(stops[n - 1].r, stops[n - 1].g, stops[n - 1].b);
}

using Lut = std::array<QRgb, kLutSize>;
Lut makeLut(QColor (*colour)(double))
{
    Lut lut{};
    for (int k = 0; k < kLutSize; ++k) lut[static_cast<std::size_t>(k)] = colour(k / double(kLutSize - 1)).rgb();
    return lut;
}

int lutIndex(double t)
{
    return static_cast<int>(std::lround(std::clamp(t, 0.0, 1.0) * (kLutSize - 1)));
}

bool sameMaterial(const FDTD::Material &a, const FDTD::Material &b)
{
    return a.epsilonR == b.epsilonR && a.muR == b.muR && a.conductivitySPerM == b.conductivitySPerM && a.pec == b.pec;
}

// Point (px, py) relative to the centre of the object: inside its shape?
bool insideShape(const WaveObject &o, double px, double py, double cell)
{
    using Type = WaveObject::Type;
    const double a = o.angleDeg * kPi / 180.0;
    const double c = std::cos(a), s = std::sin(a);
    switch (o.type)
    {
    case Type::DielectricBlock:
    case Type::ConductorBlock:
    {
        const double u = px * c + py * s, v = -px * s + py * c;
        return std::abs(u) <= 0.5 * o.width && std::abs(v) <= 0.5 * o.height;
    }
    case Type::DielectricDisk:
    case Type::ConductorDisk:
        return px * px + py * py <= o.radius * o.radius;
    case Type::SlitWall:
    {
        // Along the wall (s) and across it (t). Never thinner than about one and a half cells, so
        // that a slanted wall has no gap between its staircase nodes.
        const double along = px * c + py * s, across = -px * s + py * c;
        const double halfThickness = std::max(0.5 * o.thickness, 0.75 * cell);
        if (std::abs(along) > 0.5 * o.length || std::abs(across) > halfThickness) return false;
        const int n = std::clamp(o.slits, 0, 50);
        for (int k = 0; k < n; ++k)
        {
            const double centre = (k - 0.5 * (n - 1)) * o.slitSpacing;
            if (std::abs(along - centre) < 0.5 * o.slitWidth) return false;
        }
        return true;
    }
    default:
        return false;
    }
}

double reach(const WaveObject &o, double cell)
{
    using Type = WaveObject::Type;
    switch (o.type)
    {
    case Type::DielectricBlock:
    case Type::ConductorBlock:
        return 0.5 * std::hypot(o.width, o.height);
    case Type::DielectricDisk:
    case Type::ConductorDisk:
        return o.radius;
    case Type::SlitWall:
        return std::hypot(0.5 * o.length, std::max(0.5 * o.thickness, 0.75 * cell));
    default:
        return 0.0;
    }
}
} // namespace

double significantFrequency(const std::vector<WaveObject> &objects)
{
    double f = 0.0;
    for (const auto &o : objects)
    {
        if (!o.isSource()) continue;
        // Gaussian pulse (tau = 1.25 / f): spectrum to about 1.5 f. Ricker: to about 2 f.
        const double spread = o.waveform == 1 ? 1.5 : o.waveform == 2 ? 2.0 : 1.0;
        f = std::max(f, clampPositive(o.frequency, 1.0) * spread);
    }
    return f > 0.0 ? f : 1.0e9;
}

double maxRefractiveIndex(const std::vector<WaveObject> &objects)
{
    double n = 1.0;
    for (const auto &o : objects)
        if (o.isDielectric()) n = std::max(n, std::sqrt(clampPositive(o.epsilonR, 1.0) * clampPositive(o.muR, 1.0)));
    return n;
}

WaveGrid waveGrid(const WaveSettings &settings, const std::vector<WaveObject> &objects)
{
    WaveGrid g;
    g.width = std::clamp(std::isfinite(settings.width) ? settings.width : 1.0, 1e-3, 1e4);
    g.height = std::clamp(std::isfinite(settings.height) ? settings.height : 1.0, 1e-3, 1e4);
    g.frequency = significantFrequency(objects);
    const double lambda = qsig::constants::c0 / g.frequency;
    const double perWavelength =
        std::clamp(std::isfinite(settings.cellsPerWavelength) ? settings.cellsPerWavelength : 20.0, 4.0, 200.0);
    g.pad = settings.boundary == 0 ? kWaveCpmlCells : 1;

    double step = lambda / perWavelength;
    step = std::min(step, std::min(g.width, g.height) / kWaveMinAreaCells);
    auto cells = [&](double d)
    {
        return (std::round(g.width / d) + 2.0 * g.pad) * (std::round(g.height / d) + 2.0 * g.pad);
    };
    if (cells(step) > kWaveMaxCells)
    {
        g.limited = true;
        step = std::sqrt(g.width * g.height / (0.9 * kWaveMaxCells));
        while (cells(step) > kWaveMaxCells) step *= 1.02;
    }
    g.areaNx = std::max(kWaveMinAreaCells / 3, static_cast<int>(std::lround(g.width / step)));
    g.areaNy = std::max(kWaveMinAreaCells / 3, static_cast<int>(std::lround(g.height / step)));
    g.dx = g.width / g.areaNx;
    g.dy = g.height / g.areaNy;
    g.nx = g.areaNx + 2 * g.pad;
    g.ny = g.areaNy + 2 * g.pad;
    // Node (i, j) sits at the centre of its cell, so the cells of the area tile it exactly.
    g.x0 = -0.5 * g.width + (0.5 - g.pad) * g.dx;
    g.y0 = -0.5 * g.height + (0.5 - g.pad) * g.dy;
    g.cellsPerWavelength = lambda / std::max(g.dx, g.dy);
    return g;
}

WaveSimulation::WaveSimulation() = default;

bool WaveSimulation::update(const WaveSettings &settings, const std::vector<WaveObject> &objects)
{
    const WaveGrid g = waveGrid(settings, objects);
    const auto boundary = static_cast<FDTD::BoundaryCondition>(std::clamp(settings.boundary, 0, 2));
    const auto polarization = settings.polarization == 1 ? FDTD::Polarization::TEz : FDTD::Polarization::TMz;
    bool rebuilt = false;
    if (!m_configured || !g.sameMesh(m_grid) || boundary != m_solver.boundaryCondition())
    {
        m_solver.setCpmlCells(kWaveCpmlCells);
        m_solver.configure(g.nx, g.ny, g.dx, g.dy, kCourant, boundary);
        m_raster.assign(static_cast<std::size_t>(g.nx) * static_cast<std::size_t>(g.ny), FDTD::Material{});
        m_configured = true;
        rebuilt = true;
    }
    if (polarization != m_solver.polarization())
    {
        m_solver.setPolarization(polarization);
        rebuilt = true;
    }
    m_grid = g;
    m_settings = settings;
    rasterise(objects);
    placeSources(objects);
    // Running mean square: two stages of one period each.
    m_decay = std::exp(-m_solver.dtS() * g.frequency);
    // Phasors at the frequency of the first sinusoidal source: two stages of two periods each.
    double phaseFrequency = 0.0;
    for (const auto &o : objects)
        if (o.isSource() && o.waveform == 0)
        {
            phaseFrequency = clampPositive(o.frequency, 1.0);
            break;
        }
    if (phaseFrequency != m_phaseFrequency)
    {
        m_phaseFrequency = phaseFrequency;
        m_phasorStage = m_phasorStageB = m_phasor = m_phasorB = {};
    }
    m_phaseKeep = m_phaseFrequency > 0.0 ? std::exp(-m_solver.dtS() * m_phaseFrequency / 2.0) : 0.0;
    if (rebuilt)
    {
        m_solver.resetFields();
        clearAverages();
    }
    return rebuilt;
}

void WaveSimulation::rasterise(const std::vector<WaveObject> &objects)
{
    const WaveGrid &g = m_grid;
    const double cell = std::max(g.dx, g.dy);
    std::vector<FDTD::Material> raster(static_cast<std::size_t>(g.nx) * static_cast<std::size_t>(g.ny));
    for (const auto &o : objects) // in list order: a later object covers an earlier one
    {
        if (o.isSource()) continue;
        FDTD::Material m;
        if (o.isConductor())
            m.pec = true;
        else
        {
            m.epsilonR = clampPositive(o.epsilonR, 1.0);
            m.muR = clampPositive(o.muR, 1.0);
            m.conductivitySPerM = std::isfinite(o.conductivity) ? std::max(0.0, o.conductivity) : 0.0;
        }
        if (o.isDrawing())
        {
            stampDrawing(o, m, raster);
            continue;
        }
        const double cx = o.position.x, cy = o.position.y, r = reach(o, cell);
        const int i0 = std::max(0, static_cast<int>(std::floor((cx - r - g.x0) / g.dx)));
        const int i1 = std::min(g.nx - 1, static_cast<int>(std::ceil((cx + r - g.x0) / g.dx)));
        const int j0 = std::max(0, static_cast<int>(std::floor((cy - r - g.y0) / g.dy)));
        const int j1 = std::min(g.ny - 1, static_cast<int>(std::ceil((cy + r - g.y0) / g.dy)));
        for (int j = j0; j <= j1; ++j)
            for (int i = i0; i <= i1; ++i)
                if (insideShape(o, g.x0 + i * g.dx - cx, g.y0 + j * g.dy - cy, cell))
                    raster[static_cast<std::size_t>(j) * static_cast<std::size_t>(g.nx) + static_cast<std::size_t>(i)] =
                        m;
    }
    bool changed = raster.size() != m_raster.size();
    for (std::size_t k = 0; !changed && k < raster.size(); ++k) changed = !sameMaterial(raster[k], m_raster[k]);
    if (!changed) return;
    m_raster = std::move(raster);
    m_solver.setMaterials(m_raster);
}

// Strokes in order on a mask of the drawing: a later erase stroke removes what earlier ones painted.
// Never thinner than about one and a half cells, so that a drawn conductor line has no gap.
void WaveSimulation::stampDrawing(const WaveObject &o, const FDTD::Material &material,
                                  std::vector<FDTD::Material> &raster) const
{
    const WaveGrid &g = m_grid;
    const double cell = std::max(g.dx, g.dy);
    auto half = [cell](const WaveStroke &s) { return std::max(0.5 * clampPositive(s.width, 0.0), 0.75 * cell); };
    double xMin = 1e300, xMax = -1e300, yMin = 1e300, yMax = -1e300;
    for (const auto &s : o.strokes)
        for (std::size_t k = 0; k + 1 < s.points.size(); k += 2)
        {
            const double x = o.position.x + s.points[k], y = o.position.y + s.points[k + 1], h = half(s);
            xMin = std::min(xMin, x - h);
            xMax = std::max(xMax, x + h);
            yMin = std::min(yMin, y - h);
            yMax = std::max(yMax, y + h);
        }
    if (!(xMin <= xMax) || !(yMin <= yMax)) return;
    auto column = [&](double x) { return static_cast<int>(std::floor((x - g.x0) / g.dx)); };
    auto row = [&](double y) { return static_cast<int>(std::floor((y - g.y0) / g.dy)); };
    const int i0 = std::max(0, column(xMin)), i1 = std::min(g.nx - 1, column(xMax) + 1);
    const int j0 = std::max(0, row(yMin)), j1 = std::min(g.ny - 1, row(yMax) + 1);
    if (i0 > i1 || j0 > j1) return;
    const int w = i1 - i0 + 1, h = j1 - j0 + 1;
    std::vector<char> mask(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), 0);
    for (const auto &s : o.strokes)
    {
        const std::size_t n = s.points.size() / 2;
        if (n == 0) continue;
        const double r = half(s), r2 = r * r;
        const char value = s.erase ? 0 : 1;
        for (std::size_t k = 0; k < std::max<std::size_t>(1, n - 1); ++k)
        {
            const std::size_t next = std::min(k + 1, n - 1);
            const double ax = o.position.x + s.points[2 * k], ay = o.position.y + s.points[2 * k + 1];
            const double bx = o.position.x + s.points[2 * next], by = o.position.y + s.points[2 * next + 1];
            const int si0 = std::max(i0, column(std::min(ax, bx) - r)), si1 = std::min(i1, column(std::max(ax, bx) + r) + 1);
            const int sj0 = std::max(j0, row(std::min(ay, by) - r)), sj1 = std::min(j1, row(std::max(ay, by) + r) + 1);
            for (int j = sj0; j <= sj1; ++j)
                for (int i = si0; i <= si1; ++i)
                    if (segmentDistance2(g.x0 + i * g.dx, g.y0 + j * g.dy, ax, ay, bx, by) <= r2)
                        mask[static_cast<std::size_t>((j - j0) * w + (i - i0))] = value;
        }
    }
    for (int j = j0; j <= j1; ++j)
        for (int i = i0; i <= i1; ++i)
            if (mask[static_cast<std::size_t>((j - j0) * w + (i - i0))])
                raster[static_cast<std::size_t>(j) * static_cast<std::size_t>(g.nx) + static_cast<std::size_t>(i)] =
                    material;
}

void WaveSimulation::placeSources(const std::vector<WaveObject> &objects)
{
    const WaveGrid &g = m_grid;
    std::vector<FDTD::Source> list;
    for (const auto &o : objects)
    {
        if (!o.isSource()) continue;
        FDTD::Source s;
        s.name = o.name;
        s.waveform = static_cast<FDTD::SourceWaveform>(std::clamp(o.waveform, 0, 2));
        s.injection = o.injection == 1 ? FDTD::SourceInjection::Hard : FDTD::SourceInjection::Soft;
        s.frequencyHz = clampPositive(o.frequency, 1.0);
        s.amplitude = std::isfinite(o.strength) ? o.strength : 0.0;
        s.phaseDeg = std::isfinite(o.phaseDeg) ? o.phaseDeg : 0.0;
        long last = -1;
        auto add = [&](double x, double y)
        {
            const int i = static_cast<int>(std::lround((x - g.x0) / g.dx));
            const int j = static_cast<int>(std::lround((y - g.y0) / g.dy));
            if (i < 1 || j < 1 || i > g.nx - 2 || j > g.ny - 2) return; // outside the grid
            const long key = static_cast<long>(j) * g.nx + i;
            if (key == last) return; // a sampled line visits its nodes in order
            last = key;
            s.x = i;
            s.y = j;
            list.push_back(s);
        };
        if (o.type == WaveObject::Type::PointSource)
        {
            add(o.position.x, o.position.y);
            continue;
        }
        // Line source: every node along the segment, sampled at half a cell.
        const double a = o.angleDeg * kPi / 180.0;
        const double length = clampPositive(o.length, 0.0);
        const int samples = std::max(2, static_cast<int>(std::ceil(length / (0.5 * std::min(g.dx, g.dy)))) + 1);
        for (int k = 0; k < samples; ++k)
        {
            const double t = (k / double(samples - 1) - 0.5) * length;
            add(o.position.x + t * std::cos(a), o.position.y + t * std::sin(a));
        }
    }
    m_solver.setSources(list);
}

void WaveSimulation::clearAverages()
{
    m_meanSquareStage.assign(static_cast<std::size_t>(m_grid.nx) * static_cast<std::size_t>(m_grid.ny), 0.0);
    m_meanSquare.assign(m_meanSquareStage.size(), 0.0);
    m_trace.assign(kTraceCapacity, 0.0f);
    m_traceB.assign(kTraceCapacity, 0.0f);
    m_traceNext = m_traceCount = 0;
    m_phasorStage = m_phasorStageB = m_phasor = m_phasorB = {};
    m_scale[0] = m_scale[1] = 0.0;
}

void WaveSimulation::reset()
{
    if (!m_configured) return;
    m_solver.resetFields();
    clearAverages();
}

void WaveSimulation::setProbes(double ax, double ay, bool useB, double bx, double by)
{
    if (ax == m_probeX && ay == m_probeY && useB == m_useB && (!useB || (bx == m_probeBX && by == m_probeBY))) return;
    m_probeX = ax;
    m_probeY = ay;
    m_useB = useB;
    m_probeBX = bx;
    m_probeBY = by;
    // Both traces cover the same steps; the averages restart with them.
    m_traceNext = m_traceCount = 0;
    m_phasorStage = m_phasorStageB = m_phasor = m_phasorB = {};
}

bool WaveSimulation::nodeAt(double x, double y, int &i, int &j) const
{
    const WaveGrid &g = m_grid;
    if (!m_configured || !std::isfinite(x) || !std::isfinite(y)) return false;
    i = static_cast<int>(std::lround((x - g.x0) / g.dx));
    j = static_cast<int>(std::lround((y - g.y0) / g.dy));
    return i >= g.pad && j >= g.pad && i < g.pad + g.areaNx && j < g.pad + g.areaNy;
}

double WaveSimulation::primaryAt(double x, double y) const
{
    int i = 0, j = 0;
    if (!nodeAt(x, y, i, j)) return 0.0;
    return m_solver.primaryScalarField()[static_cast<std::size_t>(j * m_grid.nx + i)];
}

double WaveSimulation::amplitudeAt(double x, double y) const
{
    int i = 0, j = 0;
    if (!nodeAt(x, y, i, j)) return 0.0;
    return std::sqrt(2.0 * m_meanSquare[static_cast<std::size_t>(j * m_grid.nx + i)]);
}

bool WaveSimulation::conductorAt(double x, double y) const
{
    int i = 0, j = 0;
    if (!nodeAt(x, y, i, j)) return false;
    return m_raster[static_cast<std::size_t>(j * m_grid.nx + i)].pec;
}

void WaveSimulation::advance(int steps)
{
    if (!m_configured || steps <= 0) return;
    const WaveGrid &g = m_grid;
    const auto begin = std::chrono::steady_clock::now();
    const std::vector<double> &field = m_solver.primaryScalarField();
    int pi = 0, pj = 0, bi = 0, bj = 0;
    const bool probe = nodeAt(m_probeX, m_probeY, pi, pj);
    const bool probeB = m_useB && nodeAt(m_probeBX, m_probeBY, bi, bj);
    const std::size_t probeIndex = static_cast<std::size_t>(pj * g.nx + pi);
    const std::size_t probeBIndex = static_cast<std::size_t>(bj * g.nx + bi);
    const double keep = m_decay, take = 1.0 - m_decay;
    const double phaseTake = 1.0 - m_phaseKeep;
    const double omega = 2.0 * 3.14159265358979323846 * m_phaseFrequency;
    if (m_trace.size() != kTraceCapacity) m_trace.assign(kTraceCapacity, 0.0f);
    if (m_traceB.size() != kTraceCapacity) m_traceB.assign(kTraceCapacity, 0.0f);
    for (int s = 0; s < steps; ++s)
    {
        m_solver.step(1);
        for (int j = g.pad; j < g.pad + g.areaNy; ++j)
        {
            const std::size_t row = static_cast<std::size_t>(j) * static_cast<std::size_t>(g.nx);
            for (int i = g.pad; i < g.pad + g.areaNx; ++i)
            {
                const std::size_t k = row + static_cast<std::size_t>(i);
                m_meanSquareStage[k] = keep * m_meanSquareStage[k] + take * field[k] * field[k];
                m_meanSquare[k] = keep * m_meanSquare[k] + take * m_meanSquareStage[k];
            }
        }
        if (probe || probeB)
        {
            const double a = probe ? field[probeIndex] : 0.0, b = probeB ? field[probeBIndex] : 0.0;
            m_trace[m_traceNext] = static_cast<float>(a);
            m_traceB[m_traceNext] = static_cast<float>(b);
            m_traceNext = (m_traceNext + 1) % kTraceCapacity;
            m_traceCount = std::min(m_traceCount + 1, kTraceCapacity);
            if (m_phaseFrequency > 0.0)
            {
                // v = A cos(wt + phi)  ->  2 <v e^(-iwt)> = A e^(i phi)
                const std::complex<double> turn = std::polar(2.0 * phaseTake, -omega * m_solver.timeS());
                m_phasorStage = m_phaseKeep * m_phasorStage + a * turn;
                m_phasorStageB = m_phaseKeep * m_phasorStageB + b * turn;
                m_phasor = m_phaseKeep * m_phasor + phaseTake * m_phasorStage;
                m_phasorB = m_phaseKeep * m_phasorB + phaseTake * m_phasorStageB;
            }
        }
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count() / steps;
    m_msPerStep = m_msPerStep > 0.0 ? 0.85 * m_msPerStep + 0.15 * ms : ms;
}

double WaveSimulation::energy() const
{
    if (!m_configured) return 0.0;
    const WaveGrid &g = m_grid;
    const auto &ex = m_solver.ex(), &ey = m_solver.ey(), &ez = m_solver.ez();
    const auto &hx = m_solver.hx(), &hy = m_solver.hy(), &hz = m_solver.hz();
    double sum = 0.0;
    for (int j = g.pad; j < g.pad + g.areaNy; ++j)
        for (int i = g.pad; i < g.pad + g.areaNx; ++i)
        {
            const std::size_t k = static_cast<std::size_t>(j * g.nx + i);
            const FDTD::Material &m = m_raster[k];
            const double e2 = ex[k] * ex[k] + ey[k] * ey[k] + ez[k] * ez[k];
            const double h2 = hx[k] * hx[k] + hy[k] * hy[k] + hz[k] * hz[k];
            sum += 0.5 * (qsig::constants::epsilon0 * m.epsilonR * e2 + qsig::constants::mu0 * m.muR * h2);
        }
    return sum * g.dx * g.dy;
}

// Robust top of the colour scale: a high percentile of |field| over the area (a sample of it), so
// that the few cells next to a source do not darken everything else.
double WaveSimulation::percentile(const std::vector<double> &field, bool meanSquare, double fraction) const
{
    const WaveGrid &g = m_grid;
    const std::size_t count = static_cast<std::size_t>(g.areaNx) * static_cast<std::size_t>(g.areaNy);
    const std::size_t stride = std::max<std::size_t>(1, count / 20000);
    std::vector<double> values;
    values.reserve(count / stride + 1);
    double maximum = 0.0;
    for (std::size_t n = 0; n < count; n += stride)
    {
        const int i = g.pad + static_cast<int>(n % static_cast<std::size_t>(g.areaNx));
        const int j = g.pad + static_cast<int>(n / static_cast<std::size_t>(g.areaNx));
        const double v = field[static_cast<std::size_t>(j * g.nx + i)];
        const double a = meanSquare ? std::sqrt(2.0 * std::max(0.0, v)) : std::abs(v);
        values.push_back(a);
        maximum = std::max(maximum, a);
    }
    if (values.empty()) return 0.0;
    const auto at = static_cast<std::ptrdiff_t>(fraction * double(values.size() - 1));
    std::nth_element(values.begin(), values.begin() + at, values.end());
    // While a wave covers only a corner of the area, the percentile is still zero.
    return std::max(values[static_cast<std::size_t>(at)], 0.02 * maximum);
}

WaveFrame WaveSimulation::frame()
{
    WaveFrame f;
    f.grid = m_grid;
    f.polarization = m_settings.polarization;
    f.time = m_solver.timeS();
    f.dt = m_solver.dtS();
    f.steps = m_solver.stepCount();
    f.msPerStep = m_msPerStep;
    auto picture = std::make_shared<LiveFrame>();
    picture->xMin = -0.5 * m_grid.width;
    picture->xMax = 0.5 * m_grid.width;
    picture->yMin = -0.5 * m_grid.height;
    picture->yMax = 0.5 * m_grid.height;
    picture->boundary = std::clamp(m_settings.boundary, 0, 2);
    if (!m_configured)
    {
        f.picture = std::move(picture);
        return f;
    }
    f.energy = energy();

    const WaveGrid &g = m_grid;
    const std::vector<double> &field = m_solver.primaryScalarField();
    // Fast attack, slow release: the scale follows a growing wave at once and a fading one gently.
    auto follow = [](double &scale, double target)
    {
        target = std::isfinite(target) ? std::max(0.0, target) : 0.0;
        scale = target >= scale ? target : std::max(target, 0.97 * scale);
    };
    follow(m_scale[0], percentile(m_meanSquare, true, 0.995));
    follow(m_scale[1], percentile(field, false, 0.995));
    picture->scale[0] = m_scale[0] > 0.0 ? m_scale[0] : 1.0;
    picture->scale[1] = m_scale[1] > 0.0 ? m_scale[1] : 1.0;

    static const Lut magnitudeLut = makeLut(&WaveSimulation::magnitudeColour);
    static const Lut signedLut = makeLut(&WaveSimulation::signedColour);
    const QRgb conductor = qRgb(92, 97, 105);
    QImage magnitude(g.areaNx, g.areaNy, QImage::Format_RGB32);
    QImage value(g.areaNx, g.areaNy, QImage::Format_RGB32);
    const double inverseMagnitude = m_scale[0] > 0.0 ? 1.0 / m_scale[0] : 0.0;
    const double inverseValue = m_scale[1] > 0.0 ? 1.0 / m_scale[1] : 0.0;
    for (int r = 0; r < g.areaNy; ++r)
    {
        const int j = g.pad + g.areaNy - 1 - r; // first row at the top (y max)
        auto *lineM = reinterpret_cast<QRgb *>(magnitude.scanLine(r));
        auto *lineV = reinterpret_cast<QRgb *>(value.scanLine(r));
        for (int c = 0; c < g.areaNx; ++c)
        {
            const std::size_t k = static_cast<std::size_t>(j * g.nx + g.pad + c);
            if (m_raster[k].pec)
            {
                lineM[c] = lineV[c] = conductor;
                continue;
            }
            lineM[c] = magnitudeLut[static_cast<std::size_t>(
                lutIndex(std::sqrt(2.0 * std::max(0.0, m_meanSquare[k])) * inverseMagnitude))];
            lineV[c] = signedLut[static_cast<std::size_t>(lutIndex(0.5 + 0.5 * field[k] * inverseValue))];
        }
    }
    picture->images[0] = std::move(magnitude);
    picture->images[1] = std::move(value);
    f.picture = std::move(picture);

    f.probe = probeAt(m_probeX, m_probeY, m_trace, m_phasor);
    f.hasProbeB = m_useB;
    if (m_useB) f.probeB = probeAt(m_probeBX, m_probeBY, m_traceB, m_phasorB);
    f.phaseFrequency = m_phaseFrequency;
    return f;
}

WaveProbe WaveSimulation::probeAt(double x, double y, const std::vector<float> &trace, std::complex<double> phasor) const
{
    WaveProbe p;
    int i = 0, j = 0;
    if (!nodeAt(x, y, i, j)) return p;
    const WaveGrid &g = m_grid;
    const std::size_t k = static_cast<std::size_t>(j * g.nx + i);
    p.valid = true;
    p.i = i - g.pad;
    p.j = j - g.pad;
    p.primary = m_solver.primaryScalarField()[k];
    p.amplitude = std::sqrt(2.0 * std::max(0.0, m_meanSquare[k]));
    const bool tm = m_settings.polarization != 1;
    p.inPlaneU = tm ? m_solver.hx()[k] : m_solver.ex()[k];
    p.inPlaneV = tm ? m_solver.hy()[k] : m_solver.ey()[k];
    p.phasor = phasor;
    const std::size_t shown = std::min(m_traceCount, kTraceShown);
    const std::size_t every = std::max<std::size_t>(1, (shown + kTracePoints - 1) / kTracePoints);
    for (std::size_t n = 0; n < shown && trace.size() == kTraceCapacity; n += every)
    {
        const std::size_t at = (m_traceNext + kTraceCapacity - shown + n) % kTraceCapacity;
        p.trace.push_back(static_cast<double>(trace[at]));
    }
    p.traceStep = m_solver.dtS() * static_cast<double>(every);
    return p;
}

QColor WaveSimulation::signedColour(double t)
{
    // Cold for negative, warm for positive, black at zero.
    static constexpr Stop stops[] = {{0.00, 200, 240, 255}, {0.15, 96, 190, 250}, {0.32, 36, 108, 214},
                                     {0.45, 16, 38, 82},    {0.50, 9, 11, 15},    {0.55, 84, 22, 22},
                                     {0.68, 204, 62, 42},   {0.85, 250, 152, 62}, {1.00, 255, 242, 196}};
    return interpolate(stops, 9, t);
}

QColor WaveSimulation::magnitudeColour(double t)
{
    return FieldMapComputer::sequentialColour(t);
}
} // namespace qsig::app
