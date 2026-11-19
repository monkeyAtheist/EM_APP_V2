#pragma once

// 3D view of a static field module (electrostatics, magnetostatics): the sources in space, the
// cutting plane of the 2D view with its colour map, contours and field lines, a lattice of field
// vectors, the measurement points. What lies behind the cutting plane is drawn before it.
//
//   drag a source: move it parallel to the cutting plane (snapped to a tenth of the grid, Shift: freely)
//   drag elsewhere: turn      Shift, middle or right drag: slide      wheel: zoom at the cursor
//   click: measurement point A on the cutting plane (right click: point B)      double click: initial view
//   view cube: exact views from above, the front, the right...

#include "orbit_navigator.h"
#include "qsig/app/field_map.h"
#include "qsig/app/scene_base.h"

#include <QColor>
#include <QPointer>
#include <QQuickPaintedItem>
#include <QTimer>
#include <QVector3D>
#include <QtQml/qqmlregistration.h>

#include <memory>
#include <vector>

// Not final: the QML engine instantiates a subclass (QQmlElement<FieldView3D>).
class FieldView3D : public QQuickPaintedItem
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(QObject *scene READ sceneObject WRITE setScene NOTIFY sceneChanged)
    Q_PROPERTY(int plane READ plane WRITE setPlane NOTIFY viewChanged)
    Q_PROPERTY(double planeOffset READ planeOffset WRITE setPlaneOffset NOTIFY viewChanged)
    Q_PROPERTY(int quantity READ quantity WRITE setQuantity NOTIFY viewChanged)
    Q_PROPERTY(bool showFieldLines READ showFieldLines WRITE setShowFieldLines NOTIFY viewChanged)
    Q_PROPERTY(bool showMap MEMBER m_showMap NOTIFY overlayChanged)
    Q_PROPERTY(bool showContours MEMBER m_showContours NOTIFY overlayChanged)
    Q_PROPERTY(bool showVectors MEMBER m_showVectors NOTIFY overlayChanged)
    Q_PROPERTY(bool showGrid MEMBER m_showGrid NOTIFY overlayChanged)
    Q_PROPERTY(int selectedRow MEMBER m_selectedRow NOTIFY overlayChanged)
    Q_PROPERTY(bool probeVisible MEMBER m_probeVisible NOTIFY overlayChanged)
    Q_PROPERTY(QVector3D probe MEMBER m_probe NOTIFY overlayChanged)
    Q_PROPERTY(bool probeBVisible MEMBER m_probeBVisible NOTIFY overlayChanged)
    Q_PROPERTY(QVector3D probeB MEMBER m_probeB NOTIFY overlayChanged)
    Q_PROPERTY(QColor backgroundColor READ backgroundColor WRITE setBackgroundColor NOTIFY overlayChanged)
    // Light background (light theme): the map and what is drawn over it follow it. For the legend.
    Q_PROPERTY(bool lightGround READ lightGround NOTIFY groundChanged)
    Q_PROPERTY(QColor gridColor MEMBER m_grid NOTIFY overlayChanged)
    Q_PROPERTY(QColor textColor MEMBER m_text NOTIFY overlayChanged)
    Q_PROPERTY(QColor accentColor MEMBER m_accent NOTIFY overlayChanged)
    // Distance of the view cube from the top edge (the view controls sit above it).
    Q_PROPERTY(double cubeTop READ cubeTop WRITE setCubeTop NOTIFY overlayChanged)
    // Read-only, for the legend and the status line (same names as FieldView).
    Q_PROPERTY(double legendMin READ legendMin NOTIFY resultChanged)
    Q_PROPERTY(double legendMax READ legendMax NOTIFY resultChanged)
    Q_PROPERTY(bool legendLogarithmic READ legendLogarithmic NOTIFY resultChanged)
    Q_PROPERTY(bool potentialReferenced READ potentialReferenced NOTIFY resultChanged)
    Q_PROPERTY(double computeMs READ computeMs NOTIFY resultChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(double gridStep READ gridStep NOTIFY cameraChanged)
    Q_PROPERTY(QString viewName READ viewName NOTIFY cameraChanged) // "Dessus (XY)"...; empty: free view
    Q_PROPERTY(bool dragging READ dragging NOTIFY draggingChanged)

public:
    using ViewPlane = qsig::app::ViewPlane;

    explicit FieldView3D(QQuickItem *parent = nullptr);
    void paint(QPainter *painter) override;

    QObject *sceneObject() const { return m_scene; }
    void setScene(QObject *scene);
    int plane() const { return static_cast<int>(m_plane); }
    void setPlane(int plane);
    double planeOffset() const { return m_offset; }
    void setPlaneOffset(double offset);
    int quantity() const { return static_cast<int>(m_quantity); }
    void setQuantity(int quantity);
    bool showFieldLines() const { return m_showFieldLines; }
    void setShowFieldLines(bool show);
    double cubeTop() const { return m_nav.cubeTop; }
    void setCubeTop(double top);
    QColor backgroundColor() const { return m_background; }
    void setBackgroundColor(const QColor &colour);
    bool lightGround() const { return m_background.lightness() >= 128; }

    double legendMin() const { return m_result ? m_result->displayMin : 0.0; }
    double legendMax() const { return m_result ? m_result->displayMax : 1.0; }
    bool legendLogarithmic() const { return m_result && m_result->logarithmic; }
    bool potentialReferenced() const { return m_result && m_result->scalarReferenced; }
    double computeMs() const { return m_result ? m_result->elapsedMs : 0.0; }
    bool busy() const { return m_computer.busy(); }
    double gridStep() const;
    QString viewName() const { return m_nav.viewName(); }
    bool dragging() const { return m_dragRow >= 0 && m_dragMoved; }

    Q_INVOKABLE void resetView();
    // Exact view from one side of an axis (0 x, 1 y, 2 z; side +1 or -1), as the view cube.
    Q_INVOKABLE void lookFrom(int axis, int side);
    // Looks straight at the cutting plane.
    Q_INVOKABLE void facePlane();
    // Row of the source under the pixel, or -1.
    Q_INVOKABLE int hitTest(double x, double y) const;
    Q_INVOKABLE QColor colourAt(double t) const;

signals:
    void sceneChanged();
    void viewChanged();
    void overlayChanged();
    void resultChanged();
    void busyChanged();
    void cameraChanged();
    void draggingChanged();
    void groundChanged();
    void rowPicked(int row);
    void probePicked(QVector3D point);
    void probeBPicked(QVector3D point);

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void hoverMoveEvent(QHoverEvent *event) override;
    void hoverLeaveEvent(QHoverEvent *event) override;
    void itemChange(ItemChange change, const ItemChangeData &value) override;

private:
    struct LatticeArrow
    {
        qsig::Vec3 at, direction; // direction: unit vector
        double level = 0.0;       // log10 of the magnitude
    };

    void scheduleCompute();
    void compute();
    void sceneBounds(qsig::Vec3 &centre, double &radius) const;
    qsig::app::OrbitCamera::Frame frame() const;
    // Point of the cutting plane nearest to the centre of the scene.
    qsig::Vec3 planeOrigin() const;
    void updateLattice() const;
    void invalidate();
    // Colour of what is drawn over the map: dark on a light ground, white on a dark one.
    QColor ink(int alpha) const;

    QPointer<qsig::app::SceneBase> m_scene;
    qsig::app::FieldMapComputer m_computer;
    std::shared_ptr<const qsig::app::FieldMapResult> m_result;
    QTimer m_debounce;
    mutable OrbitNavigator m_nav;
    ViewPlane m_plane = ViewPlane::XY;
    qsig::app::FieldQuantity m_quantity = qsig::app::FieldQuantity::Magnitude;
    double m_offset = 0.0;
    bool m_showMap = true, m_showContours = true, m_showFieldLines = true, m_showVectors = true, m_showGrid = true;
    int m_selectedRow = -1;
    bool m_probeVisible = false, m_probeBVisible = false;
    QVector3D m_probe, m_probeB;
    QColor m_background{18, 20, 24}, m_grid{255, 255, 255, 28}, m_text{230, 232, 236}, m_accent{90, 170, 255};

    // Vectors of the lattice, computed when the scene or its bounds change.
    mutable std::vector<LatticeArrow> m_lattice;
    mutable bool m_latticeDirty = true;
    mutable qsig::Vec3 m_latticeCentre;
    mutable double m_latticeRadius = 0.0;

    // Source being dragged, and the pressed face of the view cube.
    int m_dragRow = -1;
    bool m_dragMoved = false;
    qsig::Vec3 m_dragGrab, m_dragStart;
    QPointF m_pressPos;
    int m_pressFace = -1;
};
