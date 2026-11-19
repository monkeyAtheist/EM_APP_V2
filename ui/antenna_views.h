#pragma once

// Views of the "Antennes" module, painted from AntennaScene::result() and sweep():
//   AntennaView3D  geometry turning with the mouse (view cube, plane views), wires coloured by current,
//                  3D radiation pattern; elements are dragged in the active plane
//   PolarPlot      gain in a plane (horizontal, vertical through the main lobe, perpendicular one)
//   SweepPlot      |S11|, VSWR, R and X, gain and front-to-back ratio versus frequency
//   SmithChart     reflection coefficient of the sweep, analysis frequency marked

#include "orbit_navigator.h"
#include "qsig/app/antenna_scene.h"

#include <QColor>
#include <QList>
#include <QPointer>
#include <QVariantList>
#include <QQuickPaintedItem>
#include <QtQml/qqmlregistration.h>

#include <vector>

// Common ground: the scene, the theme colours, repaint when results arrive.
class AntennaPlotBase : public QQuickPaintedItem
{
    Q_OBJECT
    QML_ANONYMOUS
    Q_PROPERTY(QObject *scene READ sceneObject WRITE setScene NOTIFY sceneChanged)
    Q_PROPERTY(QColor backgroundColor MEMBER m_background NOTIFY styleChanged)
    Q_PROPERTY(QColor gridColor MEMBER m_grid NOTIFY styleChanged)
    Q_PROPERTY(QColor textColor MEMBER m_text NOTIFY styleChanged)
    Q_PROPERTY(QColor accentColor MEMBER m_accent NOTIFY styleChanged)

public:
    explicit AntennaPlotBase(QQuickItem *parent = nullptr);
    // The pointer over the plot, for the readout of the sample under it. Hover events set it; a
    // MouseArea laid over the plot keeps those events and calls this instead.
    Q_INVOKABLE void setPointer(double x, double y, bool inside);
    QObject *sceneObject() const { return m_scene; }
    void setScene(QObject *scene);

signals:
    void sceneChanged();
    void styleChanged();

protected:
    const qsig::app::AntennaScene *scene() const { return m_scene.data(); }
    // A new result of the scene (before the repaint).
    virtual void resultsChanged() {}
    // Elements were added to the scene or removed from it.
    virtual void countChanged() {}
    // Anything of the scene changed.
    virtual void sceneEdited() {}
    void drawMessage(QPainter &p, const QString &text) const;
    void hoverMoveEvent(QHoverEvent *event) override;
    void hoverLeaveEvent(QHoverEvent *event) override;
    // A dot on the sample read, and its values in a box beside it.
    void drawReadout(QPainter &p, const QPointF &at, const QStringList &lines) const;

    QPointF m_pointer;
    bool m_pointerInside = false;

    QPointer<qsig::app::AntennaScene> m_scene;
    QColor m_background{15, 17, 20}, m_grid{255, 255, 255, 28}, m_text{230, 232, 236}, m_accent{90, 170, 255};
};

// Not final: the QML engine instantiates subclasses (QQmlElement<...>).
// Mouse: drag an element to move it in the active plane (snapped to a tenth of the grid, Shift: freely),
// drag elsewhere to turn the scene, Shift / middle / right drag to slide it, wheel to zoom, click to
// select, double click for the initial view; the view cube gives the exact plane views.
// Designer tools (property 'tool'): wire drawn by a drag on the construction plane (its ends snap to
// the ends and to the bodies of the other wires, which joins them), source placed by a click on an
// element, distance measured by a drag, element deleted by a click, plain wire cut by a click.
// Alt + drag turns the scene whatever the tool.
// Selection: Ctrl + click adds an element or takes it out; the selected elements are dragged together
// and transformed together (move, array of copies, rotation, mirror, alignment). The ends of the one
// selected plain wire are handles: dragging one reshapes the wire. Three arrows on the selection
// (x, y, z) move it along one axis when dragged. A right click opens the context menu of the element
// under the pointer, or of the scene.
class AntennaView3D : public AntennaPlotBase
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(bool showCurrents MEMBER m_showCurrents NOTIFY viewChanged)
    Q_PROPERTY(bool showPattern MEMBER m_showPattern NOTIFY viewChanged)
    // Radius of the 3D pattern: 0 field strength (linear), 1 power (linear), 2 gain in dB over patternRange.
    Q_PROPERTY(int patternScale MEMBER m_patternScale NOTIFY viewChanged)
    Q_PROPERTY(double patternRange MEMBER m_patternRange NOTIFY viewChanged) // dB of the colours below the maximum
    // Main element of the selection: the one the properties panel shows.
    Q_PROPERTY(int selectedRow READ selectedRow WRITE setSelectedRow NOTIFY selectionChanged)
    Q_PROPERTY(int selectionCount READ selectionCount NOTIFY selectionChanged)
    // For the panel of groups and constraints: the main element, the group it belongs to, the other
    // selected element when there are exactly two (the reference of a new constraint), and the
    // constraints the main element holds: [{text, enabled, met}].
    Q_PROPERTY(QString mainName READ mainName NOTIFY selectionChanged)
    Q_PROPERTY(QString groupName READ groupName NOTIFY selectionChanged)
    Q_PROPERTY(QString referenceName READ referenceName NOTIFY selectionChanged)
    Q_PROPERTY(QVariantList constraints READ constraints NOTIFY constraintsChanged)
    Q_PROPERTY(double mainLength READ mainLength NOTIFY selectionChanged) // 0: the element has no length
    // Plane in which a dragged element moves in a free view: 0 XY, 1 XZ, 2 YZ. An exact view (cube)
    // works in the plane it faces.
    Q_PROPERTY(int editPlane MEMBER m_editPlane NOTIFY viewChanged)
    Q_PROPERTY(int activePlane READ activePlane NOTIFY viewChanged)
    Q_PROPERTY(QString viewName READ viewName NOTIFY viewChanged) // "Dessus (XY)"...; empty: free 3D view
    Q_PROPERTY(double gridStep READ gridStep NOTIFY viewChanged)  // [m]
    // Steps of the designer: what a dragged element or arrow lands on (0: a tenth of the grid), and what
    // a rotation handle turns by. Shift frees both.
    Q_PROPERTY(double moveStep MEMBER m_moveStep NOTIFY viewChanged)         // [m]
    Q_PROPERTY(double rotationStep MEMBER m_rotationStep NOTIFY viewChanged) // degrees
    Q_PROPERTY(double snapStep READ snapStep NOTIFY viewChanged)             // [m], the step in use
    // What the pointer picks (and Ctrl + A takes): 0 everything, 1 wire elements, 2 surfaces, 3 fed elements.
    Q_PROPERTY(int selectionFilter MEMBER m_selectionFilter NOTIFY viewChanged)
    Q_PROPERTY(bool dragging READ dragging NOTIFY draggingChanged)
    // 0 selection, 1 wire, 2 source, 3 measure, 4 delete, 5 cut, 6 pivot (a click places the pivot of the
    // rotations, then the selection tool comes back).
    Q_PROPERTY(int tool READ tool WRITE setTool NOTIFY toolChanged)
    // Position of the construction plane along the normal of its orientation [m] (z of the XY plane...).
    Q_PROPERTY(double editOffset MEMBER m_editOffset NOTIFY viewChanged)
    // Rotation of the construction plane [deg]: about the world axis x, then y, then z, around the point
    // where the plane crosses the normal of its orientation (z = editOffset for XY). In a free view; an
    // exact view (cube) works in the plane it faces, unless that is the turned plane itself.
    Q_PROPERTY(double planeRotationX MEMBER m_planeRotationX NOTIFY viewChanged)
    Q_PROPERTY(double planeRotationY MEMBER m_planeRotationY NOTIFY viewChanged)
    Q_PROPERTY(double planeRotationZ MEMBER m_planeRotationZ NOTIFY viewChanged)
    // Readout of the gesture in progress or of the last measure ("L = 0,25 m (0,250 λ)").
    Q_PROPERTY(QString toolText READ toolText NOTIFY toolTextChanged)
    // Frames drawn in the view: the axes of the scene from the origin, the three arrows on the
    // selection (dragging one moves the selection along that axis), the small frame in the corner.
    Q_PROPERTY(bool showOrigin MEMBER m_showOrigin NOTIFY viewChanged)
    Q_PROPERTY(bool showGizmo MEMBER m_showGizmo NOTIFY viewChanged)
    Q_PROPERTY(bool showCompass MEMBER m_showCompass NOTIFY viewChanged)
    // A view of the results only (the 3D pattern of the Simulation tab): drags turn or slide the scene,
    // nothing is picked, moved or edited, no context menu.
    Q_PROPERTY(bool readOnly MEMBER m_readOnly NOTIFY viewChanged)
    // A pivot placed in the scene, for the rotations and the mirrors of the selection.
    Q_PROPERTY(bool pivotSet READ pivotSet NOTIFY viewChanged)
    // What the rotations of the selection turn around, handles included: 0 its centre, 1 the origin, 2 the
    // placed pivot (the centre while none is placed). The handles are drawn around that point.
    Q_PROPERTY(int rotationPivot READ rotationPivot WRITE setRotationPivot NOTIFY viewChanged)
    // Where the placed pivot is ("x 0,1 m · y 0 m · z 0,25 m"), empty when none is placed.
    Q_PROPERTY(QString pivotText READ pivotText NOTIFY viewChanged)
    // The source selected in the view: an entity placed on a wire, selected alone (never with elements).
    // As an entry of the scene's feedList; an empty map when no source is selected.
    Q_PROPERTY(QVariantMap selectedFeed READ selectedFeed NOTIFY feedSelectionChanged)
    Q_PROPERTY(bool feedSelected READ feedSelected NOTIFY feedSelectionChanged)

public:
    explicit AntennaView3D(QQuickItem *parent = nullptr);
    void paint(QPainter *painter) override;

    int activePlane() const;
    QString viewName() const { return m_nav.viewName(); }
    double gridStep() const;
    double snapStep() const;
    bool dragging() const { return (m_dragRow >= 0 && m_dragMoved) || m_handleMoved || m_axisMoved || m_feedMoved; }
    QVariantMap selectedFeed() const;
    bool feedSelected() const { return m_feedRow >= 0; }
    bool pivotSet() const { return m_pivotSet; }
    int rotationPivot() const { return m_rotationPivot; }
    void setRotationPivot(int pivot);
    QString pivotText() const;
    int selectedRow() const { return m_selectedRow; }
    void setSelectedRow(int row);
    int selectionCount() const { return static_cast<int>(selection().size()); }
    QString mainName() const;
    QString groupName() const;
    QString referenceName() const;
    QVariantList constraints() const { return m_constraints; }
    double mainLength() const;
    int tool() const { return m_tool; }
    void setTool(int tool);
    QString toolText() const { return m_toolText; }

    Q_INVOKABLE void resetView();
    // Exact view from one side of an axis (0 x, 1 y, 2 z; side +1 or -1), as the view cube.
    Q_INVOKABLE void lookFrom(int axis, int side);
    // The view turned to face the construction plane (its normal towards the viewer).
    Q_INVOKABLE void lookAtPlane();
    // Element under the pixel, or -1.
    Q_INVOKABLE int hitTest(double x, double y) const;
    // Transformations of the selection, one undo step each.
    // Displacement; 'copies' > 0: the elements stay and that many copies are laid at delta, 2 delta...
    Q_INVOKABLE void moveSelection(double dx, double dy, double dz, int copies);
    // axis: 0 x, 1 y, 2 z, 3 the normal of the construction plane. pivot: 0 the centre of the selection,
    // 1 the origin, 2 the placed pivot (the centre when none is placed). 'copy': the elements stay,
    // turned copies are added.
    Q_INVOKABLE void rotateSelection(int axis, double angleDeg, int pivot, bool copy);
    // Mirror image in the construction plane.
    Q_INVOKABLE void mirrorSelection(bool copy);
    // Mirror image in the plane across an axis (0 x, 1 y, 2 z), through the placed pivot or the origin.
    Q_INVOKABLE void mirrorSelectionAcross(int axis, bool copy);
    // A copy of the selection, one step of the grid further in the construction plane.
    Q_INVOKABLE void duplicateSelection();
    // The whole scene in the view again, seen from the same side.
    Q_INVOKABLE void fitView();
    // Context menu, at a pixel of the view: a new element of a type of the scene at the point of the
    // construction plane (its row); the source of the element under the pixel put there, as the source
    // tool does; the pivot placed there (on the plane, or on the end of a wire within reach).
    Q_INVOKABLE int addElementAt(int type, double x, double y);
    Q_INVOKABLE void placeFeedAt(double x, double y);
    // Selects the source 'index' of the element at 'row' (the elements are deselected); edits it
    // ("voltage", "phaseDeg", "fraction"); removes it, its wire staying.
    Q_INVOKABLE void selectFeed(int row, int index);
    Q_INVOKABLE void setFeedValue(const QString &key, double value);
    Q_INVOKABLE void deleteFeed();
    Q_INVOKABLE void placePivot(double x, double y);
    Q_INVOKABLE void clearPivot();
    // The centres take the coordinate (0 x, 1 y, 2 z) of the main element.
    Q_INVOKABLE void alignSelection(int axis);
    Q_INVOKABLE void deleteSelection();
    Q_INVOKABLE void selectAll();
    // Every element of the group of that name (nothing when no element carries it).
    Q_INVOKABLE void selectGroup(const QString &name);
    // Groups: the selection becomes one group; the groups of the selection are dissolved; the group
    // of the main element takes another name.
    Q_INVOKABLE void groupSelection();
    Q_INVOKABLE void ungroupSelection();
    Q_INVOKABLE void renameGroup(const QString &name);
    // A constraint for the main element (the last one clicked), which will follow the other selected
    // element where the constraint needs a reference. 'type': AntennaConstraint::Type; 'value': a
    // length or a distance [m], an angle [deg]; anchors: 0 centre, 1 and 2 the ends; 'axes': bits x, y, z.
    Q_INVOKABLE bool addConstraint(int type, double value, int anchor, int referenceAnchor, int axes);
    Q_INVOKABLE void removeConstraint(int index);
    Q_INVOKABLE void setConstraintEnabled(int index, bool enabled);

signals:
    void viewChanged();
    void draggingChanged();
    void toolChanged();
    void toolTextChanged();
    void selectionChanged();
    void feedSelectionChanged();
    void constraintsChanged();
    void rowPicked(int row); // main element of the selection; -1: nothing selected
    // A right click without a drag. 'row': the element under the pointer, selected by then; -1: none.
    void contextMenuRequested(double x, double y, int row);

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void hoverMoveEvent(QHoverEvent *event) override;
    void hoverLeaveEvent(QHoverEvent *event) override;
    void countChanged() override;
    void sceneEdited() override;

private:
    // Sets the selection and tells the application its main element.
    void select(const QList<int> &rows, int mainRow);
    // Selected rows that exist in the scene.
    QList<int> selection() const;
    // The other selected element when exactly two are selected, else -1.
    int referenceRow() const;
    // Reads the constraints of the main element again; tells the panel only when they changed (a drag
    // edits the scene at every mouse move).
    void refreshConstraints();
    // End of the one selected plain wire under the pixel: 0, 1, or -1.
    int handleAt(const QPointF &pos) const;
    // Source marker under the pixel: its element and its index on it. False: none.
    bool feedAtPixel(const QPointF &pos, int &row, int &index) const;
    void clearFeed();
    // Arrows on the selection (selection tool): whether they are drawn, where, and the one under a pixel.
    bool gizmoVisible() const;
    qsig::Vec3 gizmoOrigin() const;
    int axisAt(const QPointF &pos) const;
    // Rotation handles of the selection: a quarter circle per axis, in the plane of the two others.
    int ringAt(const QPointF &pos) const;
    bool passesFilter(int row) const;
    // Point of an element designated by a pixel, for a source or a cut: on a plain wire, the point of
    // the wire under the pointer; its centre otherwise.
    qsig::Vec3 pointOnElement(int row, const QPointF &pos) const;
    void sceneBounds(qsig::Vec3 &centre, double &radius) const;
    qsig::app::OrbitCamera::Frame frame() const;
    void drawPattern(QPainter &p, const qsig::app::OrbitCamera::Frame &f, const qsig::app::AntennaResult &r,
                     const qsig::Vec3 &centre, double radius) const;
    // Point designated by a pixel for the wire, measure and reshape gestures. In order: the end of a
    // wire within reach (a source too, with 'feeds': snap 1), the body of a plain wire (snap 2), else
    // the plane through 'planeOrigin' parallel to the active plane, on the grid unless 'free' (snap 0).
    // 'exclude': a row whose own points do not count.
    bool toolPoint(const QPointF &pos, bool free, const qsig::Vec3 &planeOrigin, int exclude, bool feeds,
                   qsig::Vec3 &point, int &snap) const;
    qsig::Vec3 constructionOrigin() const;
    // The construction plane in use: its two axes, its normal, and the point it turns around (the axes
    // of the grid cross there). 'onGrid': a point of the plane brought to the nearest node of the grid of
    // 'step' (none for 0), its coordinate along the normal kept.
    struct Plane
    {
        qsig::Vec3 a, b, normal, anchor;
    };
    Plane constructionPlane() const;
    qsig::Vec3 onPlaneGrid(const Plane &plane, const qsig::Vec3 &point, double step) const;
    // Centre of the rotation handles: the point the selection turns around.
    qsig::Vec3 ringCentre() const;
    void setToolText(const QString &text);
    void updateToolText();

    mutable OrbitNavigator m_nav;
    double m_patternRange = 30.0;
    int m_patternScale = 0;
    bool m_showCurrents = true, m_showPattern = true;
    int m_selectedRow = -1;
    QList<int> m_selection; // with m_selectedRow, when it is not -1
    QVariantList m_constraints; // of the main element, for the panel
    int m_editPlane = 0;
    // Element being dragged: where it was grabbed (offset to its position) and the pressed face of the
    // cube. The whole selection follows: its rows and their positions when the drag began.
    int m_dragRow = -1;
    bool m_dragMoved = false;
    qsig::Vec3 m_dragGrab, m_dragStart;
    QList<int> m_dragRows;
    std::vector<qsig::Vec3> m_dragStarts;
    // End of a plain wire being dragged: its row, which end, where it was.
    int m_handleRow = -1, m_handleEnd = -1;
    // Sources: the selected one (element and index on it), the one under the pointer, the one of a
    // ready-made element pressed (a click selects it, a drag moves its element), and the slide of a
    // source along its plain wire.
    int m_feedRow = -1, m_feedIndex = -1;
    int m_hoverFeedRow = -1, m_hoverFeedIndex = -1;
    int m_feedClickRow = -1, m_feedClickIndex = -1;
    bool m_feedDrag = false, m_feedMoved = false;
    bool m_handleMoved = false;
    qsig::Vec3 m_handleStart;
    QPointF m_pressPos;
    int m_pressFace = -1;
    // Designer tools: the gesture in progress (wire, measure), the kept measure, the element pressed
    // by the source and delete tools, the anchor under the pointer.
    int m_tool = 0;
    double m_editOffset = 0.0;
    bool m_gesture = false, m_measured = false;
    int m_gestureSnap = 0; // what the end of the gesture holds on to: 0 the plane, 1 an end, 2 a wire
    qsig::Vec3 m_gestureA, m_gestureB;
    int m_toolRow = -1;
    bool m_hoverAnchor = false;
    qsig::Vec3 m_hoverPoint;
    QString m_toolText;
    bool m_showOrigin = true, m_showGizmo = true, m_showCompass = true;
    bool m_readOnly = false;
    double m_moveStep = 0.0, m_rotationStep = 15.0;
    int m_selectionFilter = 0;
    int m_ringDrag = -1, m_hoverRing = -1; // axis of the rotation handle dragged, under the pointer
    double m_ringAngle = 0.0;              // degrees turned by the handle dragged
    double m_ringStart = 0.0;              // screen angle of the pointer around the handles at the press [rad]
    int m_hoverTheta = -1, m_hoverPhi = -1; // sample of the pattern under the pointer (results view), -1: none
    bool m_pivotSet = false;
    int m_rotationPivot = 0;
    double m_planeRotationX = 0.0, m_planeRotationY = 0.0, m_planeRotationZ = 0.0;
    qsig::Vec3 m_pivot;
    // Arrow of the selection being dragged (0 x, 1 y, 2 z), the one under the pointer, the rows that
    // follow with their positions when the drag began, and the position the grid step is counted from.
    int m_axisDrag = -1, m_hoverAxis = -1;
    bool m_axisMoved = false;
    QList<int> m_axisRows;
    std::vector<qsig::Vec3> m_axisStarts;
    qsig::Vec3 m_axisAnchor;
};

class PolarPlot : public AntennaPlotBase
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(int cut MEMBER m_cut NOTIFY plotChanged) // 0 horizontal, 1 vertical (main lobe), 2 perpendicular
    Q_PROPERTY(double range MEMBER m_range NOTIFY plotChanged)
    Q_PROPERTY(QString cutTitle READ cutTitle NOTIFY plotChanged)
    Q_PROPERTY(QString beamText READ beamText NOTIFY plotChanged)

public:
    explicit PolarPlot(QQuickItem *parent = nullptr);
    void paint(QPainter *painter) override;
    QString cutTitle() const;
    QString beamText() const; // maximum and -3 dB beamwidth of the cut

signals:
    void plotChanged();

private:
    void resultsChanged() override;
    const qsig::app::PatternCut *currentCut() const;
    int m_cut = 0;
    double m_range = 30.0;
};

class SweepPlot : public AntennaPlotBase
{
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(int quantity MEMBER m_quantity NOTIFY plotChanged) // 0 |S11|, 1 VSWR, 2 R and X, 3 gain and F/B

public:
    explicit SweepPlot(QQuickItem *parent = nullptr);
    void paint(QPainter *painter) override;
    // Frequency under a pixel of the plot area (0 outside).
    Q_INVOKABLE double frequencyAt(double x) const;

signals:
    void plotChanged();

private:
    QRectF plotArea() const;
    int m_quantity = 0;
};

class SmithChart : public AntennaPlotBase
{
    Q_OBJECT
    QML_ELEMENT

public:
    explicit SmithChart(QQuickItem *parent = nullptr);
    void paint(QPainter *painter) override;
};
