#include "field_view.h"

#include "qsig/app/units.h"
#include "qsig/app/wave_simulation.h"

#include <QFontMetricsF>
#include <QImage>
#include <QLineF>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>
#include <cmath>

using qsig::app::FieldMapRequest;
using qsig::app::SourceOutline;

namespace
{
double distanceToSegment(const QPointF &p, const QPointF &a, const QPointF &b)
{
    const QPointF ab = b - a, ap = p - a;
    const double len2 = QPointF::dotProduct(ab, ab);
    const double t = len2 > 0.0 ? std::clamp(QPointF::dotProduct(ap, ab) / len2, 0.0, 1.0) : 0.0;
    const QPointF d = p - (a + t * ab);
    return std::hypot(d.x(), d.y());
}

double niceStep(double raw)
{
    const double e = std::pow(10.0, std::floor(std::log10(raw)));
    for (double m : {1.0, 2.0, 5.0, 10.0})
        if (m * e >= raw) return m * e;
    return 10.0 * e;
}

QPointF unit(const QPointF &d)
{
    const double n = std::hypot(d.x(), d.y());
    return n > 0.0 ? d / n : QPointF();
}

// Open arrowhead at 'tip' pointing along the unit vector 'dir' (screen coordinates).
void arrowHead(QPainter &p, const QPointF &tip, const QPointF &dir, double size)
{
    const QPointF n(-dir.y(), dir.x());
    p.drawLine(tip, tip - size * dir + 0.62 * size * n);
    p.drawLine(tip, tip - size * dir - 0.62 * size * n);
}

// Current through the screen: dot (towards the viewer) or cross (away), in a circle.
void currentMark(QPainter &p, const QPointF &c, bool out, double r)
{
    p.drawEllipse(c, r, r);
    if (out)
    {
        p.save();
        p.setBrush(p.pen().color());
        p.drawEllipse(c, 0.28 * r, 0.28 * r);
        p.restore();
    }
    else
    {
        const double k = 0.6 * r;
        p.drawLine(c + QPointF(-k, -k), c + QPointF(k, k));
        p.drawLine(c + QPointF(-k, k), c + QPointF(k, -k));
    }
}
} // namespace

FieldView::FieldView(QQuickItem *parent) : QQuickPaintedItem(parent)
{
    setAntialiasing(true);
    setFlag(ItemHasContents, true);
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(0);
    connect(&m_debounce, &QTimer::timeout, this, &FieldView::compute);
    connect(&m_computer, &qsig::app::FieldMapComputer::ready, this,
            [this](std::shared_ptr<const qsig::app::FieldMapResult> result)
            {
                m_result = std::move(result);
                emit resultChanged();
                update();
            });
    connect(&m_computer, &qsig::app::FieldMapComputer::busyChanged, this, &FieldView::busyChanged);
    connect(this, &FieldView::viewChanged, this,
            [this]
            {
                scheduleCompute();
                update();
            });
    connect(this, &FieldView::overlayChanged, this, [this] { update(); });
    connect(this, &FieldView::fieldLinesChanged, this, &FieldView::scheduleCompute);
    connect(this, &FieldView::interactingChanged, this, &FieldView::scheduleCompute);
    // A fit requested before the first layout pass (size 0) is replayed once the item has a size.
    auto resized = [this]
    {
        if (m_fitPending && width() >= 1.0 && height() >= 1.0) fitScene();
        scheduleCompute();
    };
    connect(this, &QQuickItem::widthChanged, this, resized);
    connect(this, &QQuickItem::heightChanged, this, resized);
}

void FieldView::setScene(QObject *object)
{
    auto *scene = qobject_cast<qsig::app::SceneBase *>(object);
    if (m_scene == scene) return;
    if (m_scene) disconnect(m_scene, nullptr, this, nullptr);
    m_scene = scene;
    if (m_scene)
    {
        connect(m_scene, &qsig::app::SceneBase::sceneChanged, this,
                [this]
                {
                    scheduleCompute();
                    update();
                });
        connect(m_scene, &qsig::app::SceneBase::frameReady, this,
                [this]
                {
                    const qsig::app::SceneBase *current = m_scene.data();
                    m_live = current ? current->liveFrame() : nullptr;
                    emit resultChanged();
                    update();
                });
    }
    m_result.reset(); // never show the map of another module
    m_computer.cancel();
    const qsig::app::SceneBase *current = m_scene.data();
    m_live = current ? current->liveFrame() : nullptr;
    // A two-dimensional module is always seen on its own plane.
    if (current && current->isPlanar() && (m_plane != ViewPlane::XY || m_offset != 0.0))
    {
        m_plane = ViewPlane::XY;
        m_offset = 0.0;
        emit viewChanged();
    }
    emit resultChanged();
    emit sceneChanged();
    scheduleCompute();
    update();
}

bool FieldView::isLive() const
{
    const qsig::app::SceneBase *scene = m_scene.data();
    return scene && scene->isLive();
}

double FieldView::legendMin() const
{
    if (isLive()) return m_live && m_quantity == qsig::app::FieldQuantity::Scalar ? -m_live->scale[1] : 0.0;
    return m_result ? m_result->displayMin : 0.0;
}

double FieldView::legendMax() const
{
    if (isLive()) return m_live ? m_live->scale[m_quantity == qsig::app::FieldQuantity::Scalar ? 1 : 0] : 1.0;
    return m_result ? m_result->displayMax : 1.0;
}

void FieldView::setPlane(int plane)
{
    const auto p = static_cast<ViewPlane>(std::clamp(plane, 0, 2));
    if (p == m_plane) return;
    m_plane = p;
    emit viewChanged();
}

void FieldView::setPlaneOffset(double offset)
{
    if (offset == m_offset || !std::isfinite(offset)) return;
    m_offset = offset;
    emit viewChanged();
}

void FieldView::setCenterU(double u)
{
    if (u == m_centerU || !std::isfinite(u)) return;
    m_centerU = u;
    emit viewChanged();
}

void FieldView::setCenterV(double v)
{
    if (v == m_centerV || !std::isfinite(v)) return;
    m_centerV = v;
    emit viewChanged();
}

void FieldView::setPixelsPerMeter(double ppm)
{
    ppm = std::clamp(ppm, 2.0, 1e7);
    if (ppm == m_ppm) return;
    m_ppm = ppm;
    emit viewChanged();
}

void FieldView::setQuantity(int quantity)
{
    const auto q = static_cast<qsig::app::FieldQuantity>(std::clamp(quantity, 0, 1));
    if (q == m_quantity) return;
    m_quantity = q;
    emit viewChanged();
}

void FieldView::setShowFieldLines(bool show)
{
    if (show == m_showFieldLines) return;
    m_showFieldLines = show;
    emit fieldLinesChanged();
}

void FieldView::setInteracting(bool interacting)
{
    if (interacting == m_interacting) return;
    m_interacting = interacting;
    emit interactingChanged();
}

QString FieldView::resolutionText() const
{
    if (!m_result || isLive()) return {};
    return QStringLiteral("%1 × %2").arg(m_result->request.columns).arg(m_result->request.rows);
}

QString FieldView::uName() const
{
    return m_plane == ViewPlane::YZ ? QStringLiteral("y") : QStringLiteral("x");
}
QString FieldView::vName() const
{
    return m_plane == ViewPlane::XY ? QStringLiteral("y") : QStringLiteral("z");
}
QString FieldView::wName() const
{
    if (m_plane == ViewPlane::XY) return QStringLiteral("z");
    return m_plane == ViewPlane::XZ ? QStringLiteral("y") : QStringLiteral("x");
}

QPointF FieldView::worldToScreen(double u, double v) const
{
    return {0.5 * width() + (u - m_centerU) * m_ppm, 0.5 * height() - (v - m_centerV) * m_ppm};
}

QPointF FieldView::screenToWorld(double x, double y) const
{
    return {m_centerU + (x - 0.5 * width()) / m_ppm, m_centerV - (y - 0.5 * height()) / m_ppm};
}

QVector3D FieldView::screenToPoint(double x, double y) const
{
    const QPointF uv = screenToWorld(x, y);
    const qsig::Vec3 p = qsig::app::planeToWorld(m_plane, uv.x(), uv.y(), m_offset);
    return {static_cast<float>(p.x), static_cast<float>(p.y), static_cast<float>(p.z)};
}

QVector3D FieldView::sourcePlaneCoordinates(int row) const
{
    const qsig::app::SceneBase *scene = m_scene.data();
    if (!scene || row < 0 || row >= scene->count()) return {};
    double u = 0.0, v = 0.0, w = 0.0;
    qsig::app::worldToPlane(m_plane, scene->sourcePosition(row), u, v, w);
    return {static_cast<float>(u), static_cast<float>(v), static_cast<float>(w)};
}

int FieldView::hitTest(double x, double y) const
{
    const qsig::app::SceneBase *scene = m_scene.data();
    if (!scene) return -1;
    const QPointF p(x, y);
    int best = -1;
    double bestScore = 1e300;
    for (int i = scene->count() - 1; i >= 0; --i)
    {
        const SourceOutline o = scene->outline(i, m_plane, m_offset);
        const QPointF c = worldToScreen(o.centre.x(), o.centre.y());
        double score = std::hypot(c.x() - x, c.y() - y);
        if (score > 14.0) score = 1e300;
        auto toScreen = [this](const QPolygonF &poly)
        {
            QPolygonF screen;
            for (const auto &q : poly) screen << worldToScreen(q.x(), q.y());
            return screen;
        };
        for (const auto &poly : o.filled)
        {
            const QPolygonF screen = toScreen(poly);
            if (!o.infinite && screen.containsPoint(p, Qt::OddEvenFill)) score = std::min(score, 20.0);
            for (int k = 0; k + 1 < screen.size(); ++k)
                if (distanceToSegment(p, screen[k], screen[k + 1]) <= 8.0) score = std::min(score, 10.0);
        }
        for (const auto &poly : o.lines)
        {
            const QPolygonF screen = toScreen(poly);
            for (int k = 0; k + 1 < screen.size(); ++k)
                if (distanceToSegment(p, screen[k], screen[k + 1]) <= 8.0) score = std::min(score, 10.0);
        }
        for (const auto &band : o.bands)
        {
            const QPointF a = worldToScreen(band.from, band.from), b = worldToScreen(band.to, band.to);
            const double lo = band.vertical ? a.x() : b.y(), hi = band.vertical ? b.x() : a.y();
            const double at = band.vertical ? x : y;
            if (at >= lo - 8.0 && at <= hi + 8.0) score = std::min(score, 12.0);
        }
        for (const auto &mark : o.marks)
        {
            const QPointF m = worldToScreen(mark.at.x(), mark.at.y());
            if (std::hypot(m.x() - x, m.y() - y) <= 10.0) score = std::min(score, 10.0);
        }
        for (const auto &stroke : o.strokes)
        {
            if (stroke.erase) continue;
            const QPolygonF screen = toScreen(stroke.points);
            const double reach = 0.5 * stroke.width * m_ppm + 4.0;
            if (screen.size() == 1 && QLineF(screen.front(), p).length() <= reach) score = std::min(score, 10.0);
            for (int k = 0; k + 1 < screen.size(); ++k)
                if (distanceToSegment(p, screen[k], screen[k + 1]) <= reach) score = std::min(score, 10.0);
        }
        if (score < bestScore)
        {
            bestScore = score;
            best = i;
        }
    }
    return best;
}

void FieldView::zoomAt(double x, double y, double factor)
{
    const QPointF anchor = screenToWorld(x, y);
    m_ppm = std::clamp(m_ppm * factor, 2.0, 1e7);
    m_centerU = anchor.x() - (x - 0.5 * width()) / m_ppm;
    m_centerV = anchor.y() + (y - 0.5 * height()) / m_ppm;
    emit viewChanged();
}

void FieldView::panBy(double dx, double dy)
{
    m_centerU -= dx / m_ppm;
    m_centerV += dy / m_ppm;
    emit viewChanged();
}

void FieldView::fitScene()
{
    m_fitPending = width() < 1.0 || height() < 1.0;
    if (m_fitPending) return;
    const qsig::app::SceneBase *scene = m_scene.data();
    double minU = -0.5, maxU = 0.5, minV = -0.5, maxV = 0.5;
    const QRectF bounds = scene ? scene->viewBounds(m_plane) : QRectF();
    if (bounds.isValid())
    {
        // The area of the scene (a simulation domain), with a small margin.
        const double margin = 0.04 * std::max(bounds.width(), bounds.height());
        minU = bounds.left() - margin;
        maxU = bounds.right() + margin;
        minV = bounds.top() - margin; // plane coordinates: top() is the lowest v
        maxV = bounds.bottom() + margin;
    }
    else if (scene && scene->count() > 0)
    {
        minU = minV = 1e300;
        maxU = maxV = -1e300;
        auto grow = [&](const QPointF &q)
        {
            minU = std::min(minU, q.x());
            maxU = std::max(maxU, q.x());
            minV = std::min(minV, q.y());
            maxV = std::max(maxV, q.y());
        };
        for (int i = 0; i < scene->count(); ++i)
        {
            const SourceOutline o = scene->outline(i, m_plane, m_offset);
            grow(o.centre);
            if (o.infinite) continue;
            for (const auto &poly : o.filled + o.lines)
                for (const auto &q : poly) grow(q);
        }
        const double pad = std::max({0.25 * (maxU - minU), 0.25 * (maxV - minV), 0.2});
        minU -= pad;
        maxU += pad;
        minV -= pad;
        maxV += pad;
    }
    m_centerU = 0.5 * (minU + maxU);
    m_centerV = 0.5 * (minV + maxV);
    m_ppm = std::clamp(std::min(width() / (maxU - minU), height() / (maxV - minV)), 2.0, 1e7);
    emit viewChanged();
}

QColor FieldView::colourAt(double t) const
{
    if (isLive())
        return m_quantity == qsig::app::FieldQuantity::Scalar ? qsig::app::WaveSimulation::signedColour(t)
                                                              : qsig::app::WaveSimulation::magnitudeColour(t);
    return qsig::app::FieldMapComputer::mapColour(m_quantity, t, lightGround());
}

void FieldView::setBackgroundColor(const QColor &colour)
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

bool FieldView::inkIsDark() const
{
    return lightGround() && !isLive();
}

QColor FieldView::ink(int alpha) const
{
    return inkIsDark() ? QColor(24, 28, 36, alpha) : QColor(255, 255, 255, alpha);
}

void FieldView::scheduleCompute()
{
    if (!m_debounce.isActive()) m_debounce.start();
}

void FieldView::itemChange(ItemChange change, const ItemChangeData &value)
{
    // Nothing is computed while the view is hidden (another module, the 3D view): catch up when it shows.
    if (change == ItemVisibleHasChanged && value.boolValue) scheduleCompute();
    QQuickPaintedItem::itemChange(change, value);
}

void FieldView::compute()
{
    const qsig::app::SceneBase *scene = m_scene.data();
    if (!scene || !isVisible() || width() < 4.0 || height() < 4.0) return;
    if (scene->isLive()) return; // the scene renders its own pictures
    if (scene->count() == 0)
    {
        m_result.reset();
        emit resultChanged();
        update();
        return;
    }
    // Sample spacing in pixels: coarse while dragging, adapted to the measured cost at rest.
    double step = m_interacting ? 8.0 : 3.0;
    if (m_result && !m_interacting && m_result->elapsedMs > 150.0) step = 5.0;
    FieldMapRequest q;
    q.sampler = scene->sampler(static_cast<int>(m_quantity), m_plane);
    q.scalarReferenced = scene->scalarReferenced();
    q.plane = m_plane;
    q.quantity = m_quantity;
    q.offset = m_offset;
    q.du = step / m_ppm;
    q.columns = static_cast<int>(std::ceil(width() / step)) + 1;
    q.rows = static_cast<int>(std::ceil(height() / step)) + 1;
    q.lineSeparation = m_showFieldLines ? 38.0 / step : 0.0; // about 38 px between field lines
    q.lightGround = lightGround();
    const QPointF topLeft = screenToWorld(0.0, 0.0);
    q.u0 = topLeft.x();
    q.v0 = topLeft.y();
    m_computer.request(std::move(q));
}

QPointF FieldView::gridToScreen(const FieldMapRequest &q, QPointF grid) const
{
    return worldToScreen(q.u0 + grid.x() * q.du, q.v0 - grid.y() * q.du);
}

void FieldView::paint(QPainter *painter)
{
    QPainter &p = *painter;
    p.fillRect(QRectF(0, 0, width(), height()), m_background);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);

    if (isLive())
    {
        drawLiveFrame(p);
    }
    else if (m_result && !m_result->image.isNull())
    {
        const auto &q = m_result->request;
        if (m_showMap)
        {
            const QPointF tl = worldToScreen(q.u0 - 0.5 * q.du, q.v0 + 0.5 * q.du);
            const QPointF br = worldToScreen(q.u0 + (q.columns - 0.5) * q.du, q.v0 - (q.rows - 0.5) * q.du);
            p.drawImage(QRectF(tl, br), m_result->image);
        }
        if (m_showContours)
        {
            // A little stronger on the bare ground, where nothing else tells the levels.
            QPen pen(ink(m_showMap ? 80 : 120), 1.0);
            pen.setCosmetic(true);
            p.setPen(pen);
            for (const auto &seg : m_result->contours) p.drawLine(gridToScreen(q, seg.p1()), gridToScreen(q, seg.p2()));
        }
        if (m_showFieldLines) drawFieldLines(p);
        if (m_showArrows)
        {
            QPen pen(ink(170), 1.2);
            pen.setCosmetic(true);
            p.setPen(pen);
            for (const auto &a : m_result->arrows)
            {
                const QPointF c = gridToScreen(q, a.at);
                const QPointF dir(a.direction.x(), -a.direction.y());
                const double len = 9.0 + 7.0 * a.strength;
                p.drawLine(c - 0.5 * len * dir, c + 0.5 * len * dir);
                arrowHead(p, c + 0.5 * len * dir, dir, 4.5);
            }
        }
    }
    if (m_showGrid) drawGrid(p);
    drawSources(p);
    drawPreview(p);

    // With a second point, both are labelled and the segment A -> B (the path of the profiles) is drawn.
    if (m_probeVisible && m_probeBVisible && !isLive())
    {
        double ua = 0.0, va = 0.0, ub = 0.0, vb = 0.0, w = 0.0;
        qsig::app::worldToPlane(m_plane, {m_probeX, m_probeY, m_probeZ}, ua, va, w);
        qsig::app::worldToPlane(m_plane, {m_probeBX, m_probeBY, m_probeBZ}, ub, vb, w);
        QPen link(ink(150), 1.2, Qt::DashLine);
        p.setPen(link);
        p.drawLine(worldToScreen(ua, va), worldToScreen(ub, vb));
    }
    if (m_probeVisible) drawProbe(p, m_probeX, m_probeY, m_probeZ, m_accent, m_probeBVisible ? QStringLiteral("A") : QString());
    if (m_probeBVisible) drawProbe(p, m_probeBX, m_probeBY, m_probeBZ, QColor(255, 167, 38), QStringLiteral("B"));
}

void FieldView::drawProbe(QPainter &p, double x, double y, double z, const QColor &colour, const QString &label) const
{
    double u = 0.0, v = 0.0, w = 0.0;
    qsig::app::worldToPlane(m_plane, {x, y, z}, u, v, w);
    const QPointF c = worldToScreen(u, v);
    p.setPen(QPen(colour, 2.0));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(c, 7.0, 7.0);
    p.drawLine(c + QPointF(-12, 0), c + QPointF(-4, 0));
    p.drawLine(c + QPointF(4, 0), c + QPointF(12, 0));
    p.drawLine(c + QPointF(0, -12), c + QPointF(0, -4));
    p.drawLine(c + QPointF(0, 4), c + QPointF(0, 12));
    if (label.isEmpty()) return;
    QFont font = p.font();
    font.setPixelSize(12);
    font.setBold(true);
    p.setFont(font);
    p.drawText(c + QPointF(9, -9), label);
}

void FieldView::setPreview(int kind, const QVariantList &points, double width, bool erase)
{
    m_previewKind = std::clamp(kind, 0, 5);
    m_previewPoints.clear();
    for (const QVariant &point : points) m_previewPoints << point.toPointF();
    m_previewWidth = std::isfinite(width) ? std::max(0.0, width) : 0.0;
    m_previewErase = erase;
    update();
}

void FieldView::clearPreview()
{
    if (m_previewKind == 0) return;
    m_previewKind = 0;
    m_previewPoints.clear();
    update();
}

void FieldView::drawPreview(QPainter &p) const
{
    if (m_previewKind == 0 || m_previewPoints.isEmpty()) return;
    QColor colour = m_previewErase ? QColor(255, 255, 255) : m_accent;
    const QPointF a = worldToScreen(m_previewPoints.front().x(), m_previewPoints.front().y());
    const QPointF b = m_previewPoints.size() > 1 ? worldToScreen(m_previewPoints.back().x(), m_previewPoints.back().y()) : a;
    const double thickness = std::max(1.5, m_previewWidth * m_ppm);
    QPen dashed(colour, 1.6, Qt::DashLine);
    dashed.setCosmetic(true);
    QColor fill = colour;
    fill.setAlpha(45);
    p.save();
    switch (m_previewKind)
    {
    case 1: // brush stroke
    {
        QColor ink = colour;
        ink.setAlpha(m_previewErase ? 90 : 120);
        p.setPen(QPen(ink, thickness, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        QPolygonF screen;
        for (const auto &q : m_previewPoints) screen << worldToScreen(q.x(), q.y());
        if (screen.size() == 1)
            p.drawPoint(screen.front());
        else
            p.drawPolyline(screen);
        break;
    }
    case 2: // rectangle
        p.setPen(dashed);
        p.setBrush(fill);
        p.drawRect(QRectF(a, b).normalized());
        break;
    case 3: // disc: centre, then a point of the rim
    {
        const double r = QLineF(a, b).length();
        p.setPen(dashed);
        p.setBrush(fill);
        p.drawEllipse(a, r, r);
        p.drawLine(a, b);
        break;
    }
    case 4: // line of the given thickness
    {
        QColor ink = colour;
        ink.setAlpha(110);
        p.setPen(QPen(ink, thickness, Qt::SolidLine, Qt::FlatCap));
        p.drawLine(a, b);
        p.setPen(dashed);
        p.drawLine(a, b);
        break;
    }
    case 5: // outline of the brush under the mouse
        p.setPen(QPen(colour, 1.2));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(a, 0.5 * thickness, 0.5 * thickness);
        break;
    default:
        break;
    }
    p.restore();
}

// Freehand regions: painted on a layer in order (an erase stroke clears what is under it), then laid
// on the view with the transparency of the material.
void FieldView::drawStrokes(QPainter &p, const SourceOutline &o, const QColor &colour, double opacity) const
{
    QRectF box;
    for (const auto &stroke : o.strokes)
    {
        if (stroke.erase) continue;
        const double h = 0.5 * stroke.width * m_ppm + 2.0;
        for (const auto &q : stroke.points)
        {
            const QPointF c = worldToScreen(q.x(), q.y());
            box = box.united(QRectF(c.x() - h, c.y() - h, 2.0 * h, 2.0 * h));
        }
    }
    const QRect area = box.intersected(QRectF(0.0, 0.0, width(), height())).toAlignedRect();
    if (area.isEmpty()) return;
    QImage layer(area.size(), QImage::Format_ARGB32_Premultiplied);
    layer.fill(Qt::transparent);
    QPainter lp(&layer);
    lp.setRenderHint(QPainter::Antialiasing, true);
    lp.translate(-QPointF(area.topLeft()));
    for (const auto &stroke : o.strokes)
    {
        lp.setCompositionMode(stroke.erase ? QPainter::CompositionMode_Clear : QPainter::CompositionMode_SourceOver);
        lp.setPen(QPen(colour, std::max(1.0, stroke.width * m_ppm), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        QPolygonF screen;
        for (const auto &q : stroke.points) screen << worldToScreen(q.x(), q.y());
        if (screen.size() == 1)
            lp.drawPoint(screen.front());
        else if (screen.size() > 1)
            lp.drawPolyline(screen);
    }
    lp.end();
    p.save();
    p.setOpacity(opacity);
    p.drawImage(area.topLeft(), layer);
    p.restore();
}

void FieldView::drawLiveFrame(QPainter &p) const
{
    if (!m_live) return;
    const qsig::app::LiveFrame &f = *m_live;
    const QRectF area(worldToScreen(f.xMin, f.yMax), worldToScreen(f.xMax, f.yMin));
    const QImage &image = f.images[m_quantity == qsig::app::FieldQuantity::Scalar ? 1 : 0];
    if (!image.isNull()) p.drawImage(area, image);
    // Border of the simulated area: dashed when absorbing (open space), solid for a conductor.
    QPen pen(m_text, f.boundary == 2 ? 2.4 : 1.2);
    pen.setCosmetic(true);
    if (f.boundary == 0) pen.setStyle(Qt::DashLine);
    if (f.boundary == 1) pen.setStyle(Qt::DotLine);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    p.drawRect(area);
    QFont font = p.font();
    font.setPixelSize(11);
    p.setFont(font);
    const QString label = f.boundary == 0   ? tr("bord absorbant (CPML)")
                          : f.boundary == 1 ? tr("bord absorbant (Mur)")
                                            : tr("paroi conductrice");
    p.drawText(QRectF(area.left(), area.top() - 18.0, std::max(160.0, area.width()), 16.0),
               Qt::AlignLeft | Qt::AlignVCenter, label);
}

void FieldView::drawFieldLines(QPainter &p) const
{
    const auto &q = m_result->request;
    QPen pen(ink(165), 1.3);
    pen.setCosmetic(true);
    pen.setCapStyle(Qt::RoundCap);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    for (const auto &line : m_result->fieldLines)
    {
        QPolygonF screen;
        screen.reserve(static_cast<qsizetype>(line.points.size()));
        for (const auto &g : line.points) screen << gridToScreen(q, QPointF(g.c, g.r));
        p.drawPolyline(screen);
        // Direction of the field: one arrowhead per 170 px of line, at least one.
        double length = 0.0;
        for (int i = 0; i + 1 < screen.size(); ++i) length += QLineF(screen[i], screen[i + 1]).length();
        const int heads = std::max(1, static_cast<int>(length / 170.0));
        double next = length / (heads + 1), run = 0.0;
        for (int i = 0; i + 1 < screen.size() && next < length; ++i)
        {
            const double segment = QLineF(screen[i], screen[i + 1]).length();
            while (run + segment >= next && next < length)
            {
                const double t = segment > 0.0 ? (next - run) / segment : 0.0;
                const QPointF at = screen[i] + t * (screen[i + 1] - screen[i]);
                arrowHead(p, at, unit(screen[i + 1] - screen[i]), 5.5);
                next += length / (heads + 1);
            }
            run += segment;
        }
    }
}

double FieldView::gridStep() const
{
    return niceStep(90.0 / m_ppm);
}

void FieldView::drawGrid(QPainter &p) const
{
    const double step = gridStep();
    const QPointF a = screenToWorld(0.0, height()), b = screenToWorld(width(), 0.0);
    QPen minor(m_grid, 1.0);
    minor.setCosmetic(true);
    QColor axisColour = m_grid;
    axisColour.setAlphaF(std::min(1.0f, m_grid.alphaF() * 3.0f));
    QPen axis(axisColour, 1.2);
    axis.setCosmetic(true);
    QFont font = p.font();
    font.setPixelSize(11);
    p.setFont(font);
    for (double u = std::ceil(a.x() / step) * step; u <= b.x(); u += step)
    {
        const double x = worldToScreen(u, 0.0).x();
        p.setPen(std::abs(u) < 0.5 * step ? axis : minor);
        p.drawLine(QPointF(x, 0), QPointF(x, height()));
    }
    for (double v = std::ceil(a.y() / step) * step; v <= b.y(); v += step)
    {
        const double y = worldToScreen(0.0, v).y();
        p.setPen(std::abs(v) < 0.5 * step ? axis : minor);
        p.drawLine(QPointF(0, y), QPointF(width(), y));
    }
    // Scale bar, bottom left.
    const double barPx = step * m_ppm;
    const QPointF origin(16.0, height() - 18.0);
    p.setPen(QPen(m_text, 2.0));
    p.drawLine(origin, origin + QPointF(barPx, 0));
    p.drawLine(origin + QPointF(0, -4), origin + QPointF(0, 4));
    p.drawLine(origin + QPointF(barPx, -4), origin + QPointF(barPx, 4));
    p.drawText(QRectF(origin.x(), origin.y() - 20, barPx + 80, 16), Qt::AlignLeft | Qt::AlignVCenter,
               qsig::app::Units::formatQuantity(step, QStringLiteral("m"), 3));
    // Axis names at the positive ends.
    p.drawText(QRectF(width() - 30, 0.5 * height() - 22, 24, 16), Qt::AlignCenter, uName());
    p.drawText(QRectF(0.5 * width() + 6, 6, 24, 16), Qt::AlignLeft, vName());
}

void FieldView::drawSources(QPainter &p) const
{
    using Marker = SourceOutline::Marker;
    const qsig::app::SceneBase *scene = m_scene.data();
    if (!scene) return;
    auto toScreen = [this](const QPolygonF &poly)
    {
        QPolygonF screen;
        for (const auto &q : poly) screen << worldToScreen(q.x(), q.y());
        return screen;
    };
    for (int i = 0; i < scene->count(); ++i)
    {
        const SourceOutline o = scene->outline(i, m_plane, m_offset);
        const bool selected = i == m_selectedRow;
        const bool offPlane = o.gap > 0.0;
        const QColor sign = o.colour.isValid() ? o.colour
                            : o.sign > 0       ? QColor(255, 96, 96)
                            : o.sign < 0       ? QColor(90, 160, 255)
                                               : QColor(170, 170, 170);
        const QColor colour = selected ? m_accent : sign;
        QPen pen(colour, selected ? 2.4 : 1.6);
        pen.setCosmetic(true);
        if (offPlane) pen.setStyle(Qt::DashLine); // projection of a source the plane does not cut
        QPen solid = pen;
        solid.setStyle(Qt::SolidLine);
        p.setPen(pen);
        QColor fill = sign;
        fill.setAlpha(offPlane ? (selected ? 26 : 14) : o.solid ? 200 : (selected ? 60 : 36));

        if (!o.strokes.isEmpty())
            drawStrokes(p, o, sign, o.solid ? (selected ? 0.92 : 0.8) : (selected ? 0.55 : 0.35));
        p.setBrush(fill);
        for (const auto &poly : o.filled) p.drawPolygon(toScreen(poly));
        p.setBrush(Qt::NoBrush);
        for (const auto &poly : o.lines) p.drawPolyline(toScreen(poly));

        for (const auto &band : o.bands)
        {
            // Screen space, clipped to the item: the source crosses the whole view.
            const QPointF a = worldToScreen(band.from, band.from), b = worldToScreen(band.to, band.to);
            const QRectF area = band.vertical ? QRectF(QPointF(a.x(), 0.0), QPointF(b.x(), height()))
                                              : QRectF(QPointF(0.0, b.y()), QPointF(width(), a.y()));
            if (area.width() > 0.5 && area.height() > 0.5) p.fillRect(area, fill);
            const double mid = band.vertical ? 0.5 * (a.x() + b.x()) : 0.5 * (a.y() + b.y());
            if (band.vertical)
            {
                p.drawLine(QPointF(a.x(), 0.0), QPointF(a.x(), height()));
                if (band.to > band.from) p.drawLine(QPointF(b.x(), 0.0), QPointF(b.x(), height()));
            }
            else
            {
                p.drawLine(QPointF(0.0, a.y()), QPointF(width(), a.y()));
                if (band.to > band.from) p.drawLine(QPointF(0.0, b.y()), QPointF(width(), b.y()));
            }
            p.setPen(solid);
            const double extent = band.vertical ? height() : width();
            if (band.arrows != 0)
                for (double s = 60.0 + 40.0 * (i % 3); s < extent; s += 120.0)
                {
                    // +1: towards +v (up the screen) on a vertical band, +u (right) on a horizontal one.
                    const QPointF at = band.vertical ? QPointF(mid, s) : QPointF(s, mid);
                    const QPointF dir = band.vertical ? QPointF(0.0, -band.arrows) : QPointF(band.arrows, 0.0);
                    arrowHead(p, at, dir, 7.0);
                }
            if (band.currentMarks != 0)
                for (double s = 45.0 + 30.0 * (i % 3); s < extent; s += 90.0)
                    currentMark(p, band.vertical ? QPointF(mid, s) : QPointF(s, mid), band.currentMarks > 0, 6.0);
            p.setPen(pen);
        }

        p.setPen(solid);
        for (const auto &mark : o.marks) currentMark(p, worldToScreen(mark.at.x(), mark.at.y()), mark.out, 6.5);
        for (const auto &arrow : o.arrows)
        {
            const QPointF dir(arrow.direction.x(), -arrow.direction.y()); // v up -> screen y down
            const double along = std::hypot(dir.x(), dir.y());
            if (along < 0.3) continue; // seen nearly end-on: the dot/cross marks tell the direction
            const QPointF at = worldToScreen(arrow.at.x(), arrow.at.y());
            if (arrow.large)
            {
                const QPointF shaft = unit(dir) * (36.0 * std::min(1.0, along));
                p.drawLine(at - 0.5 * shaft, at + 0.5 * shaft);
                arrowHead(p, at + 0.5 * shaft, unit(dir), 8.0);
            }
            else
            {
                arrowHead(p, at + 4.0 * unit(dir), unit(dir), 7.0);
            }
        }

        // Centre marker.
        const QPointF c = worldToScreen(o.centre.x(), o.centre.y());
        switch (o.marker)
        {
        case Marker::Plus:
        case Marker::Minus:
            p.setPen(QPen(selected ? m_accent : ink(220), selected ? 2.4 : 1.4));
            p.setBrush(sign);
            p.drawEllipse(c, 8.0, 8.0);
            p.setPen(QPen(Qt::white, 1.8));
            p.drawLine(c + QPointF(-4, 0), c + QPointF(4, 0));
            if (o.marker == Marker::Plus) p.drawLine(c + QPointF(0, -4), c + QPointF(0, 4));
            break;
        case Marker::CurrentOut:
        case Marker::CurrentIn:
            p.setPen(QPen(selected ? m_accent : colour, 2.0));
            p.setBrush(inkIsDark() ? QColor(255, 255, 255, 170) : QColor(0, 0, 0, 90));
            currentMark(p, c, o.marker == Marker::CurrentOut, 9.0);
            break;
        case Marker::Dot:
            p.setPen(QPen(selected ? m_accent : ink(220), selected ? 2.4 : 1.4));
            p.setBrush(sign);
            p.drawEllipse(c, 5.0, 5.0);
            break;
        }
        p.setBrush(Qt::NoBrush);
        if (selected)
        {
            QFont font = p.font();
            font.setPixelSize(12);
            font.setBold(true);
            p.setFont(font);
            p.setPen(m_text);
            QString label = scene->sourceName(i);
            if (offPlane)
                label += tr(" · hors plan, à %1").arg(qsig::app::Units::formatQuantity(o.gap, QStringLiteral("m"), 3));
            const QRectF text = QFontMetricsF(font).boundingRect(label).translated(c + QPointF(14, -12));
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(0, 0, 0, 150));
            p.drawRoundedRect(text.adjusted(-5, -2, 5, 2), 4, 4);
            p.setBrush(Qt::NoBrush);
            p.setPen(QColor(240, 242, 245));
            p.drawText(c + QPointF(14, -12), label);
        }
    }
}
