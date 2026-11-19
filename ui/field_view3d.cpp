#include "field_view3d.h"

#include "qsig/app/units.h"
#include "qsig/app/view_plane.h"

#include <QHoverEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QRadialGradient>
#include <QTransform>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <utility>

using qsig::Vec3;
using qsig::app::FieldMapComputer;
using qsig::app::FieldMapRequest;
using qsig::app::FieldQuantity;
using qsig::app::SceneBase;
using qsig::app::SourceShape3D;
using Frame = qsig::app::OrbitCamera::Frame;

namespace
{
constexpr double kPlaneHalf = 1.35; // half-size of the drawn cutting plane, in scene radii
constexpr int kMapSamples = 181;    // samples of the map along each side of the plane
constexpr int kLattice = 6;         // field vectors along each axis

// In-plane axes (u, v of view_plane.h) and normal (w) of a cutting plane.
struct PlaneAxes
{
    Vec3 a, b, normal;
};
PlaneAxes planeAxes(qsig::app::ViewPlane plane)
{
    using qsig::app::planeToWorld;
    return {planeToWorld(plane, 1.0, 0.0, 0.0), planeToWorld(plane, 0.0, 1.0, 0.0), planeToWorld(plane, 0.0, 0.0, 1.0)};
}

QColor withAlpha(QColor c, int alpha)
{
    c.setAlpha(alpha);
    return c;
}

// Same colours as the 2D view: positive charge or current red, negative blue.
QColor signColour(int sign)
{
    return sign > 0 ? QColor(255, 96, 96) : sign < 0 ? QColor(90, 160, 255) : QColor(176, 184, 196);
}

// 1, 2 or 5 times a power of ten, at least 'raw'.
double niceStep(double raw)
{
    const double e = std::pow(10.0, std::floor(std::log10(std::max(raw, 1e-30))));
    for (const double m : {1.0, 2.0, 5.0, 10.0})
        if (m * e >= raw) return m * e;
    return 10.0 * e;
}

double distanceToSegment(const QPointF &p, const QPointF &a, const QPointF &b)
{
    const QPointF ab = b - a, ap = p - a;
    const double len2 = QPointF::dotProduct(ab, ab);
    const double t = len2 > 0.0 ? std::clamp(QPointF::dotProduct(ap, ab) / len2, 0.0, 1.0) : 0.0;
    const QPointF d = p - (a + t * ab);
    return std::hypot(d.x(), d.y());
}

QVector3D toVector(const Vec3 &v)
{
    return {static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)};
}

Vec3 fromVector(const QVector3D &v)
{
    return {static_cast<double>(v.x()), static_cast<double>(v.y()), static_cast<double>(v.z())};
}

// What the view draws, in screen space, with the depth used to paint far things first.
struct Prim
{
    enum class Kind
    {
        Path,     // polyline
        Polygon,  // filled
        Segments, // pairs of points
        Disc,     // points[0], radius
        Label,    // text at points[0]
    };
    Kind kind = Kind::Path;
    QPolygonF points;
    QPen pen;
    QBrush brush;
    double radius = 0.0;
    QString text;
    double depth = 0.0;
};

// Two layers of primitives: behind the cutting plane and in front of it (seen from the viewer). Lines
// and faces that cross the plane are cut along it. Without a plane to split on, everything is in front.
class Layers
{
public:
    Layers(const Frame &frame, const Vec3 &origin, const Vec3 &normal, bool split)
        : m_frame(frame), m_origin(origin), m_normal(normal), m_split(split),
          m_sign(qsig::dot(frame.forward, normal) < 0.0 ? 1.0 : -1.0)
    {
    }

    QPointF screen(const Vec3 &q, double *depth = nullptr) const
    {
        const auto s = m_frame.project(q);
        if (depth) *depth = s.depth;
        return {s.x, s.y};
    }
    // > 0 on the viewer's side of the plane.
    double side(const Vec3 &q) const { return m_split ? qsig::dot(q - m_origin, m_normal) * m_sign : 1.0; }

    void path(const std::vector<Vec3> &points, const QPen &pen)
    {
        if (points.size() < 2) return;
        Prim run;
        bool runFront = side(points[0]) >= 0.0;
        double depthSum = 0.0;
        auto begin = [&](bool frontSide)
        {
            run = Prim{};
            run.kind = Prim::Kind::Path;
            run.pen = pen;
            runFront = frontSide;
            depthSum = 0.0;
        };
        auto push = [&](const Vec3 &q)
        {
            double depth = 0.0;
            run.points << screen(q, &depth);
            depthSum += depth;
        };
        auto flush = [&]
        {
            if (run.points.size() < 2) return;
            run.depth = depthSum / static_cast<double>(run.points.size());
            m_layer[runFront ? 1 : 0].push_back(std::move(run));
        };
        begin(runFront);
        push(points[0]);
        for (std::size_t i = 1; i < points.size(); ++i)
        {
            const double s0 = side(points[i - 1]), s1 = side(points[i]);
            const bool nextFront = s1 >= 0.0;
            if (nextFront != runFront)
            {
                const Vec3 crossing = points[i - 1] + (points[i] - points[i - 1]) * (s0 / (s0 - s1));
                push(crossing);
                flush();
                begin(nextFront);
                push(crossing);
            }
            push(points[i]);
        }
        flush();
    }

    void face(const std::vector<Vec3> &points, const QBrush &brush)
    {
        const std::size_t n = points.size();
        if (n < 3) return;
        for (int layer = 0; layer < 2; ++layer)
        {
            if (!m_split && layer == 0) continue;
            const double keep = layer == 1 ? 1.0 : -1.0;
            Prim prim;
            prim.kind = Prim::Kind::Polygon;
            prim.pen = Qt::NoPen;
            prim.brush = brush;
            double depthSum = 0.0;
            auto push = [&](const Vec3 &q)
            {
                double depth = 0.0;
                prim.points << screen(q, &depth);
                depthSum += depth;
            };
            for (std::size_t i = 0; i < n; ++i)
            {
                const Vec3 &a = points[i], &b = points[(i + 1) % n];
                const double sa = side(a) * keep, sb = side(b) * keep;
                if (sa >= 0.0) push(a);
                if ((sa >= 0.0) != (sb >= 0.0)) push(a + (b - a) * (sa / (sa - sb)));
            }
            if (prim.points.size() < 3) continue;
            prim.depth = depthSum / static_cast<double>(prim.points.size());
            m_layer[layer].push_back(std::move(prim));
        }
    }

    // A disc of fixed size on screen (markers) or of a radius in pixels (balls); 'reach': distance in
    // metres under which it still shows in front of the plane although its centre is behind.
    void disc(const Vec3 &centre, double radiusPx, const QBrush &brush, const QPen &pen, double reach = 0.0)
    {
        Prim prim;
        prim.kind = Prim::Kind::Disc;
        prim.pen = pen;
        prim.brush = brush;
        prim.radius = radiusPx;
        prim.points << screen(centre, &prim.depth);
        m_layer[side(centre) >= -reach ? 1 : 0].push_back(std::move(prim));
    }

    void label(const Vec3 &at, const QPointF &offset, const QString &text, const QColor &colour)
    {
        Prim prim;
        prim.kind = Prim::Kind::Label;
        prim.pen = QPen(colour);
        prim.text = text;
        prim.points << screen(at, &prim.depth) + offset;
        m_layer[side(at) >= 0.0 ? 1 : 0].push_back(std::move(prim));
    }

    // Shaft from 'tail' to 'tip' with a head of 'head' pixels; a dot when it points at the viewer.
    void arrow(const Vec3 &tail, const Vec3 &tip, const QPen &pen, double head)
    {
        Prim prim;
        prim.kind = Prim::Kind::Segments;
        prim.pen = pen;
        double d0 = 0.0, d1 = 0.0;
        const QPointF s0 = screen(tail, &d0), s1 = screen(tip, &d1);
        prim.depth = 0.5 * (d0 + d1);
        const QPointF d = s1 - s0;
        const double length = std::hypot(d.x(), d.y());
        prim.points << s0 << s1;
        if (length > 2.5)
        {
            const QPointF u = d / length, n(-u.y(), u.x());
            const double size = std::min(head, 0.6 * length);
            prim.points << s1 << s1 - size * u + 0.55 * size * n << s1 << s1 - size * u - 0.55 * size * n;
        }
        else
        {
            prim.kind = Prim::Kind::Disc;
            prim.radius = 1.6;
            prim.brush = pen.color();
            prim.points = QPolygonF{s1};
        }
        m_layer[side(tail) >= 0.0 ? 1 : 0].push_back(std::move(prim));
    }

    // Arrowhead alone at 'at', along 'direction' (unit vector): the sense of a current on its wire.
    void chevron(const Vec3 &at, const Vec3 &direction, const QPen &pen, double size, double probeLength)
    {
        double depth = 0.0;
        const QPointF tip = screen(at, &depth);
        const QPointF d = screen(at + direction * probeLength) - tip;
        const double length = std::hypot(d.x(), d.y());
        if (!(length > 1e-9)) return;
        // Seen nearly end-on, the head would point anywhere: left out.
        const double scalePx = m_frame.scale * probeLength;
        if (length < 0.25 * scalePx) return;
        const QPointF u = d / length, n(-u.y(), u.x());
        const QPointF front = tip + 0.5 * size * u;
        Prim prim;
        prim.kind = Prim::Kind::Segments;
        prim.pen = pen;
        prim.depth = depth - 1e-9; // just over the wire it sits on
        prim.points << front << front - size * u + 0.6 * size * n << front << front - size * u - 0.6 * size * n;
        m_layer[side(at) >= 0.0 ? 1 : 0].push_back(std::move(prim));
    }

    // Far primitives first; equal depths keep their order (a halo stays under its line).
    void draw(QPainter &p, int layer)
    {
        auto &prims = m_layer[layer];
        std::stable_sort(prims.begin(), prims.end(), [](const Prim &a, const Prim &b) { return a.depth > b.depth; });
        for (const Prim &prim : prims)
        {
            p.setPen(prim.pen);
            p.setBrush(prim.brush);
            switch (prim.kind)
            {
            case Prim::Kind::Path:
                p.setBrush(Qt::NoBrush);
                p.drawPolyline(prim.points);
                break;
            case Prim::Kind::Polygon:
                p.drawPolygon(prim.points);
                break;
            case Prim::Kind::Segments:
                p.drawLines(prim.points.constData(), static_cast<int>(prim.points.size() / 2));
                break;
            case Prim::Kind::Disc:
                p.drawEllipse(prim.points[0], prim.radius, prim.radius);
                break;
            case Prim::Kind::Label:
                p.drawText(prim.points[0], prim.text);
                break;
            }
        }
        p.setBrush(Qt::NoBrush);
    }

private:
    Frame m_frame;
    Vec3 m_origin, m_normal;
    bool m_split;
    double m_sign;
    std::vector<Prim> m_layer[2]; // 0 behind the plane, 1 in front
};

void drawProbe(QPainter &p, const QPointF &c, const QColor &colour, const QString &label)
{
    p.setPen(QPen(colour, 2.0));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(c, 7.0, 7.0);
    p.drawLine(c + QPointF(-12, 0), c + QPointF(-4, 0));
    p.drawLine(c + QPointF(4, 0), c + QPointF(12, 0));
    p.drawLine(c + QPointF(0, -12), c + QPointF(0, -4));
    p.drawLine(c + QPointF(0, 4), c + QPointF(0, 12));
    if (!label.isEmpty()) p.drawText(c + QPointF(11, -10), label);
}
} // namespace

FieldView3D::FieldView3D(QQuickItem *parent) : QQuickPaintedItem(parent)
{
    setAntialiasing(true);
    setFlag(ItemHasContents, true);
    setAcceptedMouseButtons(Qt::LeftButton | Qt::MiddleButton | Qt::RightButton);
    setAcceptHoverEvents(true);
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(0);
    connect(&m_debounce, &QTimer::timeout, this, &FieldView3D::compute);
    connect(&m_computer, &FieldMapComputer::ready, this,
            [this](std::shared_ptr<const qsig::app::FieldMapResult> result)
            {
                m_result = std::move(result);
                emit resultChanged();
                update();
            });
    connect(&m_computer, &FieldMapComputer::busyChanged, this, &FieldView3D::busyChanged);
    connect(this, &FieldView3D::viewChanged, this,
            [this]
            {
                scheduleCompute();
                update();
            });
    connect(this, &FieldView3D::overlayChanged, this, [this] { update(); });
    connect(this, &FieldView3D::cameraChanged, this, [this] { update(); });
}

void FieldView3D::setScene(QObject *object)
{
    auto *scene = qobject_cast<SceneBase *>(object);
    if (m_scene == scene) return;
    if (m_scene) disconnect(m_scene.data(), nullptr, this, nullptr);
    m_scene = scene;
    if (scene) connect(scene, &SceneBase::sceneChanged, this, &FieldView3D::invalidate);
    m_result.reset(); // never show the map of another module
    m_computer.cancel();
    m_dragRow = -1;
    m_dragMoved = false;
    emit resultChanged();
    emit sceneChanged();
    invalidate();
}

void FieldView3D::invalidate()
{
    m_latticeDirty = true;
    scheduleCompute();
    update();
}

void FieldView3D::setPlane(int plane)
{
    const auto next = static_cast<ViewPlane>(std::clamp(plane, 0, 2));
    if (next == m_plane) return;
    m_plane = next;
    emit viewChanged();
}

void FieldView3D::setPlaneOffset(double offset)
{
    if (!std::isfinite(offset) || offset == m_offset) return;
    m_offset = offset;
    emit viewChanged();
}

void FieldView3D::setQuantity(int quantity)
{
    const auto next = quantity == 1 ? FieldQuantity::Scalar : FieldQuantity::Magnitude;
    if (next == m_quantity) return;
    m_quantity = next;
    emit viewChanged();
}

void FieldView3D::setShowFieldLines(bool show)
{
    if (show == m_showFieldLines) return;
    m_showFieldLines = show;
    emit viewChanged();
}

void FieldView3D::setCubeTop(double top)
{
    if (top == m_nav.cubeTop) return;
    m_nav.cubeTop = top;
    emit overlayChanged();
}

void FieldView3D::resetView()
{
    m_nav.camera.reset();
    emit cameraChanged();
}

void FieldView3D::lookFrom(int axis, int side)
{
    m_nav.camera.lookFrom(std::clamp(axis, 0, 2), side >= 0 ? +1 : -1);
    emit cameraChanged();
}

void FieldView3D::facePlane()
{
    // XY from above, XZ from the front, YZ from the right: as the 2D view shows them.
    if (m_plane == ViewPlane::XY)
        m_nav.camera.lookFrom(2, +1);
    else if (m_plane == ViewPlane::XZ)
        m_nav.camera.lookFrom(1, -1);
    else
        m_nav.camera.lookFrom(0, +1);
    emit cameraChanged();
}

QColor FieldView3D::colourAt(double t) const
{
    return FieldMapComputer::mapColour(m_quantity, t, lightGround());
}

void FieldView3D::setBackgroundColor(const QColor &colour)
{
    if (colour == m_background) return;
    const bool wasLight = lightGround();
    m_background = colour;
    emit overlayChanged();
    if (lightGround() == wasLight) return;
    // The other theme: the map takes the colours of its new ground.
    emit groundChanged();
    scheduleCompute();
}

QColor FieldView3D::ink(int alpha) const
{
    return lightGround() ? QColor(24, 28, 36, alpha) : QColor(255, 255, 255, alpha);
}

void FieldView3D::itemChange(ItemChange change, const ItemChangeData &value)
{
    // Nothing is computed while the view is hidden: catch up when it shows.
    if (change == ItemVisibleHasChanged && value.boolValue) scheduleCompute();
    QQuickPaintedItem::itemChange(change, value);
}

void FieldView3D::scheduleCompute()
{
    if (!m_debounce.isActive()) m_debounce.start();
}

void FieldView3D::sceneBounds(Vec3 &centre, double &radius) const
{
    const SceneBase *scene = m_scene.data();
    double lo[3] = {1e300, 1e300, 1e300}, hi[3] = {-1e300, -1e300, -1e300};
    const int count = scene ? scene->count() : 0;
    for (int row = 0; row < count; ++row)
    {
        const Vec3 position = scene->sourcePosition(row);
        const double reach = scene->sourceReach(row);
        const double v[3] = {position.x, position.y, position.z};
        for (int k = 0; k < 3; ++k)
        {
            lo[k] = std::min(lo[k], v[k] - reach);
            hi[k] = std::max(hi[k], v[k] + reach);
        }
    }
    if (count == 0)
    {
        centre = {};
        radius = 1.0;
        return;
    }
    centre = {0.5 * (lo[0] + hi[0]), 0.5 * (lo[1] + hi[1]), 0.5 * (lo[2] + hi[2])};
    // Some room around the sources; a lone point source gets half a metre.
    radius = 0.65 * std::hypot(hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]);
    if (radius < 1e-6) radius = 0.5;
}

// The camera follows the scene, except while a source is dragged: the picture must not slide under
// the pointer because the bounds change.
Frame FieldView3D::frame() const
{
    auto &camera = m_nav.camera;
    camera.width = width();
    camera.height = height();
    if (m_dragRow < 0) sceneBounds(camera.centre, camera.radius);
    return camera.frame();
}

Vec3 FieldView3D::planeOrigin() const
{
    const Vec3 normal = planeAxes(m_plane).normal;
    const Vec3 &centre = m_nav.camera.centre;
    return centre + normal * (m_offset - qsig::dot(centre, normal));
}

double FieldView3D::gridStep() const
{
    return niceStep(2.0 * kPlaneHalf * m_nav.camera.radius / 8.0);
}

void FieldView3D::compute()
{
    const SceneBase *scene = m_scene.data();
    if (!scene || !isVisible() || width() < 4.0 || height() < 4.0) return;
    if (scene->count() == 0)
    {
        m_result.reset();
        emit resultChanged();
        update();
        return;
    }
    (void)frame(); // bounds of the scene
    const double half = kPlaneHalf * m_nav.camera.radius;
    double cu = 0.0, cv = 0.0, cw = 0.0;
    qsig::app::worldToPlane(m_plane, m_nav.camera.centre, cu, cv, cw);
    // Coarser while a source is dragged.
    const int samples = m_dragRow >= 0 ? (kMapSamples + 1) / 2 : kMapSamples;
    FieldMapRequest q;
    q.sampler = scene->sampler(static_cast<int>(m_quantity), m_plane);
    q.scalarReferenced = scene->scalarReferenced();
    q.plane = m_plane;
    q.quantity = m_quantity;
    q.offset = m_offset;
    q.du = 2.0 * half / (samples - 1);
    q.columns = samples;
    q.rows = samples;
    q.u0 = cu - half;
    q.v0 = cv + half;
    q.lineSeparation = m_showFieldLines ? samples / 20.0 : 0.0;
    q.lightGround = lightGround();
    m_computer.request(std::move(q));
}

// Field vectors on a lattice filling the scene, from the exact field of the module.
void FieldView3D::updateLattice() const
{
    const Vec3 centre = m_nav.camera.centre;
    const double radius = m_nav.camera.radius;
    if (!m_latticeDirty && centre == m_latticeCentre && radius == m_latticeRadius) return;
    m_latticeDirty = false;
    m_latticeCentre = centre;
    m_latticeRadius = radius;
    m_lattice.clear();
    const SceneBase *scene = m_scene.data();
    if (!scene || scene->count() == 0) return;
    const qsig::app::FieldSampler sampler = scene->sampler(0, m_plane);
    if (!sampler) return;
    auto coordinate = [radius](int i) { return radius * (-0.9 + 1.8 * i / (kLattice - 1)); };
    for (int iz = 0; iz < kLattice; ++iz)
        for (int iy = 0; iy < kLattice; ++iy)
            for (int ix = 0; ix < kLattice; ++ix)
            {
                const Vec3 at = centre + Vec3{coordinate(ix), coordinate(iy), coordinate(iz)};
                const auto sample = sampler(at);
                const double magnitude = qsig::norm(sample.field);
                if (!sample.fieldValid || !std::isfinite(magnitude) || !(magnitude > 0.0)) continue;
                m_lattice.push_back({at, sample.field / magnitude, std::log10(magnitude)});
            }
}

int FieldView3D::hitTest(double x, double y) const
{
    const SceneBase *scene = m_scene.data();
    if (!scene) return -1;
    const auto f = frame();
    auto screen = [&f](const Vec3 &q)
    {
        const auto s = f.project(q);
        return QPointF(s.x, s.y);
    };
    const QPointF p(x, y);
    const double extent = 1.2 * m_nav.camera.radius;
    int best = -1;
    double bestDistance = 9.0;
    for (int row = 0; row < scene->count(); ++row)
    {
        const SourceShape3D shape = scene->shape3d(row, m_nav.camera.centre, extent);
        const QPointF c = screen(shape.centre);
        const double toCentre = std::hypot(p.x() - c.x(), p.y() - c.y());
        double distance = 1e300;
        if (shape.point) distance = std::max(0.0, toCentre - 8.0);
        if (shape.sphereRadius > 0.0 && toCentre <= shape.sphereRadius * f.scale) distance = 0.0;
        for (const auto &line : shape.lines)
            for (std::size_t i = 0; i + 1 < line.size(); ++i)
                distance = std::min(distance, distanceToSegment(p, screen(line[i]), screen(line[i + 1])));
        // Inside a finite plate or loop face: a hit, but an outline nearby wins.
        if (!shape.infinite && distance > 4.0)
            for (const auto &face : shape.faces)
            {
                QPolygonF polygon;
                for (const auto &q : face) polygon << screen(q);
                if (polygon.containsPoint(p, Qt::OddEvenFill)) distance = 4.0;
            }
        if (distance < bestDistance)
        {
            bestDistance = distance;
            best = row;
        }
    }
    return best;
}

void FieldView3D::mousePressEvent(QMouseEvent *event)
{
    const QPointF pos = event->position();
    m_pressPos = pos;
    m_pressFace = -1;
    event->accept();
    if (event->button() != Qt::LeftButton)
    {
        m_nav.begin(pos, true);
        return;
    }
    m_pressFace = m_nav.controlAt(pos);
    if (m_pressFace >= 0) return;
    const bool shift = event->modifiers().testFlag(Qt::ShiftModifier);
    const int hit = shift ? -1 : hitTest(pos.x(), pos.y());
    const SceneBase *scene = m_scene.data();
    if (hit >= 0 && scene)
    {
        emit rowPicked(hit);
        // Grab the source where it is, on the plane parallel to the cutting plane through it.
        const Vec3 position = scene->sourcePosition(hit);
        Vec3 under;
        if (frame().pick(pos.x(), pos.y(), position, planeAxes(m_plane).normal, under, 0.12))
        {
            m_dragRow = hit;
            m_dragMoved = false;
            m_dragStart = position;
            m_dragGrab = position - under;
        }
        return;
    }
    m_nav.begin(pos, shift);
}

void FieldView3D::mouseMoveEvent(QMouseEvent *event)
{
    const QPointF pos = event->position();
    event->accept();
    SceneBase *scene = m_scene.data();
    if (m_dragRow >= 0 && scene)
    {
        if (!m_dragMoved && std::abs(pos.x() - m_pressPos.x()) + std::abs(pos.y() - m_pressPos.y()) < 4.0) return;
        const PlaneAxes axes = planeAxes(m_plane);
        Vec3 under;
        if (!frame().pick(pos.x(), pos.y(), m_dragStart, axes.normal, under, 0.12)) return;
        const double step = event->modifiers().testFlag(Qt::ShiftModifier) ? 0.0 : gridStep() / 10.0;
        auto snap = [step](double v) { return step > 0.0 ? std::round(v / step) * step : v; };
        const Vec3 wanted = under + m_dragGrab;
        const Vec3 target{snap(wanted.x), snap(wanted.y), snap(wanted.z)};
        // The coordinate normal to the plane never changes.
        const Vec3 start = m_dragStart;
        const Vec3 next = start + axes.a * qsig::dot(target - start, axes.a) + axes.b * qsig::dot(target - start, axes.b);
        const bool first = !m_dragMoved;
        m_dragMoved = true;
        if (first) emit draggingChanged();
        scene->moveSource(m_dragRow, next.x, next.y, next.z);
        return;
    }
    if (m_nav.drag(pos)) emit cameraChanged();
}

void FieldView3D::mouseReleaseEvent(QMouseEvent *event)
{
    const QPointF pos = event->position();
    event->accept();
    if (m_pressFace >= 0)
    {
        // A face of the cube, one of its arrows or "Cadrer": released where it was pressed.
        if (m_nav.controlAt(pos) == m_pressFace)
        {
            m_nav.activate(m_pressFace);
            emit cameraChanged();
        }
        m_pressFace = -1;
        return;
    }
    SceneBase *scene = m_scene.data();
    if (m_dragRow >= 0)
    {
        const bool moved = m_dragMoved;
        m_dragRow = -1;
        m_dragMoved = false;
        if (moved)
        {
            if (scene) scene->endDrag();
            emit draggingChanged();
            invalidate(); // full resolution again, bounds of the scene as they are now
            emit cameraChanged();
        }
        return;
    }
    // A click (no drag) on the cutting plane places a measurement point: A, or B with the right button.
    const bool click = m_nav.active() && !m_nav.moved();
    m_nav.end();
    if (!click || !scene || scene->count() == 0) return;
    Vec3 hit;
    if (!frame().pick(pos.x(), pos.y(), planeOrigin(), planeAxes(m_plane).normal, hit)) return;
    if (event->button() == Qt::LeftButton)
        emit probePicked(toVector(hit));
    else if (event->button() == Qt::RightButton && scene->hasDifferentialProbe())
        emit probeBPicked(toVector(hit));
}

void FieldView3D::mouseDoubleClickEvent(QMouseEvent *event)
{
    event->accept();
    if (event->button() != Qt::LeftButton) return;
    const QPointF pos = event->position();
    // The second of two quick clicks on an arrow of the cube arrives here: it counts as a click.
    m_pressFace = m_nav.controlAt(pos);
    if (m_pressFace >= 0 || hitTest(pos.x(), pos.y()) >= 0) return;
    resetView();
}

void FieldView3D::wheelEvent(QWheelEvent *event)
{
    (void)frame(); // viewport size, for the zoom about the cursor
    m_nav.wheel(event->position(), event->angleDelta().y());
    event->accept();
    emit cameraChanged();
}

void FieldView3D::hoverMoveEvent(QHoverEvent *event)
{
    if (m_nav.setHover(event->position())) update();
}

void FieldView3D::hoverLeaveEvent(QHoverEvent *)
{
    if (m_nav.clearHover()) update();
}

void FieldView3D::paint(QPainter *painter)
{
    QPainter &p = *painter;
    p.fillRect(QRectF(0, 0, width(), height()), m_background);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    const SceneBase *scene = m_scene.data();
    if (!scene || scene->count() == 0) return; // the viewport shows its hint
    const auto f = frame();
    const Vec3 centre = m_nav.camera.centre;
    const double radius = m_nav.camera.radius;
    const PlaneAxes axes = planeAxes(m_plane);
    const Vec3 origin = planeOrigin();
    const double half = kPlaneHalf * radius;
    const bool edgeOn = std::abs(qsig::dot(f.forward, axes.normal)) < 0.03;
    Layers layers(f, origin, axes.normal, !edgeOn);
    QFont font = p.font();
    font.setPixelSize(11);
    p.setFont(font);

    // ---- Sources ----
    const double extent = 1.2 * radius;
    for (int row = 0; row < scene->count(); ++row)
    {
        const SourceShape3D shape = scene->shape3d(row, centre, extent);
        const bool selected = row == m_selectedRow;
        const QColor colour = signColour(shape.sign);
        const QPen pen(colour, selected ? 2.8 : 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        const QPen halo(withAlpha(m_accent, 120), 7.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        for (const auto &face : shape.faces) layers.face(face, withAlpha(colour, shape.infinite ? 30 : 52));
        for (const auto &line : shape.lines)
        {
            if (selected) layers.path(line, halo);
            layers.path(line, pen);
        }
        if (shape.sphereRadius > 0.0)
        {
            // A tinted ball, lit from the upper left; its three great circles are among the lines.
            const double r = shape.sphereRadius * f.scale;
            const QPointF c = layers.screen(shape.centre);
            QRadialGradient glow(c + QPointF(-0.35 * r, -0.35 * r), 1.35 * r);
            glow.setColorAt(0.0, withAlpha(colour.lighter(150), 150));
            glow.setColorAt(1.0, withAlpha(colour.darker(140), 90));
            layers.disc(shape.centre, r, glow, Qt::NoPen, shape.sphereRadius);
        }
        if (shape.point)
        {
            if (selected) layers.disc(shape.centre, 11.0, withAlpha(m_accent, 120), Qt::NoPen);
            layers.disc(shape.centre, 7.0, colour, QPen(ink(230), 1.2));
        }
        for (const auto &arrow : shape.arrows)
        {
            if (arrow.large)
            {
                const double length = 0.24 * radius;
                layers.arrow(arrow.at - arrow.direction * (0.5 * length), arrow.at + arrow.direction * (0.5 * length),
                             QPen(colour, 2.4, Qt::SolidLine, Qt::RoundCap), 9.0);
            }
            else
                layers.chevron(arrow.at, arrow.direction, QPen(colour, 2.2, Qt::SolidLine, Qt::RoundCap), 8.0, 0.02 * radius);
        }
        layers.label(shape.centre, QPointF(10.0, -10.0), scene->sourceName(row), withAlpha(m_text, selected ? 255 : 170));
    }

    // ---- Field vectors on the lattice ----
    if (m_showVectors)
    {
        updateLattice();
        double lo = 1e300, hi = -1e300;
        const bool mapScale = m_quantity == FieldQuantity::Magnitude && m_result && m_result->logarithmic &&
                              m_result->displayMax > m_result->displayMin;
        if (mapScale)
        {
            lo = m_result->displayMin;
            hi = m_result->displayMax;
        }
        else
        {
            for (const auto &a : m_lattice)
            {
                lo = std::min(lo, a.level);
                hi = std::max(hi, a.level);
            }
            lo = std::max(lo, hi - 3.0); // three decades at most, or the weak vectors hide the scale
        }
        for (const auto &a : m_lattice)
        {
            const double t = hi > lo ? std::clamp((a.level - lo) / (hi - lo), 0.0, 1.0) : 0.6;
            const double length = radius * (0.06 + 0.10 * t);
            // The colours of the map, without the end that merges with the ground.
            const double shade = lightGround() ? 0.42 + 0.58 * t : 0.25 + 0.75 * t;
            QColor colour = m_quantity == FieldQuantity::Magnitude
                                ? FieldMapComputer::mapColour(FieldQuantity::Magnitude, shade, lightGround())
                                : m_text;
            colour.setAlpha(static_cast<int>(90.0 + 150.0 * t));
            layers.arrow(a.at, a.at + a.direction * length, QPen(colour, 1.0 + 0.8 * t, Qt::SolidLine, Qt::RoundCap), 5.5);
        }
    }

    layers.draw(p, 0);

    // ---- Cutting plane ----
    auto screen = [&layers](const Vec3 &q) { return layers.screen(q); };
    const QPolygonF border{screen(origin - axes.a * half - axes.b * half), screen(origin + axes.a * half - axes.b * half),
                           screen(origin + axes.a * half + axes.b * half), screen(origin - axes.a * half + axes.b * half)};
    if (!edgeOn)
    {
        const auto *result = m_result.get();
        if (result && result->image.isNull()) result = nullptr;
        if (result && m_showMap)
        {
            // The picture is an affine image of the plane: one pixel right and one pixel down in space.
            const auto &q = result->request;
            const QPointF o = screen(qsig::app::planeToWorld(q.plane, q.u0 - 0.5 * q.du, q.v0 + 0.5 * q.du, q.offset));
            const QPointF ex = screen(qsig::app::planeToWorld(q.plane, q.u0 + 0.5 * q.du, q.v0 + 0.5 * q.du, q.offset)) - o;
            const QPointF ey = screen(qsig::app::planeToWorld(q.plane, q.u0 - 0.5 * q.du, q.v0 - 0.5 * q.du, q.offset)) - o;
            p.save();
            p.setOpacity(0.93);
            p.setTransform(QTransform(ex.x(), ex.y(), ey.x(), ey.y(), o.x(), o.y()), true);
            p.drawImage(QPointF(0.0, 0.0), result->image);
            p.restore();
        }
        else
        {
            p.setPen(Qt::NoPen);
            p.setBrush(withAlpha(m_text, 12));
            p.drawPolygon(border);
            p.setBrush(Qt::NoBrush);
        }
        if (m_showGrid)
        {
            QPen thin(m_showMap && result ? ink(34) : m_grid, 1.0);
            thin.setCosmetic(true);
            p.setPen(thin);
            const double step = gridStep();
            const int lines = static_cast<int>(std::floor(half / step));
            for (int i = -lines; i <= lines; ++i)
            {
                const double v = i * step;
                p.drawLine(screen(origin + axes.a * v - axes.b * half), screen(origin + axes.a * v + axes.b * half));
                p.drawLine(screen(origin - axes.a * half + axes.b * v), screen(origin + axes.a * half + axes.b * v));
            }
        }
        if (result)
        {
            const auto &q = result->request;
            auto gridPoint = [&](double column, double line)
            { return screen(qsig::app::planeToWorld(q.plane, q.u0 + column * q.du, q.v0 - line * q.du, q.offset)); };
            if (m_showContours)
            {
                // A little stronger on the bare plane, where nothing else tells the levels.
                QPen pen(ink(m_showMap ? 80 : 120), 1.0);
                pen.setCosmetic(true);
                p.setPen(pen);
                for (const auto &segment : result->contours)
                    p.drawLine(gridPoint(segment.x1(), segment.y1()), gridPoint(segment.x2(), segment.y2()));
            }
            if (m_showFieldLines)
            {
                QPen pen(ink(m_showMap ? 165 : 150), 1.3);
                pen.setCosmetic(true);
                pen.setCapStyle(Qt::RoundCap);
                p.setPen(pen);
                for (const auto &line : result->fieldLines)
                {
                    QPolygonF path;
                    path.reserve(static_cast<qsizetype>(line.points.size()));
                    for (const auto &g : line.points) path << gridPoint(g.c, g.r);
                    p.drawPolyline(path);
                    // Direction of the field: an arrowhead in the middle of the line.
                    const qsizetype mid = path.size() / 2;
                    if (mid >= 1)
                    {
                        const QPointF along = path[mid] - path[mid - 1];
                        const double length = std::hypot(along.x(), along.y());
                        if (length > 0.5)
                        {
                            const QPointF u = along / length, n(-u.y(), u.x());
                            p.drawLine(path[mid], path[mid] - 5.5 * u + 3.4 * n);
                            p.drawLine(path[mid], path[mid] - 5.5 * u - 3.4 * n);
                        }
                    }
                }
            }
        }
    }
    p.setPen(QPen(withAlpha(m_text, 120), 1.2));
    p.setBrush(Qt::NoBrush);
    p.drawPolygon(border);
    {
        // Name of the plane at its upper corner on screen.
        static const char *const normals[3] = {"z", "y", "x"};
        QPointF corner = border[0];
        for (const QPointF &c : border)
            if (c.y() < corner.y()) corner = c;
        p.setPen(withAlpha(m_text, 200));
        p.drawText(corner + QPointF(6.0, -6.0),
                   QStringLiteral("%1 = %2").arg(QString::fromLatin1(normals[static_cast<int>(m_plane)]),
                                                 qsig::app::Units::formatQuantity(m_offset, QStringLiteral("m"), 4)));
    }

    layers.draw(p, 1);

    // ---- Measurement points, over everything ----
    const QColor orange(255, 167, 38);
    if (m_probeVisible && m_probeBVisible)
    {
        p.setPen(QPen(ink(150), 1.2, Qt::DashLine));
        p.drawLine(screen(fromVector(m_probe)), screen(fromVector(m_probeB)));
    }
    if (m_probeVisible) drawProbe(p, screen(fromVector(m_probe)), m_accent, m_probeBVisible ? QStringLiteral("A") : QString());
    if (m_probeBVisible) drawProbe(p, screen(fromVector(m_probeB)), orange, QStringLiteral("B"));

    m_nav.drawAxes(p, QPointF(46.0, height() - 46.0));
    const bool dark = m_background.lightness() < 128;
    m_nav.drawCube(p, {dark ? m_background.lighter(240) : m_background.darker(112), withAlpha(m_text, 120), m_text, m_accent,
                       QColor(Qt::white)});
}
