import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qsig.App

// Centre of a field module: field map, interaction, and the tool header ("ruban") of the view controls.
//   left drag on a source : move it (one undo step per drag)
//   left drag elsewhere   : pan          wheel : zoom around the cursor      middle drag : pan
//   left click elsewhere  : place the measurement point (right click : point B, when the module has one)
// Modules that can draw add a tool palette: brush (right button erases), eraser, rectangle, disc,
// line, point B.
// Static modules also have a 3D view (FieldView3D, which handles its own mouse): the sources in space,
// the cutting plane with its map, a lattice of field vectors; the plane, the quantity and the overlays
// are those of the 2D view.
Item {
    id: root
    required property var scene
    property int selectedRow: -1
    property vector3d probe: Qt.vector3d(0.15, 0.2, 0)
    property bool probeVisible: false
    property vector3d probeB: Qt.vector3d(0.6, 0.2, 0)
    property bool probeBVisible: false
    property alias view: fieldView
    // 3D view asked for; shown when the module has one.
    property bool mode3d: false
    readonly property bool space: root.scene.spaceView && root.mode3d
    signal rowSelected(int row)
    signal probePlaced(vector3d point)
    signal probeBPlaced(vector3d point)

    // Drawing tools: 0 selection, 1 brush, 2 eraser, 3 rectangle, 4 disc, 5 line, 6 point B.
    property int tool: 0
    readonly property int activeTool: root.scene.canDraw ? root.tool : 0
    readonly property bool drawing: activeTool >= 1 && activeTool <= 5
    property int material: 1           // 0 dielectric, 1 conductor
    property real epsilonR: 4
    property real strokeWidth: 0.04    // brush, eraser and line thickness [m]
    readonly property var tools: [
        { name: qsTr("Sélection"), key: "V", tip: qsTr("Choisir et déplacer les éléments ; clic : point de mesure A, clic droit : point B") },
        { name: qsTr("Pinceau"), key: "P", tip: qsTr("Peindre une zone du matériau choisi (clic droit : gommer)") },
        { name: qsTr("Gomme"), key: "G", tip: qsTr("Effacer la peinture des zones dessinées") },
        { name: qsTr("Rectangle"), key: "R", tip: qsTr("Tracer un bloc : glisser d'un coin à l'autre") },
        { name: qsTr("Disque"), key: "D", tip: qsTr("Tracer un disque : glisser du centre vers le bord") },
        { name: qsTr("Trait"), key: "T", tip: qsTr("Tracer une paroi ou une lame : glisser d'un bout à l'autre") },
        { name: qsTr("Point B"), key: "B", tip: qsTr("Placer le second point de mesure (mesure différentielle)") }
    ]
    onActiveToolChanged: fieldView.clearPreview()
    // Side panels folded away: a tab on each edge brings them back.
    property bool leftFolded: false
    property bool rightFolded: false
    // Where the shell wants the tool header (across the scene, the view and the properties). Without
    // such a place, the header lies over the top of this view.
    property Item ribbonHost: null
    readonly property int ribbonHeight: 110
    // Modules that can draw have two tabs in the header: the simulation, and the drawing tools.
    property bool drawTab: false
    readonly property bool drawShown: root.scene.canDraw && root.drawTab
    readonly property int ribbonTab: root.scene.spaceView ? (root.space ? 1 : 0) : (root.drawShown ? 1 : 0)
    function showRibbonTab(index) {
        if (root.scene.spaceView) {
            root.mode3d = index === 1
        } else if (root.scene.canDraw) {
            root.drawTab = index === 1
            // Back on the simulation, the pointer selects again: no drawing by accident.
            if (index === 0)
                root.tool = 0
        }
    }
    // A drawing tool taken by its shortcut shows its tab.
    onToolChanged: if (root.tool !== 0 && root.scene.canDraw) root.drawTab = true
    signal unfoldLeft()
    signal unfoldRight()

    clip: true

    FieldView {
        id: fieldView
        anchors.fill: parent
        scene: root.scene
        selectedRow: root.selectedRow
        probeVisible: root.probeVisible
        probeX: root.probe.x
        probeY: root.probe.y
        probeZ: root.probe.z
        probeBVisible: root.probeBVisible && root.scene.differentialProbe
        probeBX: root.probeB.x
        probeBY: root.probeB.y
        probeBZ: root.probeB.z
        backgroundColor: Theme.canvas
        gridColor: Theme.grid
        textColor: Theme.text
        accentColor: Theme.accent
        showMap: mapBox.checked
        visible: !root.space
        Component.onCompleted: fitScene()
    }

    FieldView3D {
        id: fieldView3d
        anchors.fill: parent
        visible: root.space
        scene: root.scene
        plane: fieldView.plane
        planeOffset: fieldView.planeOffset
        quantity: fieldView.quantity
        showFieldLines: fieldView.showFieldLines
        showContours: fieldView.showContours
        showGrid: fieldView.showGrid
        showMap: mapBox.checked
        showVectors: vectorsBox.checked
        selectedRow: root.selectedRow
        probeVisible: root.probeVisible
        probe: root.probe
        probeBVisible: root.probeBVisible && root.scene.differentialProbe
        probeB: root.probeB
        backgroundColor: Theme.canvas
        gridColor: Theme.grid
        textColor: Theme.text
        accentColor: Theme.accent
        cubeTop: 12
        onRowPicked: (row) => root.rowSelected(row)
        onProbePicked: (point) => root.probePlaced(point)
        onProbeBPicked: (point) => root.probeBPlaced(point)
    }

    // After an example: show it on the plane where it reads best, centred, at plane offset 0.
    function showExample(plane) {
        fieldView.plane = plane
        fieldView.planeOffset = 0
        fieldView.fitScene()
        fieldView3d.resetView()
    }

    // (u, v, w) plane coordinates -> world point, as in qsig::app::planeToWorld.
    function planeToWorld(u, v, w) {
        if (fieldView.plane === 1) return Qt.vector3d(u, w, v)
        if (fieldView.plane === 2) return Qt.vector3d(w, u, v)
        return Qt.vector3d(u, v, w)
    }

    MouseArea {
        id: mouse
        anchors.fill: parent
        // The 3D view handles the mouse itself.
        visible: !root.space
        acceptedButtons: Qt.LeftButton | Qt.MiddleButton | Qt.RightButton
        hoverEnabled: true
        property int dragRow: -1
        property point last
        property point grab
        property real grabW: 0
        property bool moved: false
        // Drawing gesture in progress (brush, eraser, shapes), in plane coordinates.
        property bool gesture: false
        property bool erasing: false
        property var stroke: []
        property point anchor

        cursorShape: root.activeTool !== 0 ? Qt.CrossCursor
                   : dragRow >= 0 ? Qt.ClosedHandCursor
                   : pressed ? Qt.SizeAllCursor
                   : fieldView.hitTest(mouseX, mouseY) >= 0 ? Qt.OpenHandCursor : Qt.CrossCursor

        function brushOutline(x, y) {
            const w = fieldView.screenToWorld(x, y)
            fieldView.setPreview(5, [Qt.point(w.x, w.y)], root.strokeWidth, root.activeTool === 2)
        }

        onPressed: (m) => {
            last = Qt.point(m.x, m.y)
            moved = false
            dragRow = -1
            gesture = false
            if (m.button === Qt.MiddleButton)
                return
            const t = root.activeTool
            if (root.drawing) {
                const w = fieldView.screenToWorld(m.x, m.y)
                if (t <= 2) {
                    erasing = t === 2 || m.button === Qt.RightButton
                    stroke = [Qt.point(w.x, w.y)]
                    fieldView.setPreview(1, stroke, root.strokeWidth, erasing)
                    gesture = true
                } else if (m.button === Qt.LeftButton) {
                    anchor = Qt.point(w.x, w.y)
                    fieldView.setPreview(t - 1, [anchor, anchor], root.strokeWidth, false)
                    gesture = true
                }
                return
            }
            if (m.button !== Qt.LeftButton || t !== 0)
                return
            const hit = fieldView.hitTest(m.x, m.y)
            if (hit >= 0) {
                dragRow = hit
                root.rowSelected(hit)
                const s = fieldView.sourcePlaneCoordinates(hit)
                const w = fieldView.screenToWorld(m.x, m.y)
                grab = Qt.point(s.x - w.x, s.y - w.y)
                grabW = s.z
            }
        }
        onPositionChanged: (m) => {
            if (!pressed) {
                if (root.activeTool === 1 || root.activeTool === 2)
                    brushOutline(m.x, m.y)
                return
            }
            const dx = m.x - last.x, dy = m.y - last.y
            if (Math.abs(dx) + Math.abs(dy) > 0)
                moved = true
            if (gesture) {
                const w = fieldView.screenToWorld(m.x, m.y)
                if (root.activeTool <= 2) {
                    const end = stroke[stroke.length - 1]
                    if (Math.hypot(w.x - end.x, w.y - end.y) >= root.strokeWidth / 4) {
                        stroke.push(Qt.point(w.x, w.y))
                        fieldView.setPreview(1, stroke, root.strokeWidth, erasing)
                    }
                } else {
                    fieldView.setPreview(root.activeTool - 1, [anchor, Qt.point(w.x, w.y)], root.strokeWidth, false)
                }
                last = Qt.point(m.x, m.y)
                return
            }
            fieldView.interacting = true
            if (dragRow >= 0) {
                // Snaps to a tenth of the grid step; Shift moves freely.
                const step = (m.modifiers & Qt.ShiftModifier) ? 0 : fieldView.gridStep / 10
                const snap = (x) => step > 0 ? Math.round(x / step) * step : x
                const w = fieldView.screenToWorld(m.x, m.y)
                const p = root.planeToWorld(snap(w.x + grab.x), snap(w.y + grab.y), grabW)
                root.scene.moveSource(dragRow, p.x, p.y, p.z)
            } else if (root.activeTool === 0 || m.buttons & Qt.MiddleButton) {
                fieldView.panBy(dx, dy)
            }
            last = Qt.point(m.x, m.y)
        }
        onReleased: (m) => {
            if (gesture) {
                const t = root.activeTool
                const w = fieldView.screenToWorld(m.x, m.y)
                let row = -1
                if (t <= 2) {
                    row = root.scene.addStroke(root.selectedRow, stroke, root.strokeWidth, erasing, root.material, root.epsilonR)
                    brushOutline(m.x, m.y)
                } else {
                    row = root.scene.addDrawnShape(["rectangle", "disk", "line"][t - 3], anchor.x, anchor.y, w.x, w.y,
                                                   root.material, root.epsilonR, root.strokeWidth)
                    fieldView.clearPreview()
                }
                if (row >= 0 && !erasing)
                    root.rowSelected(row)
                gesture = false
                erasing = false
                stroke = []
                return
            }
            if (dragRow >= 0) {
                root.scene.endDrag()
            } else if (!moved && m.button === Qt.LeftButton) {
                if (root.activeTool === 6)
                    root.probeBPlaced(fieldView.screenToPoint(m.x, m.y))
                else
                    root.probePlaced(fieldView.screenToPoint(m.x, m.y))
            } else if (!moved && m.button === Qt.RightButton && root.activeTool === 0 && root.scene.differentialProbe) {
                root.probeBPlaced(fieldView.screenToPoint(m.x, m.y))
            }
            dragRow = -1
            fieldView.interacting = false
        }
        onExited: if (!gesture) fieldView.clearPreview()
        onWheel: (w) => {
            fieldView.interacting = true
            settle.restart()
            fieldView.zoomAt(w.x, w.y, Math.pow(1.0015, w.angleDelta.y))
            if (root.activeTool === 1 || root.activeTool === 2)
                brushOutline(w.x, w.y)
        }
        Timer {
            id: settle
            interval: 180
            onTriggered: fieldView.interacting = false
        }
    }

    // ---- Tool header (the "ruban"), laid across the window part given by the shell: the views of the
    // module as tabs, and under them the settings of the view in named groups ----
    Rectangle {
        id: ribbon
        parent: root.ribbonHost ? root.ribbonHost : root
        visible: root.visible
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: root.ribbonHeight
        z: 10
        color: Theme.panel

        // Static modules: the cutting plane (2D) or the scene in space (3D). The others have one view.
        Row {
            x: 8
            height: 30
            spacing: 2
            Repeater {
                model: root.scene.spaceView ? [qsTr("Coupe 2D"), qsTr("Vue 3D")]
                       : root.scene.canDraw ? [qsTr("Simulation"), qsTr("Dessin")]
                       : [root.scene.live ? qsTr("Simulation") : qsTr("Vue")]
                delegate: Item {
                    id: tabItem
                    required property string modelData
                    required property int index
                    readonly property bool current: index === root.ribbonTab
                    width: tabLabel.implicitWidth + 28
                    height: 30
                    Rectangle {
                        anchors.fill: parent
                        anchors.topMargin: 3
                        anchors.bottomMargin: 2
                        radius: 5
                        color: tabArea.containsMouse && !tabItem.current ? Qt.alpha(Theme.text, 0.08) : "transparent"
                    }
                    Label {
                        id: tabLabel
                        anchors.centerIn: parent
                        text: tabItem.modelData
                        font.pixelSize: Theme.bodyText
                        color: tabItem.current ? Theme.accent : Theme.textMuted
                    }
                    Rectangle {
                        x: 8
                        y: parent.height - 2
                        width: parent.width - 16
                        height: 2
                        color: Theme.accent
                        visible: tabItem.current
                    }
                    MouseArea {
                        id: tabArea
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: root.showRibbonTab(tabItem.index)
                    }
                }
            }
        }
        Rectangle {
            y: 30
            width: parent.width
            height: 1
            color: Theme.border
        }

        // The groups. In a window narrower than them, the header scrolls sideways (wheel, drag, or the
        // bar under the groups).
        Item {
            y: 31
            width: parent.width
            height: 78
            WheelHandler {
                enabled: toolsFlick.contentWidth > toolsFlick.width
                onWheel: (event) => {
                    const span = toolsFlick.contentWidth - toolsFlick.width
                    toolsFlick.contentX = Math.max(0, Math.min(span, toolsFlick.contentX - event.angleDelta.y - event.angleDelta.x))
                }
            }
            Flickable {
                id: toolsFlick
                anchors.fill: parent
                contentWidth: toolsRow.width
                contentHeight: height
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                flickableDirection: Flickable.HorizontalFlick
                interactive: contentWidth > width
                ScrollBar.horizontal: ScrollBar {
                    height: 6
                    policy: toolsFlick.contentWidth > toolsFlick.width ? ScrollBar.AlwaysOn : ScrollBar.AlwaysOff
                }
                Row {
                    id: toolsRow
                    leftPadding: 4

                    // Live module: the simulation runs; play, step, restart and speed.
                    RibbonGroup {
                        visible: root.scene.live
                        title: qsTr("Simulation")
                        RibbonButton {
                            glyph: root.scene.running ? "pause" : "play"
                            primary: !root.scene.running
                            text: root.scene.running ? qsTr("Pause") : qsTr("Lancer")
                            tip: qsTr("Lancer la simulation ou la mettre en pause (Espace)")
                            onClicked: root.scene.running = !root.scene.running
                        }
                        RibbonButton {
                            glyph: "step"
                            text: qsTr("Pas à pas")
                            tip: qsTr("Avance d'un pas de temps (met en pause)")
                            onClicked: root.scene.stepOnce()
                        }
                        RibbonButton {
                            glyph: "rotate"
                            text: qsTr("Réinitialiser")
                            tip: qsTr("Champs à zéro et t = 0 : les impulsions repartent")
                            onClicked: root.scene.resetSimulation()
                        }
                    }
                    RibbonGroup {
                        visible: root.scene.live && !root.drawShown
                        title: qsTr("Vitesse")
                        // Steps per frame, by powers of two (1 to 64).
                        Slider {
                            anchors.verticalCenter: parent.verticalCenter
                            width: 110
                            from: 0
                            to: 6
                            stepSize: 1
                            snapMode: Slider.SnapAlways
                            value: root.scene.live ? Math.round(Math.log2(root.scene.speed)) : 0
                            onMoved: root.scene.speed = Math.round(Math.pow(2, value))
                        }
                        Label {
                            anchors.verticalCenter: parent.verticalCenter
                            width: 72
                            text: root.scene.live ? qsTr("%1 pas/image").arg(root.scene.speed) : ""
                            color: Theme.textMuted
                            font.pixelSize: Theme.smallText
                        }
                    }
                    // Modules that can draw (tab "Dessin"): the tools, and the options of the current one.
                    RibbonGroup {
                        visible: root.drawShown
                        title: qsTr("Dessiner")
                        Repeater {
                            model: root.tools
                            delegate: RibbonButton {
                                required property var modelData
                                required property int index
                                glyph: ["pointer", "brush", "eraser", "rect", "disc", "wire", "pointb"][index]
                                text: modelData.name
                                checked: root.tool === index
                                tip: modelData.tip + "  (" + modelData.key + ")"
                                onClicked: root.tool = index
                            }
                        }
                    }
                    RibbonGroup {
                        visible: root.drawShown && root.drawing
                        title: qsTr("Matériau et trait")
                        Label {
                            visible: root.activeTool !== 2
                            anchors.verticalCenter: parent.verticalCenter
                            text: qsTr("Matériau")
                            color: Theme.textMuted
                            font.pixelSize: Theme.smallText
                            leftPadding: 4
                            rightPadding: 6
                        }
                        ComboBox {
                            visible: root.activeTool !== 2
                            anchors.verticalCenter: parent.verticalCenter
                            width: 190
                            model: [qsTr("Diélectrique"), qsTr("Conducteur (PEC)")]
                            currentIndex: root.material
                            font.pixelSize: Theme.bodyText
                            onActivated: (i) => root.material = i
                        }
                        Label {
                            visible: root.activeTool !== 2 && root.material === 0
                            anchors.verticalCenter: parent.verticalCenter
                            text: qsTr("εr")
                            color: Theme.textMuted
                            font.pixelSize: Theme.smallText
                            leftPadding: 12
                            rightPadding: 6
                        }
                        QuantityField {
                            visible: root.activeTool !== 2 && root.material === 0
                            anchors.verticalCenter: parent.verticalCenter
                            width: 80
                            label: ""
                            unit: ""
                            value: root.epsilonR
                            onEdited: (v) => root.epsilonR = Math.max(1, v)
                        }
                        Label {
                            visible: root.activeTool === 1 || root.activeTool === 2 || root.activeTool === 5
                            anchors.verticalCenter: parent.verticalCenter
                            text: root.activeTool === 5 ? qsTr("Épaisseur") : qsTr("Largeur")
                            color: Theme.textMuted
                            font.pixelSize: Theme.smallText
                            leftPadding: 12
                            rightPadding: 6
                        }
                        QuantityField {
                            visible: root.activeTool === 1 || root.activeTool === 2 || root.activeTool === 5
                            anchors.verticalCenter: parent.verticalCenter
                            width: 100
                            label: ""
                            unit: "m"
                            value: root.strokeWidth
                            onEdited: (v) => { if (v > 0) root.strokeWidth = v }
                        }
                    }
                    RibbonGroup {
                        visible: !root.scene.live
                        title: qsTr("Plan de coupe")
                        Label {
                            anchors.verticalCenter: parent.verticalCenter
                            text: qsTr("Plan")
                            color: Theme.textMuted
                            font.pixelSize: Theme.smallText
                            leftPadding: 4
                            rightPadding: 6
                        }
                        ComboBox {
                            anchors.verticalCenter: parent.verticalCenter
                            model: ["XY", "XZ", "YZ"]
                            currentIndex: fieldView.plane
                            width: 84
                            onActivated: (i) => { fieldView.plane = i; fieldView.fitScene() }
                        }
                        Label {
                            anchors.verticalCenter: parent.verticalCenter
                            text: fieldView.wName + " ="
                            color: Theme.textMuted
                            font.pixelSize: Theme.smallText
                            leftPadding: 12
                            rightPadding: 6
                        }
                        QuantityField {
                            anchors.verticalCenter: parent.verticalCenter
                            width: 108
                            label: ""
                            unit: "m"
                            value: fieldView.planeOffset
                            onEdited: (v) => fieldView.planeOffset = v
                        }
                    }
                    RibbonGroup {
                        visible: !root.drawShown
                        title: qsTr("Grandeur")
                        TabBar {
                            id: quantityTabs
                            anchors.verticalCenter: parent.verticalCenter
                            currentIndex: fieldView.quantity
                            width: 132
                            onCurrentIndexChanged: fieldView.quantity = currentIndex
                            TabButton { text: root.scene.quantityNames[0] }
                            TabButton { text: root.scene.quantityNames[1] }
                        }
                    }
                    RibbonGroup {
                        visible: !root.drawShown
                        title: qsTr("Afficher")
                        RibbonButton {
                            id: mapBox
                            visible: !root.scene.live
                            glyph: "colours"
                            text: qsTr("Couleurs")
                            checkable: true
                            checked: true
                            tip: qsTr("Colorer le plan de coupe selon la grandeur affichée. Sans les couleurs, il reste les lignes sur le fond de la vue.")
                        }
                        RibbonButton {
                            visible: !root.scene.live
                            glyph: "fieldlines"
                            text: qsTr("Lignes de champ")
                            checked: fieldView.showFieldLines
                            tip: qsTr("Tangentes au champ dans le plan de coupe, espacées régulièrement ; les pointes donnent le sens.")
                            onClicked: fieldView.showFieldLines = !fieldView.showFieldLines
                        }
                        RibbonButton {
                            visible: !root.scene.live
                            glyph: "contours"
                            text: root.scene.contourName(fieldView.quantity)
                            checked: fieldView.showContours
                            tip: qsTr("Courbes le long desquelles la grandeur affichée garde la même valeur")
                            onClicked: fieldView.showContours = !fieldView.showContours
                        }
                        RibbonButton {
                            visible: !root.scene.live && !root.space
                            glyph: "arrows"
                            text: qsTr("Flèches")
                            checked: fieldView.showArrows
                            tip: qsTr("Flèches du champ sur une grille de points du plan de coupe")
                            onClicked: fieldView.showArrows = !fieldView.showArrows
                        }
                        RibbonButton {
                            id: vectorsBox
                            visible: root.space
                            glyph: "arrows"
                            text: qsTr("Vecteurs")
                            checkable: true
                            checked: true
                            tip: qsTr("Champ sur un réseau de points de l'espace : la flèche donne la direction, sa longueur et sa couleur l'intensité (échelle logarithmique).")
                        }
                        RibbonButton {
                            glyph: "grid"
                            text: qsTr("Grille")
                            checked: fieldView.showGrid
                            onClicked: fieldView.showGrid = !fieldView.showGrid
                        }
                    }
                    RibbonGroup {
                        title: qsTr("Vue")
                        RibbonButton {
                            visible: root.space
                            glyph: "plane"
                            text: qsTr("Face au plan")
                            tip: qsTr("Regarder le plan de coupe de face, comme dans la vue 2D")
                            onClicked: fieldView3d.facePlane()
                        }
                        RibbonButton {
                            glyph: "fit"
                            text: root.space ? qsTr("Vue initiale") : qsTr("Recadrer")
                            tip: root.space ? qsTr("Revenir à la vue initiale") : qsTr("Cadrer la vue sur toute la scène")
                            onClicked: root.space ? fieldView3d.resetView() : fieldView.fitScene()
                        }
                    }
                }
            }
        }
        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width
            height: 1
            color: Theme.border
        }
    }

    // Tool shortcuts (not while typing in a field).
    Repeater {
        model: root.scene.canDraw ? root.tools : []
        delegate: Item {
            required property var modelData
            required property int index
            Shortcut {
                sequence: modelData.key
                enabled: root.visible && !(root.Window.activeFocusItem instanceof TextInput)
                onActivated: root.tool = index
            }
        }
    }
    Shortcut {
        sequence: "Escape"
        enabled: root.visible && root.scene.canDraw && root.tool !== 0 && !(root.Window.activeFocusItem instanceof TextInput)
        onActivated: root.tool = 0
    }

    EdgeTab {
        visible: root.leftFolded
        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        atLeft: true
        toolTip: qsTr("Afficher la scène (F9)")
        onClicked: root.unfoldLeft()
    }
    EdgeTab {
        visible: root.rightFolded
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        atLeft: false
        toolTip: qsTr("Afficher les propriétés (F10)")
        onClicked: root.unfoldRight()
    }

    Legend {
        view: root.space ? fieldView3d : fieldView
        scene: root.scene
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 14
        // The scale of the colours: of the map, or of the field vectors of the 3D view.
        visible: root.scene.count > 0
                 && (root.scene.live || mapBox.checked || (root.space && vectorsBox.checked && fieldView.quantity === 0))
    }

    // ---- Status (bottom centre) ----
    Rectangle {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 12
        width: statusRow.implicitWidth + 20
        height: 28
        radius: 14
        color: Qt.alpha(Theme.panel, 0.88)
        border.color: Theme.border
        visible: root.scene.count > 0
        RowLayout {
            id: statusRow
            anchors.centerIn: parent
            spacing: 8
            BusyIndicator {
                id: busyIndicator
                readonly property bool busy: root.space ? fieldView3d.busy : fieldView.busy
                // Shown for a computation that lasts: the short ones of a drag would make it blink.
                property bool lasting: false
                onBusyChanged: if (!busy) lasting = false
                running: lasting
                visible: lasting
                implicitWidth: 18
                implicitHeight: 18
                Timer {
                    interval: 250
                    running: busyIndicator.busy
                    onTriggered: busyIndicator.lasting = true
                }
            }
            Label {
                color: Theme.textMuted
                font.pixelSize: Theme.smallText
                text: (root.scene.live ? root.scene.statusText
                       : root.space ? qsTr("%1 · coupe %2%3 · %4 ms · champ exact · clic : point A, clic droit : point B")
                                      .arg(fieldView3d.viewName === "" ? qsTr("Vue 3D") : qsTr("Vue %1").arg(fieldView3d.viewName))
                                      .arg(fieldView.uName).arg(fieldView.vName).arg(fieldView3d.computeMs.toFixed(0))
                       : qsTr("Plan %1%2 · carte %3 · %4 ms · champ exact")
                         .arg(fieldView.uName).arg(fieldView.vName).arg(fieldView.resolutionText).arg(fieldView.computeMs.toFixed(0)))
                      + (mouse.dragRow >= 0 || fieldView3d.dragging ? qsTr(" · Maj : déplacement libre") : "")
            }
        }
    }

    Label {
        anchors.centerIn: parent
        visible: root.scene.count === 0
        color: Theme.textMuted
        font.pixelSize: Theme.titleText
        horizontalAlignment: Text.AlignHCenter
        text: root.scene.live ? qsTr("Ajoutez une source pour lancer une onde,\nou ouvrez un exemple depuis le panneau Scène.")
                              : qsTr("Ajoutez une source pour voir son champ,\nou ouvrez un exemple depuis le panneau Scène.")
    }
}
