#pragma once

// Evenly spaced field lines of a 2D direction field sampled on a grid, after Jobard and Lefer
// ("Creating evenly-spaced streamlines of arbitrary density", 1997), simplified. Plain C++.
//
// The lines follow the field projected on the view plane: they are true field lines whenever the
// field lies in that plane (a wire seen end-on, a loop cut through its axis...), and a projection
// otherwise. Magnetic lines usually close on themselves; such loops are returned closed.

#include <vector>

namespace qsig::app
{
struct GridPoint
{
    double c = 0.0; // column (rightwards)
    double r = 0.0; // row (downwards)
};

struct FieldLine
{
    std::vector<GridPoint> points; // in the direction of the field
    bool closed = false;           // the last point repeats the first
};

struct FieldLineSettings
{
    double separation = 12.0; // distance between neighbouring lines [grid units]
    double step = 0.5;        // integration step [grid units]
    int maxLines = 600;
};

// fc, fr: field components along the columns and rows (row-major, columns x rows); valid: 0 where
// the field is undefined (on a source). Only the directions matter.
std::vector<FieldLine> traceFieldLines(int columns, int rows, const std::vector<float> &fc,
                                       const std::vector<float> &fr, const std::vector<unsigned char> &valid,
                                       const FieldLineSettings &settings);
} // namespace qsig::app
