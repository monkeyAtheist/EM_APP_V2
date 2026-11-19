#pragma once

// Latest picture of a time-domain module: the field view paints it as it comes instead of computing
// a map. Immutable once published (shared between the worker that renders it and the view).

#include <QImage>

namespace qsig::app
{
struct LiveFrame
{
    // One pixel per grid cell. Quantity 0: magnitude (sequential scale from 0 to scale[0]);
    // quantity 1: signed value (diverging scale from -scale[1] to +scale[1]).
    QImage images[2];
    double scale[2] = {1.0, 1.0};
    // Plane rectangle covered by the images [m]; the first image row is at yMax.
    double xMin = 0.0, xMax = 1.0, yMin = 0.0, yMax = 1.0;
    // How the simulated area ends (drawn on its border): 0 absorbing layer, 1 simple absorbing
    // condition, 2 perfect conductor.
    int boundary = 0;
};
} // namespace qsig::app
