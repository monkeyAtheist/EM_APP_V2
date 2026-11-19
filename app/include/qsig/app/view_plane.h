#pragma once

// Cutting planes of the field views and the mapping between plane coordinates (u, v, w) and the
// world (x, y, z). u points right, v up; w is the offset of the plane.

#include "qsig/vec3.h"

namespace qsig::app
{
enum class ViewPlane
{
    XY, // u = x, v = y, w = z
    XZ, // u = x, v = z, w = y
    YZ, // u = y, v = z, w = x
};

inline qsig::Vec3 planeToWorld(ViewPlane plane, double u, double v, double w)
{
    switch (plane)
    {
    case ViewPlane::XZ:
        return {u, w, v};
    case ViewPlane::YZ:
        return {w, u, v};
    case ViewPlane::XY:
        break;
    }
    return {u, v, w};
}

inline void worldToPlane(ViewPlane plane, const qsig::Vec3 &p, double &u, double &v, double &w)
{
    switch (plane)
    {
    case ViewPlane::XZ:
        u = p.x;
        v = p.z;
        w = p.y;
        return;
    case ViewPlane::YZ:
        u = p.y;
        v = p.z;
        w = p.x;
        return;
    case ViewPlane::XY:
        break;
    }
    u = p.x;
    v = p.y;
    w = p.z;
}

// Unit vector pointing out of the screen, towards the viewer (u x v): +z on XY, -y on XZ, +x on YZ.
// A current along it is drawn as a dot (out), against it as a cross (in).
inline qsig::Vec3 outOfScreen(ViewPlane plane)
{
    switch (plane)
    {
    case ViewPlane::XZ:
        return {0.0, -1.0, 0.0};
    case ViewPlane::YZ:
        return {1.0, 0.0, 0.0};
    case ViewPlane::XY:
        break;
    }
    return {0.0, 0.0, 1.0};
}
} // namespace qsig::app
