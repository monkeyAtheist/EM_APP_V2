#include "orbit_navigator.h"

#include <QCoreApplication>
#include <QFont>
#include <QPainter>
#include <QPainterPath>
#include <QPen>

#include <cmath>
#include <initializer_list>
#include <utility>

using qsig::Vec3;

namespace
{
constexpr double kCubeHalf = 22.0; // half the edge of the view cube [px]
constexpr double kCubeBox = 84.0;  // square that holds it at any orientation
// Arrows around the cube and the button below, as in the designer of the 6.14.
constexpr double kArrow = 22.0, kSideArrowHeight = 30.0, kGap = 3.0;
constexpr double kFitWidth = 58.0, kFitHeight = 24.0;
constexpr double kPi = qsig::app::OrbitCamera::kPi;

Vec3 axisVector(int axis, double value)
{
    return {axis == 0 ? value : 0.0, axis == 1 ? value : 0.0, axis == 2 ? value : 0.0};
}

QString faceName(int id)
{
    // id = 2 x axis + (positive side ? 1 : 0)
    static const char *const names[6] = {QT_TRANSLATE_NOOP("OrbitNavigator", "Gauche"),  QT_TRANSLATE_NOOP("OrbitNavigator", "Droite"),
                                         QT_TRANSLATE_NOOP("OrbitNavigator", "Face"),    QT_TRANSLATE_NOOP("OrbitNavigator", "Arrière"),
                                         QT_TRANSLATE_NOOP("OrbitNavigator", "Dessous"), QT_TRANSLATE_NOOP("OrbitNavigator", "Dessus")};
    return id >= 0 && id < 6 ? QCoreApplication::translate("OrbitNavigator", names[id]) : QString();
}

// Three quarters of a circle ending with an arrowhead: the viewer turns about the vertical axis.
void drawTurnArrow(QPainter &p, const QRectF &button, bool toRight)
{
    const QPointF centre = button.center() + QPointF(0.0, 1.0);
    const double radius = 5.5;
    const QRectF box(centre.x() - radius, centre.y() - radius, 2.0 * radius, 2.0 * radius);
    // Angles as QPainter counts them: counterclockwise from 3 o'clock. Over the top, from one side to
    // the other; the head points down on the side the viewer goes to.
    const double start = toRight ? 200.0 : -20.0, sweep = toRight ? -220.0 : 220.0;
    QPainterPath arc;
    arc.arcMoveTo(box, start);
    arc.arcTo(box, start, sweep);
    p.drawPath(arc);
    const double end = (start + sweep) * kPi / 180.0, sense = sweep > 0.0 ? 1.0 : -1.0;
    const QPointF tip(centre.x() + radius * std::cos(end), centre.y() - radius * std::sin(end));
    const QPointF along(-std::sin(end) * sense, -std::cos(end) * sense), across(-along.y(), along.x());
    p.drawLine(tip, tip - 4.0 * along + 2.8 * across);
    p.drawLine(tip, tip - 4.0 * along - 2.8 * across);
}

// Straight arrow: the viewer goes up or down.
void drawTiltArrow(QPainter &p, const QRectF &button, bool up)
{
    const double s = up ? -1.0 : 1.0;
    const QPointF tip = button.center() + QPointF(0.0, 6.0 * s), tail = button.center() - QPointF(0.0, 5.5 * s);
    p.drawLine(tail, tip);
    p.drawLine(tip, tip + QPointF(-3.6, -4.2 * s));
    p.drawLine(tip, tip + QPointF(3.6, -4.2 * s));
}
} // namespace

void OrbitNavigator::begin(const QPointF &pos, bool slide)
{
    m_mode = slide ? Mode::Slide : Mode::Orbit;
    m_moved = false;
    m_press = m_last = pos;
}

bool OrbitNavigator::drag(const QPointF &pos)
{
    if (m_mode == Mode::None) return false;
    if (!m_moved)
    {
        const QPointF d = pos - m_press;
        if (std::abs(d.x()) + std::abs(d.y()) < 4.0) return false;
        m_moved = true;
        m_last = m_press;
    }
    const QPointF d = pos - m_last;
    m_last = pos;
    if (m_mode == Mode::Orbit)
        camera.orbit(d.x(), d.y());
    else
        camera.pan(d.x(), d.y());
    return true;
}

void OrbitNavigator::wheel(const QPointF &pos, double angleDelta)
{
    camera.zoomAt(pos.x(), pos.y(), std::pow(1.0015, angleDelta));
}

QRectF OrbitNavigator::cubeRect() const
{
    // Room for an arrow on each side and above.
    return {camera.width - cubeRight - kArrow - kGap - kCubeBox, cubeTop + kArrow + kGap, kCubeBox, kCubeBox};
}

QRectF OrbitNavigator::controlRect(int control) const
{
    const QRectF cube = cubeRect();
    const QPointF c = cube.center();
    switch (control)
    {
    case TurnLeft:
        return {cube.left() - kGap - kArrow, c.y() - 0.5 * kSideArrowHeight, kArrow, kSideArrowHeight};
    case TurnRight:
        return {cube.right() + kGap, c.y() - 0.5 * kSideArrowHeight, kArrow, kSideArrowHeight};
    case TiltUp:
        return {c.x() - 0.5 * kArrow, cube.top() - kGap - kArrow, kArrow, kArrow};
    case TiltDown:
        return {c.x() - 0.5 * kArrow, cube.bottom() + kGap, kArrow, kArrow};
    case Fit:
        return {c.x() - 0.5 * kFitWidth, cube.bottom() + kGap + kArrow + kGap + 1.0, kFitWidth, kFitHeight};
    default:
        return {};
    }
}

std::vector<OrbitNavigator::Face> OrbitNavigator::cubeFaces() const
{
    const auto f = camera.frame();
    const QPointF centre = cubeRect().center();
    auto screen = [&](const Vec3 &v)
    { return centre + QPointF(kCubeHalf * qsig::dot(v, f.right), -kCubeHalf * qsig::dot(v, f.up)); };
    std::vector<Face> faces;
    for (int axis = 0; axis < 3; ++axis)
        for (int positive = 0; positive < 2; ++positive)
        {
            const double side = positive ? 1.0 : -1.0;
            const Vec3 normal = axisVector(axis, side);
            const double facing = -qsig::dot(normal, f.forward);
            if (facing < 0.02) continue; // turned away
            const int b = (axis + 1) % 3, c = (axis + 2) % 3;
            Face face;
            face.id = 2 * axis + positive;
            face.facing = facing;
            for (const auto &[sb, sc] : {std::pair{-1.0, -1.0}, std::pair{1.0, -1.0}, std::pair{1.0, 1.0}, std::pair{-1.0, 1.0}})
                face.polygon << screen(normal + axisVector(b, sb) + axisVector(c, sc));
            faces.push_back(std::move(face));
        }
    return faces;
}

int OrbitNavigator::controlAt(const QPointF &pos) const
{
    for (const int control : {TurnLeft, TurnRight, TiltUp, TiltDown, Fit})
        if (controlRect(control).contains(pos)) return control;
    if (!cubeRect().contains(pos)) return NoControl;
    for (const auto &face : cubeFaces())
        if (face.polygon.containsPoint(pos, Qt::OddEvenFill)) return face.id;
    return NoControl;
}

bool OrbitNavigator::setHover(const QPointF &pos)
{
    const int control = controlAt(pos);
    if (control == m_hover) return false;
    m_hover = control;
    return true;
}

bool OrbitNavigator::clearHover()
{
    if (m_hover == NoControl) return false;
    m_hover = NoControl;
    return true;
}

void OrbitNavigator::activate(int control)
{
    switch (control)
    {
    case TurnLeft:
        camera.step(-1, 0);
        break;
    case TurnRight:
        camera.step(+1, 0);
        break;
    case TiltUp:
        camera.step(0, +1);
        break;
    case TiltDown:
        camera.step(0, -1);
        break;
    case Fit:
        camera.fit();
        break;
    default:
        if (control >= 0 && control <= 5) camera.lookFrom(control / 2, control % 2 ? +1 : -1);
        break;
    }
}

QString OrbitNavigator::viewName() const
{
    const int plane = camera.facingPlane();
    if (plane < 0) return {};
    const auto f = camera.frame();
    // The face of the cube that looks at the viewer.
    const int axis = plane == 0 ? 2 : plane == 1 ? 1 : 0;
    const double along = axis == 2 ? f.forward.z : axis == 1 ? f.forward.y : f.forward.x;
    const int id = 2 * axis + (along < 0.0 ? 1 : 0);
    static const char *const planes[3] = {"XY", "XZ", "YZ"};
    return QStringLiteral("%1 (%2)").arg(faceName(id), QString::fromLatin1(planes[plane]));
}

void OrbitNavigator::drawCube(QPainter &p, const Colours &colours) const
{
    p.save();
    QFont font = p.font();
    font.setPixelSize(9);
    p.setFont(font);
    for (const auto &face : cubeFaces())
    {
        const bool hover = face.id == m_hover;
        // Faces seen at a slant are darker: the cube reads as a volume.
        QColor fill = hover ? colours.hover : colours.face;
        if (!hover) fill = fill.darker(100 + static_cast<int>(45.0 * (1.0 - face.facing)));
        p.setPen(QPen(colours.edge, 1.0));
        p.setBrush(fill);
        p.drawPolygon(face.polygon);
        if (face.facing > 0.62)
        {
            p.setPen(hover ? colours.hoverText : colours.text);
            p.drawText(face.polygon.boundingRect(), Qt::AlignCenter, faceName(face.id));
        }
    }
    // Arrows and "Cadrer".
    font.setPixelSize(10);
    p.setFont(font);
    for (const int control : {TurnLeft, TurnRight, TiltUp, TiltDown, Fit})
    {
        const QRectF button = controlRect(control);
        const bool hover = control == m_hover;
        p.setPen(QPen(colours.edge, 1.0));
        p.setBrush(hover ? colours.hover : colours.face);
        p.drawRoundedRect(button, 4.0, 4.0);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(hover ? colours.hoverText : colours.text, 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        if (control == Fit)
            p.drawText(button, Qt::AlignCenter, QCoreApplication::translate("OrbitNavigator", "Cadrer"));
        else if (control == TiltUp || control == TiltDown)
            drawTiltArrow(p, button, control == TiltUp);
        else
            drawTurnArrow(p, button, control == TurnRight);
    }
    p.restore();
}

void OrbitNavigator::drawAxes(QPainter &p, const QPointF &origin, double length) const
{
    const auto f = camera.frame();
    const struct
    {
        Vec3 axis;
        const char *name;
        QColor colour;
    } axes[3] = {{{1, 0, 0}, "x", QColor(255, 110, 110)}, {{0, 1, 0}, "y", QColor(120, 220, 130)}, {{0, 0, 1}, "z", QColor(110, 160, 255)}};
    p.save();
    QFont font = p.font();
    font.setPixelSize(11);
    p.setFont(font);
    for (const auto &a : axes)
    {
        const QPointF tip = origin + QPointF(length * qsig::dot(a.axis, f.right), -length * qsig::dot(a.axis, f.up));
        p.setPen(QPen(a.colour, 2.0));
        p.drawLine(origin, tip);
        p.drawText(tip + QPointF(4.0, 4.0), QString::fromLatin1(a.name));
    }
    p.restore();
}
