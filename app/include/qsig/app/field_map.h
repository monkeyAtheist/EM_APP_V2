#pragma once

// Background computation of a field map on a cutting plane, for any module (the scene provides a
// FieldSampler).
//
// One request = one grid of samples. Rows are evaluated in parallel with QtConcurrent::mapped.
// The result carries the colour image, contour segments, direction arrows, field lines and legend
// limits, all in grid coordinates.
//
// Requests made while a map is being computed: only the latest one waits, and the running map is
// delivered before it is served. A source being dragged asks for a map at every mouse move, faster
// than maps are computed: cancelling the running one each time would deliver nothing until the mouse
// stops. The running map is dropped only when it has become useless (another plane, quantity or
// theme) or when the new request is much cheaper (the coarse grid of an interaction that begins
// while a fine map is being computed).

#include "qsig/app/field_lines.h"
#include "qsig/app/scene_base.h"
#include "qsig/app/view_plane.h"

#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QImage>
#include <QLineF>
#include <QObject>
#include <QPointF>

#include <memory>
#include <vector>

namespace qsig::app
{
enum class FieldQuantity
{
    Magnitude = 0, // |E|, |B|: logarithmic colour scale
    Scalar = 1     // V, normal component of B...: diverging scale centred on 0
};

struct FieldMapRequest
{
    FieldSampler sampler;
    ViewPlane plane = ViewPlane::XY;
    FieldQuantity quantity = FieldQuantity::Magnitude;
    double offset = 0.0;       // w coordinate of the plane [m]
    double u0 = 0.0, v0 = 0.0; // world coordinates of sample (0, 0): top-left [m]
    double du = 0.01;          // sample spacing along u [m] (v decreases downwards by the same step)
    int columns = 0;
    int rows = 0;
    int contourLevels = 12;
    double lineSeparation = 0.0;   // spacing of the field lines [samples]; 0: no field lines
    bool scalarReferenced = false; // passed through to the legend
    bool lightGround = false;      // the view has a light background: see FieldMapComputer::mapColour
};

struct FieldArrow
{
    QPointF at;            // grid coordinates
    QPointF direction;     // unit vector in plane (u right, v up)
    double strength = 0.0; // 0..1 on the colour scale
};

struct FieldMapResult
{
    FieldMapRequest request;
    QImage image;                 // columns x rows, ARGB32
    std::vector<QLineF> contours; // grid coordinates (0..columns-1, 0..rows-1)
    std::vector<FieldArrow> arrows;
    std::vector<FieldLine> fieldLines; // grid coordinates, along the field
    double displayMin = 0.0;           // legend limits, in the displayed quantity
    double displayMax = 1.0;
    bool logarithmic = false;      // displayMin/Max are log10 values when true
    bool scalarReferenced = false; // the scalar is relative to the reference of an infinite source
    int validSamples = 0;
    double elapsedMs = 0.0;
};

class FieldMapComputer final : public QObject
{
    Q_OBJECT

public:
    explicit FieldMapComputer(QObject *parent = nullptr);
    ~FieldMapComputer() override;

    void request(FieldMapRequest request);
    // Drops the running computation and the waiting request: nothing is delivered for them (the view
    // shows another scene).
    void cancel();
    // A map is being computed, or is computed and not delivered yet.
    bool busy() const { return m_active; }

    // Colour scales shared with the legend. t in [0, 1].
    static QColor sequentialColour(double t); // black to pale yellow
    static QColor divergingColour(double t);  // blue, white at the middle, red
    // Colour of the map on the ground of the view. The weak end of a magnitude and the zero of a
    // signed quantity take the colour of the ground (white in the light theme, black in the dark
    // one): the map fades into the view instead of covering it with a sheet of the other tone.
    static QColor mapColour(FieldQuantity quantity, double t, bool lightGround);

signals:
    void ready(std::shared_ptr<const FieldMapResult> result);
    void busyChanged();

private:
    struct Row;
    void onFinished();

    void start(FieldMapRequest request);
    void dropRunning();

    QFutureWatcher<std::shared_ptr<Row>> m_watcher;
    FieldMapRequest m_running;
    // From the start of a computation until its 'finished' signal has been handled: the future itself
    // stops "running" earlier, and a request made in between must wait like the others.
    bool m_active = false;
    bool m_dropRunning = false; // the result of the running computation is not wanted
    bool m_hasPending = false;
    FieldMapRequest m_pending;
    QElapsedTimer m_clock;
};
} // namespace qsig::app
