#pragma once

// Shape in space of a source of the static modules, for the 3D field views: wires to stroke, flat
// faces to fill lightly, arrows giving the direction of a current or of a moment. Plain C++.

#include "qsig/electrostatics.h"
#include "qsig/magnetostatics.h"
#include "qsig/vec3.h"

#include <vector>

namespace qsig::app
{
struct SourceShape3D
{
    using Path = std::vector<qsig::Vec3>;
    struct Arrow
    {
        qsig::Vec3 at, direction; // direction: unit vector
        bool large = false;       // a magnetic moment or a sheet current, drawn longer
    };
    std::vector<Path> lines; // polylines; a closed curve repeats its first point
    std::vector<Path> faces; // flat closed regions (the first point is not repeated)
    std::vector<Arrow> arrows;
    qsig::Vec3 centre;
    double sphereRadius = 0.0; // > 0: a ball around the centre ('lines' holds its three great circles)
    bool point = false;        // point-like source: a marker at the centre
    bool infinite = false;     // extends beyond the view: drawn up to 'extent' around the view centre
    int sign = 0;              // sign of the strength (colour)
};

// 'viewCentre' and 'extent' only matter for the sources that extend to infinity.
SourceShape3D sourceShape(const Electrostatics::Source &s, const qsig::Vec3 &viewCentre, double extent);
SourceShape3D sourceShape(const Magnetostatics::Source &s, const qsig::Vec3 &viewCentre, double extent);

// Radius of the sphere around its position that holds a source (the cross-section only for the ones
// extending to infinity): what the view has to frame.
double sourceReach(const Electrostatics::Source &s);
double sourceReach(const Magnetostatics::Source &s);
} // namespace qsig::app
