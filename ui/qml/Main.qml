import QtCore
import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Dialogs
import QtQuick.Layouts
import Qsig.App

ApplicationWindow {
    id: window
    width: 1480
    height: 900
    minimumWidth: 1100
    minimumHeight: 680
    visible: true
    title: "QTsignalApp — " + scene.title + (scene.fileUrl.toString().length > 0
           ? " — " + decodeURIComponent(scene.fileUrl.toString().split("/").pop()) : "")
           + (scene.modified ? " •" : "")

    Material.theme: Theme.dark ? Material.Dark : Material.Light
    Material.accent: Theme.accent
    Material.primary: Theme.panel
    Material.background: Theme.window
    color: Theme.window

    // One scene per module; the panels and the view always work on the current one.
    property int module: 0
    readonly property var scenes: [electrostatics, magnetostatics, waves, antennas]
    readonly property var scene: scenes[module]
    // Field modules share the field view; it keeps the last one while another kind of view is shown.
    readonly property bool antennaView: scene.viewKind === "antenna"
    property var fieldScene: electrostatics
    property int selectedRow: -1
    property vector3d probe: Qt.vector3d(0.15, 0.2, 0)
    property bool probeVisible: false
    // Second measurement point (modules with differential readings).
    property vector3d probeB: Qt.vector3d(0.6, 0.2, 0)
    property bool probeBVisible: false
    // Side panels can be folded away to give the view the whole width (F9, F10).
    property bool scenePanelOpen: true
    property bool inspectorOpen: true
    property bool darkTheme: true

    // Remembered between sessions (per user, under "QTsignalApp").
    Settings {
        id: settings
        category: "window"
        property alias scenePanelOpen: window.scenePanelOpen
        property alias inspectorOpen: window.inspectorOpen
        property alias darkTheme: window.darkTheme
        property var splitState
    }
    Binding {
        target: Theme
        property: "dark"
        value: window.darkTheme
    }

    function showError(message) {
        errorLabel.text = message
        errorPopup.open()
    }
    function switchModule(index) {
        if (index === module)
            return
        module = index
        selectedRow = -1
        probeVisible = false
        probeBVisible = false
        if (antennaView)
            return
        fieldScene = scene
        viewport.view.quantity = scene.defaultQuantity
        viewport.view.fitScene()
    }
    // Frame the current scene (after New, Open, an example).
    function fitView() {
        if (antennaView)
            antennaViewport.resetView()
        else
            viewport.view.fitScene()
    }

    ElectrostaticScene {
        id: electrostatics
        onErrorOccurred: (message) => window.showError(message)
        onCountChanged: if (window.scene === electrostatics && window.selectedRow >= count) window.selectedRow = count - 1
        Component.onCompleted: loadExample("dipole")
    }
    MagnetostaticScene {
        id: magnetostatics
        onErrorOccurred: (message) => window.showError(message)
        onCountChanged: if (window.scene === magnetostatics && window.selectedRow >= count) window.selectedRow = count - 1
        Component.onCompleted: loadExample("two_wires_opposite")
    }
    WaveScene {
        id: waves
        // Runs only while the module is on screen.
        active: window.module === 2
        onErrorOccurred: (message) => window.showError(message)
        onCountChanged: if (window.scene === waves && window.selectedRow >= count) window.selectedRow = count - 1
        Component.onCompleted: loadExample("double_slit")
    }
    AntennaScene {
        id: antennas
        onErrorOccurred: (message) => window.showError(message)
        onCountChanged: if (window.scene === antennas && window.selectedRow >= count) window.selectedRow = count - 1
        // No example at start: the designer opens on an empty scene (the examples are in the Scène panel).
    }
    Component.onCompleted: {
        if (settings.splitState)
            split.restoreState(settings.splitState)
        viewport.view.fitScene()
    }

    Shortcut { sequence: "F9"; onActivated: window.scenePanelOpen = !window.scenePanelOpen }
    Shortcut { sequence: "F10"; onActivated: window.inspectorOpen = !window.inspectorOpen }
    // Play / pause of a live module (not while typing in a field).
    Shortcut {
        sequence: "Space"
        enabled: window.scene.live && !(window.activeFocusItem instanceof TextInput)
        onActivated: window.scene.running = !window.scene.running
    }

    // ---- Commands ----
    Action { id: newAction; text: qsTr("Nouveau"); shortcut: StandardKey.New; onTriggered: { window.scene.newScene(); window.selectedRow = -1; window.fitView() } }
    Action { id: openAction; text: qsTr("Ouvrir…"); shortcut: StandardKey.Open; onTriggered: openDialog.open() }
    Action {
        id: saveAction
        text: qsTr("Enregistrer")
        shortcut: StandardKey.Save
        onTriggered: window.scene.fileUrl.toString().length > 0 ? window.scene.save(window.scene.fileUrl) : saveDialog.open()
    }
    Action { id: saveAsAction; text: qsTr("Enregistrer sous…"); shortcut: StandardKey.SaveAs; onTriggered: saveDialog.open() }
    Action { id: undoAction; text: qsTr("Annuler"); shortcut: StandardKey.Undo; enabled: window.scene.canUndo; onTriggered: window.scene.undo() }
    Action { id: redoAction; text: qsTr("Rétablir"); shortcut: StandardKey.Redo; enabled: window.scene.canRedo; onTriggered: window.scene.redo() }
    Action {
        id: deleteAction
        text: qsTr("Supprimer la source")
        shortcut: StandardKey.Delete
        enabled: window.selectedRow >= 0 || (window.antennaView && antennaViewport.feedSelected)
        onTriggered: {
            // Antennas: the selected source alone, or the whole selection of the designer.
            if (window.antennaView) {
                antennaViewport.deleteSelection()
                return
            }
            window.scene.removeSource(window.selectedRow)
            window.selectedRow = -1
        }
    }

    FileDialog {
        id: openDialog
        title: qsTr("Ouvrir une scène")
        nameFilters: [qsTr("Scènes « %1 » (*.%2)").arg(window.scene.title).arg(window.scene.fileSuffix), qsTr("Tous les fichiers (*)")]
        onAccepted: {
            if (window.scene.load(selectedFile)) {
                window.selectedRow = -1
                window.fitView()
            }
        }
    }
    FileDialog {
        id: saveDialog
        title: qsTr("Enregistrer la scène")
        fileMode: FileDialog.SaveFile
        defaultSuffix: window.scene.fileSuffix
        nameFilters: [qsTr("Scènes « %1 » (*.%2)").arg(window.scene.title).arg(window.scene.fileSuffix)]
        onAccepted: window.scene.save(selectedFile)
    }

    header: ToolBar {
        Material.background: Theme.panel
        height: 52
        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 16
            anchors.rightMargin: 12
            spacing: 4

            Label {
                text: "QTsignalApp"
                font.pixelSize: 16
                font.bold: true
                color: Theme.text
            }
            Label {
                text: "  /  " + window.scene.title
                font.pixelSize: 15
                color: Theme.textMuted
                Layout.rightMargin: 18
            }
            ToolButton { action: newAction }
            ToolButton { action: openAction }
            ToolButton { action: saveAction }
            Rectangle { width: 1; height: 24; color: Theme.border; Layout.leftMargin: 6; Layout.rightMargin: 6 }
            ToolButton {
                action: undoAction
                ToolTip.visible: hovered && window.scene.canUndo
                ToolTip.text: qsTr("Annuler : %1").arg(window.scene.undoText)
            }
            ToolButton {
                action: redoAction
                ToolTip.visible: hovered && window.scene.canRedo
                ToolTip.text: qsTr("Rétablir : %1").arg(window.scene.redoText)
            }
            Item { Layout.fillWidth: true }
            ToolButton {
                checkable: true
                checked: window.scenePanelOpen
                onToggled: window.scenePanelOpen = checked
                contentItem: Item {
                    implicitWidth: 24
                    implicitHeight: 24
                    PanelIcon { anchors.centerIn: parent; active: window.scenePanelOpen }
                }
                ToolTip.visible: hovered
                ToolTip.text: (window.scenePanelOpen ? qsTr("Masquer la scène") : qsTr("Afficher la scène")) + " (F9)"
            }
            ToolButton {
                checkable: true
                checked: window.inspectorOpen
                onToggled: window.inspectorOpen = checked
                contentItem: Item {
                    implicitWidth: 24
                    implicitHeight: 24
                    PanelIcon { anchors.centerIn: parent; rightSide: true; active: window.inspectorOpen }
                }
                ToolTip.visible: hovered
                ToolTip.text: (window.inspectorOpen ? qsTr("Masquer les propriétés") : qsTr("Afficher les propriétés")) + " (F10)"
            }
            Rectangle { width: 1; height: 24; color: Theme.border; Layout.leftMargin: 6; Layout.rightMargin: 6 }
            Switch {
                text: window.darkTheme ? qsTr("Sombre") : qsTr("Clair")
                checked: window.darkTheme
                onToggled: window.darkTheme = checked
            }
        }
        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width
            height: 1
            color: Theme.border
        }
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        Sidebar {
            Layout.fillHeight: true
            current: window.module
            onModuleSelected: (index) => window.switchModule(index)
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            // Tool header of the module, across the scene, the view and the properties: the view of
            // the module puts its header there, and shows it with itself.
            Item {
                id: moduleRibbon
                Layout.fillWidth: true
                Layout.preferredHeight: antennaViewport.ribbonHeight
            }

            SplitView {
                id: split
                onResizingChanged: if (!resizing) settings.splitState = saveState()
                Layout.fillWidth: true
                Layout.fillHeight: true
                orientation: Qt.Horizontal
                handle: Rectangle {
                    implicitWidth: 4
                    color: SplitHandle.hovered || SplitHandle.pressed ? Qt.alpha(Theme.accent, 0.5) : "transparent"
                }

                ScenePanel {
                    visible: window.scenePanelOpen
                    SplitView.preferredWidth: 280
                    SplitView.minimumWidth: 220
                    scene: window.scene
                    onCollapseRequested: window.scenePanelOpen = false
                    selectedRow: window.selectedRow
                    onRowSelected: (row) => window.selectedRow = row
                    selectedFeed: window.antennaView ? antennaViewport.selectedFeed : ({})
                    onFeedSelected: (row, index) => antennaViewport.selectFeed(row, index)
                    onGroupSelected: (name) => antennaViewport.selectGroup(name)
                    onExampleLoaded: (plane) => {
                        if (window.antennaView)
                            antennaViewport.resetView()
                        else
                            viewport.showExample(plane)
                    }
                }

                // Centre: the field view or the antenna views, after the kind of module.
                Item {
                    SplitView.fillWidth: true
                    SplitView.minimumWidth: 400

                    AntennaViewport {
                        id: antennaViewport
                        ribbonHost: moduleRibbon
                        anchors.fill: parent
                        visible: window.antennaView
                        scene: antennas
                        selectedRow: window.antennaView ? window.selectedRow : -1
                        leftFolded: !window.scenePanelOpen
                        rightFolded: !window.inspectorOpen
                        onUnfoldLeft: window.scenePanelOpen = true
                        onUnfoldRight: window.inspectorOpen = true
                        onRowSelected: (row) => window.selectedRow = row
                    }

                    FieldViewport {
                        id: viewport
                        ribbonHost: moduleRibbon
                        anchors.fill: parent
                        visible: !window.antennaView
                        scene: window.fieldScene
                        selectedRow: window.scene === window.fieldScene ? window.selectedRow : -1
                        probe: window.probe
                        probeVisible: window.probeVisible
                        leftFolded: !window.scenePanelOpen
                        rightFolded: !window.inspectorOpen
                        onUnfoldLeft: window.scenePanelOpen = true
                        onUnfoldRight: window.inspectorOpen = true
                        onRowSelected: (row) => window.selectedRow = row
                        onProbePlaced: (point) => {
                            window.probe = point
                            window.probeVisible = true
                        }
                        probeB: window.probeB
                        probeBVisible: window.probeBVisible
                        onProbeBPlaced: (point) => {
                            window.probeB = point
                            window.probeBVisible = true
                            window.probeVisible = true
                        }
                    }
                }

                Inspector {
                    visible: window.inspectorOpen
                    SplitView.preferredWidth: 340
                    SplitView.minimumWidth: 280
                    scene: window.scene
                    onCollapseRequested: window.inspectorOpen = false
                    row: window.selectedRow
                    feed: window.antennaView ? antennaViewport.selectedFeed : ({})
                    onFeedEdited: (key, value) => antennaViewport.setFeedValue(key, value)
                    onFeedRemoved: antennaViewport.deleteFeed()
                    onFeedRequested: (row, index) => antennaViewport.selectFeed(row, index)
                    probe: window.probe
                    onProbeEdited: (point) => {
                        window.probe = point
                        window.probeVisible = true
                    }
                    probeB: window.probeB
                    probeBEnabled: window.probeBVisible
                    onProbeBEdited: (point) => {
                        window.probeB = point
                        window.probeBVisible = true
                    }
                    onProbeBToggled: (enabled) => {
                        window.probeBVisible = enabled
                        if (enabled)
                            window.probeVisible = true
                    }
                    onRowSelected: (row) => window.selectedRow = row
                }
            }
        }
    }

    Popup {
        id: errorPopup
        x: (window.width - width) / 2
        y: 70
        padding: 14
        background: Rectangle {
            radius: Theme.radius
            color: Theme.panel
            border.color: Theme.error
        }
        Label {
            id: errorLabel
            color: Theme.text
        }
    }
}
