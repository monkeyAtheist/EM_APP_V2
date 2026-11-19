#include "antenna_views.h"

#include "qsig/app/units.h"
#include "qsig/constants.h"

#include <QFontMetricsF>
#include <QHoverEvent>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QStringList>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <limits>
#include <utility>
#include <vector>

using qsig::Vec3;
using qsig::app::AntennaResult;
using qsig::app::Units;

namespace
{
constexpr double kPi = qsig::constants::pi;
constexpr double kDeg = kPi / 180.0;

Vec3 direction(double thetaDeg, double phiDeg)
{
    const double t = thetaDeg * kDeg, p = phiDeg * kDeg;
    return {std::sin(t) * std::cos(p), std::sin(t) * std::sin(p), std::cos(t)};
}

// Radius of the radiation pattern for a level in dB under its maximum, between 0 and 1: proportional to
// the field (scale 0), to the power (1), or linear in dB over 'range' dB (2). As in drawPattern.
double patternRadius(int scale, double db, double range)
{
    if (scale == 0) return std::pow(10.0, db / 20.0);
    if (scale == 1) return std::pow(10.0, db / 10.0);
    return std::clamp((db + range) / range, 0.0, 1.0);
}

QColor withAlpha(QColor c, int alpha)
{
    c.setAlpha(alpha);
    return c;
}

// Current and gain scale, readable on the dark and the light canvas: blue (low) to red (high).
QColor levelColour(double t)
{
    struct Stop
    {
        double at;
        int r, g, b;
    };
    static constexpr Stop stops[] = {{0.0, 70, 110, 220}, {0.35, 40, 180, 200}, {0.6, 120, 200, 80}, {0.8, 245, 180, 40}, {1.0, 235, 60, 50}};
    t = std::clamp(t, 0.0, 1.0);
    std::size_t k = 1;
    while (k + 1 < std::size(stops) && t > stops[k].at) ++k;
    const Stop &a = stops[k - 1], &b = stops[k];
    const double u = (t - a.at) / (b.at - a.at);
    auto mix = [u](int x, int y) { return static_cast<int>(std::lround(x + u * (y - x))); };
    return QColor(mix(a.r, b.r), mix(a.g, b.g), mix(a.b, b.b));
}

double distanceToSegment(const QPointF &p, const QPointF &a, const QPointF &b)
{
    const QPointF ab = b - a, ap = p - a;
    const double len2 = QPointF::dotProduct(ab, ab);
    const double t = len2 > 0.0 ? std::clamp(QPointF::dotProduct(ap, ab) / len2, 0.0, 1.0) : 0.0;
    const QPointF d = p - (a + t * ab);
    return std::hypot(d.x(), d.y());
}

// Round step for about 'count' intervals over 'span'.
double niceStep(double span, int count)
{
    const double raw = std::max(span, 1e-30) / std::max(1, count);
    const double e = std::pow(10.0, std::floor(std::log10(raw)));
    for (double m : {1.0, 2.0, 5.0, 10.0})
        if (m * e >= raw) return m * e;
    return 10.0 * e;
}

QString plain(double v, int decimals)
{
    QString s = QString::number(v, 'f', decimals);
    s.replace(QLatin1Char('.'), QLatin1Char(','));
    s.replace(QLatin1Char('-'), QStringLiteral("−"));
    return s;
}
} // namespace

// ---- Base ----

AntennaPlotBase::AntennaPlotBase(QQuickItem *parent) : QQuickPaintedItem(parent)
{
    setAntialiasing(true);
    setAcceptHoverEvents(true);
    connect(this, &AntennaPlotBase::styleChanged, this, [this] { update(); });
}

void AntennaPlotBase::setScene(QObject *object)
{
    auto *s = qobject_cast<qsig::app::AntennaScene *>(object);
    if (m_scene == s) return;
    if (m_scene) disconnect(m_scene.data(), nullptr, this, nullptr);
    m_scene = s;
    if (s)
    {
        connect(s, &qsig::app::AntennaScene::resultChanged, this,
                [this]
                {
                    resultsChanged();
                    update();
                });
        connect(s, &qsig::app::AntennaScene::sweepChanged, this, [this] { update(); });
        connect(s, &qsig::app::AntennaScene::sweepRunningChanged, this, [this] { update(); });
        connect(s, &qsig::app::SceneBase::sceneChanged, this,
                [this]
                {
                    sceneEdited();
                    update();
                });
        connect(s, &qsig::app::SceneBase::countChanged, this,
                [this]
                {
                    countChanged();
                    update();
                });
    }
    emit sceneChanged();
    update();
}

void AntennaPlotBase::setPointer(double x, double y, bool inside)
{
    if (inside == m_pointerInside && (!inside || (x == m_pointer.x() && y == m_pointer.y()))) return;
    m_pointer = QPointF(x, y);
    m_pointerInside = inside;
    update();
}

void AntennaPlotBase::hoverMoveEvent(QHoverEvent *event)
{
    setPointer(event->position().x(), event->position().y(), true);
}

void AntennaPlotBase::hoverLeaveEvent(QHoverEvent *)
{
    setPointer(0.0, 0.0, false);
}

void AntennaPlotBase::drawReadout(QPainter &p, const QPointF &at, const QStringList &lines) const
{
    p.setPen(QPen(m_background, 1.5));
    p.setBrush(m_text);
    p.drawEllipse(at, 4.0, 4.0);
    const QFontMetricsF readoutMetrics(p.font());
    double readoutWidth = 0.0;
    for (const QString &textLine : lines) readoutWidth = std::max(readoutWidth, readoutMetrics.horizontalAdvance(textLine));
    const double readoutLine = readoutMetrics.height() + 2.0;
    QRectF readoutBox(at.x() + 14.0, at.y() - 12.0, readoutWidth + 20.0, readoutLine * static_cast<double>(lines.size()) + 14.0);
    if (readoutBox.right() > width() - 6.0) readoutBox.moveRight(at.x() - 14.0);
    if (readoutBox.bottom() > height() - 6.0) readoutBox.moveBottom(height() - 6.0);
    if (readoutBox.top() < 6.0) readoutBox.moveTop(6.0);
    p.setPen(QPen(withAlpha(m_text, 90), 1.0));
    p.setBrush(withAlpha(m_background, 235));
    p.drawRoundedRect(readoutBox, 6.0, 6.0);
    p.setPen(m_text);
    double readoutY = readoutBox.top() + 7.0;
    for (const QString &textLine : lines)
    {
        p.drawText(QRectF(readoutBox.left() + 10.0, readoutY, readoutWidth, readoutLine), Qt::AlignLeft | Qt::AlignVCenter, textLine);
        readoutY += readoutLine;
    }
    p.setBrush(Qt::NoBrush);
}

void AntennaPlotBase::drawMessage(QPainter &p, const QString &text) const
{
    p.setPen(withAlpha(m_text, 150));
    QFont font = p.font();
    font.setPixelSize(13);
    p.setFont(font);
    p.drawText(QRectF(20, 0, width() - 40, height()), Qt::AlignCenter | Qt::TextWordWrap, text);
}

// ---- 3D view ----

namespace
{
// Unit normal and the two in-plane axes of a plane of the 2D views (0 XY, 1 XZ, 2 YZ).
struct PlaneAxes
{
    Vec3 a, b, normal;
};
PlaneAxes planeAxes(int plane)
{
    if (plane == 1) return {{1.0, 0.0, 0.0}, {0.0, 0.0, 1.0}, {0.0, 1.0, 0.0}};
    if (plane == 2) return {{0.0, 1.0, 0.0}, {0.0, 0.0, 1.0}, {1.0, 0.0, 0.0}};
    return {{1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, 0.0, 1.0}};
}

// Arrows of the selection and axes of the scene: x, y, z in the colours of the frame in the corner.
constexpr double kArrowLength = 64.0; // pixels, for an arrow seen side-on
constexpr double kRingRadius = 40.0;  // pixels, for the rotation handles (quarter circles)
Vec3 unitAxis(int axis)
{
    return axis == 0 ? Vec3{1.0, 0.0, 0.0} : axis == 1 ? Vec3{0.0, 1.0, 0.0} : Vec3{0.0, 0.0, 1.0};
}
QColor axisColour(int axis)
{
    return axis == 0 ? QColor(255, 110, 110) : axis == 1 ? QColor(120, 220, 130) : QColor(110, 160, 255);
}
QString axisName(int axis)
{
    return axis == 0 ? QStringLiteral("x") : axis == 1 ? QStringLiteral("y") : QStringLiteral("z");
}

OrbitNavigator::Colours cubeColours(const QColor &background, const QColor &text, const QColor &accent)
{
    // A face a little lighter (dark theme) or darker (light theme) than the canvas.
    const bool dark = background.lightness() < 128;
    return {dark ? background.lighter(240) : background.darker(112), withAlpha(text, 120), text, accent,
            QColor(Qt::white)};
}
} // namespace

AntennaView3D::AntennaView3D(QQuickItem *parent) : AntennaPlotBase(parent)
{
    setAcceptedMouseButtons(Qt::LeftButton | Qt::MiddleButton | Qt::RightButton);
    setAcceptHoverEvents(true);
    connect(this, &AntennaView3D::viewChanged, this, [this] { update(); });
}

void AntennaView3D::resetView()
{
    m_nav.camera.reset();
    emit viewChanged();
}

void AntennaView3D::lookFrom(int axis, int side)
{
    m_nav.camera.lookFrom(std::clamp(axis, 0, 2), side >= 0 ? +1 : -1);
    emit viewChanged();
}

void AntennaView3D::setTool(int tool)
{
    const int next = std::clamp(tool, 0, 6);
    if (next == m_tool) return;
    m_tool = next;
    m_gesture = false;
    m_measured = false;
    m_hoverAnchor = false;
    m_gestureSnap = 0;
    m_toolRow = -1;
    setToolText({});
    emit toolChanged();
    update();
}

void AntennaView3D::setToolText(const QString &text)
{
    if (text == m_toolText) return;
    m_toolText = text;
    emit toolTextChanged();
}

// Length of the wire being drawn or of the measure, in metres and in wavelengths.
void AntennaView3D::updateToolText()
{
    if (!m_gesture && !m_measured)
    {
        setToolText({});
        return;
    }
    const Vec3 span = m_gestureB - m_gestureA;
    const double length = qsig::norm(span);
    const auto *s = scene();
    const double lambda = s ? qsig::constants::c0 / std::max(1.0, s->frequency()) : 1.0;
    QString text = QStringLiteral("%1 = %2 (%3 λ)")
                       .arg(m_tool == 3 ? QStringLiteral("d") : QStringLiteral("L"),
                            Units::formatQuantity(length, QStringLiteral("m"), 4), plain(length / lambda, 3));
    if (m_tool == 3)
    {
        text += QStringLiteral(" · Δx %1 · Δy %2 · Δz %3")
                    .arg(Units::formatQuantity(span.x, QStringLiteral("m"), 3), Units::formatQuantity(span.y, QStringLiteral("m"), 3),
                         Units::formatQuantity(span.z, QStringLiteral("m"), 3));
        // Direction of the measured segment: azimuth from +x in the xy plane, elevation above it.
        if (length > 0.0)
            text += tr(" · azimut %1° · élévation %2°")
                        .arg(plain(std::atan2(span.y, span.x) / kDeg, 1), plain(std::atan2(span.z, std::hypot(span.x, span.y)) / kDeg, 1));
    }
    else if (m_gestureSnap == 1)
        text += tr(" · raccordé au bout");
    else if (m_gestureSnap == 2)
        text += tr(" · raccordé en T");
    else if (m_gestureSnap == 3)
        text += tr(" · posé sur la surface");
    setToolText(text);
}

AntennaView3D::Plane AntennaView3D::constructionPlane() const
{
    // The orientation chosen, turned about x, then y, then z, around the point at 'editOffset' on its normal.
    const int chosen = std::clamp(m_editPlane, 0, 2);
    const PlaneAxes base = planeAxes(chosen);
    const double rx = (std::isfinite(m_planeRotationX) ? m_planeRotationX : 0.0) * kDeg;
    const double ry = (std::isfinite(m_planeRotationY) ? m_planeRotationY : 0.0) * kDeg;
    const double rz = (std::isfinite(m_planeRotationZ) ? m_planeRotationZ : 0.0) * kDeg;
    auto turn = [rx, ry, rz](const Vec3 &v)
    {
        const Vec3 x{v.x, v.y * std::cos(rx) - v.z * std::sin(rx), v.y * std::sin(rx) + v.z * std::cos(rx)};
        const Vec3 y{x.x * std::cos(ry) + x.z * std::sin(ry), x.y, -x.x * std::sin(ry) + x.z * std::cos(ry)};
        return Vec3{y.x * std::cos(rz) - y.y * std::sin(rz), y.x * std::sin(rz) + y.y * std::cos(rz), y.z};
    };
    const Vec3 anchor = base.normal * m_editOffset;
    const Plane turned{turn(base.a), turn(base.b), turn(base.normal), anchor};
    // An exact view works in the plane it faces, unless the turned plane faces the viewer too.
    const int facing = m_nav.camera.facingPlane();
    if (facing < 0 || std::abs(qsig::dot(turned.normal, m_nav.camera.frame().forward)) > 1.0 - 1e-9) return turned;
    const PlaneAxes seen = planeAxes(facing);
    return {seen.a, seen.b, seen.normal, seen.normal * m_editOffset};
}

Vec3 AntennaView3D::onPlaneGrid(const Plane &plane, const Vec3 &point, double step) const
{
    // Coordinates in the plane from its anchor, on the grid; the coordinate along the normal kept.
    const Vec3 rel = point - plane.anchor;
    auto onGrid = [step](double v) { return step > 0.0 ? std::round(v / step) * step : v; };
    return plane.anchor + plane.a * onGrid(qsig::dot(rel, plane.a)) + plane.b * onGrid(qsig::dot(rel, plane.b)) +
           plane.normal * qsig::dot(rel, plane.normal);
}

// The construction plane goes through this point: the centre of the view brought onto the plane.
Vec3 AntennaView3D::constructionOrigin() const
{
    const Plane plane = constructionPlane();
    const Vec3 &centre = m_nav.camera.centre;
    return centre + plane.normal * qsig::dot(plane.anchor - centre, plane.normal);
}

void AntennaView3D::lookAtPlane()
{
    m_nav.camera.faceTowards(constructionPlane().normal);
    emit viewChanged();
}

void AntennaView3D::setRotationPivot(int pivot)
{
    const int next = std::clamp(pivot, 0, 2);
    if (next == m_rotationPivot) return;
    m_rotationPivot = next;
    emit viewChanged();
}

QString AntennaView3D::pivotText() const
{
    if (!m_pivotSet) return {};
    return QStringLiteral("x %1 · y %2 · z %3")
        .arg(Units::formatQuantity(m_pivot.x, QStringLiteral("m"), 4), Units::formatQuantity(m_pivot.y, QStringLiteral("m"), 4),
             Units::formatQuantity(m_pivot.z, QStringLiteral("m"), 4));
}

Vec3 AntennaView3D::ringCentre() const
{
    if (m_rotationPivot == 1) return Vec3{};
    if (m_rotationPivot == 2 && m_pivotSet) return m_pivot;
    return gizmoOrigin();
}

bool AntennaView3D::toolPoint(const QPointF &pos, bool free, const Vec3 &planeOrigin, int exclude, bool feeds, Vec3 &point,
                              int &snap) const
{
    snap = 0;
    const auto *s = scene();
    if (!s) return false;
    const auto f = frame();
    // An end of a wire (a source too, for a measure) within 10 pixels: the exact point.
    double best = 10.0;
    for (const Vec3 &anchor : qsig::app::antennaAnchors(s->sources(), exclude, feeds))
    {
        const auto sp = f.project(anchor);
        const double distance = std::hypot(sp.x - pos.x(), sp.y - pos.y());
        if (distance < best)
        {
            best = distance;
            point = anchor;
            snap = 1;
        }
    }
    if (snap != 0) return true;
    // The body of a plain wire within 8 pixels: the point of the wire under the pointer (the view is
    // an orthographic projection, so the fraction along the wire is the one seen on screen). The
    // solver joins a wire that ends there.
    best = 8.0;
    for (int i = 0; i < s->count(); ++i)
    {
        const auto &element = s->sources()[static_cast<std::size_t>(i)];
        if (i == exclude || element.type != qsig::app::AntennaElement::Type::Wire) continue;
        Vec3 a, b;
        qsig::app::wireEndpoints(element, a, b);
        const auto sa = f.project(a), sb = f.project(b);
        const double dx = sb.x - sa.x, dy = sb.y - sa.y, length2 = dx * dx + dy * dy;
        if (!(length2 > 1.0)) continue; // seen end-on
        const double t = ((pos.x() - sa.x) * dx + (pos.y() - sa.y) * dy) / length2;
        if (t <= 0.0 || t >= 1.0) continue;
        const double distance = std::hypot(pos.x() - (sa.x + t * dx), pos.y() - (sa.y + t * dy));
        if (distance < best)
        {
            best = distance;
            point = a + (b - a) * t;
            snap = 2;
        }
    }
    if (snap != 0) return true;
    const Plane plane = constructionPlane();
    Vec3 under;
    if (!f.pick(pos.x(), pos.y(), planeOrigin, plane.normal, under, 0.12)) return false;
    // On the grid within the plane; the coordinate along the normal is that of the plane.
    point = onPlaneGrid(plane, under, free ? 0.0 : snapStep());
    // On a surface, or within 8 pixels of it: the point lands on the surface (a wire that ends there is
    // attached to it).
    for (int i = 0; i < s->count(); ++i)
    {
        const auto &element = s->sources()[static_cast<std::size_t>(i)];
        if (i == exclude || !qsig::app::elementIsSurface(element.type)) continue;
        const Vec3 landing = qsig::app::projectOnSurface(element, point);
        if (qsig::app::surfaceAt(s->sources(), landing) != i) continue;
        const auto from = f.project(point), to = f.project(landing);
        if (std::hypot(to.x - from.x, to.y - from.y) > 8.0) continue;
        point = landing;
        snap = 3;
        break;
    }
    return true;
}

// ---- Selection ----

void AntennaView3D::setSelectedRow(int row)
{
    const bool known = row < 0 ? m_selection.isEmpty() : m_selection.contains(row);
    if (row == m_selectedRow && known) return;
    if (row >= 0 && m_feedRow >= 0)
    {
        m_feedRow = m_feedIndex = -1;
        emit feedSelectionChanged();
    }
    m_selectedRow = row;
    if (row < 0)
        m_selection.clear();
    else if (!m_selection.contains(row))
        m_selection = QList<int>{row};
    emit selectionChanged();
    refreshConstraints();
    update();
}

void AntennaView3D::select(const QList<int> &rows, int mainRow)
{
    // Elements and sources are not selected together.
    if (m_feedRow >= 0)
    {
        m_feedRow = m_feedIndex = -1;
        emit feedSelectionChanged();
    }
    m_selection = rows;
    std::sort(m_selection.begin(), m_selection.end());
    m_selectedRow = m_selection.contains(mainRow) ? mainRow : m_selection.isEmpty() ? -1 : m_selection.last();
    emit selectionChanged();
    refreshConstraints();
    emit rowPicked(m_selectedRow);
    update();
}

QList<int> AntennaView3D::selection() const
{
    const auto *s = scene();
    QList<int> rows;
    for (const int row : m_selection)
        if (s && row >= 0 && row < s->count()) rows.append(row);
    return rows;
}

void AntennaView3D::countChanged()
{
    // Elements were added or removed: the other rows of the selection may now be other elements.
    const auto *s = scene();
    m_selection = s && m_selectedRow >= 0 && m_selectedRow < s->count() ? QList<int>{m_selectedRow} : QList<int>{};
    clearFeed(); // its row may now be another element
    emit selectionChanged();
    refreshConstraints();
}

void AntennaView3D::sceneEdited()
{
    // Names, groups and constraints shown by the panel may have changed.
    emit selectionChanged();
    refreshConstraints();
    // The selected source may be gone (undo, its element made passive), or its values may have changed.
    if (m_feedRow >= 0)
    {
        const auto *s = scene();
        if (!s || m_feedRow >= s->count() || m_feedIndex >= qsig::app::feedCount(s->sources()[static_cast<std::size_t>(m_feedRow)]))
            m_feedRow = m_feedIndex = -1;
        emit feedSelectionChanged();
    }
}

QVariantMap AntennaView3D::selectedFeed() const
{
    const auto *s = scene();
    if (!s || m_feedRow < 0) return {};
    const QVariantList feeds = s->feedList();
    for (const QVariant &item : feeds)
    {
        const QVariantMap feed = item.toMap();
        if (feed.value(QStringLiteral("row")).toInt() == m_feedRow && feed.value(QStringLiteral("index")).toInt() == m_feedIndex)
            return feed;
    }
    return {};
}

void AntennaView3D::clearFeed()
{
    if (m_feedRow < 0) return;
    m_feedRow = m_feedIndex = -1;
    emit feedSelectionChanged();
    update();
}

void AntennaView3D::selectFeed(int row, int index)
{
    const auto *s = scene();
    if (!s || row < 0 || row >= s->count() || index < 0 ||
        index >= qsig::app::feedCount(s->sources()[static_cast<std::size_t>(row)]))
    {
        clearFeed();
        return;
    }
    select({}, -1); // the elements are deselected, and the source selected before with them
    m_feedRow = row;
    m_feedIndex = index;
    emit feedSelectionChanged();
    update();
}

void AntennaView3D::setFeedValue(const QString &key, double value)
{
    auto *editable = m_scene.data();
    if (editable && m_feedRow >= 0) editable->setFeedValue(m_feedRow, m_feedIndex, key, value);
}

void AntennaView3D::deleteFeed()
{
    auto *editable = m_scene.data();
    if (!editable || m_feedRow < 0) return;
    const int row = m_feedRow, index = m_feedIndex;
    clearFeed();
    editable->deleteFeed(row, index);
}

bool AntennaView3D::feedAtPixel(const QPointF &pos, int &row, int &index) const
{
    row = index = -1;
    const auto *s = scene();
    if (!s) return false;
    const auto f = frame();
    double nearest = 9.0;
    for (int candidate = 0; candidate < s->count(); ++candidate)
    {
        const auto points = qsig::app::antennaShape(s->sources()[static_cast<std::size_t>(candidate)]).feeds;
        for (std::size_t k = 0; k < points.size(); ++k)
        {
            const auto sp = f.project(points[k]);
            const double distance = std::hypot(sp.x - pos.x(), sp.y - pos.y());
            if (distance > nearest) continue;
            nearest = distance;
            row = candidate;
            index = static_cast<int>(k);
        }
    }
    return row >= 0;
}

void AntennaView3D::refreshConstraints()
{
    const auto *s = scene();
    QVariantList list = s ? s->constraintList(m_selectedRow) : QVariantList();
    if (list == m_constraints) return;
    m_constraints = std::move(list);
    emit constraintsChanged();
}

int AntennaView3D::referenceRow() const
{
    const QList<int> rows = selection();
    if (rows.size() != 2 || !rows.contains(m_selectedRow)) return -1;
    return rows.first() == m_selectedRow ? rows.last() : rows.first();
}

QString AntennaView3D::mainName() const
{
    const auto *s = scene();
    return s && m_selectedRow >= 0 && m_selectedRow < s->count() ? s->sourceName(m_selectedRow) : QString();
}

QString AntennaView3D::groupName() const
{
    const auto *s = scene();
    return s ? s->groupOf(m_selectedRow) : QString();
}

QString AntennaView3D::referenceName() const
{
    const auto *s = scene();
    const int row = referenceRow();
    return s && row >= 0 ? s->sourceName(row) : QString();
}

double AntennaView3D::mainLength() const
{
    const auto *s = scene();
    if (!s || m_selectedRow < 0 || m_selectedRow >= s->count()) return 0.0;
    const auto &element = s->sources()[static_cast<std::size_t>(m_selectedRow)];
    return qsig::app::elementHasLength(element.type) ? element.length : 0.0;
}

void AntennaView3D::groupSelection()
{
    auto *editable = m_scene.data();
    const QList<int> rows = selection();
    if (editable && rows.size() >= 2) editable->groupElements(rows);
}

void AntennaView3D::ungroupSelection()
{
    auto *editable = m_scene.data();
    if (editable) editable->ungroupElements(selection());
}

void AntennaView3D::renameGroup(const QString &name)
{
    auto *editable = m_scene.data();
    if (editable) editable->renameGroup(m_selectedRow, name);
}

bool AntennaView3D::addConstraint(int type, double value, int anchor, int referenceAnchor, int axes)
{
    auto *editable = m_scene.data();
    if (!editable || m_selectedRow < 0) return false;
    return editable->addConstraint(m_selectedRow, type, referenceRow(), value, anchor, referenceAnchor, axes);
}

void AntennaView3D::removeConstraint(int index)
{
    auto *editable = m_scene.data();
    if (editable) editable->removeConstraint(m_selectedRow, index);
}

void AntennaView3D::setConstraintEnabled(int index, bool enabled)
{
    auto *editable = m_scene.data();
    if (editable) editable->setConstraintEnabled(m_selectedRow, index, enabled);
}

int AntennaView3D::handleAt(const QPointF &pos) const
{
    const auto *s = scene();
    if (!s || m_tool != 0 || m_selection.size() != 1) return -1;
    const int row = m_selection.first();
    if (row < 0 || row >= s->count()) return -1;
    const auto &element = s->sources()[static_cast<std::size_t>(row)];
    if (element.type != qsig::app::AntennaElement::Type::Wire) return -1;
    Vec3 ends[2];
    qsig::app::wireEndpoints(element, ends[0], ends[1]);
    const auto f = frame();
    for (int k = 0; k < 2; ++k)
    {
        const auto sp = f.project(ends[k]);
        if (std::hypot(sp.x - pos.x(), sp.y - pos.y()) <= 8.0) return k;
    }
    return -1;
}

void AntennaView3D::moveSelection(double dx, double dy, double dz, int copies)
{
    auto *editable = m_scene.data();
    const QList<int> rows = selection();
    const Vec3 delta{dx, dy, dz};
    if (!editable || rows.isEmpty() || !std::isfinite(dx + dy + dz)) return;
    if (qsig::norm(delta) == 0.0) return; // nothing to move, and copies would lie on their originals
    const int mainRow = m_selectedRow;
    select(editable->translateElements(rows, delta, copies), mainRow);
}

void AntennaView3D::rotateSelection(int axis, double angleDeg, int pivot, bool copy)
{
    auto *editable = m_scene.data();
    const QList<int> rows = selection();
    if (!editable || rows.isEmpty() || !std::isfinite(angleDeg) || angleDeg == 0.0) return;
    const Vec3 direction = axis == 3   ? constructionPlane().normal
                           : axis == 0 ? Vec3{1.0, 0.0, 0.0}
                           : axis == 1 ? Vec3{0.0, 1.0, 0.0}
                                       : Vec3{0.0, 0.0, 1.0};
    // The pivot asked for; the placed one while none is placed: the centre of the selection.
    const Vec3 centre = pivot == 1 ? Vec3{} : pivot == 2 && m_pivotSet ? m_pivot : editable->centreOf(rows);
    const int mainRow = m_selectedRow;
    select(editable->rotateElements(rows, centre, direction, angleDeg, copy), mainRow);
}

void AntennaView3D::mirrorSelection(bool copy)
{
    auto *editable = m_scene.data();
    const QList<int> rows = selection();
    if (!editable || rows.isEmpty()) return;
    const int mainRow = m_selectedRow;
    select(editable->mirrorElements(rows, constructionOrigin(), constructionPlane().normal, copy), mainRow);
}

void AntennaView3D::mirrorSelectionAcross(int axis, bool copy)
{
    auto *editable = m_scene.data();
    const QList<int> rows = selection();
    if (!editable || rows.isEmpty()) return;
    const int mainRow = m_selectedRow;
    select(editable->mirrorElements(rows, m_pivotSet ? m_pivot : Vec3{}, unitAxis(std::clamp(axis, 0, 2)), copy), mainRow);
}

void AntennaView3D::duplicateSelection()
{
    auto *editable = m_scene.data();
    const QList<int> rows = selection();
    if (!editable || rows.isEmpty()) return;
    const int mainRow = m_selectedRow;
    select(editable->translateElements(rows, constructionPlane().a * gridStep(), 1), mainRow);
}

void AntennaView3D::fitView()
{
    m_nav.activate(OrbitNavigator::Fit);
    emit viewChanged();
}

int AntennaView3D::addElementAt(int type, double x, double y)
{
    auto *editable = m_scene.data();
    if (!editable) return -1;
    // On the construction plane, on a tenth of the grid; at the centre of the plane when it is seen edge-on.
    const Plane plane = constructionPlane();
    const Vec3 origin = constructionOrigin();
    Vec3 point = origin, under;
    if (frame().pick(x, y, origin, plane.normal, under, 0.12)) point = onPlaneGrid(plane, under, snapStep());
    const int row = editable->addSourceAt(type, point);
    if (row >= 0) select(QList<int>{row}, row);
    return row;
}

Vec3 AntennaView3D::pointOnElement(int row, const QPointF &pos) const
{
    const auto *s = scene();
    if (!s || row < 0 || row >= s->count()) return {};
    const auto &element = s->sources()[static_cast<std::size_t>(row)];
    if (element.type != qsig::app::AntennaElement::Type::Wire)
    {
        if (!qsig::app::feedSlides(element.type)) return element.position;
        // A curve (an arc): the point of its polyline under the pointer, on the nearest piece seen on screen.
        const auto curveFrame = frame();
        Vec3 nearestPoint = element.position;
        double nearestDistance = std::numeric_limits<double>::infinity();
        for (const auto &curve : qsig::app::antennaShape(element).polylines)
            for (std::size_t k = 0; k + 1 < curve.size(); ++k)
            {
                const auto pa = curveFrame.project(curve[k]), pb = curveFrame.project(curve[k + 1]);
                const double ux = pb.x - pa.x, uy = pb.y - pa.y, span2 = ux * ux + uy * uy;
                const double t = span2 > 0.0 ? std::clamp(((pos.x() - pa.x) * ux + (pos.y() - pa.y) * uy) / span2, 0.0, 1.0) : 0.0;
                const double distance = std::hypot(pa.x + ux * t - pos.x(), pa.y + uy * t - pos.y());
                if (distance >= nearestDistance) continue;
                nearestDistance = distance;
                nearestPoint = curve[k] + (curve[k + 1] - curve[k]) * t;
            }
        return nearestPoint;
    }
    Vec3 a, b;
    qsig::app::wireEndpoints(element, a, b);
    const auto f = frame();
    const auto sa = f.project(a), sb = f.project(b);
    const double dx = sb.x - sa.x, dy = sb.y - sa.y, length2 = dx * dx + dy * dy;
    const double t = length2 > 0.0 ? std::clamp(((pos.x() - sa.x) * dx + (pos.y() - sa.y) * dy) / length2, 0.0, 1.0) : 0.5;
    return a + (b - a) * t;
}

void AntennaView3D::placeFeedAt(double x, double y)
{
    auto *editable = m_scene.data();
    const int row = hitTest(x, y);
    if (!editable || row < 0) return;
    const int placed = editable->placeFeed(row, pointOnElement(row, QPointF(x, y)));
    if (placed >= 0)
        selectFeed(row, placed);
    else
        select(QList<int>{row}, row);
}

void AntennaView3D::placePivot(double x, double y)
{
    Vec3 point;
    int snap = 0;
    if (!toolPoint(QPointF(x, y), false, constructionOrigin(), -1, true, point, snap)) return;
    m_pivot = point;
    m_pivotSet = true;
    m_rotationPivot = 2; // placed to be used
    emit viewChanged();
}

void AntennaView3D::clearPivot()
{
    if (!m_pivotSet) return;
    m_pivotSet = false;
    if (m_rotationPivot == 2) m_rotationPivot = 0;
    emit viewChanged();
}

// ---- Arrows on the selection ----

bool AntennaView3D::gizmoVisible() const
{
    return m_showGizmo && m_tool == 0 && scene() && !selection().isEmpty();
}

Vec3 AntennaView3D::gizmoOrigin() const
{
    const auto *s = scene();
    return s ? s->centreOf(selection()) : Vec3{};
}

int AntennaView3D::ringAt(const QPointF &pos) const
{
    if (!gizmoVisible()) return -1;
    const auto f = frame();
    const auto at = f.project(ringCentre());
    const QPointF from(at.x, at.y);
    int best = -1;
    double bestDistance = 6.0;
    for (int axis = 0; axis < 3; ++axis)
    {
        const Vec3 first = unitAxis((axis + 1) % 3), second = unitAxis((axis + 2) % 3);
        const QPointF pa(qsig::dot(first, f.right), -qsig::dot(first, f.up)), pb(qsig::dot(second, f.right), -qsig::dot(second, f.up));
        if (std::abs(pa.x() * pb.y() - pa.y() * pb.x()) < 0.2) continue; // its plane is seen edge-on
        QPointF previous = from + kRingRadius * pa;
        for (int k = 1; k <= 12; ++k)
        {
            const double t = 0.5 * kPi * k / 12.0;
            const QPointF next = from + kRingRadius * (std::cos(t) * pa + std::sin(t) * pb);
            const double distance = distanceToSegment(pos, previous, next);
            if (distance < bestDistance)
            {
                bestDistance = distance;
                best = axis;
            }
            previous = next;
        }
    }
    return best;
}

int AntennaView3D::axisAt(const QPointF &pos) const
{
    if (!gizmoVisible()) return -1;
    const auto f = frame();
    const auto at = f.project(gizmoOrigin());
    const QPointF from(at.x, at.y);
    int best = -1;
    double bestDistance = 7.0;
    for (int axis = 0; axis < 3; ++axis)
    {
        const Vec3 unit = unitAxis(axis);
        const QPointF along(qsig::dot(unit, f.right), -qsig::dot(unit, f.up));
        if (std::hypot(along.x(), along.y()) < 0.22) continue; // seen end-on: nothing to grab
        const double distance = distanceToSegment(pos, from, from + kArrowLength * along);
        if (distance < bestDistance)
        {
            bestDistance = distance;
            best = axis;
        }
    }
    return best;
}

void AntennaView3D::alignSelection(int axis)
{
    auto *editable = m_scene.data();
    const QList<int> rows = selection();
    if (!editable || rows.size() < 2 || !rows.contains(m_selectedRow)) return;
    const Vec3 reference = editable->sourcePosition(m_selectedRow);
    editable->alignElements(rows, axis, axis == 0 ? reference.x : axis == 1 ? reference.y : reference.z);
}

void AntennaView3D::deleteSelection()
{
    auto *editable = m_scene.data();
    // A selected source: it goes, its wire stays.
    if (m_feedRow >= 0)
    {
        deleteFeed();
        return;
    }
    const QList<int> rows = selection();
    if (!editable || rows.isEmpty()) return;
    select({}, -1);
    editable->removeElements(rows);
}

void AntennaView3D::selectAll()
{
    const auto *s = scene();
    QList<int> rows;
    for (int row = 0; s && row < s->count(); ++row)
        if (passesFilter(row)) rows.append(row);
    select(rows, rows.contains(m_selectedRow) || rows.isEmpty() ? m_selectedRow : rows.last());
}

void AntennaView3D::selectGroup(const QString &name)
{
    const auto *s = scene();
    if (!s || name.isEmpty()) return;
    QList<int> rows;
    for (int row = 0; row < s->count(); ++row)
        if (s->groupOf(row) == name) rows.append(row);
    if (!rows.isEmpty()) select(rows, rows.first());
}

int AntennaView3D::activePlane() const
{
    const int facing = m_nav.camera.facingPlane();
    return facing >= 0 ? facing : std::clamp(m_editPlane, 0, 2);
}

void AntennaView3D::sceneBounds(Vec3 &centre, double &radius) const
{
    const auto *s = scene();
    double lo[3] = {1e300, 1e300, 1e300}, hi[3] = {-1e300, -1e300, -1e300};
    bool any = false;
    if (s)
        for (const auto &e : s->sources())
            for (const auto &line : qsig::app::antennaShape(e).polylines)
                for (const auto &q : line)
                {
                    const double v[3] = {q.x, q.y, q.z};
                    for (int k = 0; k < 3; ++k)
                    {
                        lo[k] = std::min(lo[k], v[k]);
                        hi[k] = std::max(hi[k], v[k]);
                    }
                    any = true;
                }
    // Half a wavelength around the origin for an empty scene: room to draw a first antenna.
    const double lambda = s ? qsig::constants::c0 / std::max(1.0, s->frequency()) : 1.0;
    if (!any)
    {
        centre = {};
        radius = 0.5 * lambda;
        return;
    }
    centre = {0.5 * (lo[0] + hi[0]), 0.5 * (lo[1] + hi[1]), 0.5 * (lo[2] + hi[2])};
    radius = std::max(1e-6, 0.5 * std::hypot(hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]));
}

// The camera follows the scene, except while an element is dragged: the picture must not slide
// under the pointer because the bounds change.
qsig::app::OrbitCamera::Frame AntennaView3D::frame() const
{
    auto &camera = m_nav.camera;
    camera.width = width();
    camera.height = height();
    if (m_dragRow < 0 && m_handleEnd < 0 && m_axisDrag < 0) sceneBounds(camera.centre, camera.radius);
    return camera.frame();
}

double AntennaView3D::gridStep() const
{
    return niceStep(2.6 * m_nav.camera.radius, 8);
}

double AntennaView3D::snapStep() const
{
    return std::isfinite(m_moveStep) && m_moveStep > 0.0 ? m_moveStep : gridStep() / 10.0;
}

int AntennaView3D::hitTest(double x, double y) const
{
    const auto *s = scene();
    if (!s) return -1;
    const auto f = frame();
    auto screen = [&f](const Vec3 &q)
    {
        const auto sp = f.project(q);
        return QPointF(sp.x, sp.y);
    };
    const QPointF p(x, y);
    int best = -1;
    double bestDistance = 9.0;
    // A surface is picked by its inside too. A line within reach wins over it (a wire in front of a
    // plate, an edge); of two surfaces under the pointer, the nearer to the viewer.
    int inside = -1;
    double insideDepth = 0.0;
    for (int i = 0; i < s->count(); ++i)
    {
        if (m_tool == 0 && !passesFilter(i)) continue; // the filter is the selection tool's
        const auto &element = s->sources()[static_cast<std::size_t>(i)];
        const auto shape = qsig::app::antennaShape(element);
        for (const auto &line : shape.polylines)
            for (std::size_t k = 0; k + 1 < line.size(); ++k)
            {
                const double distance = distanceToSegment(p, screen(line[k]), screen(line[k + 1]));
                if (distance < bestDistance)
                {
                    bestDistance = distance;
                    best = i;
                }
            }
        if (!shape.surface) continue;
        // Inside one of the faces of its skin (the outline of a plate or a disc, the faces of a solid).
        bool within = false;
        for (const auto &skinFace : shape.faces)
        {
            QPolygonF outline;
            for (const auto &corner : skinFace) outline << screen(corner);
            if (!outline.containsPoint(p, Qt::OddEvenFill)) continue;
            within = true;
            break;
        }
        if (!within) continue;
        const double depth = f.project(element.position).depth;
        if (inside < 0 || depth < insideDepth)
        {
            inside = i;
            insideDepth = depth;
        }
    }
    return best >= 0 ? best : inside;
}

bool AntennaView3D::passesFilter(int row) const
{
    const auto *s = scene();
    if (!s || row < 0 || row >= s->count()) return false;
    if (m_selectionFilter <= 0) return true;
    const auto &element = s->sources()[static_cast<std::size_t>(row)];
    const bool surface = qsig::app::elementIsSurface(element.type);
    // 3: the sources only, which are entities of their own (feedAtPixel), not elements.
    return m_selectionFilter == 1 ? !surface : m_selectionFilter == 2 && surface;
}

void AntennaView3D::mousePressEvent(QMouseEvent *event)
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
    // A view of the results only: a drag turns the scene (Shift: slides it), nothing is picked or edited.
    if (m_readOnly)
    {
        m_hoverTheta = m_hoverPhi = -1;
        m_nav.begin(pos, event->modifiers().testFlag(Qt::ShiftModifier));
        return;
    }
    auto *editable = m_scene.data();
    const bool shift = event->modifiers().testFlag(Qt::ShiftModifier);
    // Alt + drag turns the scene whatever the tool.
    if (event->modifiers().testFlag(Qt::AltModifier))
    {
        m_nav.begin(pos, false);
        return;
    }
    if (m_tool == 1 || m_tool == 3)
    {
        // Wire and measure: a drag from one point to the other. A wire must not end on a source.
        Vec3 point;
        int snap = 0;
        m_measured = false;
        if (toolPoint(pos, shift, constructionOrigin(), -1, m_tool == 3, point, snap))
        {
            m_gesture = true;
            m_gestureA = m_gestureB = point;
            m_gestureSnap = 0;
        }
        updateToolText();
        update();
        return;
    }
    if (m_tool == 6)
    {
        // Pivot: placed on release, where the pointer is (on the plane, or on the end of a wire within
        // reach); a drag turns the scene.
        m_nav.begin(pos, shift);
        return;
    }
    if (m_tool == 2 || m_tool == 4 || m_tool == 5)
    {
        // Source, delete and cut act on the element clicked; a drag from elsewhere turns the scene.
        m_toolRow = hitTest(pos.x(), pos.y());
        if (m_toolRow < 0) m_nav.begin(pos, shift);
        return;
    }
    // Selection tool. Ctrl + click adds an element to the selection or takes it out.
    const bool ctrl = event->modifiers().testFlag(Qt::ControlModifier);
    const int handle = shift || ctrl ? -1 : handleAt(pos);
    // A source is an entity of its own. On a plain wire: selected at once, and a drag slides it along
    // its wire. On a ready-made element, whose source has its place: a click selects it (on release), a
    // drag from there moves the element as from anywhere else on it.
    int feedRow = -1, feedIndex = -1;
    m_feedClickRow = m_feedClickIndex = -1;
    if (handle < 0 && !shift && !ctrl && editable && (m_selectionFilter == 0 || m_selectionFilter == 3) &&
        feedAtPixel(pos, feedRow, feedIndex))
    {
        if (qsig::app::feedSlides(editable->sources()[static_cast<std::size_t>(feedRow)].type))
        {
            selectFeed(feedRow, feedIndex);
            m_feedDrag = true;
            m_feedMoved = false;
            return;
        }
        m_feedClickRow = feedRow;
        m_feedClickIndex = feedIndex;
    }
    if (handle >= 0 && editable)
    {
        // An end of the selected wire: dragging it reshapes the wire.
        Vec3 ends[2];
        m_handleRow = m_selection.first();
        qsig::app::wireEndpoints(editable->sources()[static_cast<std::size_t>(m_handleRow)], ends[0], ends[1]);
        m_handleEnd = handle;
        m_handleStart = ends[handle];
        m_handleMoved = false;
        return;
    }
    const int arrow = shift || ctrl ? -1 : axisAt(pos);
    if (arrow >= 0 && editable)
    {
        // An arrow of the selection: dragging it moves the selection along that axis.
        m_axisDrag = arrow;
        m_axisMoved = false;
        m_axisRows = selection();
        m_axisStarts.clear();
        for (const int row : m_axisRows) m_axisStarts.push_back(editable->sourcePosition(row));
        m_axisAnchor = m_axisRows.contains(m_selectedRow) ? editable->sourcePosition(m_selectedRow) : gizmoOrigin();
        update();
        return;
    }
    const int ring = shift || ctrl ? -1 : ringAt(pos);
    if (ring >= 0 && editable)
    {
        // A rotation handle of the selection: dragging it turns the selection around that axis.
        const auto handlesAt = frame().project(ringCentre());
        m_ringDrag = ring;
        m_ringAngle = 0.0;
        m_ringStart = std::atan2(pos.y() - handlesAt.y, pos.x() - handlesAt.x);
        update();
        return;
    }
    const int hit = shift ? -1 : hitTest(pos.x(), pos.y());
    if (hit >= 0 && editable)
    {
        // An element of a group stands for its whole group.
        const QList<int> members = editable->groupRows(hit);
        if (ctrl)
        {
            QList<int> rows = selection();
            const bool remove = rows.contains(hit);
            for (const int member : members)
            {
                rows.removeAll(member);
                if (!remove) rows.append(member);
            }
            select(rows, hit);
            return;
        }
        // One of several selected elements: the whole selection is dragged, and this one becomes the
        // main one. A click without a drag keeps it alone, with its group (on release).
        if (!m_selection.contains(hit))
            select(members, hit);
        else if (hit != m_selectedRow)
            select(selection(), hit);
        // Grab the element where it is on the active plane (not when that plane is seen edge-on).
        const Vec3 position = editable->sourcePosition(hit);
        Vec3 under;
        m_dragRow = hit;
        m_dragMoved = false;
        m_dragRows.clear();
        m_dragStarts.clear();
        if (frame().pick(pos.x(), pos.y(), position, constructionPlane().normal, under, 0.12))
        {
            m_dragStart = position;
            m_dragGrab = position - under;
            m_dragRows = selection();
            for (const int row : m_dragRows) m_dragStarts.push_back(editable->sourcePosition(row));
        }
        return;
    }
    m_nav.begin(pos, shift);
}

void AntennaView3D::mouseMoveEvent(QMouseEvent *event)
{
    const QPointF pos = event->position();
    event->accept();
    const bool shift = event->modifiers().testFlag(Qt::ShiftModifier);
    if (m_gesture)
    {
        Vec3 point;
        int snap = 0;
        if (toolPoint(pos, shift, constructionOrigin(), -1, m_tool == 3, point, snap))
        {
            m_gestureB = point;
            m_gestureSnap = snap;
            m_hoverAnchor = snap != 0;
            m_hoverPoint = point;
        }
        updateToolText();
        update();
        return;
    }
    auto *editable = m_scene.data();
    if (m_feedDrag && editable)
    {
        if (!m_feedMoved && std::abs(pos.x() - m_pressPos.x()) + std::abs(pos.y() - m_pressPos.y()) < 4.0) return;
        const bool firstMove = !m_feedMoved;
        m_feedMoved = true;
        if (firstMove) emit draggingChanged();
        // The source follows the pointer along its wire (the quarters and the middle attract it).
        editable->moveFeed(m_feedRow, m_feedIndex, pointOnElement(m_feedRow, pos));
        if (m_feedRow >= 0 && m_feedRow < editable->count())
        {
            const auto &host = editable->sources()[static_cast<std::size_t>(m_feedRow)];
            const double along = qsig::app::feedAt(host, m_feedIndex).fraction;
            const double lambda = qsig::constants::c0 / std::max(1.0, editable->frequency());
            // The length of the wire, or of the arc.
            const double span = host.type == qsig::app::AntennaElement::Type::Arc ? host.radius * host.sweepDeg * kDeg : host.length;
            setToolText(tr("Source à %1 % du fil · %2 (%3 λ) du premier bout")
                            .arg(plain(100.0 * along, 0), Units::formatQuantity(along * span, QStringLiteral("m"), 4),
                                 plain(along * span / lambda, 3)));
        }
        update();
        return;
    }
    if (m_handleEnd >= 0 && editable)
    {
        if (!m_handleMoved && std::abs(pos.x() - m_pressPos.x()) + std::abs(pos.y() - m_pressPos.y()) < 4.0) return;
        // The end moves parallel to the active plane from where it was, or joins another wire.
        Vec3 point;
        int snap = 0;
        if (!toolPoint(pos, shift, m_handleStart, m_handleRow, false, point, snap)) return;
        const bool first = !m_handleMoved;
        m_handleMoved = true;
        if (first) emit draggingChanged();
        m_hoverAnchor = snap != 0;
        m_hoverPoint = point;
        if (editable->moveWireEnd(m_handleRow, m_handleEnd, point))
        {
            const double length = editable->sources()[static_cast<std::size_t>(m_handleRow)].length;
            const double lambda = qsig::constants::c0 / std::max(1.0, editable->frequency());
            setToolText(QStringLiteral("L = %1 (%2 λ)%3")
                            .arg(Units::formatQuantity(length, QStringLiteral("m"), 4), plain(length / lambda, 3),
                                 snap == 1 ? tr(" · raccordé au bout") : snap == 2 ? tr(" · raccordé en T") : QString()));
        }
        update();
        return;
    }
    if (m_ringDrag >= 0 && editable)
    {
        const auto ringFrame = frame();
        const auto handlesAt = ringFrame.project(ringCentre());
        const Vec3 first = unitAxis((m_ringDrag + 1) % 3), second = unitAxis((m_ringDrag + 2) % 3);
        const double ax = qsig::dot(first, ringFrame.right), ay = -qsig::dot(first, ringFrame.up);
        const double bx = qsig::dot(second, ringFrame.right), by = -qsig::dot(second, ringFrame.up);
        // A positive turn around the axis takes the first of the two other axes towards the second: on
        // screen, that is the sense in which the angle of the pointer grows, or the opposite one.
        const double sense = ax * by - ay * bx >= 0.0 ? 1.0 : -1.0;
        double turned = std::atan2(pos.y() - handlesAt.y, pos.x() - handlesAt.x) - m_ringStart;
        while (turned > kPi) turned -= 2.0 * kPi;
        while (turned < -kPi) turned += 2.0 * kPi;
        double angle = sense * turned / kDeg;
        const double angleStep = event->modifiers().testFlag(Qt::ShiftModifier) ? 0.0 : std::clamp(m_rotationStep, 0.0, 180.0);
        if (angleStep > 0.0) angle = std::round(angle / angleStep) * angleStep;
        if (angle != m_ringAngle)
        {
            m_ringAngle = angle;
            const QString about = m_rotationPivot == 1 ? tr(" · autour de l'origine") : m_rotationPivot == 2 && m_pivotSet ? tr(" · autour du pivot") : QString();
            setToolText(tr("Rotation autour de %1 : %2°").arg(axisName(m_ringDrag), plain(angle, 1)) + about);
            update();
        }
        return;
    }
    if (m_axisDrag >= 0 && editable)
    {
        if (!m_axisMoved && std::abs(pos.x() - m_pressPos.x()) + std::abs(pos.y() - m_pressPos.y()) < 4.0) return;
        // The view is an orthographic projection: the displacement along the axis is the part of the
        // pointer's path along the axis as it is seen on screen.
        const auto f = frame();
        const Vec3 unit = unitAxis(m_axisDrag);
        const double sx = f.scale * qsig::dot(unit, f.right), sy = -f.scale * qsig::dot(unit, f.up);
        const double seen2 = sx * sx + sy * sy;
        if (!(seen2 > 0.0)) return;
        double along = ((pos.x() - m_pressPos.x()) * sx + (pos.y() - m_pressPos.y()) * sy) / seen2;
        // The main element lands on a tenth of the grid step along the axis, unless Shift is held.
        const double step = shift ? 0.0 : snapStep();
        if (step > 0.0)
        {
            const double from = qsig::dot(m_axisAnchor, unit);
            along = std::round((from + along) / step) * step - from;
        }
        std::vector<Vec3> positions;
        positions.reserve(m_axisStarts.size());
        for (const Vec3 &from : m_axisStarts) positions.push_back(from + unit * along);
        const bool first = !m_axisMoved;
        m_axisMoved = true;
        if (first) emit draggingChanged();
        editable->dragElements(m_axisRows, positions);
        const double lambda = qsig::constants::c0 / std::max(1.0, editable->frequency());
        setToolText(QStringLiteral("Δ%1 = %2 (%3 λ)")
                        .arg(axisName(m_axisDrag), Units::formatQuantity(along, QStringLiteral("m"), 4), plain(along / lambda, 3)));
        return;
    }
    if (m_dragRow >= 0 && editable)
    {
        if (m_dragRows.isEmpty()) return; // the plane is seen edge-on: nothing to drag along
        if (!m_dragMoved && std::abs(pos.x() - m_pressPos.x()) + std::abs(pos.y() - m_pressPos.y()) < 4.0) return;
        const Plane plane = constructionPlane();
        Vec3 under;
        if (!frame().pick(pos.x(), pos.y(), m_dragStart, plane.normal, under, 0.12)) return;
        // In-plane displacement, the element on the grid of the plane unless Shift is held. The
        // coordinate normal to the plane never changes. The other selected elements follow.
        const Vec3 start = m_dragStart;
        const Vec3 target = onPlaneGrid(plane, under + m_dragGrab, shift ? 0.0 : snapStep());
        const Vec3 moved = target - start;
        const Vec3 delta = plane.a * qsig::dot(moved, plane.a) + plane.b * qsig::dot(moved, plane.b);
        std::vector<Vec3> positions;
        positions.reserve(m_dragStarts.size());
        for (const Vec3 &from : m_dragStarts) positions.push_back(from + delta);
        const bool first = !m_dragMoved;
        m_dragMoved = true;
        if (first) emit draggingChanged();
        editable->dragElements(m_dragRows, positions);
        return;
    }
    if (m_nav.drag(pos)) emit viewChanged();
}

void AntennaView3D::mouseReleaseEvent(QMouseEvent *event)
{
    const QPointF pos = event->position();
    event->accept();
    if (m_pressFace >= 0)
    {
        // A face of the cube, one of its arrows or "Cadrer": released where it was pressed.
        if (m_nav.controlAt(pos) == m_pressFace)
        {
            m_nav.activate(m_pressFace);
            emit viewChanged();
        }
        m_pressFace = -1;
        return;
    }
    auto *editable = m_scene.data();
    // A click on the source of a ready-made element selects the source; a drag from there moved the
    // element (or turned the view) and is finished below.
    const int clickRow = m_feedClickRow, clickIndex = m_feedClickIndex;
    m_feedClickRow = m_feedClickIndex = -1;
    if (clickRow >= 0 && event->button() == Qt::LeftButton &&
        std::abs(pos.x() - m_pressPos.x()) + std::abs(pos.y() - m_pressPos.y()) < 4.0 && !(m_dragRow >= 0 && m_dragMoved) && !m_axisMoved)
    {
        m_dragRow = -1;
        m_dragMoved = false;
        m_dragRows.clear();
        m_dragStarts.clear();
        m_axisDrag = -1;
        m_axisRows.clear();
        m_axisStarts.clear();
        m_ringDrag = -1;
        m_ringAngle = 0.0;
        if (m_nav.active()) m_nav.end();
        selectFeed(clickRow, clickIndex);
        return;
    }
    if (m_feedDrag)
    {
        const bool slid = m_feedMoved;
        m_feedDrag = false;
        m_feedMoved = false;
        if (slid)
        {
            if (editable) editable->endDrag();
            emit draggingChanged();
            setToolText({});
        }
        update();
        return;
    }
    if (m_gesture)
    {
        m_gesture = false;
        const bool drawn = std::abs(pos.x() - m_pressPos.x()) + std::abs(pos.y() - m_pressPos.y()) >= 5.0;
        if (m_tool == 3)
            m_measured = drawn; // the measure stays on screen until the next one
        else if (drawn && editable)
        {
            const int row = editable->addWire(m_gestureA, m_gestureB);
            if (row >= 0) select(QList<int>{row}, row);
        }
        m_hoverAnchor = false;
        m_gestureSnap = m_tool == 3 ? m_gestureSnap : 0;
        updateToolText();
        update();
        return;
    }
    if (m_toolRow >= 0)
    {
        const int row = m_toolRow;
        m_toolRow = -1;
        if (!editable || hitTest(pos.x(), pos.y()) != row) return;
        if (m_tool == 4)
        {
            // On a source: the source goes, its wire stays.
            int feedRow = -1, feedIndex = -1;
            if (feedAtPixel(pos, feedRow, feedIndex) && feedRow == row)
            {
                clearFeed();
                editable->deleteFeed(feedRow, feedIndex);
                return;
            }
            select({}, -1);
            editable->removeSource(row);
            return;
        }
        // Source and cut: on a plain wire, at the point of the wire under the pointer.
        const Vec3 at = pointOnElement(row, pos);
        if (m_tool == 5)
        {
            if (editable->cutWire(row, at) >= 0) select(QList<int>{row}, row);
            return;
        }
        const int placed = editable->placeFeed(row, at);
        if (placed >= 0)
            selectFeed(row, placed); // the new source, or the one already at that place
        else
            select(QList<int>{row}, row);
        return;
    }
    if (m_handleEnd >= 0)
    {
        const bool moved = m_handleMoved;
        m_handleEnd = -1;
        m_handleRow = -1;
        m_handleMoved = false;
        m_hoverAnchor = false;
        if (moved)
        {
            if (editable) editable->endDrag();
            emit draggingChanged();
            setToolText({});
        }
        update();
        return;
    }
    if (m_ringDrag >= 0)
    {
        // One undoable rotation around the point chosen (centre, origin or pivot), by the angle shown.
        const int ringAxis = m_ringDrag;
        const double ringAngle = m_ringAngle;
        m_ringDrag = -1;
        m_ringAngle = 0.0;
        setToolText({});
        if (ringAngle != 0.0) rotateSelection(ringAxis, ringAngle, m_rotationPivot, false);
        update();
        return;
    }
    if (m_axisDrag >= 0)
    {
        const bool moved = m_axisMoved;
        m_axisDrag = -1;
        m_axisMoved = false;
        m_axisRows.clear();
        m_axisStarts.clear();
        if (moved)
        {
            if (editable) editable->endDrag();
            emit draggingChanged();
            setToolText({});
        }
        update();
        return;
    }
    if (m_dragRow >= 0)
    {
        const bool moved = m_dragMoved;
        const int row = m_dragRow;
        m_dragRow = -1;
        m_dragMoved = false;
        m_dragRows.clear();
        m_dragStarts.clear();
        if (moved)
        {
            if (editable) editable->endDrag();
            emit draggingChanged();
        }
        else if (m_selection.size() > 1 && editable)
        {
            select(editable->groupRows(row), row); // a plain click on one of several selected elements
        }
        update();
        return;
    }
    const bool click = m_nav.active() && !m_nav.moved() && !m_readOnly;
    m_nav.end();
    // The pattern is drawn lighter while the view is turned: back to the full mesh.
    update();
    // A click on nothing clears the selection.
    if (click && event->button() == Qt::LeftButton && m_tool == 0) select({}, -1);
    // Pivot tool: a click places the pivot there, the rotations turn about it, the selection tool is back.
    if (click && event->button() == Qt::LeftButton && m_tool == 6)
    {
        placePivot(pos.x(), pos.y());
        setTool(0);
    }
    // A right click: the context menu of the element under the pointer (selected first, with its
    // group, unless it already is), or of the scene.
    if (click && event->button() == Qt::RightButton)
    {
        const int hit = hitTest(pos.x(), pos.y());
        if (hit >= 0 && editable)
        {
            if (!m_selection.contains(hit))
                select(editable->groupRows(hit), hit);
            else if (hit != m_selectedRow)
                select(selection(), hit);
        }
        emit contextMenuRequested(pos.x(), pos.y(), hit);
    }
}

void AntennaView3D::mouseDoubleClickEvent(QMouseEvent *event)
{
    event->accept();
    if (event->button() != Qt::LeftButton) return;
    const QPointF pos = event->position();
    // The second of two quick clicks on an arrow of the cube arrives here: it counts as a click.
    m_pressFace = m_nav.controlAt(pos);
    if (m_pressFace >= 0 || hitTest(pos.x(), pos.y()) >= 0) return;
    resetView();
}

void AntennaView3D::wheelEvent(QWheelEvent *event)
{
    (void)frame(); // viewport size, for the zoom about the cursor
    m_nav.wheel(event->position(), event->angleDelta().y());
    event->accept();
    emit viewChanged();
}

void AntennaView3D::hoverMoveEvent(QHoverEvent *event)
{
    bool repaint = m_nav.setHover(event->position());
    // Results view: the sample of the radiation pattern under the pointer, read out beside it. Among the
    // samples drawn within 9 pixels, the one nearest to the viewer.
    if (m_readOnly)
    {
        int theta = -1, phi = -1;
        const auto *s = scene();
        if (s && m_showPattern && !m_nav.active())
        {
            const auto result = s->result();
            if (result && result->valid && result->hasPattern)
            {
                const AntennaResult &r = *result;
                const std::size_t nt = r.thetaDeg.size(), np = r.phiDeg.size();
                if (nt >= 2 && np >= 3 && r.gainDbi.size() == nt * np)
                {
                    const auto f = frame();
                    const double range = std::clamp(m_patternRange, 5.0, 60.0);
                    const double extent = 1.15 * m_nav.camera.radius;
                    const QPointF pos = event->position();
                    double nearest = 0.0;
                    for (std::size_t t = 0; t < nt; ++t)
                        for (std::size_t q = 0; q < np; ++q)
                        {
                            const double g = r.gainDbi[t * np + q];
                            const double db = std::isfinite(g) ? g - r.directivityDbi : -400.0;
                            const auto sp = f.project(m_nav.camera.centre + direction(r.thetaDeg[t], r.phiDeg[q]) *
                                                                                (extent * patternRadius(m_patternScale, db, range)));
                            const double dx = sp.x - pos.x(), dy = sp.y - pos.y();
                            if (dx * dx + dy * dy > 81.0) continue;
                            if (theta < 0 || sp.depth < nearest)
                            {
                                nearest = sp.depth;
                                theta = static_cast<int>(t);
                                phi = static_cast<int>(q);
                            }
                        }
                }
            }
        }
        if (theta != m_hoverTheta || phi != m_hoverPhi)
        {
            m_hoverTheta = theta;
            m_hoverPhi = phi;
            repaint = true;
        }
    }
    // Wire, measure and pivot tools: show what the pointer would snap to (an end, the body of a wire).
    if (m_tool == 1 || m_tool == 3 || m_tool == 6)
    {
        Vec3 point;
        int snap = 0;
        const bool found = toolPoint(event->position(), false, constructionOrigin(), -1, m_tool != 1, point, snap) && snap != 0;
        if (found != m_hoverAnchor || (found && !(point == m_hoverPoint))) repaint = true;
        m_hoverAnchor = found;
        m_hoverPoint = point;
    }
    // The source under the pointer is ringed: it can be selected, slid or deleted.
    int hoverRow = -1, hoverIndex = -1;
    if (!m_readOnly && (m_tool == 0 || m_tool == 4)) feedAtPixel(event->position(), hoverRow, hoverIndex);
    if (hoverRow != m_hoverFeedRow || hoverIndex != m_hoverFeedIndex)
    {
        m_hoverFeedRow = hoverRow;
        m_hoverFeedIndex = hoverIndex;
        repaint = true;
    }
    const int arrow = axisAt(event->position());
    if (arrow != m_hoverAxis)
    {
        m_hoverAxis = arrow;
        repaint = true;
    }
    const int ring = arrow >= 0 ? -1 : ringAt(event->position());
    if (ring != m_hoverRing)
    {
        m_hoverRing = ring;
        repaint = true;
    }
    if (repaint) update();
}

void AntennaView3D::hoverLeaveEvent(QHoverEvent *)
{
    const bool arrow = m_hoverAxis >= 0 || m_hoverRing >= 0 || m_hoverFeedRow >= 0, sample = m_hoverTheta >= 0;
    m_hoverFeedRow = m_hoverFeedIndex = -1;
    m_hoverAxis = -1;
    m_hoverRing = -1;
    m_hoverTheta = m_hoverPhi = -1;
    if (m_nav.clearHover() || arrow || sample) update();
}

// Radiation pattern around the antenna. The radius of a direction follows the chosen scale; the
// colour always tells the gain in dB over the last 'patternRange' dB. Opaque faces painted back to
// front and shaded by a light above the viewer's left shoulder: the lobes read as volumes.
void AntennaView3D::drawPattern(QPainter &p, const qsig::app::OrbitCamera::Frame &f, const AntennaResult &r,
                                const Vec3 &centre, double radius) const
{
    const std::size_t nt = r.thetaDeg.size(), np = r.phiDeg.size();
    if (nt < 2 || np < 3 || r.gainDbi.size() != nt * np) return;
    const double range = std::clamp(m_patternRange, 5.0, 60.0), gmax = r.directivityDbi;
    const double extent = 1.15 * radius;
    auto decibels = [&](std::size_t t, std::size_t q)
    {
        const double g = r.gainDbi[t * np + q % np]; // -inf in a null
        return std::isfinite(g) ? g - gmax : -400.0;
    };
    auto radial = [&](double db)
    {
        switch (m_patternScale)
        {
        case 0:
            return std::pow(10.0, db / 20.0);
        case 1:
            return std::pow(10.0, db / 10.0);
        default:
            return std::clamp((db + range) / range, 0.0, 1.0);
        }
    };
    // Rows 0 and nt + 1 are the poles (the grid stops half a step short of them): mean of the nearest row.
    const std::size_t rows = nt + 2;
    struct Vertex
    {
        double x, y, depth; // screen pixels; depth in pixels too
        double db;
        double rho;
    };
    std::vector<Vertex> grid(rows * np);
    double poleDb[2] = {0.0, 0.0};
    for (std::size_t q = 0; q < np; ++q)
    {
        poleDb[0] += decibels(0, q) / static_cast<double>(np);
        poleDb[1] += decibels(nt - 1, q) / static_cast<double>(np);
    }
    for (std::size_t row = 0; row < rows; ++row)
        for (std::size_t q = 0; q < np; ++q)
        {
            const bool pole = row == 0 || row == rows - 1;
            const double theta = row == 0 ? 0.0 : row == rows - 1 ? 180.0 : r.thetaDeg[row - 1];
            const double db = pole ? poleDb[row == 0 ? 0 : 1] : decibels(row - 1, q);
            const double rho = radial(db);
            const auto sp = f.project(centre + direction(theta, r.phiDeg[q]) * (extent * rho));
            grid[row * np + q] = {sp.x, sp.y, sp.depth * f.scale, db, rho};
        }
    struct Quad
    {
        std::size_t row, q;
        double depth;
    };
    std::vector<Quad> quads;
    // While the view is being turned or slid, one face out of four is drawn, without smoothing: the
    // full mesh comes back when the mouse is released.
    const std::size_t step = (m_nav.active() && np % 2 == 0) ? std::size_t{2} : std::size_t{1};
    quads.reserve((rows - 1) * np / (step * step) + np);
    for (std::size_t row = 0; row + 1 < rows; row += step)
        for (std::size_t q = 0; q < np; q += step)
        {
            const std::size_t row1 = std::min(row + step, rows - 1), q1 = (q + step) % np;
            const Vertex &a = grid[row * np + q], &b = grid[row * np + q1];
            const Vertex &c = grid[row1 * np + q1], &e = grid[row1 * np + q];
            if (std::max({a.rho, b.rho, c.rho, e.rho}) < 2e-3) continue; // nothing to see in a null
            quads.push_back({row, q, 0.25 * (a.depth + b.depth + c.depth + e.depth)});
        }
    std::sort(quads.begin(), quads.end(), [](const Quad &a, const Quad &b) { return a.depth > b.depth; });
    // Light direction in screen space (x right, y down, depth away from the viewer).
    const double lx = -0.38, ly = -0.50, lz = -0.78;
    QPointF corners[4];
    if (step > 1) p.setRenderHint(QPainter::Antialiasing, false);
    for (const Quad &quad : quads)
    {
        const std::size_t row1 = std::min(quad.row + step, rows - 1), q1 = (quad.q + step) % np;
        const Vertex &a = grid[quad.row * np + quad.q], &b = grid[quad.row * np + q1];
        const Vertex &c = grid[row1 * np + q1], &e = grid[row1 * np + quad.q];
        // Normal from the diagonals; two-sided lighting.
        const double ux = c.x - a.x, uy = c.y - a.y, uz = c.depth - a.depth;
        const double vx = e.x - b.x, vy = e.y - b.y, vz = e.depth - b.depth;
        const double nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
        const double length = std::sqrt(nx * nx + ny * ny + nz * nz);
        const double lit = length > 0.0 ? std::abs(nx * lx + ny * ly + nz * lz) / length : 1.0;
        const double shade = 0.52 + 0.48 * lit;
        const double level = std::clamp((0.25 * (a.db + b.db + c.db + e.db) + range) / range, 0.0, 1.0);
        const QColor base = levelColour(level);
        const QColor colour(static_cast<int>(base.red() * shade), static_cast<int>(base.green() * shade),
                            static_cast<int>(base.blue() * shade), 238);
        corners[0] = {a.x, a.y};
        corners[1] = {b.x, b.y};
        corners[2] = {c.x, c.y};
        corners[3] = {e.x, e.y};
        // The pen of the fill colour closes the hairlines between neighbouring faces.
        p.setPen(QPen(colour, 0.8));
        p.setBrush(colour);
        p.drawConvexPolygon(corners, 4);
        // Parallels and meridians every 15 degrees, hidden like the faces they belong to.
        const bool parallel = quad.row % 5 == 0 && quad.row > 0, meridian = quad.q % 5 == 0;
        if (parallel || meridian)
        {
            p.setPen(QPen(QColor(0, 0, 0, 70), 0.8));
            if (parallel) p.drawLine(corners[0], corners[1]);
            if (meridian) p.drawLine(corners[0], corners[3]);
        }
    }
    if (step > 1) p.setRenderHint(QPainter::Antialiasing, true);
    p.setBrush(Qt::NoBrush);
}

void AntennaView3D::paint(QPainter *painter)
{
    QPainter &p = *painter;
    p.fillRect(QRectF(0, 0, width(), height()), m_background);
    p.setRenderHint(QPainter::Antialiasing, true);
    const auto *s = scene();
    if (!s) return;
    // An empty scene still shows its construction plane: the first wire can be drawn on it.
    if (s->count() == 0)
        drawMessage(p, tr("Ajoutez un élément ou ouvrez un exemple depuis le panneau Scène,\nou tracez un fil avec l'outil Fil."));
    const auto f = frame();
    const Vec3 centre = m_nav.camera.centre;
    const double radius = m_nav.camera.radius;
    auto screen = [&f](const Vec3 &q)
    {
        const auto sp = f.project(q);
        return QPointF(sp.x, sp.y);
    };
    const auto result = s->result();
    const AntennaResult *r = result && result->valid ? result.get() : nullptr;

    // Construction plane: the grid of the active plane at 'editOffset' along its normal. Wires are drawn
    // on it; a dragged element moves parallel to it.
    {
        // Lines on the grid of the plane (through its anchor), around the centre of the view.
        const Plane plane = constructionPlane();
        const PlaneAxes axes{plane.a, plane.b, plane.normal};
        const double half = 1.3 * radius, step = gridStep();
        const Vec3 gridCentre = onPlaneGrid(plane, constructionOrigin(), step);
        const Vec3 origin = gridCentre;
        const int firstA = static_cast<int>(std::lround(qsig::dot(gridCentre - plane.anchor, plane.a) / step));
        const int firstB = static_cast<int>(std::lround(qsig::dot(gridCentre - plane.anchor, plane.b) / step));
        const int lines = static_cast<int>(std::floor(half / step));
        QPen thin(m_grid, 1.0);
        thin.setCosmetic(true);
        QPen strong(withAlpha(m_grid, std::min(255, m_grid.alpha() * 3)), 1.2);
        strong.setCosmetic(true);
        // The two lines through the anchor (where the plane turns) are drawn stronger.
        for (int i = -lines; i <= lines; ++i)
        {
            const double v = i * step;
            p.setPen(firstA + i == 0 ? strong : thin);
            p.drawLine(screen(origin + axes.a * v - axes.b * half), screen(origin + axes.a * v + axes.b * half));
            p.setPen(firstB + i == 0 ? strong : thin);
            p.drawLine(screen(origin - axes.a * half + axes.b * v), screen(origin + axes.a * half + axes.b * v));
        }
    }

    // Axes of the scene, from the origin to the edge of the grid.
    if (m_showOrigin)
    {
        p.save();
        QFont small = p.font();
        small.setPixelSize(11);
        p.setFont(small);
        const QPointF from = screen(Vec3{});
        for (int axis = 0; axis < 3; ++axis)
        {
            const QPointF tip = screen(unitAxis(axis) * (1.3 * radius));
            p.setPen(QPen(withAlpha(axisColour(axis), 170), 1.3));
            p.drawLine(from, tip);
            p.drawText(tip + QPointF(5.0, 4.0), QStringLiteral("+") + axisName(axis));
        }
        p.restore();
    }

    // The pattern first when it is drawn around the antenna, so that the wires stay readable over it.
    if (r && m_showPattern && r->hasPattern) drawPattern(p, f, *r, centre, radius);

    const QColor plainWire(176, 184, 196);
    // Surfaces, before the wires: their triangles coloured by |J| once solved (the farthest first), or
    // their plain outline filled. While an element is dragged the currents are those of the previous
    // position: plain surfaces.
    const bool surfaceCurrents = r && m_showCurrents && r->maxSurfaceCurrent > 0.0 && !r->facets.empty() && !dragging();
    if (surfaceCurrents)
    {
        std::vector<std::pair<double, const qsig::app::AntennaFacet *>> order;
        order.reserve(r->facets.size());
        for (const auto &facet : r->facets) order.push_back({f.project((facet.a + facet.b + facet.c) / 3.0).depth, &facet});
        std::sort(order.begin(), order.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
        for (const auto &entry : order)
        {
            const qsig::app::AntennaFacet *facet = entry.second;
            const QColor colour = levelColour((facet->ja + facet->jb + facet->jc) / (3.0 * r->maxSurfaceCurrent));
            p.setPen(QPen(withAlpha(m_background, 110), 0.6));
            p.setBrush(withAlpha(colour, 215));
            const QPointF corners[3] = {screen(facet->a), screen(facet->b), screen(facet->c)};
            p.drawPolygon(corners, 3);
        }
        p.setBrush(Qt::NoBrush);
    }
    else
    {
        p.setPen(Qt::NoPen);
        p.setBrush(withAlpha(plainWire, 70));
        for (const auto &e : s->sources())
        {
            const qsig::app::AntennaShape shape = qsig::app::antennaShape(e);
            if (!shape.surface) continue;
            // A dielectric solid in the accent colour, a conductor in that of the wires.
            p.setBrush(qsig::app::elementIsDielectric(e) ? withAlpha(m_accent, 55) : withAlpha(plainWire, 70));
            // Every face of the skin: a solid shows through, its far faces under its near ones.
            for (const auto &skinFace : shape.faces)
            {
                QPolygonF outline;
                for (const auto &q : skinFace) outline << screen(q);
                p.drawPolygon(outline);
            }
        }
        p.setBrush(Qt::NoBrush);
    }

    // Wires: underlay of the selected element, then the wires coloured by |I| (or plain).
    for (const int row : selection())
    {
        // The main element of the selection (the one the properties show) has the stronger halo.
        QPen halo(withAlpha(m_accent, row == m_selectedRow ? 130 : 80), 8.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
        p.setPen(halo);
        for (const auto &line : qsig::app::antennaShape(s->sources()[static_cast<std::size_t>(row)]).polylines)
        {
            QPolygonF path;
            for (const auto &q : line) path << screen(q);
            p.drawPolyline(path);
        }
    }
    // While an element is dragged the currents are those of the previous position: plain wires.
    if (r && m_showCurrents && r->maxCurrent > 0.0 && !dragging())
    {
        for (const auto &seg : r->segments)
        {
            const double level = 0.5 * (std::abs(seg.i0) + std::abs(seg.i1)) / r->maxCurrent;
            p.setPen(QPen(levelColour(level), 3.2, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(screen(seg.p0), screen(seg.p1));
        }
        // The rim of the surfaces whose currents are not drawn (scene solved without them).
        if (!surfaceCurrents)
        {
            p.setPen(QPen(plainWire, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            for (const auto &e : s->sources())
            {
                const qsig::app::AntennaShape shape = qsig::app::antennaShape(e);
                if (!shape.surface) continue;
                for (const auto &skinEdge : shape.polylines)
                {
                    QPolygonF outline;
                    for (const auto &q : skinEdge) outline << screen(q);
                    p.drawPolyline(outline);
                }
            }
        }
    }
    else
    {
        p.setPen(QPen(plainWire, 2.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        for (const auto &e : s->sources())
            for (const auto &line : qsig::app::antennaShape(e).polylines)
            {
                QPolygonF path;
                for (const auto &q : line) path << screen(q);
                p.drawPolyline(path);
            }
    }
    // Sources: entities placed on the wires. The one under the pointer is ringed; the selected one is
    // ringed in the accent colour and drawn larger.
    for (int feedHost = 0; feedHost < s->count(); ++feedHost)
    {
        const auto feedSpots = qsig::app::antennaShape(s->sources()[static_cast<std::size_t>(feedHost)]).feeds;
        for (std::size_t feedSlot = 0; feedSlot < feedSpots.size(); ++feedSlot)
        {
            const bool feedChosen = !m_readOnly && feedHost == m_feedRow && static_cast<int>(feedSlot) == m_feedIndex;
            const bool feedHovered = feedHost == m_hoverFeedRow && static_cast<int>(feedSlot) == m_hoverFeedIndex;
            const QPointF feedSpot = screen(feedSpots[feedSlot]);
            if (feedChosen || feedHovered)
            {
                p.setPen(QPen(feedChosen ? m_accent : m_text, feedChosen ? 2.0 : 1.4));
                p.setBrush(Qt::NoBrush);
                p.drawEllipse(feedSpot, 9.5, 9.5);
            }
            p.setPen(QPen(Qt::white, 1.4));
            p.setBrush(QColor(255, 90, 90));
            p.drawEllipse(feedSpot, feedChosen ? 5.5 : 4.5, feedChosen ? 5.5 : 4.5);
        }
    }
    p.setBrush(Qt::NoBrush);

    // Ends of the selected plain wire: handles to drag.
    if (m_tool == 0 && m_selection.size() == 1 && m_selection.first() >= 0 && m_selection.first() < s->count())
    {
        const auto &element = s->sources()[static_cast<std::size_t>(m_selection.first())];
        if (element.type == qsig::app::AntennaElement::Type::Wire)
        {
            Vec3 ends[2];
            qsig::app::wireEndpoints(element, ends[0], ends[1]);
            p.setPen(QPen(m_accent, 1.6));
            p.setBrush(m_background);
            for (const Vec3 &end : ends)
            {
                const QPointF at = screen(end);
                p.drawRect(QRectF(at.x() - 4.5, at.y() - 4.5, 9.0, 9.0));
            }
            p.setBrush(Qt::NoBrush);
        }
    }

    // Arrows on the selection: drag one to move the selection along that axis.
    if (gizmoVisible())
    {
        p.save();
        QFont small = p.font();
        small.setPixelSize(11);
        small.setBold(true);
        p.setFont(small);
        const QPointF from = screen(gizmoOrigin());
        for (int axis = 0; axis < 3; ++axis)
        {
            const Vec3 unit = unitAxis(axis);
            const QPointF along(qsig::dot(unit, f.right), -qsig::dot(unit, f.up));
            const double seen = std::hypot(along.x(), along.y());
            if (seen < 0.22) continue; // seen end-on
            const bool active = axis == m_axisDrag || (m_axisDrag < 0 && axis == m_hoverAxis);
            const QColor colour = active ? axisColour(axis).lighter(125) : axisColour(axis);
            const QPointF tip = from + kArrowLength * along, forward = along / seen, side(-forward.y(), forward.x());
            p.setPen(QPen(colour, active ? 4.2 : 2.8, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(from, tip);
            QPolygonF head;
            head << tip + 10.0 * forward << tip + 4.8 * side << tip - 4.8 * side;
            p.setPen(Qt::NoPen);
            p.setBrush(colour);
            p.drawPolygon(head);
            p.setBrush(Qt::NoBrush);
            p.setPen(colour);
            p.drawText(tip + 18.0 * forward + QPointF(-3.0, 4.0), axisName(axis));
        }
        // Rotation handles: a quarter circle per axis, in its colour, around the point the selection
        // turns about (a dashed line joins it to the selection when it is elsewhere); dragging one turns
        // the selection.
        const QPointF turnsAbout = screen(ringCentre());
        if (std::hypot(turnsAbout.x() - from.x(), turnsAbout.y() - from.y()) > 2.0)
        {
            p.setPen(QPen(withAlpha(m_text, 150), 1.0, Qt::DashLine));
            p.drawLine(from, turnsAbout);
            p.setPen(QPen(m_text, 1.2));
            p.setBrush(m_background);
            p.drawEllipse(turnsAbout, 3.0, 3.0);
            p.setBrush(Qt::NoBrush);
        }
        for (int axis = 0; axis < 3; ++axis)
        {
            const Vec3 firstAxis = unitAxis((axis + 1) % 3), secondAxis = unitAxis((axis + 2) % 3);
            const QPointF pa(qsig::dot(firstAxis, f.right), -qsig::dot(firstAxis, f.up)), pb(qsig::dot(secondAxis, f.right), -qsig::dot(secondAxis, f.up));
            if (std::abs(pa.x() * pb.y() - pa.y() * pb.x()) < 0.2) continue; // its plane is seen edge-on
            const bool activeRing = axis == m_ringDrag || (m_ringDrag < 0 && m_axisDrag < 0 && axis == m_hoverRing);
            const QColor ringColour = activeRing ? axisColour(axis).lighter(125) : withAlpha(axisColour(axis), 200);
            QPolygonF arc;
            for (int k = 0; k <= 16; ++k)
            {
                const double t = 0.5 * kPi * k / 16.0;
                arc << turnsAbout + kRingRadius * (std::cos(t) * pa + std::sin(t) * pb);
            }
            p.setPen(QPen(ringColour, activeRing ? 3.6 : 2.2, Qt::SolidLine, Qt::RoundCap));
            p.drawPolyline(arc);
        }
        // While a handle is dragged: the selection as it will be once turned, in dashes.
        if (m_ringDrag >= 0 && m_ringAngle != 0.0)
        {
            const Vec3 turnCentre = ringCentre(), turnAxis = unitAxis(m_ringDrag);
            const double cosTurn = std::cos(m_ringAngle * kDeg), sinTurn = std::sin(m_ringAngle * kDeg);
            p.setPen(QPen(m_accent, 1.6, Qt::DashLine));
            for (const int row : selection())
            {
                if (row < 0 || row >= s->count()) continue;
                for (const auto &line : qsig::app::antennaShape(s->sources()[static_cast<std::size_t>(row)]).polylines)
                {
                    QPolygonF ghost;
                    for (const Vec3 &q : line)
                    {
                        const Vec3 v = q - turnCentre;
                        ghost << screen(turnCentre + v * cosTurn + qsig::cross(turnAxis, v) * sinTurn +
                                        turnAxis * (qsig::dot(turnAxis, v) * (1.0 - cosTurn)));
                    }
                    p.drawPolyline(ghost);
                }
            }
        }
        p.setPen(QPen(m_text, 1.2));
        p.setBrush(m_background);
        p.drawEllipse(from, 3.5, 3.5);
        p.restore();
    }
    // The focus of a reflector: a small cross and F (several along the focal line of a parabolic cylinder).
    p.setPen(QPen(withAlpha(m_text, 200), 1.3));
    for (const auto &e : s->sources())
        for (const Vec3 &mark : qsig::app::antennaShape(e).marks)
        {
            const QPointF spot = screen(mark);
            p.drawLine(spot + QPointF(-5.0, -5.0), spot + QPointF(5.0, 5.0));
            p.drawLine(spot + QPointF(-5.0, 5.0), spot + QPointF(5.0, -5.0));
            p.drawText(spot + QPointF(7.0, -6.0), QStringLiteral("F"));
        }
    // The pivot placed for the rotations and the mirrors.
    if (m_pivotSet)
    {
        const QPointF at = screen(m_pivot);
        const QColor colour(224, 150, 60);
        p.setPen(QPen(colour, 1.8));
        QPolygonF diamond;
        diamond << at + QPointF(0.0, -5.0) << at + QPointF(5.0, 0.0) << at + QPointF(0.0, 5.0) << at + QPointF(-5.0, 0.0);
        p.drawPolygon(diamond);
        p.drawLine(at + QPointF(-9.0, 0.0), at + QPointF(9.0, 0.0));
        p.drawLine(at + QPointF(0.0, -9.0), at + QPointF(0.0, 9.0));
        p.drawText(at + QPointF(10.0, -8.0), tr("Pivot"));
    }

    // Designer tools: the wire being drawn, the measure, the anchor the pointer snaps to.
    if (m_gesture || m_measured)
    {
        const QPointF a = screen(m_gestureA), b = screen(m_gestureB);
        const bool measure = m_tool == 3;
        p.setPen(measure ? QPen(m_text, 1.4, Qt::DashLine) : QPen(m_accent, 2.6, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(a, b);
        p.setPen(QPen(measure ? m_text : m_accent, 1.4));
        p.setBrush(m_background);
        p.drawEllipse(a, 4.0, 4.0);
        p.drawEllipse(b, 4.0, 4.0);
        p.setBrush(Qt::NoBrush);
        if (!m_toolText.isEmpty())
        {
            QFont readout = p.font();
            readout.setPixelSize(12);
            p.setFont(readout);
            const QRectF box = QRectF(p.fontMetrics().boundingRect(m_toolText)).adjusted(-6.0, -3.0, 6.0, 3.0);
            const QPointF at = 0.5 * (a + b) + QPointF(12.0, -14.0);
            p.setPen(Qt::NoPen);
            p.setBrush(withAlpha(m_background, 225));
            p.drawRoundedRect(box.translated(at), 4.0, 4.0);
            p.setBrush(Qt::NoBrush);
            p.setPen(m_text);
            p.drawText(at, m_toolText);
        }
    }
    if (m_hoverAnchor && (m_tool == 1 || m_tool == 3 || m_tool == 6 || m_handleEnd >= 0))
    {
        p.setPen(QPen(m_accent, 1.6));
        p.drawEllipse(screen(m_hoverPoint), 8.0, 8.0);
    }

    if (m_showCompass) m_nav.drawAxes(p, QPointF(46.0, height() - 46.0));
    m_nav.drawCube(p, cubeColours(m_background, m_text, m_accent));

    // Legend: the colour scale when something is coloured, then what it measures.
    QFont font = p.font();
    font.setPixelSize(11);
    p.setFont(font);
    const bool currents = r && m_showCurrents && r->maxCurrent > 0.0 && !dragging();
    const bool pattern = r && m_showPattern && r->hasPattern;
    // Results view: the direction of the maximum, and the sample of the pattern under the pointer.
    if (m_readOnly && pattern)
    {
        const std::size_t sampleRows = r->thetaDeg.size(), sampleColumns = r->phiDeg.size();
        const double hoverRange = std::clamp(m_patternRange, 5.0, 60.0);
        auto patternSpot = [&](double thetaDeg, double phiDeg, double levelDb)
        {
            return screen(centre + direction(thetaDeg, phiDeg) * (1.15 * radius * patternRadius(m_patternScale, levelDb, hoverRange)));
        };
        const QPointF maxSpot = patternSpot(r->mainThetaDeg, r->mainPhiDeg, 0.0);
        p.setPen(QPen(m_background, 1.5));
        p.setBrush(m_accent);
        p.drawEllipse(maxSpot, 4.5, 4.5);
        p.setPen(m_accent);
        p.drawText(maxSpot + QPointF(9.0, 4.0), tr("max : θ = %1°, φ = %2°").arg(plain(r->mainThetaDeg, 0), plain(r->mainPhiDeg, 0)));
        if (m_hoverTheta >= 0 && m_hoverPhi >= 0 && static_cast<std::size_t>(m_hoverTheta) < sampleRows &&
            static_cast<std::size_t>(m_hoverPhi) < sampleColumns && r->gainDbi.size() == sampleRows * sampleColumns)
        {
            const std::size_t ht = static_cast<std::size_t>(m_hoverTheta), hq = static_cast<std::size_t>(m_hoverPhi);
            const double hoverGain = r->gainDbi[ht * sampleColumns + hq];
            const bool hoverFinite = std::isfinite(hoverGain);
            const double hoverLevel = hoverFinite ? hoverGain - r->directivityDbi : -400.0;
            const QPointF hoverSpot = patternSpot(r->thetaDeg[ht], r->phiDeg[hq], hoverLevel);
            p.setPen(QPen(m_background, 1.5));
            p.setBrush(m_text);
            p.drawEllipse(hoverSpot, 4.0, 4.0);
            const QString minusInfinity = QStringLiteral("−∞");
            const QStringList tipLines{tr("θ = %1°").arg(plain(r->thetaDeg[ht], 1)),
                                       tr("φ = %1°").arg(plain(r->phiDeg[hq], 1)),
                                       tr("Gain : %1 dBi").arg(hoverFinite ? plain(hoverGain, 2) : minusInfinity),
                                       tr("Niveau relatif : %1 dB").arg(hoverFinite ? plain(hoverLevel, 2) : minusInfinity),
                                       tr("Champ normalisé : %1").arg(plain(std::pow(10.0, hoverLevel / 20.0), 3))};
            const QFontMetricsF tipMetrics(p.font());
            double tipWidth = 0.0;
            for (const QString &tipLine : tipLines) tipWidth = std::max(tipWidth, tipMetrics.horizontalAdvance(tipLine));
            const double tipLineHeight = tipMetrics.height() + 2.0;
            QRectF tipBox(hoverSpot.x() + 14.0, hoverSpot.y() - 12.0, tipWidth + 20.0,
                          tipLineHeight * static_cast<double>(tipLines.size()) + 14.0);
            if (tipBox.right() > width() - 8.0) tipBox.moveRight(hoverSpot.x() - 14.0);
            if (tipBox.bottom() > height() - 34.0) tipBox.moveBottom(height() - 34.0);
            if (tipBox.top() < 8.0) tipBox.moveTop(8.0);
            p.setPen(QPen(withAlpha(m_text, 90), 1.0));
            p.setBrush(withAlpha(m_background, 235));
            p.drawRoundedRect(tipBox, 6.0, 6.0);
            p.setPen(m_text);
            double tipY = tipBox.top() + 7.0;
            for (const QString &tipLine : tipLines)
            {
                p.drawText(QRectF(tipBox.left() + 10.0, tipY, tipWidth, tipLineHeight), Qt::AlignLeft | Qt::AlignVCenter, tipLine);
                tipY += tipLineHeight;
            }
        }
        p.setBrush(Qt::NoBrush);
    }
    QString legend;
    if (currents)
        legend = tr("Fils : |I| relatif au maximum (%1)").arg(Units::formatQuantity(r->maxCurrent, QStringLiteral("A"), 3));
    if (surfaceCurrents)
        legend += (legend.isEmpty() ? QString() : QStringLiteral("   ·   ")) +
                  tr("Surfaces : |J| relatif au maximum (%1)").arg(Units::formatQuantity(r->maxSurfaceCurrent, QStringLiteral("A/m"), 3));
    if (pattern)
    {
        const QString scale = m_patternScale == 0   ? tr("rayon ∝ champ |E|")
                              : m_patternScale == 1 ? tr("rayon ∝ puissance")
                                                    : tr("rayon en dB");
        legend += (legend.isEmpty() ? QString() : QStringLiteral("   ·   ")) +
                  tr("Diagramme : %1, couleurs de %2 à %3 dBi")
                      .arg(scale, plain(r->directivityDbi - std::clamp(m_patternRange, 5.0, 60.0), 1), plain(r->directivityDbi, 2));
    }
    if (!r) legend = s->count() == 0 ? QString() : s->busy() ? tr("Calcul…") : tr("Pas encore de résultat");
    double textLeft = 96.0;
    if (currents || surfaceCurrents || pattern)
    {
        const QRectF bar(96.0, height() - 22.0, 70.0, 8.0);
        QLinearGradient gradient(bar.topLeft(), bar.topRight());
        for (int k = 0; k <= 8; ++k) gradient.setColorAt(k / 8.0, levelColour(k / 8.0));
        p.setPen(Qt::NoPen);
        p.setBrush(gradient);
        p.drawRoundedRect(bar, 3.0, 3.0);
        p.setBrush(Qt::NoBrush);
        textLeft = bar.right() + 8.0;
    }
    p.setPen(withAlpha(m_text, 200));
    p.drawText(QRectF(textLeft, height() - 27.0, width() - textLeft - 8.0, 18.0), Qt::AlignLeft | Qt::AlignVCenter, legend);
}

// ---- Polar plot ----

PolarPlot::PolarPlot(QQuickItem *parent) : AntennaPlotBase(parent)
{
    connect(this, &PolarPlot::plotChanged, this, [this] { update(); });
    connect(this, &AntennaPlotBase::sceneChanged, this, &PolarPlot::plotChanged);
}

void PolarPlot::resultsChanged()
{
    emit plotChanged(); // title and beam text follow the result
}

const qsig::app::PatternCut *PolarPlot::currentCut() const
{
    const auto *s = scene();
    const auto result = s ? s->result() : nullptr;
    if (!result || !result->valid || !result->hasPattern) return nullptr;
    return m_cut == 0 ? &result->horizontal : m_cut == 1 ? &result->vertical : &result->vertical90;
}

QString PolarPlot::cutTitle() const
{
    const auto *s = scene();
    const auto result = s ? s->result() : nullptr;
    const double phi = result && result->hasPattern ? result->mainPhiDeg : 0.0;
    if (m_cut == 0) return tr("Plan horizontal (θ = 90°), angle φ depuis +x");
    if (m_cut == 1) return tr("Plan vertical du lobe principal (φ = %1°), angle θ depuis +z").arg(plain(phi, 0));
    return tr("Plan vertical perpendiculaire (φ = %1°), angle θ depuis +z").arg(plain(std::fmod(phi + 90.0, 360.0), 0));
}

QString PolarPlot::beamText() const
{
    const auto *cut = currentCut();
    if (!cut || cut->gainDbi.size() < 3) return {};
    const auto &g = cut->gainDbi;
    const std::size_t n = g.size() - 1; // the last angle repeats the first (360 = 0)
    std::size_t top = 0;
    for (std::size_t i = 1; i < n; ++i)
        if (g[i] > g[top]) top = i;
    const double step = cut->angleDeg[1] - cut->angleDeg[0];
    std::size_t left = 0, right = 0;
    while (left < n && g[(top + n - left) % n] >= g[top] - 3.0) ++left;
    while (right < n && g[(top + right) % n] >= g[top] - 3.0) ++right;
    QString text = tr("Maximum %1 dBi à %2°").arg(plain(g[top], 2), plain(cut->angleDeg[top], 0));
    if (left + right < n) text += tr(" · ouverture à −3 dB : %1°").arg(plain(static_cast<double>(left + right - 1) * step, 0));
    const auto *s = scene();
    if (const auto result = s ? s->result() : nullptr)
        text += tr(" · centre : %1 dBi").arg(plain(result->directivityDbi - std::clamp(m_range, 10.0, 60.0), 1));
    return text;
}

void PolarPlot::paint(QPainter *painter)
{
    QPainter &p = *painter;
    p.fillRect(QRectF(0, 0, width(), height()), m_background);
    p.setRenderHint(QPainter::Antialiasing, true);
    const auto *cut = currentCut();
    const auto *s = scene();
    const auto result = s ? s->result() : nullptr;
    if (!cut || !result)
    {
        drawMessage(p, s && s->busy() ? tr("Calcul…") : tr("Pas de diagramme : la scène n'a pas encore de résultat."));
        return;
    }
    const QPointF centre(0.5 * width(), 0.5 * height());
    const double radius = 0.5 * std::min(width(), height()) - 34.0;
    if (radius < 20.0) return;
    const double range = std::clamp(m_range, 10.0, 60.0), gmax = result->directivityDbi;
    auto radiusOf = [&](double g) { return radius * std::clamp((g - (gmax - range)) / range, 0.0, 1.0); };
    // Horizontal cut: angle counterclockwise from the right. Vertical cuts: theta clockwise from the top.
    auto point = [&](double angleDeg, double rr)
    {
        const double a = angleDeg * kDeg;
        return m_cut == 0 ? centre + QPointF(rr * std::cos(a), -rr * std::sin(a)) : centre + QPointF(rr * std::sin(a), -rr * std::cos(a));
    };

    QFont font = p.font();
    font.setPixelSize(11);
    p.setFont(font);
    // Ring values written along the spoke (every 15 degrees, between two of the 30-degree spokes)
    // where the cut is lowest: away from the curve.
    double labelAngle = 15.0, lowest = 1e300;
    for (std::size_t i = 0; i + 1 < cut->angleDeg.size(); ++i)
    {
        const double a = cut->angleDeg[i];
        const double offGrid = std::fmod(a + 15.0, 30.0);
        if (std::abs(offGrid) > 1e-6 && std::abs(offGrid - 30.0) > 1e-6) continue;
        if (cut->gainDbi[i] < lowest)
        {
            lowest = cut->gainDbi[i];
            labelAngle = a;
        }
    }
    // Rings every 10 dB below the maximum, spokes every 30 degrees.
    for (int k = 0; k * 10.0 <= range + 1e-9; ++k)
    {
        const double rr = radiusOf(gmax - k * 10.0);
        QPen pen(k == 0 ? withAlpha(m_text, 110) : m_grid, 1.0);
        pen.setCosmetic(true);
        p.setPen(pen);
        p.drawEllipse(centre, rr, rr);
        if (rr < 20.0) continue; // the centre value is given under the plot: no room for it here
        p.setPen(withAlpha(m_text, 150));
        const QPointF at = point(labelAngle, rr);
        p.drawText(QRectF(at.x() - 32.0, at.y() - 8.0, 64.0, 16.0), Qt::AlignCenter,
                   plain(gmax - k * 10.0, 1) + QStringLiteral(" dBi"));
    }
    for (int a = 0; a < 360; a += 30)
    {
        QPen pen(m_grid, 1.0);
        pen.setCosmetic(true);
        p.setPen(pen);
        p.drawLine(centre, point(a, radius));
        p.setPen(withAlpha(m_text, 170));
        const QPointF label = point(a, radius + 16.0);
        p.drawText(QRectF(label.x() - 20.0, label.y() - 8.0, 40.0, 16.0), Qt::AlignCenter, QStringLiteral("%1°").arg(a));
    }
    // The cut.
    QPolygonF curve;
    for (std::size_t i = 0; i < cut->angleDeg.size(); ++i) curve << point(cut->angleDeg[i], radiusOf(cut->gainDbi[i]));
    p.setPen(QPen(m_accent, 2.0));
    p.setBrush(withAlpha(m_accent, 45));
    p.drawPolygon(curve);
    p.setBrush(Qt::NoBrush);
    // The sample of the cut in the direction of the pointer.
    if (m_pointerInside && !cut->angleDeg.empty())
    {
        const double pointerX = m_pointer.x() - centre.x(), pointerY = centre.y() - m_pointer.y();
        const double pointerRadius = std::hypot(pointerX, pointerY);
        if (pointerRadius > 6.0 && pointerRadius < radius + 30.0)
        {
            double pointerAngle = (m_cut == 0 ? std::atan2(pointerY, pointerX) : std::atan2(pointerX, pointerY)) / kDeg;
            if (pointerAngle < 0.0) pointerAngle += 360.0;
            std::size_t nearestIndex = 0;
            double nearestGap = 1e300;
            for (std::size_t sampleIndex = 0; sampleIndex < cut->angleDeg.size(); ++sampleIndex)
            {
                double sampleGap = std::abs(cut->angleDeg[sampleIndex] - pointerAngle);
                sampleGap = std::min(sampleGap, 360.0 - sampleGap);
                if (sampleGap < nearestGap)
                {
                    nearestGap = sampleGap;
                    nearestIndex = sampleIndex;
                }
            }
            const double sampleAngle = cut->angleDeg[nearestIndex], sampleGain = cut->gainDbi[nearestIndex];
            const QString minusInfinity = QStringLiteral("−∞");
            p.setPen(QPen(withAlpha(m_text, 120), 1.0, Qt::DashLine));
            p.drawLine(centre, point(sampleAngle, radius));
            drawReadout(p, point(sampleAngle, radiusOf(sampleGain)),
                        {(m_cut == 0 ? tr("φ = %1°") : tr("θ = %1°")).arg(plain(sampleAngle, 1)),
                         tr("Gain : %1 dBi").arg(std::isfinite(sampleGain) ? plain(sampleGain, 2) : minusInfinity),
                         tr("Niveau relatif : %1 dB").arg(std::isfinite(sampleGain) ? plain(sampleGain - gmax, 2) : minusInfinity)});
        }
    }
}

// ---- Sweep plot ----

SweepPlot::SweepPlot(QQuickItem *parent) : AntennaPlotBase(parent)
{
    connect(this, &SweepPlot::plotChanged, this, [this] { update(); });
}

QRectF SweepPlot::plotArea() const
{
    return QRectF(QPointF(70.0, 18.0), QPointF(width() - 18.0, height() - 42.0));
}

double SweepPlot::frequencyAt(double x) const
{
    const auto *s = scene();
    const auto sweep = s ? s->sweep() : nullptr;
    if (!sweep || sweep->size() < 2) return 0.0;
    const QRectF area = plotArea();
    if (x < area.left() || x > area.right()) return 0.0;
    const double f0 = sweep->front().frequency, f1 = sweep->back().frequency;
    return f0 + (x - area.left()) / area.width() * (f1 - f0);
}

void SweepPlot::paint(QPainter *painter)
{
    QPainter &p = *painter;
    p.fillRect(QRectF(0, 0, width(), height()), m_background);
    p.setRenderHint(QPainter::Antialiasing, true);
    const auto *s = scene();
    const auto sweep = s ? s->sweep() : nullptr;
    if (!sweep || sweep->size() < 2)
    {
        drawMessage(p, s && s->sweepRunning() ? tr("Balayage en cours…")
                                              : tr("Lancez le balayage pour voir l'impédance, le TOS et le gain en fonction de la fréquence."));
        return;
    }
    const QRectF area = plotArea();
    if (area.width() < 40.0 || area.height() < 40.0) return;

    // Curves of the quantity: one or two series.
    std::vector<std::vector<double>> series(m_quantity >= 2 ? 2 : 1);
    QStringList names;
    QString unit;
    for (const auto &pt : *sweep)
    {
        const double nan = std::nan("");
        switch (m_quantity)
        {
        case 0:
            series[0].push_back(pt.valid ? -pt.returnLossDb : nan);
            break;
        case 1:
            series[0].push_back(pt.valid ? std::min(pt.vswr, 20.0) : nan);
            break;
        case 2:
            series[0].push_back(pt.valid ? pt.impedance.real() : nan);
            series[1].push_back(pt.valid ? pt.impedance.imag() : nan);
            break;
        default:
            series[0].push_back(pt.valid ? pt.gainDbi : nan);
            series[1].push_back(pt.valid ? pt.frontToBackDb : nan);
            break;
        }
    }
    switch (m_quantity)
    {
    case 0:
        names << QStringLiteral("|S11|");
        unit = QStringLiteral("dB");
        break;
    case 1:
        names << tr("TOS");
        break;
    case 2:
        names << QStringLiteral("R") << QStringLiteral("X");
        unit = QStringLiteral("Ω");
        break;
    default:
        names << tr("Gain (dBi)") << tr("Avant/arrière (dB)");
        unit = QStringLiteral("dB");
        break;
    }
    double lo = 1e300, hi = -1e300;
    for (const auto &values : series)
        for (double v : values)
            if (std::isfinite(v))
            {
                lo = std::min(lo, v);
                hi = std::max(hi, v);
            }
    if (!(lo <= hi))
    {
        drawMessage(p, tr("Aucun point valide dans le balayage."));
        return;
    }
    if (m_quantity == 0)
    {
        hi = 0.0;
        lo = std::min(lo, -20.0);
    }
    if (m_quantity == 1)
    {
        lo = 1.0;
        hi = std::max(hi, 3.0);
    }
    if (hi - lo < 1e-9) hi = lo + 1.0;
    if (m_quantity >= 2)
    {
        const double pad = 0.05 * (hi - lo);
        lo -= pad;
        hi += pad;
    }
    const double f0 = sweep->front().frequency, f1 = sweep->back().frequency;
    auto mapX = [&](double f) { return area.left() + (f - f0) / (f1 - f0) * area.width(); };
    auto mapY = [&](double v) { return area.bottom() - (v - lo) / (hi - lo) * area.height(); };

    QFont font = p.font();
    font.setPixelSize(11);
    p.setFont(font);
    QPen gridPen(m_grid, 1.0);
    gridPen.setCosmetic(true);
    // Vertical grid and frequency labels.
    const double fs = niceStep(f1 - f0, 6);
    for (double f = std::ceil(f0 / fs) * fs; f <= f1 + 1e-9 * fs; f += fs)
    {
        p.setPen(gridPen);
        p.drawLine(QPointF(mapX(f), area.top()), QPointF(mapX(f), area.bottom()));
        p.setPen(withAlpha(m_text, 170));
        p.drawText(QRectF(mapX(f) - 40.0, area.bottom() + 4.0, 80.0, 16.0), Qt::AlignCenter,
                   Units::formatQuantity(f, QStringLiteral("Hz"), 4));
    }
    // Horizontal grid and value labels.
    const double vs = niceStep(hi - lo, 5);
    for (double v = std::ceil(lo / vs) * vs; v <= hi + 1e-9 * vs; v += vs)
    {
        p.setPen(gridPen);
        p.drawLine(QPointF(area.left(), mapY(v)), QPointF(area.right(), mapY(v)));
        p.setPen(withAlpha(m_text, 170));
        p.drawText(QRectF(4.0, mapY(v) - 8.0, area.left() - 10.0, 16.0), Qt::AlignRight | Qt::AlignVCenter,
                   plain(v, vs < 1.0 ? 1 : 0) + (unit.isEmpty() ? QString() : QLatin1Char(' ') + unit));
    }
    if (m_quantity == 1)
    {
        // VSWR 2: the usual limit of a good match.
        QPen limit(QColor(76, 195, 138, 170), 1.2, Qt::DashLine);
        p.setPen(limit);
        p.drawLine(QPointF(area.left(), mapY(2.0)), QPointF(area.right(), mapY(2.0)));
    }
    p.setPen(QPen(withAlpha(m_text, 120), 1.0));
    p.drawRect(area);

    const QColor colours[2] = {m_accent, QColor(255, 167, 38)};
    p.save();
    p.setClipRect(area);
    for (std::size_t k = 0; k < series.size(); ++k)
    {
        QPainterPath path;
        bool started = false;
        for (std::size_t i = 0; i < sweep->size(); ++i)
        {
            const double v = series[k][i];
            if (!std::isfinite(v))
            {
                started = false;
                continue;
            }
            const QPointF q(mapX((*sweep)[i].frequency), mapY(v));
            if (started)
                path.lineTo(q);
            else
                path.moveTo(q);
            started = true;
        }
        p.setPen(QPen(colours[k], 2.0));
        p.drawPath(path);
    }
    // Analysis frequency.
    const double f = s->frequency();
    if (f >= f0 && f <= f1)
    {
        p.setPen(QPen(QColor(255, 90, 90), 1.4, Qt::DashLine));
        p.drawLine(QPointF(mapX(f), area.top()), QPointF(mapX(f), area.bottom()));
    }
    p.restore();

    // Legend and staleness.
    double x = area.left() + 10.0;
    for (int k = 0; k < names.size(); ++k)
    {
        p.setPen(QPen(colours[k], 3.0));
        p.drawLine(QPointF(x, area.top() + 12.0), QPointF(x + 16.0, area.top() + 12.0));
        p.setPen(m_text);
        p.drawText(QPointF(x + 22.0, area.top() + 16.0), names[k]);
        x += 30.0 + p.fontMetrics().horizontalAdvance(names[k]);
    }
    if (s->sweepStale())
    {
        p.setPen(QColor(255, 167, 38));
        p.drawText(QRectF(area.left(), area.top() + 4.0, area.width() - 8.0, 16.0), Qt::AlignRight,
                   tr("La scène a changé : relancez le balayage"));
    }
    // The sweep point nearest to the pointer: its frequency and the values of the curves there.
    if (m_pointerInside && m_pointer.x() >= area.left() && m_pointer.x() <= area.right())
    {
        std::size_t nearestIndex = 0;
        double nearestGap = 1e300;
        for (std::size_t sampleIndex = 0; sampleIndex < sweep->size(); ++sampleIndex)
        {
            const double sampleGap = std::abs(mapX((*sweep)[sampleIndex].frequency) - m_pointer.x());
            if (sampleGap < nearestGap)
            {
                nearestGap = sampleGap;
                nearestIndex = sampleIndex;
            }
        }
        const double sampleX = mapX((*sweep)[nearestIndex].frequency);
        p.setPen(QPen(withAlpha(m_text, 120), 1.0, Qt::DashLine));
        p.drawLine(QPointF(sampleX, area.top()), QPointF(sampleX, area.bottom()));
        QStringList readoutLines{Units::formatQuantity((*sweep)[nearestIndex].frequency, QStringLiteral("Hz"), 5)};
        QPointF readoutAt(sampleX, std::clamp(m_pointer.y(), area.top(), area.bottom()));
        bool readoutPlaced = false;
        for (std::size_t seriesIndex = 0; seriesIndex < series.size(); ++seriesIndex)
        {
            if (nearestIndex >= series[seriesIndex].size()) continue;
            const double seriesValue = series[seriesIndex][nearestIndex];
            if (!std::isfinite(seriesValue)) continue;
            const QPointF seriesAt(sampleX, std::clamp(mapY(seriesValue), area.top(), area.bottom()));
            p.setPen(QPen(m_background, 1.5));
            p.setBrush(m_text);
            p.drawEllipse(seriesAt, 4.0, 4.0);
            if (!readoutPlaced) readoutAt = seriesAt;
            readoutPlaced = true;
            readoutLines << QStringLiteral("%1 : %2 %3").arg(names.value(static_cast<qsizetype>(seriesIndex)), plain(seriesValue, 2), unit).trimmed();
        }
        readoutLines << tr("Clic : analyser à cette fréquence");
        drawReadout(p, readoutAt, readoutLines);
    }
}

// ---- Smith chart ----

SmithChart::SmithChart(QQuickItem *parent) : AntennaPlotBase(parent) {}

void SmithChart::paint(QPainter *painter)
{
    QPainter &p = *painter;
    p.fillRect(QRectF(0, 0, width(), height()), m_background);
    p.setRenderHint(QPainter::Antialiasing, true);
    const auto *s = scene();
    const QPointF centre(0.5 * width(), 0.5 * height());
    const double radius = 0.5 * std::min(width(), height()) - 26.0;
    if (radius < 30.0) return;
    auto map = [&](std::complex<double> g) { return centre + QPointF(radius * g.real(), -radius * g.imag()); };

    QFont font = p.font();
    font.setPixelSize(10);
    p.setFont(font);
    QPainterPath disc;
    disc.addEllipse(centre, radius, radius);
    QPen gridPen(m_grid, 1.0);
    gridPen.setCosmetic(true);
    p.save();
    p.setClipPath(disc);
    // Constant resistance circles: centre r/(1+r), radius 1/(1+r).
    for (double r : {0.2, 0.5, 1.0, 2.0, 5.0})
    {
        p.setPen(r == 1.0 ? QPen(withAlpha(m_text, 90), 1.0) : gridPen);
        const double rr = radius / (1.0 + r);
        p.drawEllipse(map({r / (1.0 + r), 0.0}), rr, rr);
    }
    // Constant reactance arcs: centre (1, 1/x), radius 1/|x|.
    for (double x : {0.2, 0.5, 1.0, 2.0, 5.0})
        for (double sign : {1.0, -1.0})
        {
            p.setPen(gridPen);
            const double rr = radius / x;
            p.drawEllipse(map({1.0, sign / x}), rr, rr);
        }
    p.setPen(gridPen);
    p.drawLine(map({-1.0, 0.0}), map({1.0, 0.0}));
    // VSWR = 2 circle: |Gamma| = 1/3.
    p.setPen(QPen(QColor(76, 195, 138, 150), 1.2, Qt::DashLine));
    p.drawEllipse(centre, radius / 3.0, radius / 3.0);
    p.restore();
    p.setPen(QPen(withAlpha(m_text, 140), 1.2));
    p.drawEllipse(centre, radius, radius);
    p.setPen(withAlpha(m_text, 150));
    for (double r : {0.2, 0.5, 1.0, 2.0, 5.0})
    {
        const double z0 = s ? s->referenceOhm() : 50.0;
        p.drawText(map({(r - 1.0) / (r + 1.0), 0.0}) + QPointF(2.0, -3.0), plain(r * z0, 0));
    }

    const auto sweep = s ? s->sweep() : nullptr;
    if (sweep && sweep->size() > 1)
    {
        QPainterPath path;
        bool started = false;
        for (const auto &pt : *sweep)
        {
            if (!pt.valid) continue;
            const QPointF q = map(pt.reflection);
            if (started)
                path.lineTo(q);
            else
                path.moveTo(q);
            started = true;
        }
        p.setPen(QPen(m_accent, 2.0));
        p.drawPath(path);
        // Start and end of the sweep.
        p.setBrush(m_accent);
        if (sweep->front().valid) p.drawEllipse(map(sweep->front().reflection), 3.0, 3.0);
        p.setBrush(Qt::NoBrush);
        if (sweep->back().valid) p.drawEllipse(map(sweep->back().reflection), 4.0, 4.0);
    }
    const auto result = s ? s->result() : nullptr;
    if (result && result->valid && !result->feeds.empty())
    {
        const QPointF q = map(result->feeds.front().reflection);
        p.setPen(QPen(Qt::white, 1.4));
        p.setBrush(QColor(255, 90, 90));
        p.drawEllipse(q, 5.0, 5.0);
        p.setBrush(Qt::NoBrush);
        p.setPen(m_text);
        p.drawText(q + QPointF(8.0, -6.0), Units::formatQuantity(result->frequency, QStringLiteral("Hz"), 4));
    }
    if (!sweep || sweep->size() < 2)
    {
        p.setPen(withAlpha(m_text, 150));
        p.drawText(QRectF(0, height() - 22.0, width(), 18.0), Qt::AlignCenter,
                   tr("Point rouge : fréquence d'analyse. Lancez le balayage pour tracer la courbe."));
    }
    // The sweep point nearest to the pointer: its frequency, its impedance and its match.
    if (m_pointerInside && sweep && sweep->size() > 1)
    {
        std::size_t nearestIndex = 0;
        double nearestGap = 1e300;
        for (std::size_t sampleIndex = 0; sampleIndex < sweep->size(); ++sampleIndex)
        {
            if (!(*sweep)[sampleIndex].valid) continue;
            const QPointF sampleAt = map((*sweep)[sampleIndex].reflection);
            const double sampleGap = std::hypot(sampleAt.x() - m_pointer.x(), sampleAt.y() - m_pointer.y());
            if (sampleGap < nearestGap)
            {
                nearestGap = sampleGap;
                nearestIndex = sampleIndex;
            }
        }
        if (nearestGap < 40.0)
        {
            const auto &nearest = (*sweep)[nearestIndex];
            const double reactance = nearest.impedance.imag();
            drawReadout(p, map(nearest.reflection),
                        {Units::formatQuantity(nearest.frequency, QStringLiteral("Hz"), 5),
                         tr("Z = %1 %2 j%3 Ω").arg(plain(nearest.impedance.real(), 1), reactance < 0.0 ? QStringLiteral("−") : QStringLiteral("+"),
                                                 plain(std::abs(reactance), 1)),
                         tr("|S11| = %1 dB").arg(plain(-nearest.returnLossDb, 1)),
                         tr("TOS = %1").arg(plain(nearest.vswr, 2))});
        }
    }
}
