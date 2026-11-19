#pragma once

// Helpers for the scenes to describe their sources on a cutting plane (see SourceOutline).

#include "qsig/app/source_outline.h"
#include "qsig/app/view_plane.h"
#include "qsig/constants.h"

#include <cmath>
#include <initializer_list>
#include <limits>
#include <vector>

namespace qsig::app::detail
{
class OutlineBuilder
{
public:
    OutlineBuilder(ViewPlane plane, double offset) : m_plane(plane), m_offset(offset) {}

    ViewPlane plane() const { return m_plane; }
    // Signed distance of a world point to the view plane.
    double distance(const qsig::Vec3 &p) const
    {
        double u = 0.0, v = 0.0, w = 0.0;
        worldToPlane(m_plane, p, u, v, w);
        return w - m_offset;
    }
    QPointF project(const qsig::Vec3 &p) const
    {
        double u = 0.0, v = 0.0, w = 0.0;
        worldToPlane(m_plane, p, u, v, w);
        return {u, v};
    }
    // Projection that also widens the range of distances used for the gap.
    QPointF track(const qsig::Vec3 &p)
    {
        const double w = distance(p);
        m_wMin = std::min(m_wMin, w);
        m_wMax = std::max(m_wMax, w);
        return project(p);
    }
    // Gap from the tracked points: 0 when the plane passes between them.
    void gapFromTrackedPoints()
    {
        if (!std::isfinite(m_wMin)) return;
        o.gap = (m_wMin <= 0.0 && m_wMax >= 0.0) ? 0.0 : std::min(std::abs(m_wMin), std::abs(m_wMax));
    }

    static QPolygonF planeCircle(const QPointF &c, double r)
    {
        QPolygonF poly;
        for (int i = 0; i <= 72; ++i)
        {
            const double a = 2.0 * qsig::constants::pi * i / 72.0;
            poly << QPointF(c.x() + r * std::cos(a), c.y() + r * std::sin(a));
        }
        return poly;
    }

    // Points of a circle of the world (centre, unit vectors e1 and e2 spanning its plane).
    static std::vector<qsig::Vec3> circle(const qsig::Vec3 &c, const qsig::Vec3 &e1, const qsig::Vec3 &e2, double r,
                                          int segments = 96)
    {
        std::vector<qsig::Vec3> points;
        points.reserve(static_cast<std::size_t>(segments) + 1);
        for (int i = 0; i <= segments; ++i)
        {
            const double a = 2.0 * qsig::constants::pi * i / segments;
            points.push_back(c + (e1 * std::cos(a) + e2 * std::sin(a)) * r);
        }
        return points;
    }

    // A current filament along 'points' (in their order, current > 0 flows that way): drawn as a
    // polyline, with a dot/cross where it crosses the view plane and arrows at the given fractions.
    void addCurrentPath(const std::vector<qsig::Vec3> &points, double current,
                        std::initializer_list<double> arrowsAt = {})
    {
        if (points.size() < 2) return;
        QPolygonF line;
        for (const auto &p : points) line << track(p);
        o.lines << line;
        const double sense = current >= 0.0 ? 1.0 : -1.0;
        const qsig::Vec3 out = outOfScreen(m_plane);
        for (std::size_t i = 0; i + 1 < points.size(); ++i)
        {
            const double a = distance(points[i]), b = distance(points[i + 1]);
            if ((a < 0.0) == (b < 0.0) || a == b) continue;
            const double t = a / (a - b);
            const qsig::Vec3 at = points[i] + (points[i + 1] - points[i]) * t;
            const double along = qsig::dot(points[i + 1] - points[i], out) * sense;
            o.marks << SourceOutline::Mark{project(at), along > 0.0};
        }
        for (const double f : arrowsAt)
        {
            const auto i = std::min(points.size() - 2, static_cast<std::size_t>(f * double(points.size() - 1)));
            const QPointF a = project(points[i]), b = project(points[i + 1]);
            const double length = (points[i + 1] - points[i]).norm();
            if (length <= 0.0) continue;
            // Unit in 3D: its projected length (0..1) says how much the current runs along the plane.
            o.arrows << SourceOutline::Arrow{(a + b) * 0.5, (b - a) * (sense / length), false};
        }
    }

    SourceOutline o;

private:
    ViewPlane m_plane;
    double m_offset;
    double m_wMin = std::numeric_limits<double>::infinity();
    double m_wMax = -std::numeric_limits<double>::infinity();
};
} // namespace qsig::app::detail
