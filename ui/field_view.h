#pragma once

// Field viewport of any module: draws the field map of the scene on a plane (colour map, contours,
// field lines, arrows, grid, scale bar, source outlines, probe) and maps screen <-> world coordinates.
// Interaction (drag, zoom, pan) is handled in QML through the invokable helpers.
// A live module (time-domain simulation) brings its own pictures: the view paints the latest one.

#include "qsig/app/field_map.h"
#include "qsig/app/scene_base.h"

#include <QColor>
#include <QPointer>
#include <QQuickPaintedItem>
#include <QTimer>
#include <QVector3D>
#include <QtQml/qqmlregistration.h>

#include <memory>

// Not final: the QML engine instantiates a subclass (QQmlElement<FieldView>).
class FieldView : public QQuickPaintedItem
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(QObject *scene READ sceneObject WRITE setScene NOTIFY sceneChanged)
    Q_PROPERTY(int plane READ plane WRITE setPlane NOTIFY viewChanged)
    Q_PROPERTY(double planeOffset READ planeOffset WRITE setPlaneOffset NOTIFY viewChanged)
    Q_PROPERTY(double centerU READ centerU WRITE setCenterU NOTIFY viewChanged)
    Q_PROPERTY(double centerV READ centerV WRITE setCenterV NOTIFY viewChanged)
    Q_PROPERTY(double pixelsPerMeter READ pixelsPerMeter WRITE setPixelsPerMeter NOTIFY viewChanged)
    Q_PROPERTY(int quantity READ quantity WRITE setQuantity NOTIFY viewChanged)
    // Colour map of the quantity; without it the lines are drawn on the bare ground of the view.
    Q_PROPERTY(bool showMap MEMBER m_showMap NOTIFY overlayChanged)
    Q_PROPERTY(bool showContours MEMBER m_showContours NOTIFY overlayChanged)
    Q_PROPERTY(bool showArrows MEMBER m_showArrows NOTIFY overlayChanged)
    Q_PROPERTY(bool showGrid MEMBER m_showGrid NOTIFY overlayChanged)
    Q_PROPERTY(bool showFieldLines READ showFieldLines WRITE setShowFieldLines NOTIFY fieldLinesChanged)
    Q_PROPERTY(bool interacting READ interacting WRITE setInteracting NOTIFY interactingChanged)
    Q_PROPERTY(int selectedRow MEMBER m_selectedRow NOTIFY overlayChanged)
    Q_PROPERTY(bool probeVisible MEMBER m_probeVisible NOTIFY overlayChanged)
    Q_PROPERTY(double probeX MEMBER m_probeX NOTIFY overlayChanged)
    Q_PROPERTY(double probeY MEMBER m_probeY NOTIFY overlayChanged)
    Q_PROPERTY(double probeZ MEMBER m_probeZ NOTIFY overlayChanged)
    // Second measurement point B (differential readings); A is then labelled.
    Q_PROPERTY(bool probeBVisible MEMBER m_probeBVisible NOTIFY overlayChanged)
    Q_PROPERTY(double probeBX MEMBER m_probeBX NOTIFY overlayChanged)
    Q_PROPERTY(double probeBY MEMBER m_probeBY NOTIFY overlayChanged)
    Q_PROPERTY(double probeBZ MEMBER m_probeBZ NOTIFY overlayChanged)
    Q_PROPERTY(QColor backgroundColor READ backgroundColor WRITE setBackgroundColor NOTIFY overlayChanged)
    // Light background (light theme): the map and what is drawn over it follow it. For the legend.
    Q_PROPERTY(bool lightGround READ lightGround NOTIFY groundChanged)
    Q_PROPERTY(QColor gridColor MEMBER m_grid NOTIFY overlayChanged)
    Q_PROPERTY(QColor textColor MEMBER m_text NOTIFY overlayChanged)
    Q_PROPERTY(QColor accentColor MEMBER m_accent NOTIFY overlayChanged)
    // Read-only, for the legend and the status line.
    Q_PROPERTY(double legendMin READ legendMin NOTIFY resultChanged)
    Q_PROPERTY(double legendMax READ legendMax NOTIFY resultChanged)
    Q_PROPERTY(bool legendLogarithmic READ legendLogarithmic NOTIFY resultChanged)
    Q_PROPERTY(bool potentialReferenced READ potentialReferenced NOTIFY resultChanged)
    // Spacing of the grid lines [m] (about 90 px); dragging snaps to a tenth of it.
    Q_PROPERTY(double gridStep READ gridStep NOTIFY viewChanged)
    Q_PROPERTY(double computeMs READ computeMs NOTIFY resultChanged)
    Q_PROPERTY(QString resolutionText READ resolutionText NOTIFY resultChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString uName READ uName NOTIFY viewChanged)
    Q_PROPERTY(QString vName READ vName NOTIFY viewChanged)
    Q_PROPERTY(QString wName READ wName NOTIFY viewChanged)

public:
    using ViewPlane = qsig::app::ViewPlane;

    explicit FieldView(QQuickItem *parent = nullptr);

    void paint(QPainter *painter) override;

    QObject *sceneObject() const { return m_scene; }
    void setScene(QObject *scene);
    int plane() const { return static_cast<int>(m_plane); }
    void setPlane(int plane);
    double planeOffset() const { return m_offset; }
    void setPlaneOffset(double offset);
    double centerU() const { return m_centerU; }
    void setCenterU(double u);
    double centerV() const { return m_centerV; }
    void setCenterV(double v);
    double pixelsPerMeter() const { return m_ppm; }
    void setPixelsPerMeter(double ppm);
    int quantity() const { return static_cast<int>(m_quantity); }
    void setQuantity(int quantity);
    bool showFieldLines() const { return m_showFieldLines; }
    void setShowFieldLines(bool show);
    bool interacting() const { return m_interacting; }
    void setInteracting(bool interacting);
    QColor backgroundColor() const { return m_background; }
    void setBackgroundColor(const QColor &colour);
    bool lightGround() const { return m_background.lightness() >= 128; }

    double legendMin() const;
    double legendMax() const;
    bool legendLogarithmic() const { return !m_live && m_result && m_result->logarithmic; }
    bool potentialReferenced() const { return !m_live && m_result && m_result->scalarReferenced; }
    double gridStep() const;
    double computeMs() const { return m_result ? m_result->elapsedMs : 0.0; }
    QString resolutionText() const;
    bool busy() const { return m_computer.busy(); }
    QString uName() const;
    QString vName() const;
    QString wName() const;

    // Coordinates: (u, v) on the plane in metres <-> item pixels.
    Q_INVOKABLE QPointF worldToScreen(double u, double v) const;
    Q_INVOKABLE QPointF screenToWorld(double x, double y) const;
    // World point (x, y, z) of a screen pixel on the current plane.
    Q_INVOKABLE QVector3D screenToPoint(double x, double y) const;
    // Row of the source under the pixel (near its outline, inside it, or on its centre), or -1.
    Q_INVOKABLE int hitTest(double x, double y) const;
    // Plane coordinates (u, v, w) of a source.
    Q_INVOKABLE QVector3D sourcePlaneCoordinates(int row) const;
    Q_INVOKABLE void zoomAt(double x, double y, double factor);
    // Gesture being drawn, in plane coordinates: 1 brush stroke (points), 2 rectangle (two corners),
    // 3 disc (centre, rim point), 4 line (two ends, 'width' thick), 5 brush outline at points[0].
    // 'erase': the eraser. Kind 0 or clearPreview() hides it.
    Q_INVOKABLE void setPreview(int kind, const QVariantList &points, double width, bool erase);
    Q_INVOKABLE void clearPreview();
    Q_INVOKABLE void panBy(double dx, double dy);
    Q_INVOKABLE void fitScene();
    Q_INVOKABLE QColor colourAt(double t) const;

protected:
    void itemChange(ItemChange change, const ItemChangeData &value) override;

signals:
    void sceneChanged();
    void viewChanged();
    void overlayChanged();
    void fieldLinesChanged();
    void interactingChanged();
    void resultChanged();
    void busyChanged();
    void groundChanged();

private:
    void scheduleCompute();
    void compute();
    // Colour of what is drawn over the map or the bare ground: dark on a light ground, white on a
    // dark one. The pictures of a live module are dark in both themes.
    bool inkIsDark() const;
    QColor ink(int alpha) const;
    void drawGrid(QPainter &p) const;
    void drawFieldLines(QPainter &p) const;
    void drawSources(QPainter &p) const;
    void drawLiveFrame(QPainter &p) const;
    void drawStrokes(QPainter &p, const qsig::app::SourceOutline &o, const QColor &colour, double opacity) const;
    void drawPreview(QPainter &p) const;
    void drawProbe(QPainter &p, double x, double y, double z, const QColor &colour, const QString &label) const;
    bool isLive() const;
    QPointF gridToScreen(const qsig::app::FieldMapRequest &q, QPointF grid) const;

    QPointer<qsig::app::SceneBase> m_scene;
    qsig::app::FieldMapComputer m_computer;
    std::shared_ptr<const qsig::app::FieldMapResult> m_result;
    std::shared_ptr<const qsig::app::LiveFrame> m_live;
    QTimer m_debounce;
    ViewPlane m_plane = ViewPlane::XY;
    qsig::app::FieldQuantity m_quantity = qsig::app::FieldQuantity::Magnitude;
    double m_offset = 0.0;
    double m_centerU = 0.0, m_centerV = 0.0;
    double m_ppm = 300.0;
    bool m_fitPending = false;
    bool m_showMap = true, m_showContours = true, m_showArrows = false, m_showGrid = true, m_showFieldLines = true;
    bool m_interacting = false;
    int m_selectedRow = -1;
    bool m_probeVisible = false;
    double m_probeX = 0.0, m_probeY = 0.0, m_probeZ = 0.0;
    bool m_probeBVisible = false;
    double m_probeBX = 0.0, m_probeBY = 0.0, m_probeBZ = 0.0;
    int m_previewKind = 0;
    QPolygonF m_previewPoints;
    double m_previewWidth = 0.0;
    bool m_previewErase = false;
    QColor m_background{18, 20, 24}, m_grid{255, 255, 255, 28}, m_text{230, 232, 236}, m_accent{90, 170, 255};
};
