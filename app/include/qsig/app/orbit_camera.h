#pragma once

// Orthographic camera of the 3D views, turning around the scene. Plain C++ (no Qt): the views draw
// with it, the tests check its geometry.
//
// Screen axes in world coordinates: right() to the right, up() upwards, forward() away from the viewer
// (the depth of project() grows along it). yaw turns the scene about z, pitch tilts it: pitch > 0 is
// seen from above, pitch = 90 from straight above. yaw = pitch = 0 looks along +y, x to the right and
// z up: the XZ plane of the 2D views; pitch = 90 is their XY plane, yaw = 90 their YZ plane.

#include "qsig/vec3.h"

#include <algorithm>
#include <cmath>

namespace qsig::app
{
struct OrbitCamera
{
    double yaw = 35.0, pitch = 20.0; // [deg]
    double zoom = 1.0;
    double panX = 0.0, panY = 0.0; // [px]
    // What is looked at, and the viewport it is drawn in.
    qsig::Vec3 centre;
    double radius = 1.0;              // half-size of the scene [m]
    double width = 1.0, height = 1.0; // [px]

    struct ScreenPoint
    {
        double x = 0.0, y = 0.0, depth = 0.0;
    };

    // The axes and scale of one picture, computed once for all its points.
    struct Frame
    {
        qsig::Vec3 right, up, forward, centre;
        double scale = 1.0; // pixels per metre
        double originX = 0.0, originY = 0.0;

        ScreenPoint project(const qsig::Vec3 &p) const
        {
            const qsig::Vec3 d = p - centre;
            return {originX + scale * qsig::dot(d, right), originY - scale * qsig::dot(d, up), qsig::dot(d, forward)};
        }
        // World point under the pixel (x, y), 'depth' metres behind the plane facing the viewer
        // through the centre.
        qsig::Vec3 unproject(double x, double y, double depth = 0.0) const
        {
            return centre + right * ((x - originX) / scale) + up * (-(y - originY) / scale) + forward * depth;
        }
        // Point of the plane (through 'origin', normal 'normal') under the pixel; false when the plane
        // is seen too close to edge-on.
        bool pick(double x, double y, const qsig::Vec3 &origin, const qsig::Vec3 &normal, qsig::Vec3 &out,
                  double minimumCosine = 0.05) const
        {
            const double length = qsig::norm(normal);
            if (!(length > 0.0)) return false;
            const qsig::Vec3 n = normal / length;
            const double along = qsig::dot(forward, n);
            if (std::abs(along) < minimumCosine) return false;
            const qsig::Vec3 start = unproject(x, y);
            out = start + forward * (qsig::dot(origin - start, n) / along);
            return true;
        }
    };

    static constexpr double kPi = 3.14159265358979323846;

    double scale() const { return zoom * 0.34 * std::min(width, height) / std::max(radius, 1e-12); }

    Frame frame() const
    {
        const double y = yaw * kPi / 180.0, p = pitch * kPi / 180.0;
        const double cy = std::cos(y), sy = std::sin(y), cp = std::cos(p), sp = std::sin(p);
        Frame f;
        f.right = {cy, sy, 0.0};
        f.up = {-sp * sy, sp * cy, cp};
        f.forward = {-cp * sy, cp * cy, -sp};
        f.centre = centre;
        f.scale = scale();
        f.originX = 0.5 * width + panX;
        f.originY = 0.5 * height + panY;
        return f;
    }

    // Mouse motion in pixels: turn (0.4 degree per pixel), slide, zoom about a pixel.
    void orbit(double dx, double dy)
    {
        yaw = std::fmod(yaw - 0.4 * dx, 360.0);
        pitch = std::clamp(pitch + 0.4 * dy, -90.0, 90.0);
    }
    void pan(double dx, double dy)
    {
        panX += dx;
        panY += dy;
    }
    // The world point under (x, y) stays there.
    void zoomAt(double x, double y, double factor)
    {
        const double next = std::clamp(zoom * factor, 0.05, 80.0);
        const double applied = next / zoom;
        const double cx = 0.5 * width, cy = 0.5 * height;
        panX = (1.0 - applied) * (x - cx) + applied * panX;
        panY = (1.0 - applied) * (y - cy) + applied * panY;
        zoom = next;
    }
    void reset()
    {
        yaw = 35.0;
        pitch = 20.0;
        zoom = 1.0;
        panX = panY = 0.0;
    }
    // The whole scene in the viewport again, seen from the same side.
    void fit()
    {
        zoom = 1.0;
        panX = panY = 0.0;
    }
    // Arrows around the view cube: the viewer goes to the right (yawSteps > 0) or the left, up
    // (pitchSteps > 0) or down, by steps of 45 degrees. The result is on the 45-degree grid, which holds
    // the views from the front, the sides, the back and the top: a view off the grid joins its nearest
    // grid angle first, so that a step is never a small correction. The top and the bottom are ends.
    void step(int yawSteps, int pitchSteps)
    {
        auto onGrid = [](double angle, int steps) { return (std::round(angle / 45.0) + steps) * 45.0; };
        if (yawSteps != 0) yaw = std::fmod(onGrid(yaw, yawSteps), 360.0);
        if (pitchSteps != 0) pitch = std::clamp(onGrid(pitch, pitchSteps), -90.0, 90.0);
    }

    // Exact view from one side of an axis (0 x, 1 y, 2 z): from above (z, +), from below (z, -), from
    // the front (y, -: the viewer stands at -y), the back (y, +), the right (x, +), the left (x, -).
    void lookFrom(int axis, int side)
    {
        if (axis == 2)
        {
            yaw = 0.0;
            pitch = side >= 0 ? 90.0 : -90.0;
        }
        else if (axis == 1)
        {
            yaw = side >= 0 ? 180.0 : 0.0;
            pitch = 0.0;
        }
        else
        {
            yaw = side >= 0 ? 90.0 : -90.0;
            pitch = 0.0;
        }
    }

    // The viewer on the side of 'normal', looking along -normal: a plane of that normal is seen face on.
    // Straight up or down, the yaw is kept.
    void faceTowards(const qsig::Vec3 &normal)
    {
        const double length = qsig::norm(normal);
        if (!(length > 0.0)) return;
        const qsig::Vec3 n = normal / length;
        // forward = (-cos p sin y, cos p cos y, -sin p) = -n.
        pitch = std::asin(std::clamp(n.z, -1.0, 1.0)) * 180.0 / kPi;
        if (std::hypot(n.x, n.y) > 1e-9) yaw = std::atan2(n.x, -n.y) * 180.0 / kPi;
    }

    // Plane of the 2D views that faces the viewer exactly: 0 XY, 1 XZ, 2 YZ; -1 for any other view.
    int facingPlane() const
    {
        const qsig::Vec3 f = frame().forward;
        constexpr double tolerance = 1e-6;
        if (std::abs(std::abs(f.z) - 1.0) < tolerance) return 0;
        if (std::abs(std::abs(f.y) - 1.0) < tolerance) return 1;
        if (std::abs(std::abs(f.x) - 1.0) < tolerance) return 2;
        return -1;
    }
};
} // namespace qsig::app
