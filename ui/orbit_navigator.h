#pragma once

// Mouse navigation and view cube shared by the 3D views (antennas, static fields). The camera itself
// is plain C++ (qsig/app/orbit_camera.h); this adds the gestures and the drawing with QPainter.
//
//   drag: turn the scene      Shift + drag, middle or right drag: slide it      wheel: zoom at the cursor
//   view cube (top right): its faces follow the camera; a click on a face gives the exact view from
//   that side (Dessus = plane XY, Face = plane XZ, Droite = plane YZ).
//   arrows around the cube: the viewer goes round the scene by steps of 45 degrees (left, right, up,
//   down); "Cadrer" below: the whole scene in the view again, seen from the same side.

#include "qsig/app/orbit_camera.h"

#include <QColor>
#include <QPointF>
#include <QPolygonF>
#include <QRectF>
#include <QString>

#include <vector>

class QPainter;

class OrbitNavigator
{
public:
    // What the pointer can press around the view cube. 0 to 5 are the faces of the cube:
    // 2 x axis + (positive side ? 1 : 0).
    enum Control
    {
        NoControl = -1,
        TurnLeft = 6, // the viewer goes to the left of the scene
        TurnRight = 7,
        TiltUp = 8, // the viewer goes up: towards the view from above
        TiltDown = 9,
        Fit = 10,
    };

    qsig::app::OrbitCamera camera;
    double cubeTop = 12.0;   // distance of the controls from the top edge [px]
    double cubeRight = 14.0; // and from the right edge

    // A gesture: begin() on press, drag() on every move, end() on release. Nothing moves before the
    // pointer has left the press point by a few pixels, so that moved() tells a drag from a click.
    void begin(const QPointF &pos, bool slide);
    bool drag(const QPointF &pos); // true when the picture changed
    void end() { m_mode = Mode::None; }
    bool active() const { return m_mode != Mode::None; }
    bool moved() const { return m_moved; }
    void wheel(const QPointF &pos, double angleDelta);

    struct Colours
    {
        QColor face, edge, text, hover, hoverText;
    };
    // The cube, its four arrows and the "Cadrer" button.
    void drawCube(QPainter &p, const Colours &colours) const;
    // x, y, z arrows turning with the scene, around 'origin'.
    void drawAxes(QPainter &p, const QPointF &origin, double length = 28.0) const;
    // Control under the pixel: a face of the cube (0 to 5), an arrow, Fit, or NoControl.
    int controlAt(const QPointF &pos) const;
    // Highlights the control under the pointer; true when it changed (repaint).
    bool setHover(const QPointF &pos);
    bool clearHover();
    // A click on a control: exact view from a face, step of 45 degrees, or the scene framed again.
    void activate(int control);
    QRectF cubeRect() const;
    QRectF controlRect(int control) const; // of an arrow or of Fit
    // Name of the exact view ("Dessus (XY)"...), empty for a free view.
    QString viewName() const;

private:
    enum class Mode
    {
        None,
        Orbit,
        Slide,
    };
    struct Face
    {
        QPolygonF polygon;
        int id = -1;
        double facing = 0.0; // 1: seen straight on
    };
    std::vector<Face> cubeFaces() const;

    Mode m_mode = Mode::None;
    bool m_moved = false;
    QPointF m_press, m_last;
    int m_hover = NoControl;
};
