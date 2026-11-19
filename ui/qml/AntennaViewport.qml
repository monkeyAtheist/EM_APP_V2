import QtCore
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qsig.App

// Centre of the "Antennes" module, under one tool header (the "ruban") laid across the window:
//   tabs        Conception, Simulation, Optimiser, Validation; the tools under them follow the tab
//   Conception  the design view in 3D: wires coloured by their current, sources; no radiation pattern,
//               so that editing stays light. Drag an element: move it in the active plane   drag elsewhere: turn
//               Shift, middle or right drag: slide   wheel: zoom   click: select   view cube: plane views
//               tools of the header in groups: select, create, feed, modify, assemble, construct,
//               inspect, view; Ctrl + click selects several elements
//               arrows on the selection: drag one to move it along x, y or z
//               right click: context menu of the element under the pointer, or of the scene
//               the window of the selection tools (move, array, rotation, groups, constraints) floats:
//               the header opens it
//   Simulation  Rayonnement 3D: the antenna and its radiation pattern, a view of the results only
//               Diagrammes: gain in the horizontal plane and in a vertical plane (polar, dBi)
//               Balayage: |S11|, VSWR, R and X, gain and front-to-back ratio versus frequency
//               (click on the curve: analyse at that frequency)   Smith: reflection of the sweep
//   Optimiser   Yagi-Uda: lengths and spacings for gain, front-to-back ratio and match
//   Validation  bench of the wire solver against independent references
// The commands are declared once (Action): the header and the context menus use the same ones.
Item {
    id: root
    required property var scene
    property int selectedRow: -1
    signal rowSelected(int row)
    property bool leftFolded: false
    property bool rightFolded: false
    signal unfoldLeft()
    signal unfoldRight()
    // Page shown: 0 the design view (3D, no pattern), 1 pattern cuts, 2 sweep, 3 Smith chart, 4 optimiser,
    // 5 validation, 6 radiation in 3D.
    property int page: 0
    // Pages 6, 1, 2 and 3 make the "Simulation" tab: the last one shown comes back with the tab.
    property int analysisPage: 6
    // Tab of the tool header: 0 Créer and 1 Modifier, the two tabs of the design view (the tools are
    // sorted between them so that each fits the width of the window); 2 Simulation, 3 Optimiser,
    // 4 Validation.
    property int designTab: 0
    readonly property bool designing: page === 0
    readonly property int tab: page === 0 ? designTab : (page <= 3 || page === 6) ? 2 : page - 1
    // Where the shell wants the tool header (across the scene, the view and the properties). Without
    // such a place, the header stays at the top of this view.
    property Item ribbonHost: null
    readonly property int ribbonHeight: 110
    // Where an element created from a menu goes, in pixels of the 3D view: the right click, or the
    // centre of the view for the tool header.
    property real createX: 0
    property real createY: 0
    // What a click in the design view picks: 0 everything, 1 wire elements, 2 surfaces, 3 fed elements.
    property int selectionFilter: 0
    // A text field has the keyboard: the one-letter shortcuts of the tools must not fire.
    readonly property bool typing: Window.activeFocusItem instanceof TextInput || Window.activeFocusItem instanceof TextEdit
    // Designer tools of the 3D view, in the order of AntennaView3D.tool.
    readonly property var tools: [
        { name: qsTr("Sélection"), key: "V", tip: qsTr("Choisir un élément et le glisser parallèlement au plan de construction. Ctrl + clic : ajouter ou retirer un élément. Les flèches x, y, z posées sur la sélection la déplacent le long d'un axe. Les carrés aux bouts d'un fil tracé se glissent pour le redimensionner. Clic droit : menu de l'élément ou de la scène.") },
        { name: qsTr("Fil"), key: "F", tip: qsTr("Tracer un fil : glisser d'un bout à l'autre sur le plan de construction. Près du bout ou du corps d'un autre fil, il s'y raccorde (jonction en T) ; posé sur une surface, il y est relié.") },
        { name: qsTr("Source"), key: "S", tip: qsTr("Placer une source : cliquer sur un fil, à l'endroit de la source. Un fil droit en porte plusieurs ; un élément tout fait a la sienne, à sa place. Une source se sélectionne d'un clic, se glisse le long de son fil et se supprime sans son fil.") },
        { name: qsTr("Mesurer"), key: "M", tip: qsTr("Distance entre deux points : glisser de l'un à l'autre") },
        { name: qsTr("Supprimer"), key: "X", tip: qsTr("Supprimer l'élément cliqué ; sur une source, la source seule") },
        { name: qsTr("Couper"), key: "C", tip: qsTr("Couper un fil tracé en deux à l'endroit du clic : les deux parties restent raccordées tant qu'on ne les déplace pas") },
        { name: qsTr("Pivot"), key: "P", tip: qsTr("Placer le pivot des rotations : cliquer dans la vue, sur le plan de construction ou sur un bout de fil. Les rotations de la sélection tournent alors autour de lui.") }
    ]

    // Names of the element types of the scene, for the menu that creates one.
    readonly property var typeNames: scene ? scene.typeNames() : []
    // Stacking of the floating panels: the last one pressed is in front.
    property int panelZ: 2

    function resetView() {
        view3d.resetView()
        patternView.resetView()
    }
    function typeName(index) { return index < typeNames.length ? typeNames[index] : "" }
    function showTab(index) {
        if (index <= 1) {
            root.designTab = index
            root.page = 0
            return
        }
        const last = root.analysisPage
        root.page = index === 2 ? (last >= 1 && last <= 3 ? last : 6) : index + 1
    }
    // A tool taken by its key or from a menu: the header shows the tab that holds it (Fil and Source
    // are tools of Créer, Supprimer and Couper of Modifier; the others are on both).
    function followTool(tool) {
        if (tool === 1 || tool === 2)
            root.designTab = 0
        else if (tool === 4 || tool === 5)
            root.designTab = 1
    }
    function showAnalysis(index) {
        root.analysisPage = index
        root.page = index
    }
    // The floating window of the selection tools, on one of its tabs: 0 transform, 1 group, 2 constraints.
    function showSelectionTools(index) {
        transformPanel.tab = index
        transformPanel.folded = false
        transformPanel.open = true
        transformPanel.z = ++root.panelZ
    }
    // Button of the tool header: shows that tab of the window, closes the window when it shows it already.
    function toggleSelectionTools(index) {
        if (transformPanel.open && !transformPanel.folded && transformPanel.tab === index)
            transformPanel.open = false
        else
            showSelectionTools(index)
    }
    function createElement(type) { view3d.addElementAt(type, root.createX, root.createY) }
    // From the tool header: the new element goes to the centre of the view, on the construction plane.
    function aimAtCentre() {
        root.createX = view3d.width / 2
        root.createY = view3d.height / 2
    }
    function createAtCentre(type) {
        aimAtCentre()
        createElement(type)
    }
    // Every element of the group of that name, as a click on one of them does.
    function selectGroup(name) { view3d.selectGroup(name) }

    // Sources are entities placed on the wires: the one selected in the design view (an entry of the
    // scene's feedList, an empty object when none), and what the panels do with it.
    readonly property var selectedFeed: view3d.selectedFeed
    readonly property bool feedSelected: view3d.feedSelected
    function selectFeed(row, index) { view3d.selectFeed(row, index) }
    function setFeedValue(key, value) { view3d.setFeedValue(key, value) }
    function deleteFeed() { view3d.deleteFeed() }
    // The selected source, or the selected elements.
    function deleteSelection() { view3d.deleteSelection() }
    // Right click in the 3D view: on an element (selected by then), or on the scene.
    function openContextMenu(x, y, row) {
        const menu = row >= 0 ? elementMenu : sceneMenu
        menu.px = x
        menu.py = y
        root.createX = x
        root.createY = y
        menu.popup(view3d, x, y)
    }

    // Remembered between sessions: the window of the selection tools, the frames shown in the view,
    // the page of the "Simulation" tab.
    Settings {
        category: "antennaView"
        property alias selectionToolsOpen: transformPanel.open
        property alias toolsFolded: transformPanel.folded
        property alias toolsX: transformPanel.marginX
        property alias toolsY: transformPanel.marginY
        property alias showOrigin: originBox.checked
        property alias showArrows: gizmoBox.checked
        property alias showCompass: compassBox.checked
        property alias moveStep: moveStepField.value
        property alias rotationStep: rotationStepBox.value
        property alias simulationPage: root.analysisPage
        property alias designTab: root.designTab
    }

    clip: true

    Rectangle {
        anchors.fill: parent
        color: Theme.canvas
    }

    // ---- Commands, declared once: the tool header and the context menus of the view use them ----
    Action { id: duplicateAction; text: qsTr("Dupliquer"); enabled: view3d.selectionCount > 0; onTriggered: view3d.duplicateSelection() }
    Action { id: groupAction; text: qsTr("Grouper la sélection"); enabled: view3d.selectionCount > 1; onTriggered: view3d.groupSelection() }
    Action { id: ungroupAction; text: qsTr("Dégrouper"); enabled: view3d.groupName !== ""; onTriggered: view3d.ungroupSelection() }
    Action {
        id: deleteAction
        text: view3d.feedSelected ? qsTr("Supprimer la source") : view3d.selectionCount > 1 ? qsTr("Supprimer la sélection") : qsTr("Supprimer")
        enabled: view3d.selectionCount > 0 || view3d.feedSelected
        onTriggered: view3d.deleteSelection()
    }
    Action { id: constraintAction; text: qsTr("Ajouter une contrainte…"); onTriggered: root.showSelectionTools(2) }
    Action { id: selectAllAction; text: qsTr("Tout sélectionner"); onTriggered: view3d.selectAll() }
    Action { id: fitAction; text: qsTr("Cadrer la vue"); onTriggered: view3d.fitView() }
    Action {
        id: clearPivotAction
        text: qsTr("Retirer le pivot")
        enabled: view3d.pivotSet
        onTriggered: view3d.clearPivot()
    }
    // Turns of 45°: in the construction plane (axis 3), around x, y, z; about the pivot chosen.
    Action { id: rotatePlanePlus; text: qsTr("+45° dans le plan de construction"); enabled: view3d.selectionCount > 0; onTriggered: view3d.rotateSelection(3, 45, view3d.rotationPivot, false) }
    Action { id: rotatePlaneMinus; text: qsTr("−45° dans le plan de construction"); enabled: view3d.selectionCount > 0; onTriggered: view3d.rotateSelection(3, -45, view3d.rotationPivot, false) }
    Action { id: rotateXPlus; text: qsTr("+45° autour de x"); enabled: view3d.selectionCount > 0; onTriggered: view3d.rotateSelection(0, 45, view3d.rotationPivot, false) }
    Action { id: rotateXMinus; text: qsTr("−45° autour de x"); enabled: view3d.selectionCount > 0; onTriggered: view3d.rotateSelection(0, -45, view3d.rotationPivot, false) }
    Action { id: rotateYPlus; text: qsTr("+45° autour de y"); enabled: view3d.selectionCount > 0; onTriggered: view3d.rotateSelection(1, 45, view3d.rotationPivot, false) }
    Action { id: rotateYMinus; text: qsTr("−45° autour de y"); enabled: view3d.selectionCount > 0; onTriggered: view3d.rotateSelection(1, -45, view3d.rotationPivot, false) }
    Action { id: rotateZPlus; text: qsTr("+45° autour de z"); enabled: view3d.selectionCount > 0; onTriggered: view3d.rotateSelection(2, 45, view3d.rotationPivot, false) }
    Action { id: rotateZMinus; text: qsTr("−45° autour de z"); enabled: view3d.selectionCount > 0; onTriggered: view3d.rotateSelection(2, -45, view3d.rotationPivot, false) }
    // Mirror copies: across a plane of the axes, or across the construction plane.
    Action { id: mirrorX; text: qsTr("x inversé (plan yz)"); enabled: view3d.selectionCount > 0; onTriggered: view3d.mirrorSelectionAcross(0, true) }
    Action { id: mirrorY; text: qsTr("y inversé (plan xz)"); enabled: view3d.selectionCount > 0; onTriggered: view3d.mirrorSelectionAcross(1, true) }
    Action { id: mirrorZ; text: qsTr("z inversé (plan xy)"); enabled: view3d.selectionCount > 0; onTriggered: view3d.mirrorSelectionAcross(2, true) }
    Action { id: mirrorPlane; text: qsTr("Dans le plan de construction"); enabled: view3d.selectionCount > 0; onTriggered: view3d.mirrorSelection(true) }
    // Centres of the selected elements brought to one coordinate of the main element (the last one clicked).
    Action { id: alignX; text: qsTr("Coordonnée x"); enabled: view3d.selectionCount > 1; onTriggered: view3d.alignSelection(0) }
    Action { id: alignY; text: qsTr("Coordonnée y"); enabled: view3d.selectionCount > 1; onTriggered: view3d.alignSelection(1) }
    Action { id: alignZ; text: qsTr("Coordonnée z"); enabled: view3d.selectionCount > 1; onTriggered: view3d.alignSelection(2) }
    // One element of each type, at (createX, createY) of the view.
    Action { id: create0; text: root.typeName(0); onTriggered: root.createElement(0) }
    Action { id: create1; text: root.typeName(1); onTriggered: root.createElement(1) }
    Action { id: create2; text: root.typeName(2); onTriggered: root.createElement(2) }
    Action { id: create3; text: root.typeName(3); onTriggered: root.createElement(3) }
    Action { id: create4; text: root.typeName(4); onTriggered: root.createElement(4) }
    Action { id: create5; text: root.typeName(5); onTriggered: root.createElement(5) }
    Action { id: create6; text: root.typeName(6); onTriggered: root.createElement(6) }
    Action { id: create7; text: root.typeName(7); onTriggered: root.createElement(7) }
    // Conducting solids.
    Action { id: create8; text: root.typeName(8); onTriggered: root.createElement(8) }
    Action { id: create9; text: root.typeName(9); onTriggered: root.createElement(9) }
    Action { id: create10; text: root.typeName(10); onTriggered: root.createElement(10) }
    Action { id: create11; text: root.typeName(11); onTriggered: root.createElement(11) }
    // Reflectors (open surfaces) and curved wires.
    Action { id: create12; text: root.typeName(12); onTriggered: root.createElement(12) }
    Action { id: create13; text: root.typeName(13); onTriggered: root.createElement(13) }
    Action { id: create14; text: root.typeName(14); onTriggered: root.createElement(14) }
    Action { id: create15; text: root.typeName(15); onTriggered: root.createElement(15) }

    StackLayout {
        anchors.fill: parent
        anchors.topMargin: ribbon.parent === root ? root.ribbonHeight : 0
        anchors.bottomMargin: 46
        currentIndex: root.page

        // ---- 3D view ----
        Item {
            // The view handles the mouse itself: drag an element to move it in the active plane, drag
            // elsewhere to turn, Shift / middle / right drag to slide, wheel to zoom, view cube for the
            // plane views.
            AntennaView3D {
                id: view3d
                anchors.fill: parent
                scene: root.scene
                selectedRow: root.selectedRow
                backgroundColor: Theme.canvas
                gridColor: Theme.grid
                textColor: Theme.text
                accentColor: Theme.accent
                showCurrents: currentsBox.checked
                // The radiation pattern is not drawn here: it belongs to the Simulation tab (patternView).
                showPattern: false
                editPlane: planeField.value
                planeRotationX: planeRxField.value
                planeRotationY: planeRyField.value
                planeRotationZ: planeRzField.value
                moveStep: moveStepField.value
                selectionFilter: root.selectionFilter
                rotationStep: rotationStepBox.value
                showOrigin: originBox.checked
                showGizmo: gizmoBox.checked
                showCompass: compassBox.checked
                onRowPicked: (row) => root.rowSelected(row)
                onContextMenuRequested: (x, y, row) => root.openContextMenu(x, y, row)
            }

            // What the current tool reads, top left of the view.
            Label {
                x: root.leftFolded ? 34 : 12
                y: 8
                visible: text.length > 0
                color: view3d.toolText !== "" ? Theme.text : Theme.textMuted
                font.pixelSize: Theme.smallText
                style: Text.Outline
                styleColor: Theme.canvas
                text: view3d.toolText !== "" ? view3d.toolText
                      : view3d.tool === 1 ? qsTr("Fil : glisser sur le plan de construction · Maj : hors grille · Alt + glisser : tourner la vue")
                      : view3d.tool === 2 ? qsTr("Source : cliquer sur un fil, à l'endroit de la source (un fil droit en porte plusieurs)")
                      : view3d.tool === 3 ? qsTr("Mesure : glisser d'un point à l'autre")
                      : view3d.tool === 4 ? qsTr("Supprimer : cliquer sur un élément, ou sur une source pour ne retirer qu'elle (Ctrl+Z pour revenir)")
                      : view3d.tool === 5 ? qsTr("Couper : cliquer sur un fil tracé, à l'endroit de la coupe")
                      : view3d.tool === 6 ? qsTr("Pivot : cliquer sur le plan de construction ou sur un bout de fil · Échap : annuler") : ""
            }

            // Transformations, groups and constraints of the selection: a floating panel, under the view
            // cube at first.
            FloatingPanel {
                id: transformPanel
                title: qsTr("Outils de la sélection")
                open: false
                alignRight: true
                keepOut: root.rightFolded ? 30 : 12
                marginX: 12
                marginY: 190
                width: 262
                bodyHeight: panelTabs.y + panelTabs.height + 8 + [transformColumn, groupColumn, constraintColumn][tab].implicitHeight + 8
                z: 2
                onActivated: z = ++root.panelZ
                property int tab: 0
                property real dx: 0
                property real dy: 0
                property real dz: 0
                property real angle: 90
                // Values of a new constraint: an imposed length, a distance, an angle.
                property real lengthValue: 0.25
                property real distanceValue: 0.1
                property real angleValue: 90
                readonly property int count: view3d.selectionCount
                // Kinds of constraints, in the order of AntennaConstraint::Type.
                readonly property var constraintNames: [qsTr("Position fixée"), qsTr("Longueur imposée"), qsTr("Même longueur"),
                                                        qsTr("Parallèle"), qsTr("Perpendiculaire"), qsTr("Angle"),
                                                        qsTr("Points confondus"), qsTr("Distance")]
                readonly property bool needsReference: constraintType.value >= 2

                TabBar {
                    id: panelTabs
                    x: 4
                    y: 4
                    width: parent.width - 8
                    currentIndex: transformPanel.tab
                    onCurrentIndexChanged: transformPanel.tab = currentIndex
                    TabButton { text: qsTr("Transformer"); font.pixelSize: Theme.smallText + 1 }
                    TabButton { text: qsTr("Groupe"); font.pixelSize: Theme.smallText + 1 }
                    TabButton { text: qsTr("Contraintes"); font.pixelSize: Theme.smallText + 1 }
                }

                ColumnLayout {
                    id: transformColumn
                    visible: transformPanel.tab === 0
                    x: 8
                    y: panelTabs.y + panelTabs.height + 8
                    width: parent.width - 16
                    spacing: 4
                    Label {
                        text: transformPanel.count === 0 ? qsTr("Transformer")
                              : transformPanel.count === 1 ? qsTr("Transformer · 1 élément")
                              : qsTr("Transformer · %1 éléments").arg(transformPanel.count)
                        color: Theme.textMuted
                        font.pixelSize: Theme.smallText
                        font.bold: true
                    }
                    Label {
                        Layout.fillWidth: true
                        visible: transformPanel.count === 0
                        wrapMode: Text.WordWrap
                        color: Theme.textMuted
                        font.pixelSize: Theme.smallText
                        text: qsTr("Sélectionnez un élément. Ctrl + clic en ajoute ou en retire, Ctrl + A les prend tous.")
                    }
                    // Displacement, or an array of copies.
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 4
                        QuantityField { label: "Δx"; unit: "m"; value: transformPanel.dx; onEdited: (v) => transformPanel.dx = v }
                        QuantityField { label: "Δy"; unit: "m"; value: transformPanel.dy; onEdited: (v) => transformPanel.dy = v }
                        QuantityField { label: "Δz"; unit: "m"; value: transformPanel.dz; onEdited: (v) => transformPanel.dz = v }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 6
                        Label {
                            text: qsTr("Copies")
                            color: Theme.textMuted
                            font.pixelSize: Theme.smallText
                        }
                        SpinBox {
                            id: copiesBox
                            from: 0
                            to: 20
                            value: 0
                            editable: true
                            Layout.preferredWidth: 96
                            ToolTip.visible: hovered
                            ToolTip.delay: 600
                            ToolTip.text: qsTr("0 : déplacer la sélection. N : la laisser en place et poser N copies, chacune décalée de Δ par rapport à la précédente (réseau).")
                        }
                        Button {
                            Layout.fillWidth: true
                            enabled: transformPanel.count > 0
                            text: copiesBox.value === 0 ? qsTr("Déplacer") : qsTr("Dupliquer")
                            font.pixelSize: Theme.bodyText
                            onClicked: view3d.moveSelection(transformPanel.dx, transformPanel.dy, transformPanel.dz, copiesBox.value)
                        }
                    }
                    // Rotation.
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 4
                        ChoiceField {
                            id: axisField
                            label: qsTr("Tourner autour de")
                            choices: ["X", "Y", "Z", qsTr("Normale au plan")]
                            value: 2
                            onEdited: (index) => value = index
                        }
                        ChoiceField {
                            id: pivotField
                            label: qsTr("Axe passant par")
                            choices: [qsTr("Centre"), qsTr("Origine"), qsTr("Pivot placé")]
                            value: view3d.rotationPivot
                            onEdited: (index) => view3d.rotationPivot = index
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 4
                        QuantityField {
                            Layout.preferredWidth: 84
                            Layout.fillWidth: false
                            label: qsTr("Angle")
                            unit: "°"
                            value: transformPanel.angle
                            onEdited: (v) => transformPanel.angle = v
                        }
                        Button {
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignBottom
                            enabled: transformPanel.count > 0
                            text: qsTr("Tourner")
                            font.pixelSize: Theme.bodyText
                            onClicked: view3d.rotateSelection(axisField.value, transformPanel.angle, view3d.rotationPivot, false)
                        }
                        Button {
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignBottom
                            enabled: transformPanel.count > 0
                            text: qsTr("Copier")
                            font.pixelSize: Theme.bodyText
                            onClicked: view3d.rotateSelection(axisField.value, transformPanel.angle, view3d.rotationPivot, true)
                            ToolTip.visible: hovered
                            ToolTip.delay: 600
                            ToolTip.text: qsTr("Laisser la sélection en place et ajouter sa copie tournée")
                        }
                    }
                    // Mirror in the construction plane.
                    Label {
                        text: qsTr("Miroir dans le plan de construction")
                        color: Theme.textMuted
                        font.pixelSize: Theme.smallText
                        Layout.topMargin: 2
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 4
                        Button {
                            Layout.fillWidth: true
                            enabled: transformPanel.count > 0
                            text: qsTr("Miroir")
                            font.pixelSize: Theme.bodyText
                            onClicked: view3d.mirrorSelection(false)
                        }
                        Button {
                            Layout.fillWidth: true
                            enabled: transformPanel.count > 0
                            text: qsTr("Copie miroir")
                            font.pixelSize: Theme.bodyText
                            onClicked: view3d.mirrorSelection(true)
                        }
                    }
                    // Alignment on the main element (the one of the properties panel).
                    Label {
                        text: qsTr("Aligner sur l'élément principal")
                        color: Theme.textMuted
                        font.pixelSize: Theme.smallText
                        Layout.topMargin: 2
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 4
                        Repeater {
                            model: ["x", "y", "z"]
                            delegate: Button {
                                required property string modelData
                                required property int index
                                Layout.fillWidth: true
                                enabled: transformPanel.count > 1
                                text: modelData
                                font.pixelSize: Theme.bodyText
                                onClicked: view3d.alignSelection(index)
                                ToolTip.visible: hovered
                                ToolTip.delay: 600
                                ToolTip.text: qsTr("Donner aux centres des éléments sélectionnés la coordonnée %1 du dernier élément cliqué").arg(modelData)
                            }
                        }
                    }
                    Button {
                        Layout.fillWidth: true
                        enabled: transformPanel.count > 0
                        flat: true
                        text: transformPanel.count > 1 ? qsTr("Supprimer la sélection") : qsTr("Supprimer")
                        font.pixelSize: Theme.bodyText
                        onClicked: view3d.deleteSelection()
                    }
                }

                // Groups: elements selected and transformed together.
                ColumnLayout {
                    id: groupColumn
                    visible: transformPanel.tab === 1
                    x: 8
                    y: panelTabs.y + panelTabs.height + 8
                    width: parent.width - 16
                    spacing: 4
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: Theme.textMuted
                        font.pixelSize: Theme.smallText
                        text: qsTr("Les éléments d'un groupe se sélectionnent d'un clic et se déplacent, tournent et se copient ensemble. Pour agir sur un seul d'entre eux, le choisir dans la liste de la scène.")
                    }
                    Label {
                        text: qsTr("Groupe de l'élément principal")
                        color: Theme.textMuted
                        font.pixelSize: Theme.smallText
                        Layout.topMargin: 4
                    }
                    TextField {
                        id: groupField
                        Layout.fillWidth: true
                        enabled: view3d.groupName !== ""
                        placeholderText: qsTr("aucun groupe")
                        text: view3d.groupName
                        font.pixelSize: Theme.bodyText
                        selectByMouse: true
                        onEditingFinished: {
                            view3d.renameGroup(text)
                            text = Qt.binding(function() { return view3d.groupName })
                        }
                        ToolTip.visible: hovered && enabled
                        ToolTip.delay: 600
                        ToolTip.text: qsTr("Nom du groupe : le modifier le renomme")
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 4
                        Button {
                            Layout.fillWidth: true
                            enabled: transformPanel.count > 1
                            text: qsTr("Grouper")
                            font.pixelSize: Theme.bodyText
                            onClicked: view3d.groupSelection()
                            ToolTip.visible: hovered
                            ToolTip.delay: 600
                            ToolTip.text: qsTr("Faire un groupe des éléments sélectionnés (Ctrl + clic pour en sélectionner plusieurs)")
                        }
                        Button {
                            Layout.fillWidth: true
                            enabled: view3d.groupName !== ""
                            text: qsTr("Dégrouper")
                            font.pixelSize: Theme.bodyText
                            onClicked: view3d.ungroupSelection()
                        }
                    }
                }

                // Constraints the main element keeps with another one, or with itself.
                ColumnLayout {
                    id: constraintColumn
                    visible: transformPanel.tab === 2
                    x: 8
                    y: panelTabs.y + panelTabs.height + 8
                    width: parent.width - 16
                    spacing: 4
                    Label {
                        Layout.fillWidth: true
                        elide: Text.ElideRight
                        text: view3d.mainName === "" ? qsTr("Contraintes") : qsTr("Contraintes de %1").arg(view3d.mainName)
                        color: Theme.textMuted
                        font.pixelSize: Theme.smallText
                        font.bold: true
                    }
                    Label {
                        Layout.fillWidth: true
                        visible: view3d.constraints.length === 0
                        wrapMode: Text.WordWrap
                        color: Theme.textMuted
                        font.pixelSize: Theme.smallText
                        text: view3d.mainName === "" ? qsTr("Sélectionnez l'élément à contraindre.")
                                                     : qsTr("Aucune. L'élément qui porte une contrainte est celui qui bouge pour la respecter, à chaque modification de la scène.")
                    }
                    Repeater {
                        model: view3d.constraints
                        delegate: RowLayout {
                            required property var modelData
                            required property int index
                            Layout.fillWidth: true
                            spacing: 2
                            CheckBox {
                                checked: modelData.enabled
                                onToggled: view3d.setConstraintEnabled(index, checked)
                                ToolTip.visible: hovered
                                ToolTip.delay: 600
                                ToolTip.text: qsTr("Décocher suspend la contrainte sans la retirer")
                            }
                            Label {
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
                                color: modelData.met ? Theme.text : Theme.error
                                font.pixelSize: Theme.smallText + 1
                                text: (modelData.met ? "" : qsTr("Non respectée : ")) + modelData.text
                            }
                            ToolButton {
                                text: "×"
                                font.pixelSize: Theme.titleText
                                onClicked: view3d.removeConstraint(index)
                                ToolTip.visible: hovered
                                ToolTip.delay: 600
                                ToolTip.text: qsTr("Retirer la contrainte")
                            }
                        }
                    }
                    ChoiceField {
                        id: constraintType
                        Layout.topMargin: 4
                        label: qsTr("Nouvelle contrainte")
                        choices: transformPanel.constraintNames
                        value: 3
                        onEdited: (index) => {
                            value = index
                            // An imposed length starts from the length the element has.
                            if (index === 1 && view3d.mainLength > 0)
                                transformPanel.lengthValue = view3d.mainLength
                        }
                    }
                    Label {
                        Layout.fillWidth: true
                        visible: text.length > 0
                        wrapMode: Text.WordWrap
                        color: Theme.textMuted
                        font.pixelSize: Theme.smallText
                        text: view3d.mainName === "" ? ""
                              : !transformPanel.needsReference ? qsTr("S'applique à %1 seul.").arg(view3d.mainName)
                              : view3d.referenceName === "" ? qsTr("Il faut deux éléments : cliquez la référence, puis Ctrl + clic sur celui qui doit la suivre.")
                              : qsTr("%1 suivra %2.").arg(view3d.mainName).arg(view3d.referenceName)
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        visible: constraintType.value >= 6
                        spacing: 4
                        ChoiceField {
                            id: ownAnchor
                            label: qsTr("Point de l'élément")
                            choices: [qsTr("Centre"), qsTr("Bout 1"), qsTr("Bout 2")]
                            value: 1
                            onEdited: (index) => value = index
                        }
                        ChoiceField {
                            id: referenceAnchor
                            label: qsTr("Point de la référence")
                            choices: [qsTr("Centre"), qsTr("Bout 1"), qsTr("Bout 2")]
                            value: 2
                            onEdited: (index) => value = index
                        }
                    }
                    QuantityField {
                        visible: constraintType.value === 1
                        label: qsTr("Longueur")
                        unit: "m"
                        value: transformPanel.lengthValue
                        onEdited: (v) => transformPanel.lengthValue = v
                    }
                    QuantityField {
                        visible: constraintType.value === 7
                        label: qsTr("Distance")
                        unit: "m"
                        value: transformPanel.distanceValue
                        onEdited: (v) => transformPanel.distanceValue = v
                    }
                    QuantityField {
                        visible: constraintType.value === 5
                        label: qsTr("Angle entre les axes")
                        unit: "°"
                        value: transformPanel.angleValue
                        onEdited: (v) => transformPanel.angleValue = v
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        visible: constraintType.value === 0
                        spacing: 2
                        CheckBox { id: fixX; text: "x"; checked: true }
                        CheckBox { id: fixY; text: "y"; checked: true }
                        CheckBox { id: fixZ; text: "z"; checked: true }
                    }
                    Button {
                        Layout.fillWidth: true
                        enabled: view3d.mainName !== "" && (!transformPanel.needsReference || view3d.referenceName !== "")
                        text: qsTr("Ajouter")
                        font.pixelSize: Theme.bodyText
                        onClicked: {
                            const type = constraintType.value
                            const value = type === 1 ? transformPanel.lengthValue
                                        : type === 7 ? transformPanel.distanceValue
                                        : type === 5 ? transformPanel.angleValue : 0
                            const axes = (fixX.checked ? 1 : 0) + (fixY.checked ? 2 : 0) + (fixZ.checked ? 4 : 0)
                            view3d.addConstraint(type, value, ownAnchor.value, referenceAnchor.value, axes)
                        }
                    }
                }
            }

            // Display options: a floating panel, at the top left at first.
            // Context menus of the 3D view (right click). px, py: where the click fell, in the view.
            Menu {
                id: elementMenu
                property real px: 0
                property real py: 0
                width: 290
                MenuItem { action: duplicateAction }
                MenuItem { action: groupAction }
                MenuItem { action: ungroupAction }
                Menu {
                    title: qsTr("Tourner")
                    width: 310
                    MenuItem { action: rotatePlanePlus }
                    MenuItem { action: rotatePlaneMinus }
                    MenuSeparator {}
                    MenuItem { action: rotateXPlus }
                    MenuItem { action: rotateXMinus }
                    MenuItem { action: rotateYPlus }
                    MenuItem { action: rotateYMinus }
                    MenuItem { action: rotateZPlus }
                    MenuItem { action: rotateZMinus }
                }
                Menu {
                    title: qsTr("Copie miroir")
                    width: 310
                    MenuItem { action: mirrorX }
                    MenuItem { action: mirrorY }
                    MenuItem { action: mirrorZ }
                    MenuSeparator {}
                    MenuItem { action: mirrorPlane }
                }
                Menu {
                    title: qsTr("Aligner sur l'élément principal")
                    MenuItem { action: alignX }
                    MenuItem { action: alignY }
                    MenuItem { action: alignZ }
                }
                MenuSeparator {}
                MenuItem { action: constraintAction }
                MenuItem {
                    text: qsTr("Placer la source ici")
                    onTriggered: view3d.placeFeedAt(elementMenu.px, elementMenu.py)
                }
                MenuItem {
                    text: qsTr("Placer le pivot de rotation ici")
                    onTriggered: view3d.placePivot(elementMenu.px, elementMenu.py)
                }
                MenuSeparator {}
                MenuItem { action: deleteAction }
            }
            Menu {
                id: sceneMenu
                property real px: 0
                property real py: 0
                width: 290
                Menu {
                    title: qsTr("Créer")
                    width: 250
                    MenuItem {
                        text: qsTr("Fil tracé à la souris (outil Fil)")
                        onTriggered: {
                            view3d.tool = 1
                            root.followTool(1)
                        }
                    }
                    MenuSeparator {}
                    // Wires, then surfaces, then solids.
                    MenuItem { action: create0 }
                    MenuItem { action: create1 }
                    MenuItem { action: create2 }
                    MenuItem { action: create3 }
                    MenuItem { action: create4 }
                    MenuItem { action: create5 }
                    MenuItem { action: create14 }
                    MenuItem { action: create15 }
                    MenuSeparator {}
                    MenuItem { action: create6 }
                    MenuItem { action: create7 }
                    MenuItem { action: create12 }
                    MenuItem { action: create13 }
                    MenuSeparator {}
                    MenuItem { action: create8 }
                    MenuItem { action: create9 }
                    MenuItem { action: create10 }
                    MenuItem { action: create11 }
                }
                MenuItem {
                    text: qsTr("Mesurer une distance, un angle")
                    onTriggered: view3d.tool = 3
                }
                MenuItem {
                    text: qsTr("Placer le pivot de rotation ici")
                    onTriggered: view3d.placePivot(sceneMenu.px, sceneMenu.py)
                }
                MenuItem { action: clearPivotAction }
                MenuSeparator {}
                MenuItem { action: selectAllAction }
                MenuItem { action: fitAction }
            }
        }

        // ---- Pattern cuts ----
        Item {
            RowLayout {
                anchors.fill: parent
                anchors.topMargin: 8
                anchors.leftMargin: root.leftFolded ? 30 : 12
                anchors.rightMargin: root.rightFolded ? 30 : 12
                spacing: 12
                Repeater {
                    model: 2
                    delegate: ColumnLayout {
                        id: cutColumn
                        required property int index
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        spacing: 4
                        Label {
                            text: plot.cutTitle
                            color: Theme.text
                            font.pixelSize: Theme.bodyText
                            font.bold: true
                            elide: Text.ElideRight
                            horizontalAlignment: Text.AlignHCenter
                            Layout.fillWidth: true
                        }
                        PolarPlot {
                            id: plot
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            scene: root.scene
                            cut: cutColumn.index === 0 ? 0 : secondCutState.cut
                            range: cutRange.value
                            backgroundColor: Theme.canvas
                            gridColor: Theme.grid
                            textColor: Theme.text
                            accentColor: Theme.accent
                        }
                        Label {
                            text: plot.beamText
                            color: Theme.textMuted
                            font.pixelSize: Theme.smallText
                            horizontalAlignment: Text.AlignHCenter
                            Layout.fillWidth: true
                        }
                    }
                }
            }
            // Cut of the right plot: 1 vertical in the main lobe, 2 perpendicular.
            QtObject {
                id: secondCutState
                property int cut: 1
            }
        }

        // ---- Sweep ----
        Item {
            SweepPlot {
                id: sweepPlot
                anchors.fill: parent
                anchors.topMargin: 8
                anchors.leftMargin: root.leftFolded ? 24 : 6
                anchors.rightMargin: root.rightFolded ? 24 : 6
                scene: root.scene
                quantity: sweepBar.quantity
                backgroundColor: Theme.canvas
                gridColor: Theme.grid
                textColor: Theme.text
                accentColor: Theme.accent
                MouseArea {
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: sweepPlot.frequencyAt(mouseX) > 0 ? Qt.CrossCursor : Qt.ArrowCursor
                    onClicked: (mouse) => {
                        const f = sweepPlot.frequencyAt(mouse.x)
                        if (f > 0)
                            root.scene.setFrequency(f)
                    }
                    // The plot reads out the sweep point under the pointer (this area keeps the hover events).
                    onPositionChanged: sweepPlot.setPointer(mouseX, mouseY, true)
                    onExited: sweepPlot.setPointer(0, 0, false)
                }
            }
        }

        // ---- Smith chart ----
        Item {
            SmithChart {
                anchors.fill: parent
                anchors.topMargin: 8
                scene: root.scene
                backgroundColor: Theme.canvas
                gridColor: Theme.grid
                textColor: Theme.text
                accentColor: Theme.accent
            }
        }

        // ---- Yagi optimiser ----
        ScrollView {
            id: optimiserPage
            contentWidth: availableWidth
            clip: true
            ColumnLayout {
                width: Math.min(optimiserPage.availableWidth - 24, 720)
                x: (optimiserPage.availableWidth - width) / 2
                spacing: 12
                Label {
                    text: qsTr("Optimisation d'une Yagi-Uda")
                    color: Theme.text
                    font.pixelSize: Theme.titleText
                    font.bold: true
                }
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: Theme.textMuted
                    font.pixelSize: Theme.bodyText
                    text: qsTr("Cherche les longueurs des éléments droits (fils, dipôles) et leurs espacements le long du boom, "
                               + "à la fréquence d'analyse ; le premier élément alimenté reste en place. Chaque essai est une "
                               + "résolution complète par la méthode des moments (maillage limité à 60 segments par λ pendant "
                               + "la recherche, résultat recalculé au maillage de la scène). Le résultat s'applique en une "
                               + "seule étape, annulable.")
                }
                GridLayout {
                    columns: 3
                    columnSpacing: 16
                    Layout.fillWidth: true
                    ChoiceField {
                        id: goalField
                        label: qsTr("Objectif")
                        choices: root.scene.goalNames()
                        value: 2
                        onEdited: (index) => value = index
                        Layout.preferredWidth: 260
                    }
                    ColumnLayout {
                        spacing: 2
                        Label {
                            text: qsTr("Essais (résolutions)")
                            color: Theme.textMuted
                            font.pixelSize: Theme.smallText
                        }
                        SpinBox {
                            id: budgetBox
                            from: 20
                            to: 1000
                            stepSize: 20
                            value: 240
                            editable: true
                        }
                    }
                    Switch {
                        id: matchSwitch
                        checked: true
                        text: qsTr("Adapter à Z₀ (|S11| ≤ −10 dB)")
                        Layout.alignment: Qt.AlignBottom
                    }
                }
                RowLayout {
                    spacing: 12
                    Button {
                        text: root.scene.optimising ? qsTr("Arrêter") : qsTr("Optimiser")
                        highlighted: !root.scene.optimising
                        enabled: root.scene.optimising || root.scene.count >= 2
                        onClicked: root.scene.optimising ? root.scene.cancelOptimise()
                                                         : root.scene.optimise(goalField.value, matchSwitch.checked, budgetBox.value)
                    }
                    ProgressBar {
                        visible: root.scene.optimising
                        value: root.scene.optimiseProgress
                        Layout.preferredWidth: 180
                    }
                    BusyIndicator {
                        visible: root.scene.optimising
                        running: visible
                        implicitWidth: 22
                        implicitHeight: 22
                    }
                }
                Label {
                    visible: text.length > 0
                    text: root.scene.optimiserText
                    color: Theme.text
                    font.pixelSize: Theme.bodyText
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }
                // Before / after.
                RowLayout {
                    visible: (root.scene.optimiserResult.rows ?? []).length > 0
                    spacing: 24
                    Item { Layout.preferredWidth: 80 }
                    Label { text: qsTr("Avant"); color: Theme.textMuted; font.pixelSize: Theme.smallText; font.bold: true; Layout.preferredWidth: 120 }
                    Label { text: qsTr("Après"); color: Theme.textMuted; font.pixelSize: Theme.smallText; font.bold: true; Layout.preferredWidth: 120 }
                }
                Repeater {
                    model: root.scene.optimiserResult.rows ?? []
                    delegate: RowLayout {
                        required property var modelData
                        spacing: 24
                        Label {
                            text: modelData.name
                            color: Theme.textMuted
                            font.pixelSize: Theme.bodyText
                            Layout.preferredWidth: 80
                        }
                        Label {
                            text: Units.format(modelData.before, modelData.unit, 4)
                            color: Theme.text
                            font.pixelSize: Theme.bodyText
                            font.family: "Consolas"
                            Layout.preferredWidth: 120
                        }
                        Label {
                            text: Units.format(modelData.after, modelData.unit, 4)
                            color: Theme.text
                            font.pixelSize: Theme.bodyText
                            font.family: "Consolas"
                            font.bold: true
                            Layout.preferredWidth: 120
                        }
                    }
                }
                Label {
                    Layout.fillWidth: true
                    Layout.topMargin: 8
                    wrapMode: Text.WordWrap
                    color: Theme.textMuted
                    font.pixelSize: Theme.smallText
                    text: qsTr("Score maximisé : gain + poids × min(Av/Ar, 30 dB), moins 2 points par dB manquant sous "
                               + "|S11| = −10 dB quand l'adaptation est demandée. Un rapport avant/arrière très élevé tient à "
                               + "un zéro étroit : vérifiez la bande avec le balayage.")
                }
            }
        }

        // ---- Validation bench ----
        ScrollView {
            id: benchPage
            contentWidth: availableWidth
            clip: true
            ColumnLayout {
                width: Math.min(benchPage.availableWidth - 24, 900)
                x: (benchPage.availableWidth - width) / 2
                spacing: 10
                Label {
                    text: qsTr("Banc de validation — antennes filaires")
                    color: Theme.text
                    font.pixelSize: Theme.titleText
                    font.bold: true
                }
                Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    color: Theme.textMuted
                    font.pixelSize: Theme.bodyText
                    text: qsTr("Le solveur du module, au maillage de la scène, contre des références qui ne viennent pas de "
                               + "lui : l'équation de Hallén résolue par un outil indépendant, les formules des antennes "
                               + "électriquement petites, et des lois que toute solution doit respecter. « Conforme » "
                               + "signifie un écart sous le seuil que la référence elle-même permet.")
                }
                RowLayout {
                    spacing: 12
                    Button {
                        text: root.scene.benchRunning ? qsTr("En cours…") : qsTr("Lancer le banc")
                        highlighted: true
                        enabled: !root.scene.benchRunning
                        onClicked: root.scene.runBench()
                    }
                    ProgressBar {
                        visible: root.scene.benchRunning
                        value: root.scene.benchProgress
                        Layout.preferredWidth: 180
                    }
                    Label {
                        text: root.scene.benchSummary
                        color: Theme.text
                        font.pixelSize: Theme.bodyText
                        wrapMode: Text.WordWrap
                        Layout.fillWidth: true
                    }
                }
                Repeater {
                    model: root.scene.benchCases
                    delegate: Rectangle {
                        id: benchRow
                        required property var modelData
                        required property int index
                        readonly property bool newGroup: index === 0 || root.scene.benchCases[index - 1].group !== modelData.group
                        readonly property color verdictColour: modelData.verdict === 0 ? Theme.success
                                                               : modelData.verdict === 1 ? "#FFA726" : Theme.error
                        Layout.fillWidth: true
                        Layout.topMargin: newGroup ? 8 : 0
                        implicitHeight: rowColumn.implicitHeight + 14
                        radius: Theme.radius
                        color: Theme.panel
                        border.color: Theme.border
                        ColumnLayout {
                            id: rowColumn
                            x: 10
                            y: 7
                            width: parent.width - 20
                            spacing: 3
                            Label {
                                visible: benchRow.newGroup
                                text: benchRow.modelData.group
                                color: Theme.accent
                                font.pixelSize: Theme.smallText
                                font.bold: true
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 12
                                Label {
                                    text: benchRow.modelData.quantity
                                    color: Theme.text
                                    font.pixelSize: Theme.bodyText
                                    font.bold: true
                                    Layout.preferredWidth: 220
                                    elide: Text.ElideRight
                                }
                                Label {
                                    text: qsTr("calculé %1 · référence %2").arg(benchRow.modelData.value).arg(benchRow.modelData.reference)
                                    color: Theme.text
                                    font.pixelSize: Theme.bodyText
                                    Layout.fillWidth: true
                                    elide: Text.ElideRight
                                }
                                Label {
                                    text: qsTr("écart %1 (seuil %2)").arg(benchRow.modelData.deviation).arg(benchRow.modelData.passLimit)
                                    color: Theme.textMuted
                                    font.pixelSize: Theme.smallText
                                }
                                Rectangle {
                                    implicitWidth: verdictLabel.implicitWidth + 16
                                    implicitHeight: 22
                                    radius: 11
                                    color: Qt.alpha(benchRow.verdictColour, 0.15)
                                    border.color: Qt.alpha(benchRow.verdictColour, 0.6)
                                    Label {
                                        id: verdictLabel
                                        anchors.centerIn: parent
                                        text: benchRow.modelData.verdictName
                                        color: benchRow.verdictColour
                                        font.pixelSize: Theme.smallText
                                        font.bold: true
                                    }
                                }
                            }
                            Label {
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
                                color: Theme.textMuted
                                font.pixelSize: Theme.smallText
                                text: benchRow.modelData.source
                                      + (benchRow.modelData.note.length > 0 ? " — " + benchRow.modelData.note : "")
                            }
                        }
                    }
                }
            }
        }
        // ---- Radiation in 3D (Simulation): the antenna and its pattern, looked at, never edited ----
        Item {
            AntennaView3D {
                id: patternView
                anchors.fill: parent
                scene: root.scene
                readOnly: true
                backgroundColor: Theme.canvas
                gridColor: Theme.grid
                textColor: Theme.text
                accentColor: Theme.accent
                showCurrents: currentsBox.checked
                showPattern: true
                patternScale: scaleBox.currentIndex
                patternRange: rangeBox.value
                showOrigin: originBox.checked
                showGizmo: false
                showCompass: compassBox.checked
            }
            Label {
                x: root.leftFolded ? 34 : 12
                y: 8
                color: Theme.textMuted
                font.pixelSize: Theme.smallText
                style: Text.Outline
                styleColor: Theme.canvas
                text: qsTr("Rendu du rayonnement : glisser pour tourner, Maj + glisser pour déplacer, molette pour zoomer. L'antenne se modifie dans l'onglet Conception.")
            }
        }
    }

    // Start / stop the sweep, its progress, its band; the quantity plotted on the sweep page.
    component SweepBar: RowLayout {
        id: bar
        // Inline component: no access to the ids of this file, the scene is passed in.
        required property var scene
        property bool showQuantity: false
        property int quantity: 0
        spacing: 12
        Button {
            text: bar.scene.sweepRunning ? qsTr("Arrêter") : (bar.scene.hasSweep ? qsTr("Relancer le balayage") : qsTr("Lancer le balayage"))
            highlighted: !bar.scene.sweepRunning && (!bar.scene.hasSweep || bar.scene.sweepStale)
            enabled: bar.scene.count > 0
            onClicked: bar.scene.sweepRunning ? bar.scene.cancelSweep() : bar.scene.runSweep()
        }
        ProgressBar {
            visible: bar.scene.sweepRunning
            value: bar.scene.sweepProgress
            Layout.preferredWidth: 140
        }
        Label {
            // Read again whenever the settings change (frequency and referenceOhm notify on every scene change).
            readonly property var settings: { bar.scene.frequency; bar.scene.referenceOhm; bar.scene.count; return bar.scene.settingsData() }
            text: qsTr("%1 → %2 · %3 points · Z₀ = %4")
                  .arg(Units.format(settings.sweepStart ?? 0, "Hz", 4))
                  .arg(Units.format(settings.sweepStop ?? 0, "Hz", 4))
                  .arg(settings.sweepPoints ?? 0)
                  .arg(Units.format(settings.referenceOhm ?? 50, "Ω", 3))
            color: Theme.textMuted
            font.pixelSize: Theme.smallText
            ToolTip.visible: bandArea.containsMouse
            ToolTip.text: qsTr("Bande et nombre de points : réglages de l'inspecteur")
            MouseArea {
                id: bandArea
                anchors.fill: parent
                hoverEnabled: true
            }
        }
        ComboBox {
            visible: bar.showQuantity
            model: [qsTr("|S11| (dB)"), qsTr("TOS"), qsTr("Impédance R et X"), qsTr("Gain et rapport Av/Ar")]
            currentIndex: bar.quantity
            onActivated: (index) => bar.quantity = index
            Layout.preferredWidth: 220
            font.pixelSize: Theme.bodyText
        }
    }

    // ---- Tool header (the "ruban"): the tabs, and under them the tools of the tab in named groups ----
    Rectangle {
        id: ribbon
        parent: root.ribbonHost ? root.ribbonHost : root
        // In the place of the shell, the header no longer hides with this view: it follows it.
        visible: root.visible
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        height: root.ribbonHeight
        z: 10
        color: Theme.panel

        Row {
            x: 8
            height: 30
            spacing: 2
            Repeater {
                // Créer and Modifier are the two tabs of the design view; a hairline parts them from the
                // tabs that show results.
                model: [qsTr("Créer"), qsTr("Modifier"), qsTr("Simulation"), qsTr("Optimiser"), qsTr("Validation")]
                delegate: Item {
                    id: tabItem
                    required property string modelData
                    required property int index
                    readonly property bool current: root.tab === index
                    width: tabLabel.implicitWidth + 28
                    height: 30
                    Rectangle {
                        visible: tabItem.index === 2
                        x: -1
                        y: 8
                        width: 1
                        height: 14
                        color: Theme.border
                    }
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
                        onClicked: root.showTab(tabItem.index)
                    }
                }
            }
        }
        // Quick tools of the design view, beside the tabs: the view and what it shows, whatever the tab.
        Row {
            visible: root.designing
            anchors.right: parent.right
            anchors.rightMargin: 10
            y: 3
            height: 24
            spacing: 2
            RibbonButton {
                id: viewsButton
                compact: true
                glyph: "views"
                text: qsTr("Vues")
                dropDown: true
                tip: qsTr("Vue libre en 3D, ou vue en plan : de dessus, de face, de droite (aussi par le cube de la vue)")
                onClicked: viewsMenu.popup(viewsButton, 0, viewsButton.height + 2)
            }
            RibbonButton {
                compact: true
                glyph: "fit"
                action: fitAction
                text: qsTr("Cadrer")
                tip: qsTr("Cadrer la vue sur toute l'antenne")
            }
            RibbonButton {
                compact: true
                glyph: "display"
                text: qsTr("Affichage")
                dropDown: true
                checked: displayPopup.opened
                tip: qsTr("Ce que la vue montre : courants sur les fils et les surfaces, repères")
                onClicked: displayPopup.opened ? displayPopup.close() : displayPopup.open()
                Popup {
                    id: displayPopup
                    y: parent.height + 2
                    width: 260
                    margins: 8
                    padding: 10
                    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent
                    ColumnLayout {
                        width: displayPopup.availableWidth
                        spacing: 2
                        CheckBox {
                            id: currentsBox
                            text: qsTr("Courants (fils et surfaces)")
                            checked: true
                            font.pixelSize: Theme.bodyText
                        }
                        Label {
                            text: qsTr("Repères")
                            color: Theme.textMuted
                            font.pixelSize: Theme.smallText
                            font.bold: true
                            Layout.topMargin: 8
                        }
                        CheckBox {
                            id: originBox
                            text: qsTr("Axes de la scène (origine)")
                            checked: true
                            font.pixelSize: Theme.bodyText
                            Layout.preferredHeight: 34
                        }
                        CheckBox {
                            id: gizmoBox
                            text: qsTr("Flèches et poignées sur la sélection")
                            checked: true
                            font.pixelSize: Theme.bodyText
                            Layout.preferredHeight: 34
                            ToolTip.visible: hovered
                            ToolTip.delay: 600
                            ToolTip.text: qsTr("Sur la sélection (outil Pointeur) : trois flèches x, y, z pour la déplacer le long d'un axe, trois quarts de cercle pour la faire tourner autour d'un axe")
                        }
                        CheckBox {
                            id: compassBox
                            text: qsTr("Repère d'orientation (coin)")
                            checked: true
                            font.pixelSize: Theme.bodyText
                            Layout.preferredHeight: 34
                        }
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

        // The groups of the current tab. Each tab is made to fit a window of ordinary width; in a
        // narrower one the header scrolls sideways (wheel, drag, or the bar under the groups).
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

                    // ---- Conception: the tools of the 3D view ----
                    RibbonGroup {
                        visible: root.designing
                        title: qsTr("Sélection")
                        RibbonButton {
                            glyph: "pointer"
                            text: qsTr("Pointeur")
                            checked: view3d.tool === 0
                            tip: root.tools[0].tip + "  (V)"
                            onClicked: view3d.tool = 0
                        }
                        RibbonButton {
                            id: filterButton
                            glyph: "filter"
                            text: [qsTr("Tout"), qsTr("Fils"), qsTr("Surfaces"), qsTr("Sources")][root.selectionFilter]
                            dropDown: true
                            checked: root.selectionFilter !== 0
                            tip: qsTr("Ce que le clic sélectionne dans la vue (et ce que prend Ctrl + A) : tout, les fils seulement, les surfaces et les solides seulement, ou les sources seulement")
                            onClicked: filterMenu.popup(filterButton, 0, filterButton.height + 2)
                        }
                    }
                    RibbonGroup {
                        visible: root.tab === 0
                        // Shapes by nature: wires (curves that carry currents and sources), surfaces, solids.
                        title: qsTr("Créer un fil")
                        RibbonButton {
                            glyph: "wire"
                            text: qsTr("Fil")
                            checked: view3d.tool === 1
                            tip: root.tools[1].tip + "  (F)"
                            onClicked: view3d.tool = 1
                        }
                        RibbonButton {
                            id: dipoleButton
                            glyph: "dipole"
                            text: qsTr("Dipôle")
                            dropDown: true
                            tip: qsTr("Dipôle droit ou en V, dipôle replié : au centre de la vue, sur le plan de construction, avec sa source posée au centre")
                            onClicked: {
                                root.aimAtCentre()
                                dipoleMenu.popup(dipoleButton, 0, dipoleButton.height + 2)
                            }
                        }
                        RibbonButton {
                            id: loopButton
                            glyph: "loop"
                            text: qsTr("Boucle")
                            dropDown: true
                            tip: qsTr("Boucle circulaire ou cadre rectangulaire, avec sa source posée")
                            onClicked: {
                                root.aimAtCentre()
                                loopMenu.popup(loopButton, 0, loopButton.height + 2)
                            }
                        }
                        RibbonButton {
                            id: curveButton
                            glyph: "curve"
                            text: qsTr("Courbe")
                            dropDown: true
                            tip: qsTr("Arc de fil (passif, une source se glisse le long), hélice, spirale à deux bras (avec sa source au centre)")
                            onClicked: {
                                root.aimAtCentre()
                                curveMenu.popup(curveButton, 0, curveButton.height + 2)
                            }
                        }
                    }
                    RibbonGroup {
                        visible: root.tab === 0
                        title: qsTr("Créer une surface")
                        RibbonButton {
                            glyph: "plate"
                            text: qsTr("Plaque")
                            tip: qsTr("Plaque rectangulaire conductrice, au centre de la vue. Deux plaques qui se touchent le long d'un bord sont reliées ; un fil posé dessus y est relié.")
                            onClicked: root.createAtCentre(6)
                        }
                        RibbonButton {
                            glyph: "discflat"
                            text: qsTr("Disque")
                            tip: qsTr("Disque conducteur, au centre de la vue")
                            onClicked: root.createAtCentre(7)
                        }
                        RibbonButton {
                            id: parabolaButton
                            glyph: "dish"
                            text: qsTr("Parabole")
                            dropDown: true
                            tip: qsTr("Réflecteur parabolique : parabole de révolution, ou cylindre parabolique. Son foyer est marqué d'une croix : un dipôle posé là donne un faisceau le long de l'axe.")
                            onClicked: {
                                root.aimAtCentre()
                                parabolaMenu.popup(parabolaButton, 0, parabolaButton.height + 2)
                            }
                        }
                    }
                    RibbonGroup {
                        visible: root.tab === 0
                        title: qsTr("Créer un solide")
                        Repeater {
                            // Conducting solids (types 8 to 11 of the scene): closed skins. A wire whose
                            // end is put on one is attached to it.
                            model: [
                                { glyph: "box", name: qsTr("Pavé"), type: 8,
                                  tip: qsTr("Pavé conducteur (boîtier) : ses six faces sont jointes. Un fil dont un bout est posé sur une face y est relié ; une plaque qui le touche le long d'un bord aussi.") },
                                { glyph: "cylinder", name: qsTr("Cylindre"), type: 9,
                                  tip: qsTr("Cylindre conducteur fermé (mât, tube). Un fil se pose sur ses bouts ou sur sa paroi.") },
                                { glyph: "cone", name: qsTr("Cône"), type: 10,
                                  tip: qsTr("Cône conducteur fermé : sa base au pied de l'axe, sa pointe au bout. Un fil se pose sur sa base ou sur son flanc.") },
                                { glyph: "sphere", name: qsTr("Sphère"), type: 11,
                                  tip: qsTr("Sphère conductrice. Un fil se pose n'importe où sur elle.") }
                            ]
                            delegate: RibbonButton {
                                required property var modelData
                                glyph: modelData.glyph
                                text: modelData.name
                                tip: modelData.tip + qsTr(" Créé au centre de la vue, sur le plan de construction ; il ne s'alimente pas lui-même.")
                                onClicked: root.createAtCentre(modelData.type)
                            }
                        }
                    }
                    RibbonGroup {
                        visible: root.tab === 0
                        title: qsTr("Alimenter")
                        RibbonButton {
                            glyph: "source"
                            text: qsTr("Source")
                            checked: view3d.tool === 2
                            tip: root.tools[2].tip + "  (S)"
                            onClicked: view3d.tool = 2
                        }
                    }
                    RibbonGroup {
                        visible: root.tab === 1
                        title: qsTr("Modifier")
                        RibbonButton {
                            glyph: "move"
                            text: qsTr("Déplacer")
                            checked: transformPanel.open && !transformPanel.folded && transformPanel.tab === 0
                            tip: qsTr("Déplacer d'une distance saisie, copier en réseau, tourner d'un angle saisi : ouvre ou ferme la fenêtre des outils de la sélection")
                            onClicked: root.toggleSelectionTools(0)
                        }
                        RibbonButton {
                            glyph: "cut"
                            text: qsTr("Couper")
                            checked: view3d.tool === 5
                            tip: root.tools[5].tip + "  (C)"
                            onClicked: view3d.tool = 5
                        }
                        RibbonButton {
                            glyph: "duplicate"
                            action: duplicateAction
                            tip: qsTr("Copier la sélection à côté d'elle")
                        }
                        RibbonButton {
                            id: rotateButton
                            glyph: "rotate"
                            text: qsTr("Tourner")
                            dropDown: true
                            enabled: view3d.selectionCount > 0
                            tip: qsTr("Tourner la sélection de 45° autour d'un axe (pour un autre angle : Déplacer)")
                            onClicked: rotateMenu.popup(rotateButton, 0, rotateButton.height + 2)
                        }
                        RibbonButton {
                            id: mirrorButton
                            glyph: "mirror"
                            text: qsTr("Miroir")
                            dropDown: true
                            enabled: view3d.selectionCount > 0
                            tip: qsTr("Retourner la sélection, ou en poser une copie miroir")
                            onClicked: mirrorMenu.popup(mirrorButton, 0, mirrorButton.height + 2)
                        }
                        RibbonButton {
                            id: alignButton
                            glyph: "align"
                            text: qsTr("Aligner")
                            dropDown: true
                            enabled: view3d.selectionCount > 1
                            tip: qsTr("Donner aux centres des éléments sélectionnés une coordonnée du dernier élément cliqué")
                            onClicked: alignMenu.popup(alignButton, 0, alignButton.height + 2)
                        }
                        RibbonButton {
                            glyph: "delete"
                            text: qsTr("Supprimer")
                            checked: view3d.tool === 4
                            tip: qsTr("Supprimer la sélection (éléments, ou la source sélectionnée). Sans sélection : outil Supprimer, cliquer sur un élément ou sur une source (X). Ctrl+Z pour revenir.")
                            onClicked: {
                                if (view3d.selectionCount > 0 || view3d.feedSelected)
                                    view3d.deleteSelection()
                                else
                                    view3d.tool = 4
                            }
                        }
                    }
                    RibbonGroup {
                        visible: root.tab === 1
                        title: qsTr("Assembler")
                        RibbonButton {
                            glyph: "group"
                            action: groupAction
                            text: qsTr("Grouper")
                            tip: qsTr("Faire un groupe des éléments sélectionnés (Ctrl + clic pour en sélectionner plusieurs) : ils se sélectionnent, se déplacent et se copient ensemble")
                        }
                        RibbonButton {
                            glyph: "ungroup"
                            action: ungroupAction
                            tip: qsTr("Défaire le groupe de l'élément principal")
                        }
                        RibbonButton {
                            glyph: "constraint"
                            text: qsTr("Contraintes")
                            checked: transformPanel.open && !transformPanel.folded && transformPanel.tab === 2
                            tip: qsTr("Contraintes entre éléments (longueur imposée, parallèle, distance…) : ouvre ou ferme la fenêtre des outils de la sélection")
                            onClicked: root.toggleSelectionTools(2)
                        }
                    }
                    RibbonGroup {
                        visible: root.designing
                        title: qsTr("Construire")
                        RibbonButton {
                            glyph: "plane"
                            text: qsTr("Plan")
                            dropDown: true
                            checked: planePopup.opened
                            tip: qsTr("Plan de construction : la grille où l'on trace les fils et où glissent les éléments")
                            onClicked: planePopup.opened ? planePopup.close() : planePopup.open()
                            Popup {
                                id: planePopup
                                y: parent.height + 2
                                width: 300
                                margins: 8
                                padding: 10
                                closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent
                                ColumnLayout {
                                    width: planePopup.availableWidth
                                    spacing: 4
                                    ChoiceField {
                                        id: planeField
                                        Layout.fillWidth: true
                                        enabled: view3d.viewName === ""
                                        label: view3d.viewName === "" ? qsTr("Orientation") : qsTr("Orientation : celle de la vue")
                                        choices: [qsTr("XY (horizontal)"), qsTr("XZ"), qsTr("YZ")]
                                        value: 0
                                        onEdited: (index) => value = index
                                    }
                                    QuantityField {
                                        Layout.fillWidth: true
                                        label: qsTr("Position (%1)").arg(["z", "y", "x"][view3d.activePlane])
                                        unit: "m"
                                        value: view3d.editOffset
                                        onEdited: (v) => view3d.editOffset = v
                                    }
                                    // Rotation of the plane about the world axes, around the point where it
                                    // crosses the normal of its orientation (as in the 6.14).
                                    Label {
                                        Layout.topMargin: 6
                                        text: view3d.viewName === "" ? qsTr("Rotation (autour de x, puis y, puis z)")
                                                                     : qsTr("Rotation : en vue libre (la vue exacte garde son plan)")
                                        color: Theme.textMuted
                                        font.pixelSize: Theme.smallText
                                        wrapMode: Text.WordWrap
                                        Layout.fillWidth: true
                                    }
                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: 4
                                        QuantityField {
                                            id: planeRxField
                                            label: "Rx"
                                            unit: "°"
                                            value: 0
                                            onEdited: (v) => value = v
                                        }
                                        QuantityField {
                                            id: planeRyField
                                            label: "Ry"
                                            unit: "°"
                                            value: 0
                                            onEdited: (v) => value = v
                                        }
                                        QuantityField {
                                            id: planeRzField
                                            label: "Rz"
                                            unit: "°"
                                            value: 0
                                            onEdited: (v) => value = v
                                        }
                                    }
                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: 4
                                        Button {
                                            Layout.fillWidth: true
                                            text: qsTr("Vue de face")
                                            font.pixelSize: Theme.bodyText
                                            onClicked: view3d.lookAtPlane()
                                            ToolTip.visible: hovered
                                            ToolTip.delay: 600
                                            ToolTip.text: qsTr("Tourner la vue pour voir le plan de construction de face")
                                        }
                                        Button {
                                            Layout.fillWidth: true
                                            text: qsTr("Remettre à plat")
                                            font.pixelSize: Theme.bodyText
                                            enabled: planeRxField.value !== 0 || planeRyField.value !== 0 || planeRzField.value !== 0
                                            onClicked: {
                                                planeRxField.value = 0
                                                planeRyField.value = 0
                                                planeRzField.value = 0
                                            }
                                        }
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        Layout.topMargin: 4
                                        wrapMode: Text.WordWrap
                                        color: Theme.textMuted
                                        font.pixelSize: Theme.smallText
                                        text: qsTr("La grille est ce plan : on y trace les fils, les éléments glissés s'y déplacent parallèlement (pas de %1, Maj : libre).").arg(Units.format(view3d.snapStep, "m", 2))
                                    }
                                }
                            }
                        }
                        RibbonButton {
                            glyph: "grid"
                            text: qsTr("Pas")
                            dropDown: true
                            checked: stepPopup.opened
                            tip: qsTr("Pas des déplacements à la souris et des poignées de rotation (Maj : libre)")
                            onClicked: stepPopup.opened ? stepPopup.close() : stepPopup.open()
                            Popup {
                                id: stepPopup
                                y: parent.height + 2
                                width: 270
                                margins: 8
                                padding: 10
                                closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent
                                ColumnLayout {
                                    width: stepPopup.availableWidth
                                    spacing: 4
                                    QuantityField {
                                        id: moveStepField
                                        Layout.fillWidth: true
                                        label: qsTr("Pas de déplacement (0 : un dixième de la grille)")
                                        unit: "m"
                                        value: 0
                                        onEdited: (v) => value = Math.max(0, v)
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        color: Theme.textMuted
                                        font.pixelSize: Theme.smallText
                                        text: qsTr("Pas en cours : %1").arg(Units.format(view3d.snapStep, "m", 3))
                                    }
                                    Label {
                                        text: qsTr("Pas de rotation (°)")
                                        color: Theme.textMuted
                                        font.pixelSize: Theme.smallText
                                        Layout.topMargin: 6
                                    }
                                    SpinBox {
                                        id: rotationStepBox
                                        from: 1
                                        to: 90
                                        value: 15
                                        editable: true
                                        Layout.fillWidth: true
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        Layout.topMargin: 4
                                        wrapMode: Text.WordWrap
                                        color: Theme.textMuted
                                        font.pixelSize: Theme.smallText
                                        text: qsTr("Un élément ou une flèche glissés se posent sur le pas de déplacement. Les quarts de cercle posés sur la sélection la font tourner du pas de rotation, autour de l'axe de leur couleur. Maj : sans pas.")
                                    }
                                }
                            }
                        }
                        RibbonButton {
                            id: pivotButton
                            glyph: "pivot"
                            text: qsTr("Pivot")
                            dropDown: true
                            checked: view3d.tool === 6
                            tip: qsTr("Point autour duquel tourne la sélection, poignées comprises : son centre, l'origine, ou un pivot placé d'un clic dans la vue (P)")
                            onClicked: pivotMenu.popup(pivotButton, 0, pivotButton.height + 2)
                        }
                    }
                    RibbonGroup {
                        visible: root.designing
                        title: qsTr("Inspecter")
                        RibbonButton {
                            glyph: "measure"
                            text: qsTr("Mesurer")
                            checked: view3d.tool === 3
                            tip: root.tools[3].tip + "  (M)"
                            onClicked: view3d.tool = 3
                        }
                    }
                    // ---- Simulation: the page shown, and its settings ----
                    RibbonGroup {
                        visible: root.tab === 2
                        title: qsTr("Afficher")
                        RibbonButton {
                            glyph: "lobes"
                            text: qsTr("Rayonnement 3D")
                            checked: root.page === 6
                            tip: qsTr("L'antenne et son diagramme de rayonnement en 3D (vue de rendu : rien ne s'y modifie)")
                            onClicked: root.showAnalysis(6)
                        }
                        RibbonButton {
                            glyph: "pattern"
                            text: qsTr("Diagrammes")
                            checked: root.page === 1
                            tip: qsTr("Gain dans le plan horizontal et dans un plan vertical (diagrammes polaires, dBi)")
                            onClicked: root.showAnalysis(1)
                        }
                        RibbonButton {
                            glyph: "sweep"
                            text: qsTr("Balayage")
                            checked: root.page === 2
                            tip: qsTr("|S11|, TOS, impédance, gain et rapport avant/arrière en fonction de la fréquence")
                            onClicked: root.showAnalysis(2)
                        }
                        RibbonButton {
                            glyph: "smith"
                            text: qsTr("Smith")
                            checked: root.page === 3
                            tip: qsTr("Coefficient de réflexion du balayage sur l'abaque de Smith")
                            onClicked: root.showAnalysis(3)
                        }
                    }
                    RibbonGroup {
                        visible: root.page === 6
                        title: qsTr("Diagramme 3D")
                        Label {
                            anchors.verticalCenter: parent.verticalCenter
                            text: qsTr("Rayon")
                            color: Theme.textMuted
                            font.pixelSize: Theme.smallText
                            leftPadding: 4
                            rightPadding: 6
                        }
                        ComboBox {
                            id: scaleBox
                            anchors.verticalCenter: parent.verticalCenter
                            width: 210
                            // In the order of AntennaView3D.patternScale.
                            model: [qsTr("Champ |E| (linéaire)"), qsTr("Puissance (linéaire)"), qsTr("Gain en dB")]
                            currentIndex: 2
                            font.pixelSize: Theme.bodyText
                        }
                        Label {
                            anchors.verticalCenter: parent.verticalCenter
                            text: scaleBox.currentIndex === 2 ? qsTr("Dynamique (dB)") : qsTr("Couleurs sur (dB)")
                            color: Theme.textMuted
                            font.pixelSize: Theme.smallText
                            leftPadding: 14
                            rightPadding: 6
                        }
                        SpinBox {
                            id: rangeBox
                            anchors.verticalCenter: parent.verticalCenter
                            from: 10
                            to: 60
                            stepSize: 5
                            value: 40
                            editable: true
                            ToolTip.visible: hovered
                            ToolTip.delay: 600
                            ToolTip.text: qsTr("En dB, le rayon va du maximum (bord) à ce nombre de dB sous lui (centre). 30 à 40 dB détachent bien les lobes ; à 60 dB tout se tasse en boule.")
                        }
                        RibbonButton {
                            glyph: "views"
                            text: qsTr("Vue 3D")
                            tip: qsTr("Revenir à la vue initiale")
                            onClicked: patternView.resetView()
                        }
                        RibbonButton {
                            glyph: "fit"
                            text: qsTr("Cadrer")
                            tip: qsTr("Cadrer la vue sur l'antenne et son diagramme")
                            onClicked: patternView.fitView()
                        }
                    }
                    RibbonGroup {
                        visible: root.page === 1
                        title: qsTr("Coupes du diagramme")
                        Label {
                            anchors.verticalCenter: parent.verticalCenter
                            text: qsTr("Second plan")
                            color: Theme.textMuted
                            font.pixelSize: Theme.smallText
                            leftPadding: 4
                            rightPadding: 6
                        }
                        ComboBox {
                            anchors.verticalCenter: parent.verticalCenter
                            width: 300
                            model: [qsTr("Vertical, dans l'axe du lobe principal"), qsTr("Vertical, perpendiculaire au lobe")]
                            currentIndex: secondCutState.cut - 1
                            font.pixelSize: Theme.bodyText
                            onActivated: (index) => secondCutState.cut = index + 1
                        }
                        Label {
                            anchors.verticalCenter: parent.verticalCenter
                            text: qsTr("Dynamique (dB)")
                            color: Theme.textMuted
                            font.pixelSize: Theme.smallText
                            leftPadding: 14
                            rightPadding: 6
                        }
                        SpinBox {
                            id: cutRange
                            anchors.verticalCenter: parent.verticalCenter
                            from: 10
                            to: 60
                            stepSize: 5
                            value: 30
                            editable: true
                        }
                    }
                    RibbonGroup {
                        visible: root.page === 2 || root.page === 3
                        title: qsTr("Balayage en fréquence")
                        SweepBar {
                            id: sweepBar
                            anchors.verticalCenter: parent.verticalCenter
                            scene: root.scene
                            showQuantity: root.page === 2
                        }
                    }

                    // ---- Optimiser, Validation: what starts the calculation (their settings are in the page) ----
                    RibbonGroup {
                        visible: root.tab === 3
                        title: qsTr("Optimisation Yagi-Uda")
                        RibbonButton {
                            glyph: root.scene.optimising ? "stop" : "play"
                            primary: !root.scene.optimising
                            text: root.scene.optimising ? qsTr("Arrêter") : qsTr("Optimiser")
                            enabled: root.scene.optimising || root.scene.count >= 2
                            tip: qsTr("Lancer l'optimisation avec l'objectif et les options choisis dans la page")
                            onClicked: root.scene.optimising ? root.scene.cancelOptimise()
                                                             : root.scene.optimise(goalField.value, matchSwitch.checked, budgetBox.value)
                        }
                        ProgressBar {
                            anchors.verticalCenter: parent.verticalCenter
                            visible: root.scene.optimising
                            value: root.scene.optimiseProgress
                            width: 160
                        }
                    }
                    RibbonGroup {
                        visible: root.tab === 4
                        title: qsTr("Banc de validation")
                        RibbonButton {
                            glyph: "check"
                            primary: true
                            text: root.scene.benchRunning ? qsTr("En cours…") : qsTr("Lancer")
                            enabled: !root.scene.benchRunning
                            tip: qsTr("Comparer le solveur du module à ses références indépendantes")
                            onClicked: root.scene.runBench()
                        }
                        ProgressBar {
                            anchors.verticalCenter: parent.verticalCenter
                            visible: root.scene.benchRunning
                            value: root.scene.benchProgress
                            width: 160
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

    // Lists opened by the buttons of the header.
    Menu {
        id: filterMenu
        width: 300
        onAboutToShow: {
            filterAllItem.checked = root.selectionFilter === 0
            filterWiresItem.checked = root.selectionFilter === 1
            filterSurfacesItem.checked = root.selectionFilter === 2
            filterFedItem.checked = root.selectionFilter === 3
        }
        MenuItem {
            id: filterAllItem
            text: qsTr("Tout")
            checkable: true
            onTriggered: root.selectionFilter = 0
        }
        MenuItem {
            id: filterWiresItem
            text: qsTr("Fils et éléments filaires seulement")
            checkable: true
            onTriggered: root.selectionFilter = 1
        }
        MenuItem {
            id: filterSurfacesItem
            text: qsTr("Surfaces et solides seulement")
            checkable: true
            onTriggered: root.selectionFilter = 2
        }
        MenuItem {
            id: filterFedItem
            text: qsTr("Sources seulement")
            checkable: true
            onTriggered: root.selectionFilter = 3
        }
    }
    Menu {
        id: curveMenu
        width: 250
        MenuItem { action: create14 }
        MenuItem { action: create5 }
        MenuItem { action: create15 }
    }
    Menu {
        id: parabolaMenu
        width: 250
        MenuItem { action: create12 }
        MenuItem { action: create13 }
    }
    Menu {
        id: dipoleMenu
        width: 250
        MenuItem { action: create1 }
        MenuItem { action: create2 }
    }
    Menu {
        id: loopMenu
        width: 250
        MenuItem { action: create3 }
        MenuItem { action: create4 }
    }
    Menu {
        id: rotateMenu
        width: 310
        MenuItem { action: rotatePlanePlus }
        MenuItem { action: rotatePlaneMinus }
        MenuSeparator {}
        MenuItem { action: rotateXPlus }
        MenuItem { action: rotateXMinus }
        MenuItem { action: rotateYPlus }
        MenuItem { action: rotateYMinus }
        MenuItem { action: rotateZPlus }
        MenuItem { action: rotateZMinus }
    }
    Menu {
        id: mirrorMenu
        width: 310
        MenuItem {
            text: qsTr("Retourner dans le plan de construction")
            onTriggered: view3d.mirrorSelection(false)
        }
        MenuSeparator {}
        MenuItem {
            text: qsTr("Copie miroir :")
            enabled: false
        }
        MenuItem { action: mirrorX }
        MenuItem { action: mirrorY }
        MenuItem { action: mirrorZ }
        MenuItem { action: mirrorPlane }
    }
    Menu {
        id: alignMenu
        width: 250
        MenuItem {
            text: qsTr("Sur l'élément principal :")
            enabled: false
        }
        MenuItem { action: alignX }
        MenuItem { action: alignY }
        MenuItem { action: alignZ }
    }
    Menu {
        id: viewsMenu
        width: 270
        MenuItem {
            text: qsTr("3D (vue libre initiale)")
            onTriggered: view3d.resetView()
        }
        MenuItem {
            text: qsTr("Dessus (plan XY, vu de +z)")
            onTriggered: view3d.lookFrom(2, 1)
        }
        MenuItem {
            text: qsTr("Face (plan XZ, vu de −y)")
            onTriggered: view3d.lookFrom(1, -1)
        }
        MenuItem {
            text: qsTr("Droite (plan YZ, vu de +x)")
            onTriggered: view3d.lookFrom(0, 1)
        }
    }
    Menu {
        id: pivotMenu
        width: 330
        // What the rotations turn about (handles, menus, panel), then the pivot itself.
        MenuItem {
            text: qsTr("Tourner autour du centre de la sélection")
            checkable: true
            checked: view3d.rotationPivot === 0
            onTriggered: view3d.rotationPivot = 0
        }
        MenuItem {
            text: qsTr("Tourner autour de l'origine")
            checkable: true
            checked: view3d.rotationPivot === 1
            onTriggered: view3d.rotationPivot = 1
        }
        MenuItem {
            text: view3d.pivotSet ? qsTr("Tourner autour du pivot (%1)").arg(view3d.pivotText) : qsTr("Tourner autour du pivot (aucun placé)")
            checkable: true
            checked: view3d.rotationPivot === 2 && view3d.pivotSet
            enabled: view3d.pivotSet
            onTriggered: view3d.rotationPivot = 2
        }
        MenuSeparator {}
        MenuItem {
            text: view3d.pivotSet ? qsTr("Déplacer le pivot (cliquer dans la vue)  P") : qsTr("Placer un pivot (cliquer dans la vue)  P")
            onTriggered: {
                view3d.tool = 6
                root.followTool(6)
            }
        }
        MenuItem { action: clearPivotAction }
    }
    // Tool shortcuts on the 3D page (not while typing in a field).
    Repeater {
        model: root.tools
        delegate: Item {
            required property var modelData
            required property int index
            Shortcut {
                sequence: modelData.key
                enabled: root.visible && root.page === 0 && !root.typing
                onActivated: {
                    view3d.tool = index
                    root.followTool(index)
                }
            }
        }
    }
    Shortcut {
        sequence: "Escape"
        enabled: root.visible && root.page === 0 && view3d.tool !== 0 && !root.typing
        onActivated: view3d.tool = 0
    }
    Shortcut {
        sequence: StandardKey.SelectAll
        enabled: root.visible && root.page === 0 && !root.typing
        onActivated: view3d.selectAll()
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

    // ---- Status (bottom centre) ----
    Rectangle {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 10
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
                running: root.scene.busy
                visible: root.scene.busy
                implicitWidth: 18
                implicitHeight: 18
            }
            Label {
                color: Theme.textMuted
                font.pixelSize: Theme.smallText
                text: root.scene.statusText
            }
        }
    }
}
