#pragma once

// Time-domain simulation behind the "Ondes" module: a scene of sources and obstacles rasterised on
// the 2D FDTD solver (Yee scheme). No Qt threads here: WaveScene runs it on a worker, one call at a
// time; the tests drive it directly.
//
// The simulated area is a rectangle centred on the origin. The grid adds the absorbing layers
// (CPML) around it, so the whole area is physical. Its step follows the highest significant
// frequency of the sources (cells per wavelength), within a cell budget.

#include "qsig/app/live_frame.h"
#include "qsig/fdtd_solver.h"
#include "qsig/vec3.h"

#include <QColor>

#include <chrono>
#include <complex>
#include <memory>
#include <string>
#include <vector>

namespace qsig::app
{
// One gesture of a freehand drawing: a thick polyline (a single point is a dot).
struct WaveStroke
{
    std::vector<double> points; // x0, y0, x1, y1... relative to the position of the drawing [m]
    double width = 0.04;        // [m]
    bool erase = false;         // removes what the earlier strokes of the drawing painted
};

struct WaveObject
{
    enum class Type
    {
        PointSource,
        LineSource,
        DielectricBlock,
        DielectricDisk,
        ConductorBlock,
        ConductorDisk,
        SlitWall,
        DielectricDrawing, // freehand areas painted with the brush
        ConductorDrawing,
    };

    std::string name;
    Type type = Type::PointSource;
    qsig::Vec3 position;   // centre [m]; z is not used
    double strength = 1.0; // source amplitude: V/m in TMz (Ez), A/m in TEz (Hz); unused by obstacles

    // Sources.
    double frequency = 1.0e9; // [Hz]
    double phaseDeg = 0.0;
    int waveform = 0;  // FDTD::SourceWaveform: 0 continuous sine, 1 Gaussian pulse, 2 Ricker wavelet
    int injection = 0; // FDTD::SourceInjection: 0 soft (added to the field), 1 hard (imposed)

    // Shapes [m]. angleDeg: direction of a line source or a wall (90: vertical), rotation of a block.
    double width = 0.4, height = 0.3;
    double radius = 0.2;
    double length = 1.0;
    double thickness = 0.05;
    double angleDeg = 0.0;
    int slits = 2;
    double slitWidth = 0.06, slitSpacing = 0.3;

    // Dielectric media.
    double epsilonR = 4.0, muR = 1.0, conductivity = 0.0; // conductivity [S/m]

    // Freehand drawings, painted and erased in order.
    std::vector<WaveStroke> strokes;

    bool isSource() const { return type == Type::PointSource || type == Type::LineSource; }
    bool isConductor() const
    {
        return type == Type::ConductorBlock || type == Type::ConductorDisk || type == Type::SlitWall ||
               type == Type::ConductorDrawing;
    }
    bool isDielectric() const
    {
        return type == Type::DielectricBlock || type == Type::DielectricDisk || type == Type::DielectricDrawing;
    }
    bool isDrawing() const { return type == Type::DielectricDrawing || type == Type::ConductorDrawing; }
};

struct WaveSettings
{
    double width = 3.0, height = 2.0; // simulated area, centred on the origin [m]
    double cellsPerWavelength = 20.0; // in vacuum, at the highest significant source frequency
    int polarization = 0;             // FDTD::Polarization: 0 TMz (Ez, Hx, Hy), 1 TEz (Hz, Ex, Ey)
    int boundary = 0;                 // FDTD::BoundaryCondition: 0 CPML, 1 Mur first order, 2 PEC
    bool operator==(const WaveSettings &) const = default;
};

// Mesh derived from the settings and the sources.
struct WaveGrid
{
    double dx = 0.0, dy = 0.0;
    int nx = 0, ny = 0;         // whole grid, absorbing layers included
    int pad = 0;                // cells on each side outside the simulated area
    int areaNx = 0, areaNy = 0; // cells of the simulated area
    double x0 = 0.0, y0 = 0.0;  // position of node (0, 0) [m]; node (i, j) is at x0 + i dx, y0 + j dy
    double width = 0.0, height = 0.0;
    double frequency = 0.0;          // frequency the mesh is built for [Hz]
    double cellsPerWavelength = 0.0; // achieved, in vacuum, at that frequency
    bool limited = false;            // the cell budget lowered the resolution
    bool sameMesh(const WaveGrid &o) const
    {
        return dx == o.dx && dy == o.dy && nx == o.nx && ny == o.ny && pad == o.pad && x0 == o.x0 && y0 == o.y0;
    }
};

inline constexpr int kWaveMaxCells = 480000;
inline constexpr int kWaveCpmlCells = 12;
inline constexpr int kWaveMinAreaCells = 24; // across the smaller side of the area

// Highest frequency the excitation carries significantly: pulses spread above their carrier.
double significantFrequency(const std::vector<WaveObject> &objects);
double maxRefractiveIndex(const std::vector<WaveObject> &objects);
WaveGrid waveGrid(const WaveSettings &settings, const std::vector<WaveObject> &objects);

struct WaveProbe
{
    bool valid = false;
    int i = 0, j = 0;
    double primary = 0.0;   // Ez (TMz) or Hz (TEz)
    double amplitude = 0.0; // sqrt(2) x running RMS of the primary field (last two periods or so)
    double inPlaneU = 0.0, inPlaneV = 0.0; // Hx, Hy (TMz) or Ex, Ey (TEz)
    // Complex amplitude of the primary field at the phase frequency (v = |P| cos(wt + arg P)),
    // averaged over the last few periods.
    std::complex<double> phasor;
    std::vector<double> trace; // recent primary values, oldest first
    double traceStep = 0.0;    // time between trace samples [s]
};

struct WaveFrame
{
    std::shared_ptr<const LiveFrame> picture;
    WaveGrid grid;
    int polarization = 0;
    double time = 0.0, dt = 0.0;
    std::size_t steps = 0;
    double energy = 0.0;     // per metre along z, simulated area [J/m]
    double msPerStep = 0.0;  // measured cost of one time step
    WaveProbe probe;         // point A
    WaveProbe probeB;        // second point, for differential measurements
    bool hasProbeB = false;
    // Frequency of the phasors: the first sinusoidal source, 0 when none (no phase to measure).
    double phaseFrequency = 0.0;
};

class WaveSimulation
{
public:
    WaveSimulation();

    // Applies the scene. Returns true when the mesh or the polarisation changed: the fields then
    // restart from zero. Obstacles and sources otherwise change under the running waves.
    bool update(const WaveSettings &settings, const std::vector<WaveObject> &objects);
    void reset(); // fields, time, averages and colour scales back to zero
    void advance(int steps);
    // Measurement points: A, and B when 'useB'. Moving one restarts the traces and the phasors.
    void setProbes(double ax, double ay, bool useB, double bx, double by);
    void setProbe(double x, double y) { setProbes(x, y, m_useB, m_probeBX, m_probeBY); }
    // Pictures and readings of the current state. Not const: the colour scales follow the frames.
    WaveFrame frame();

    const WaveGrid &grid() const { return m_grid; }
    const FDTD::Solver2D &solver() const { return m_solver; }
    const std::vector<FDTD::Source> &sources() const { return m_solver.sources(); }
    bool nodeAt(double x, double y, int &i, int &j) const; // nearest node, false outside the area
    double primaryAt(double x, double y) const;            // 0 outside the area
    double amplitudeAt(double x, double y) const;
    bool conductorAt(double x, double y) const;
    double energy() const;

    // Colour scales of the pictures and the legend, t in [0, 1]: dark at zero, so that waves read
    // as light on a dark background.
    static QColor signedColour(double t);
    static QColor magnitudeColour(double t);

private:
    void rasterise(const std::vector<WaveObject> &objects);
    void stampDrawing(const WaveObject &o, const FDTD::Material &material, std::vector<FDTD::Material> &raster) const;
    WaveProbe probeAt(double x, double y, const std::vector<float> &trace, std::complex<double> phasor) const;
    void placeSources(const std::vector<WaveObject> &objects);
    void clearAverages();
    double percentile(const std::vector<double> &field, bool square, double fraction) const;

    FDTD::Solver2D m_solver;
    WaveGrid m_grid;
    WaveSettings m_settings;
    bool m_configured = false;
    std::vector<FDTD::Material> m_raster;
    // Running mean of the primary field squared, in two cascaded stages of one period each: the 2w
    // ripple of a single stage would show as a +-2 % pattern on the amplitude map.
    std::vector<double> m_meanSquareStage, m_meanSquare;
    double m_decay = 0.0; // of each stage, per step
    // Ring buffers of the primary field at the probes (same steps for both).
    std::vector<float> m_trace, m_traceB;
    std::size_t m_traceNext = 0, m_traceCount = 0;
    double m_probeX = 0.0, m_probeY = 0.0;
    bool m_useB = false;
    double m_probeBX = 0.0, m_probeBY = 0.0;
    // Two cascaded running averages: the 2w ripple of a single one would shift the phase by a degree.
    std::complex<double> m_phasorStage, m_phasorStageB, m_phasor, m_phasorB;
    double m_phaseFrequency = 0.0, m_phaseKeep = 0.0;
    double m_scale[2] = {0.0, 0.0};
    double m_msPerStep = 0.0;
};
} // namespace qsig::app
