#pragma once

// What the field view draws for one source on a cutting plane, in plane coordinates (u, v) [m].
// Built by each scene (it knows its source shapes); drawn, hit-tested and framed by the view.

#include <QColor>
#include <QList>
#include <QPointF>
#include <QPolygonF>

namespace qsig::app
{
struct SourceOutline
{
    enum class Marker
    {
        Dot,        // plain source position
        Plus,       // positive point charge
        Minus,      // negative point charge
        CurrentOut, // current towards the viewer (dot in a circle)
        CurrentIn,  // current away from the viewer (cross in a circle)
    };

    // A source that crosses the whole view (infinite line, cylinder or plane seen edge-on), drawn in
    // screen space: a band [from, to] of u (vertical) or v (horizontal); from == to is a line.
    struct Band
    {
        double from = 0.0, to = 0.0;
        bool vertical = true;
        int arrows = 0;       // +1 / -1: current along +v (vertical) or +u (horizontal), drawn as arrowheads
        int currentMarks = 0; // +1 / -1: current out of / into the screen along the band
    };

    // Where a current filament crosses the view plane.
    struct Mark
    {
        QPointF at;
        bool out = true;
    };

    // Direction of a current (or of a magnetic moment when 'large').
    struct Arrow
    {
        QPointF at;
        QPointF direction; // plane coordinates, not normalised: a short projection means "seen end-on"
        bool large = false;
    };

    // One gesture of a freehand region: a thick polyline (one point: a dot). Painted in order, an
    // erase stroke removing what the earlier ones painted.
    struct Stroke
    {
        QPolygonF points;
        double width = 0.0; // [m]
        bool erase = false;
    };

    QList<QPolygonF> filled; // closed regions, lightly filled
    QList<QPolygonF> lines;  // polylines (a closed curve repeats its first point)
    QList<Band> bands;
    QList<Mark> marks;
    QList<Arrow> arrows;
    QList<Stroke> strokes;
    QPointF centre;
    Marker marker = Marker::Dot;
    int sign = 0;          // +1 / -1 / 0: colour (charge sign, current direction)
    QColor colour;         // when valid, replaces the colour of the sign (materials)
    bool solid = false;    // opaque fill (conductors)
    bool infinite = false; // never hit by an area test, not used to frame the view
    // Distance from the view plane to the nearest point of the source; 0 when the plane cuts it.
    // A source off the plane is drawn as a dashed projection.
    double gap = 0.0;
};
} // namespace qsig::app
