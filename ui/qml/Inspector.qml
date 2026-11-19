import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qsig.App

// Right panel: properties of the selected source, its physical model, the settings of the scene
// (when the module has some) and the measurement point.
Rectangle {
    id: root
    required property var scene
    property int row: -1
    property vector3d probe: Qt.vector3d(0.15, 0.2, 0)
    // Second point B (modules with differential readings).
    property vector3d probeB: Qt.vector3d(0.6, 0.2, 0)
    property bool probeBEnabled: false
    readonly property bool useB: probeBEnabled && scene.differentialProbe
    signal probeEdited(vector3d point)
    signal probeBEdited(vector3d point)
    signal probeBToggled(bool enabled)
    signal rowSelected(int row)
    signal collapseRequested()
    // Antennas: sources are entities placed on the wires. 'feed': the one selected in the view (an entry
    // of the scene's feedList, an empty object when none), shown when no element is selected.
    property var feed: ({})
    readonly property bool hasFeed: !hasSource && feed.row !== undefined
    signal feedEdited(string key, real value)
    signal feedRemoved()
    signal feedRequested(int row, int index)
    // The sources placed on the selected element, and whether it can carry any (a wire element).
    property var elementFeeds: []
    property bool feedable: false

    color: Theme.panel

    property var source: ({})
    property var reading: ({})
    property var settings: ({})
    property string settingsNote: ""
    readonly property bool hasSource: row >= 0 && row < scene.count && source.name !== undefined

    function refresh() {
        source = row >= 0 && row < scene.count ? scene.sourceData(row) : ({})
        feedable = scene.viewKind === "antenna" && row >= 0 && row < scene.count && !scene.isSurface(row)
        elementFeeds = feedable ? scene.feedList.filter((entry) => entry.row === row) : []
        settings = scene.settingsData()
        settingsNote = scene.settingsNote()
        scene.setProbePoints(probe, useB, probeB)
        reading = scene.probe(probe.x, probe.y, probe.z)
    }
    onRowChanged: refresh()
    onProbeChanged: refresh()
    onProbeBChanged: refresh()
    onUseBChanged: refresh()
    onSceneChanged: refresh()
    Component.onCompleted: refresh()
    Connections {
        target: root.scene
        function onSceneChanged() { root.refresh() }
        // Live modules: only the reading moves (the fields being edited keep their text).
        function onReadingsChanged() {
            root.reading = root.scene.probe(root.probe.x, root.probe.y, root.probe.z)
            root.settingsNote = root.scene.settingsNote()
        }
    }

    Rectangle {
        anchors.left: parent.left
        width: 1
        height: parent.height
        color: Theme.border
    }

    ScrollView {
        id: scroller
        anchors.fill: parent
        anchors.margins: Theme.gap
        contentWidth: availableWidth

        ColumnLayout {
            width: scroller.availableWidth
            spacing: 10

            RowLayout {
                Layout.fillWidth: true
                Label {
                    text: root.hasSource ? (root.source.name ?? "") : root.hasFeed ? (root.feed.name ?? "") : qsTr("Propriétés")
                    font.pixelSize: Theme.titleText
                    font.bold: true
                    color: Theme.text
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                }
                FoldButton {
                    pointsLeft: false
                    toolTip: qsTr("Masquer les propriétés (F10)")
                    onClicked: root.collapseRequested()
                }
            }
            Label {
                visible: !root.hasSource && !root.hasFeed
                text: root.scene.viewKind === "antenna" ? qsTr("Sélectionnez un élément ou une source dans la scène ou dans la vue pour les modifier.")
                                                        : qsTr("Sélectionnez une source dans la scène ou dans la vue pour la modifier.")
                wrapMode: Text.WordWrap
                color: Theme.textMuted
                Layout.fillWidth: true
            }

            // ---- Antennas: the selected source, an entity placed on a wire ----
            ColumnLayout {
                visible: root.hasFeed
                Layout.fillWidth: true
                spacing: 10

                Label {
                    text: qsTr("Source de tension")
                    color: Theme.accent
                    font.pixelSize: Theme.smallText
                    font.bold: true
                }
                Label {
                    text: root.hasFeed ? qsTr("Placée sur %1, %2.").arg(root.feed.host ?? "").arg(root.feed.place ?? "") : ""
                    wrapMode: Text.WordWrap
                    color: Theme.text
                    font.pixelSize: Theme.bodyText
                    Layout.fillWidth: true
                }
                QuantityField {
                    visible: root.feed.along === true
                    label: qsTr("Position le long du fil (0 à 1)")
                    unit: ""
                    value: root.feed.fraction ?? 0.5
                    onEdited: (v) => root.feedEdited("fraction", v)
                }
                QuantityField {
                    label: qsTr("V — tension")
                    unit: "V"
                    value: root.feed.voltage ?? 1
                    onEdited: (v) => root.feedEdited("voltage", v)
                }
                QuantityField {
                    label: qsTr("φ — phase")
                    unit: "°"
                    value: root.feed.phaseDeg ?? 0
                    onEdited: (v) => root.feedEdited("phaseDeg", v)
                }
                Label {
                    text: root.feed.movable === true ? qsTr("Dans la vue, glissez la source pour la déplacer le long de son fil : les quarts et le milieu l'attirent. À 0 ou à 1, elle est au bout même, entre le fil et la surface qu'il touche.")
                          : root.feed.along === true ? qsTr("À 0, la source est au pied de l'hélice (mode axial, sur un plan de masse) ; à 0,5, au milieu du fil (mode normal).")
                                                    : qsTr("La source d'un élément tout fait est à la place que lui donne son type. Pour la placer librement, tracez un fil et posez-y une source.")
                    wrapMode: Text.WordWrap
                    color: Theme.textMuted
                    font.pixelSize: Theme.smallText
                    Layout.fillWidth: true
                }
                RowLayout {
                    Layout.topMargin: 4
                    Button {
                        text: qsTr("Sélectionner son fil")
                        flat: true
                        onClicked: root.rowSelected(root.feed.row)
                    }
                    Button {
                        text: qsTr("Supprimer la source")
                        flat: true
                        onClicked: root.feedRemoved()
                    }
                }
            }

            // ---- Selected source ----
            ColumnLayout {
                visible: root.hasSource
                Layout.fillWidth: true
                spacing: 10

                Label {
                    text: root.source.typeName ?? ""
                    color: Theme.accent
                    font.pixelSize: Theme.smallText
                    font.bold: true
                }
                TextField {
                    Layout.fillWidth: true
                    text: root.source.name ?? ""
                    placeholderText: qsTr("Nom")
                    selectByMouse: true
                    onEditingFinished: if (text !== root.source.name) root.scene.setSourceValue(root.row, "name", text)
                }
                QuantityField {
                    visible: label.length > 0
                    label: root.hasSource ? root.scene.strengthLabel(root.source.type) : ""
                    unit: root.hasSource ? root.scene.strengthUnit(root.source.type) : "C"
                    value: root.source.strength ?? 0
                    onEdited: (v) => root.scene.setSourceValue(root.row, "strength", v)
                }

                // Charge given as a density of the shape: Q = density x volume (surface, length...).
                ColumnLayout {
                    visible: root.source.hasDensity === true
                    Layout.fillWidth: true
                    spacing: 6

                    QuantityField {
                        label: qsTr("%1 — densité %2").arg(root.source.densitySymbol ?? "").arg(root.source.densityName ?? "")
                        unit: root.source.densityUnit ?? "C/m³"
                        value: root.source.density ?? 0
                        onEdited: (v) => root.scene.setSourceValue(root.row, "density", v)
                    }
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: Theme.textMuted
                        font.pixelSize: Theme.smallText
                        text: qsTr("%1 de la forme : %2  →  %3 = %4 × %5")
                              .arg(root.source.measureName ?? "")
                              .arg(Units.format(root.source.measure ?? 0, root.source.measureUnit ?? "m³", 4))
                              .arg(root.hasSource ? root.scene.strengthSymbol(root.source.type) : "")
                              .arg(root.source.densitySymbol ?? "")
                              .arg((root.source.measureName ?? "").toLowerCase())
                    }
                    Switch {
                        Layout.fillWidth: true
                        checked: root.source.keepDensity === true
                        text: qsTr("Garder %1 constante quand les dimensions changent").arg(root.source.densitySymbol ?? "")
                        onToggled: {
                            root.scene.setSourceValue(root.row, "keepDensity", checked)
                            checked = Qt.binding(() => root.source.keepDensity === true)
                        }
                        ToolTip.visible: hovered
                        ToolTip.delay: 600
                        ToolTip.text: checked ? qsTr("Agrandir la forme augmente %1.").arg(root.hasSource ? root.scene.strengthSymbol(root.source.type) : "")
                                              : qsTr("%1 reste fixe : agrandir la forme diminue la densité.").arg(root.hasSource ? root.scene.strengthSymbol(root.source.type) : "")
                        contentItem: Label {
                            leftPadding: parent.indicator.width + parent.spacing
                            text: parent.text
                            wrapMode: Text.WordWrap
                            color: Theme.text
                            font.pixelSize: Theme.smallText
                            verticalAlignment: Text.AlignVCenter
                        }
                    }
                }

                SectionTitle { text: qsTr("Position") }
                GridLayout {
                    columns: 3
                    columnSpacing: 8
                    Layout.fillWidth: true
                    QuantityField { label: "x"; unit: "m"; value: root.source.x ?? 0; onEdited: (v) => root.scene.setSourceValue(root.row, "x", v) }
                    QuantityField { label: "y"; unit: "m"; value: root.source.y ?? 0; onEdited: (v) => root.scene.setSourceValue(root.row, "y", v) }
                    QuantityField { visible: !root.scene.planar; label: "z"; unit: "m"; value: root.source.z ?? 0; onEdited: (v) => root.scene.setSourceValue(root.row, "z", v) }
                }

                SectionTitle {
                    text: root.hasSource ? root.scene.fieldsTitle(root.source.type) : ""
                    visible: dimensions.count > 0
                }
                Repeater {
                    id: dimensions
                    model: root.hasSource ? root.scene.editableFields(root.source.type) : []
                    // A number, or a list when the field has choices (its value is then an index).
                    delegate: ColumnLayout {
                        id: fieldItem
                        required property string modelData
                        readonly property var choices: root.scene.fieldChoices(root.source.type, modelData)
                        Layout.fillWidth: true
                        spacing: 0
                        QuantityField {
                            visible: fieldItem.choices.length === 0
                            label: root.scene.fieldLabel(root.source.type, fieldItem.modelData)
                            unit: root.scene.fieldUnit(root.source.type, fieldItem.modelData)
                            value: root.source[fieldItem.modelData] ?? 0
                            onEdited: (v) => root.scene.setSourceValue(root.row, fieldItem.modelData, v)
                        }
                        ChoiceField {
                            visible: fieldItem.choices.length > 0
                            label: root.scene.fieldLabel(root.source.type, fieldItem.modelData)
                            choices: fieldItem.choices
                            value: root.source[fieldItem.modelData] ?? 0
                            onEdited: (index) => root.scene.setSourceValue(root.row, fieldItem.modelData, index)
                        }
                    }
                }

                // Antennas: the sources placed on this element, entities of their own.
                SectionTitle {
                    text: qsTr("Sources")
                    visible: root.feedable
                }
                Label {
                    visible: root.feedable && root.elementFeeds.length === 0
                    text: qsTr("Aucune : l'élément est passif.")
                    color: Theme.textMuted
                    font.pixelSize: Theme.smallText
                    Layout.fillWidth: true
                }
                Repeater {
                    model: root.feedable ? root.elementFeeds : []
                    delegate: Button {
                        required property var modelData
                        Layout.fillWidth: true
                        flat: true
                        text: qsTr("%1 · %2 · %3").arg(modelData.name).arg(modelData.place).arg(modelData.text)
                        ToolTip.visible: hovered
                        ToolTip.delay: 600
                        ToolTip.text: qsTr("Sélectionner cette source : sa tension, sa phase, sa place")
                        onClicked: root.feedRequested(modelData.row, modelData.index)
                    }
                }
                Button {
                    // A plain wire (type 0) carries any number of sources, a ready-made element its own.
                    visible: root.feedable && (root.elementFeeds.length === 0 || root.source.type === 0)
                    text: qsTr("+  Placer une source")
                    flat: true
                    onClicked: {
                        const placed = root.scene.addFeedTo(root.row)
                        if (placed >= 0)
                            root.feedRequested(root.row, placed)
                    }
                }

                SectionTitle { text: qsTr("Modèle physique") }
                Label {
                    text: root.hasSource ? root.scene.formula(root.source.type) : ""
                    wrapMode: Text.WordWrap
                    color: Theme.text
                    font.pixelSize: Theme.bodyText
                    lineHeight: 1.15
                    Layout.fillWidth: true
                }
                Rectangle {
                    Layout.fillWidth: false
                    implicitWidth: accuracyLabel.implicitWidth + 16
                    implicitHeight: 24
                    radius: 12
                    color: Qt.alpha(Theme.success, 0.15)
                    border.color: Qt.alpha(Theme.success, 0.5)
                    Label {
                        id: accuracyLabel
                        anchors.centerIn: parent
                        text: root.hasSource ? root.scene.accuracy(root.source.type) : ""
                        color: Theme.success
                        font.pixelSize: Theme.smallText
                        font.bold: true
                    }
                }
                RowLayout {
                    Layout.topMargin: 4
                    Button {
                        text: qsTr("Dupliquer")
                        flat: true
                        onClicked: root.rowSelected(root.scene.duplicateSource(root.row))
                    }
                    Button {
                        text: qsTr("Supprimer")
                        flat: true
                        onClicked: {
                            root.scene.removeSource(root.row)
                            root.rowSelected(-1)
                        }
                    }
                }
            }

            // ---- Settings of the scene (simulation parameters) ----
            ColumnLayout {
                visible: settingsFields.count > 0
                Layout.fillWidth: true
                spacing: 10

                SectionTitle { text: root.scene.viewKind === "antenna" ? qsTr("Analyse") : qsTr("Simulation") }
                Repeater {
                    id: settingsFields
                    model: root.scene.settingFields()
                    delegate: ColumnLayout {
                        id: settingItem
                        required property string modelData
                        readonly property var choices: root.scene.settingChoices(modelData)
                        Layout.fillWidth: true
                        spacing: 0
                        QuantityField {
                            visible: settingItem.choices.length === 0
                            label: root.scene.settingLabel(settingItem.modelData)
                            unit: root.scene.settingUnit(settingItem.modelData)
                            value: root.settings[settingItem.modelData] ?? 0
                            onEdited: (v) => root.scene.setSetting(settingItem.modelData, v)
                        }
                        ChoiceField {
                            visible: settingItem.choices.length > 0
                            label: root.scene.settingLabel(settingItem.modelData)
                            choices: settingItem.choices
                            value: root.settings[settingItem.modelData] ?? 0
                            onEdited: (index) => root.scene.setSetting(settingItem.modelData, index)
                        }
                    }
                }
                Label {
                    visible: root.settingsNote.length > 0
                    text: root.settingsNote
                    wrapMode: Text.WordWrap
                    color: Theme.textMuted
                    font.pixelSize: Theme.smallText
                    lineHeight: 1.15
                    Layout.fillWidth: true
                }
            }

            // ---- Measurement points (or the results of the scene, for modules without a point) ----
            SectionTitle {
                text: !root.scene.hasProbe ? qsTr("Résultats")
                                           : root.scene.differentialProbe ? qsTr("Point de mesure A") : qsTr("Point de mesure")
            }
            Label {
                visible: root.scene.hasProbe
                text: qsTr("Cliquez dans la vue pour placer le point.")
                color: Theme.textMuted
                font.pixelSize: Theme.smallText
            }
            GridLayout {
                visible: root.scene.hasProbe
                columns: 3
                columnSpacing: 8
                Layout.fillWidth: true
                QuantityField { label: "x"; unit: "m"; value: root.probe.x; onEdited: (v) => root.probeEdited(Qt.vector3d(v, root.probe.y, root.probe.z)) }
                QuantityField { label: "y"; unit: "m"; value: root.probe.y; onEdited: (v) => root.probeEdited(Qt.vector3d(root.probe.x, v, root.probe.z)) }
                QuantityField { visible: !root.scene.planar; label: "z"; unit: "m"; value: root.probe.z; onEdited: (v) => root.probeEdited(Qt.vector3d(root.probe.x, root.probe.y, v)) }
            }
            GridLayout {
                columns: 2
                columnSpacing: 12
                rowSpacing: 4
                Layout.fillWidth: true
                Repeater {
                    model: root.reading.rows ?? []
                    delegate: RowLayout {
                        required property var modelData
                        Layout.columnSpan: 2
                        Layout.fillWidth: true
                        Label {
                            text: modelData.name
                            textFormat: Text.StyledText
                            color: Theme.textMuted
                            font.pixelSize: Theme.bodyText
                            Layout.preferredWidth: 56
                        }
                        Label {
                            text: isNaN(modelData.value) ? qsTr("non défini") : Units.format(modelData.value, modelData.unit, 5)
                            color: Theme.text
                            font.pixelSize: Theme.bodyText
                            font.bold: modelData.strong
                            font.family: "Consolas"
                            Layout.fillWidth: true
                        }
                    }
                }
            }
            // ---- Point B: differential readings ----
            ColumnLayout {
                visible: root.scene.differentialProbe
                Layout.fillWidth: true
                spacing: 6
                Switch {
                    Layout.fillWidth: true
                    checked: root.probeBEnabled
                    text: qsTr("Point B — mesure différentielle")
                    // A click replaces the binding by a value: restore it, B can also be placed from the view.
                    onToggled: {
                        root.probeBToggled(checked)
                        checked = Qt.binding(() => root.probeBEnabled)
                    }
                    ToolTip.visible: hovered
                    ToolTip.delay: 600
                    ToolTip.text: qsTr("Clic droit dans la vue pour le placer (ou l'outil « Point B » quand la vue a des outils).")
                }
                GridLayout {
                    visible: root.probeBEnabled
                    columns: 2
                    columnSpacing: 8
                    Layout.fillWidth: true
                    QuantityField { label: "x (B)"; unit: "m"; value: root.probeB.x; onEdited: (v) => root.probeBEdited(Qt.vector3d(v, root.probeB.y, root.probeB.z)) }
                    QuantityField { label: "y (B)"; unit: "m"; value: root.probeB.y; onEdited: (v) => root.probeBEdited(Qt.vector3d(root.probeB.x, v, root.probeB.z)) }
                    QuantityField { visible: !root.scene.planar; label: "z (B)"; unit: "m"; value: root.probeB.z; onEdited: (v) => root.probeBEdited(Qt.vector3d(root.probeB.x, root.probeB.y, v)) }
                }
            }
            // Extra readings of the module: point B and the differences.
            Repeater {
                model: root.reading.groups ?? []
                delegate: ColumnLayout {
                    id: group
                    required property var modelData
                    Layout.fillWidth: true
                    spacing: 4
                    Label {
                        text: group.modelData.title
                        color: Theme.accent
                        font.pixelSize: Theme.smallText
                        font.bold: true
                        Layout.topMargin: 4
                    }
                    Repeater {
                        model: group.modelData.rows
                        delegate: RowLayout {
                            required property var modelData
                            Layout.fillWidth: true
                            Label {
                                text: modelData.name
                                color: Theme.textMuted
                                font.pixelSize: Theme.bodyText
                                Layout.preferredWidth: 56
                            }
                            Label {
                                text: isNaN(modelData.value) ? qsTr("non défini") : Units.format(modelData.value, modelData.unit, 5)
                                color: Theme.text
                                font.pixelSize: Theme.bodyText
                                font.bold: modelData.strong
                                font.family: "Consolas"
                                Layout.fillWidth: true
                            }
                        }
                    }
                }
            }
            // Static modules with two points: the quantities along the segment from A to B.
            Repeater {
                model: root.useB ? (root.reading.profiles ?? []) : []
                delegate: ProfileChart {
                    required property var modelData
                    name: modelData.name
                    unit: modelData.unit
                    values: modelData.values
                    length: root.reading.profileLength ?? 0
                }
            }
            // Live modules: recent values at the points.
            TraceChart {
                visible: series.length > 0 && series[0].values.length > 1
                series: root.reading.traces ?? []
                name: root.reading.traceName ?? ""
                unit: root.reading.traceUnit ?? ""
                span: root.reading.traceSpan ?? 0
            }
            Label {
                visible: (root.reading.note ?? "").length > 0
                text: root.reading.note ?? ""
                wrapMode: Text.WordWrap
                color: Theme.textMuted
                font.pixelSize: Theme.smallText
                Layout.fillWidth: true
            }
        }
    }
}
