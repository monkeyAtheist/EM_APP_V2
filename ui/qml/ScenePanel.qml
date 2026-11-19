import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Left panel: the sources of the scene (a flat list; for the antennas a tree by nature), add menu and examples.
Rectangle {
    id: root
    required property var scene
    property int selectedRow: -1
    signal rowSelected(int row)
    // 'plane': the cutting plane where the example reads best (0 XY, 1 XZ, 2 YZ).
    signal exampleLoaded(int plane)
    signal collapseRequested()
    // Antennas: the sources are entities placed on the wires, listed in their own table. The one
    // selected in the view (an entry of the scene's feedList, an empty object when none).
    property var selectedFeed: ({})
    signal feedSelected(int row, int index)

    function openExample(example) {
        scene.loadExample(example.key)
        rowSelected(-1)
        exampleLoaded(example.plane ?? 0)
    }

    color: Theme.panel

    // Antennas: the scene is a tree by nature. Wires, surfaces and solids are the shapes; the sources are
    // entities placed on the wires; the media are what the shapes are made of; the groups gather elements.
    readonly property bool antenna: root.scene.viewKind === "antenna"
    signal groupSelected(string name)
    property bool openWires: true
    property bool openSurfaces: true
    property bool openSolids: true
    property bool openSources: true
    property bool openMedia: true
    property bool openGroups: true
    // What each row of the scene is (a surface: a plate, a disc or the skin of a solid; a solid) and the
    // group it belongs to; the totals; the names
    // of the groups and the sources, replaced only when they change so that their lines are not rebuilt
    // at every step of a drag.
    property var rowSurface: []
    property var rowSolid: []
    property var rowGroup: []
    property int wireTotal: 0
    property int surfaceTotal: 0
    property int solidTotal: 0
    property var groupNames: []
    property var feedEntries: []
    property var mediaEntries: []

    function refreshTree() {
        // Asked of the scene itself: when the module changes, 'antenna' may not be up to date yet.
        if (scene.viewKind !== "antenna") {
            rowSurface = []
            rowSolid = []
            rowGroup = []
            wireTotal = 0
            surfaceTotal = 0
            solidTotal = 0
            if (groupNames.length > 0)
                groupNames = []
            if (feedEntries.length > 0)
                feedEntries = []
            if (mediaEntries.length > 0)
                mediaEntries = []
            return
        }
        const surfaces = []
        const solids = []
        const groups = []
        const names = []
        let wires = 0
        let bodies = 0
        for (let i = 0; i < scene.count; ++i) {
            const surface = scene.isSurface(i)
            const solid = scene.isSolid(i)
            const group = scene.groupOf(i)
            surfaces.push(surface)
            solids.push(solid)
            groups.push(group)
            if (!surface)
                ++wires
            if (solid)
                ++bodies
            if (group !== "" && names.indexOf(group) < 0)
                names.push(group)
        }
        rowSurface = surfaces
        rowSolid = solids
        rowGroup = groups
        wireTotal = wires
        solidTotal = bodies
        surfaceTotal = scene.count - wires - bodies
        if (names.join("\n") !== groupNames.join("\n"))
            groupNames = names
        const feeds = scene.feedList
        if (JSON.stringify(feeds) !== JSON.stringify(feedEntries))
            feedEntries = feeds
        const media = scene.mediaList
        if (JSON.stringify(media) !== JSON.stringify(mediaEntries))
            mediaEntries = media
    }
    function groupSize(name) {
        let size = 0
        for (let i = 0; i < rowGroup.length; ++i)
            if (rowGroup[i] === name)
                ++size
        return size
    }

    onSceneChanged: refreshTree()
    onAntennaChanged: refreshTree()
    Component.onCompleted: refreshTree()
    Connections {
        target: root.scene
        function onSceneChanged() { root.refreshTree() }
        function onCountChanged() { root.refreshTree() }
    }

    // Head of a branch of the tree: folds and unfolds it.
    component Branch: ItemDelegate {
        id: branch
        property string title: ""
        property int total: 0
        property bool open: true
        height: 30
        leftPadding: 6
        rightPadding: 8
        topPadding: 0
        bottomPadding: 0
        background: Rectangle {
            radius: Theme.radius
            color: branch.hovered ? Qt.alpha(Theme.text, 0.09) : Qt.alpha(Theme.text, 0.04)
        }
        contentItem: RowLayout {
            spacing: 7
            RibbonIcon {
                Layout.preferredWidth: 9
                Layout.preferredHeight: 9
                name: "caret"
                tint: Theme.textMuted
                rotation: branch.open ? 0 : -90
            }
            Label {
                text: branch.title
                color: Theme.text
                font.pixelSize: Theme.smallText + 1
                font.bold: true
                Layout.fillWidth: true
            }
            Label {
                text: String(branch.total)
                color: Theme.textMuted
                font.pixelSize: Theme.smallText
                font.family: "Consolas"
            }
        }
    }

    // A line of the tree: a name, what it is under it, a value at the right. 'shown': in an open branch
    // it belongs to. 'dot': the colour of its mark.
    component Leaf: ItemDelegate {
        id: leaf
        property string title: ""
        property string detail: ""
        property string value: ""
        property bool shown: true
        property bool chosen: false
        property color dot: Theme.textMuted
        property real inset: 14
        visible: shown
        height: shown ? 46 : 0
        leftPadding: 8 + inset
        rightPadding: 8
        topPadding: 0
        bottomPadding: 0
        highlighted: chosen
        background: Rectangle {
            radius: Theme.radius
            color: leaf.highlighted ? Qt.alpha(Theme.accent, 0.16) : leaf.hovered ? Qt.alpha(Theme.text, 0.05) : "transparent"
            border.color: leaf.highlighted ? Qt.alpha(Theme.accent, 0.45) : "transparent"
        }
        contentItem: RowLayout {
            spacing: 9
            Rectangle {
                Layout.preferredWidth: 10
                Layout.preferredHeight: 10
                radius: 5
                color: leaf.dot
            }
            ColumnLayout {
                spacing: 0
                Layout.fillWidth: true
                Label {
                    text: leaf.title
                    color: Theme.text
                    font.pixelSize: Theme.bodyText
                    font.bold: leaf.highlighted
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                }
                Label {
                    visible: leaf.detail !== ""
                    text: leaf.detail
                    color: Theme.textMuted
                    font.pixelSize: Theme.smallText
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                }
            }
            Label {
                text: leaf.value
                color: Theme.text
                font.pixelSize: Theme.smallText
                font.family: "Consolas"
            }
        }
    }

    // A word under the head of an empty branch.
    component Hint: Label {
        leftPadding: 22
        rightPadding: 8
        topPadding: 2
        bottomPadding: 6
        wrapMode: Text.WordWrap
        color: Theme.textMuted
        font.pixelSize: Theme.smallText
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.gap
        spacing: 10

        RowLayout {
            Layout.fillWidth: true
            Label {
                text: qsTr("Scène")
                font.pixelSize: Theme.titleText
                font.bold: true
                color: Theme.text
            }
            Label {
                text: root.antenna ? (root.scene.count === 1 ? qsTr("1 élément") : qsTr("%1 éléments").arg(root.scene.count))
                                     + (root.scene.feedTotal === 1 ? qsTr(" · 1 source") : qsTr(" · %1 sources").arg(root.scene.feedTotal))
                      : root.scene.live ? (root.scene.count === 1 ? qsTr("1 élément") : qsTr("%1 éléments").arg(root.scene.count))
                                      : (root.scene.count === 1 ? qsTr("1 source") : qsTr("%1 sources").arg(root.scene.count))
                color: Theme.textMuted
                font.pixelSize: Theme.smallText
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignRight
            }
            FoldButton {
                pointsLeft: true
                toolTip: qsTr("Masquer la scène (F9)")
                onClicked: root.collapseRequested()
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 6
            Button {
                text: qsTr("+  Ajouter")
                highlighted: true
                Layout.fillWidth: true
                onClicked: addMenu.popup()
                Menu {
                    id: addMenu
                    Instantiator {
                        model: root.scene.typeNames()
                        delegate: MenuItem {
                            required property string modelData
                            required property int index
                            text: modelData
                            onTriggered: root.rowSelected(root.scene.addSource(index))
                        }
                        onObjectAdded: (index, object) => addMenu.insertItem(index, object)
                        onObjectRemoved: (index, object) => addMenu.removeItem(object)
                    }
                }
            }
            Button {
                text: qsTr("Exemples")
                flat: true
                onClicked: examplesMenu.popup()
                Menu {
                    id: examplesMenu
                    Instantiator {
                        model: root.scene.examples()
                        delegate: MenuItem {
                            required property var modelData
                            text: modelData.name
                            onTriggered: root.openExample(modelData)
                        }
                        onObjectAdded: (index, object) => examplesMenu.insertItem(index, object)
                        onObjectRemoved: (index, object) => examplesMenu.removeItem(object)
                    }
                }
            }
        }

        // Antennas: the tree of the scene by nature.
        Flickable {
            id: tree
            visible: root.antenna
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            contentWidth: width
            contentHeight: treeColumn.height
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}

            Column {
                id: treeColumn
                width: tree.width
                spacing: 2

                // ---- Wires: the curves that carry currents, and the sources ----
                Branch {
                    width: treeColumn.width
                    title: qsTr("Fils")
                    total: root.wireTotal
                    open: root.openWires
                    onClicked: root.openWires = !root.openWires
                }
                Hint {
                    width: treeColumn.width
                    visible: root.openWires && root.wireTotal === 0
                    text: qsTr("Aucun. Tracez-en un avec l'outil Fil, ou prenez un dipôle, une boucle, une hélice dans le ruban.")
                }
                Repeater {
                    model: root.antenna ? root.scene : null
                    delegate: Leaf {
                        required property int index
                        required property string name
                        required property string typeName
                        required property string strengthText
                        width: treeColumn.width
                        shown: root.openWires && root.rowSurface[index] === false
                        chosen: index === root.selectedRow
                        title: name
                        detail: typeName
                        value: strengthText
                        dot: Theme.positive
                        onClicked: root.rowSelected(index)
                    }
                }

                // ---- Surfaces ----
                Branch {
                    width: treeColumn.width
                    title: qsTr("Surfaces")
                    total: root.surfaceTotal
                    open: root.openSurfaces
                    onClicked: root.openSurfaces = !root.openSurfaces
                }
                Hint {
                    width: treeColumn.width
                    visible: root.openSurfaces && root.surfaceTotal === 0
                    text: qsTr("Aucune. Plaque et disque sont dans le ruban.")
                }
                Repeater {
                    model: root.antenna ? root.scene : null
                    delegate: Leaf {
                        required property int index
                        required property string name
                        required property string typeName
                        required property string strengthText
                        width: treeColumn.width
                        shown: root.openSurfaces && root.rowSurface[index] === true && root.rowSolid[index] === false
                        chosen: index === root.selectedRow
                        title: name
                        detail: typeName
                        value: strengthText
                        dot: Theme.accent
                        onClicked: root.rowSelected(index)
                    }
                }

                // ---- Solids: closed conducting skins ----
                Branch {
                    width: treeColumn.width
                    title: qsTr("Solides")
                    total: root.solidTotal
                    open: root.openSolids
                    onClicked: root.openSolids = !root.openSolids
                }
                Hint {
                    width: treeColumn.width
                    visible: root.openSolids && root.solidTotal === 0
                    text: qsTr("Aucun. Pavé, cylindre, cône et sphère sont dans le ruban.")
                }
                Repeater {
                    model: root.antenna ? root.scene : null
                    delegate: Leaf {
                        required property int index
                        required property string name
                        required property string typeName
                        required property string strengthText
                        width: treeColumn.width
                        shown: root.openSolids && root.rowSolid[index] === true
                        chosen: index === root.selectedRow
                        title: name
                        detail: typeName
                        value: strengthText
                        dot: Theme.text
                        onClicked: root.rowSelected(index)
                    }
                }

                // ---- Sources: entities placed on the wires ----
                Branch {
                    width: treeColumn.width
                    title: qsTr("Sources")
                    total: root.feedEntries.length
                    open: root.openSources
                    onClicked: root.openSources = !root.openSources
                }
                Hint {
                    width: treeColumn.width
                    visible: root.openSources && root.feedEntries.length === 0
                    text: qsTr("Aucune. Outil Source du ruban : cliquez sur un fil, à l'endroit de la source.")
                }
                Repeater {
                    model: root.openSources ? root.feedEntries : []
                    delegate: Leaf {
                        required property var modelData
                        width: treeColumn.width
                        chosen: root.selectedFeed.row === modelData.row && root.selectedFeed.index === modelData.index
                        title: modelData.name
                        detail: qsTr("sur %1, %2").arg(modelData.host).arg(modelData.place)
                        value: modelData.text
                        dot: "#ff5a5a"
                        onClicked: root.feedSelected(modelData.row, modelData.index)
                    }
                }

                // ---- Media: what the shapes are made of (air, the perfect conductor, each dielectric in use) ----
                Branch {
                    width: treeColumn.width
                    title: qsTr("Milieux")
                    total: root.mediaEntries.length
                    open: root.openMedia
                    onClicked: root.openMedia = !root.openMedia
                }
                Repeater {
                    model: root.openMedia ? root.mediaEntries : []
                    delegate: Leaf {
                        required property var modelData
                        width: treeColumn.width
                        hoverEnabled: false
                        title: modelData.name
                        detail: modelData.detail
                        dot: modelData.kind === 2 ? Theme.accent : modelData.kind === 1 ? Theme.text : Theme.textMuted
                    }
                }
                Hint {
                    width: treeColumn.width
                    visible: root.openMedia
                    text: qsTr("Le milieu d'un solide se choisit dans ses propriétés : conducteur parfait, ou diélectrique (εr, tan δ).")
                }

                // ---- Groups: a click on a group selects its elements ----
                Branch {
                    width: treeColumn.width
                    title: qsTr("Groupes")
                    total: root.groupNames.length
                    open: root.openGroups
                    onClicked: root.openGroups = !root.openGroups
                }
                Hint {
                    width: treeColumn.width
                    visible: root.openGroups && root.groupNames.length === 0
                    text: qsTr("Aucun. Sélectionnez plusieurs éléments (Ctrl + clic), puis Grouper.")
                }
                Repeater {
                    model: root.openGroups ? root.groupNames : []
                    delegate: Column {
                        id: groupBlock
                        required property string modelData
                        width: treeColumn.width
                        spacing: 2
                        Leaf {
                            width: groupBlock.width
                            title: groupBlock.modelData
                            detail: qsTr("%1 éléments : un clic les sélectionne tous").arg(root.groupSize(groupBlock.modelData))
                            dot: Theme.accent
                            onClicked: root.groupSelected(groupBlock.modelData)
                        }
                        Repeater {
                            model: root.antenna ? root.scene : null
                            delegate: Leaf {
                                required property int index
                                required property string name
                                required property string typeName
                                width: groupBlock.width
                                inset: 30
                                shown: root.rowGroup[index] === groupBlock.modelData
                                chosen: index === root.selectedRow
                                title: name
                                detail: typeName
                                dot: root.rowSolid[index] === true ? Theme.text : root.rowSurface[index] === true ? Theme.accent : Theme.positive
                                onClicked: root.rowSelected(index)
                            }
                        }
                    }
                }
            }
        }

        // The other modules: the flat list of the sources of the scene.
        ListView {
            id: list
            visible: !root.antenna
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 2
            model: root.antenna ? null : root.scene
            currentIndex: root.selectedRow
            ScrollBar.vertical: ScrollBar {}

            delegate: ItemDelegate {
                id: row
                required property int index
                required property string name
                required property string typeName
                required property string strengthText
                required property real strength
                width: ListView.view.width
                height: 54
                highlighted: index === root.selectedRow
                onClicked: root.rowSelected(index)

                background: Rectangle {
                    radius: Theme.radius
                    color: row.highlighted ? Qt.alpha(Theme.accent, 0.16) : row.hovered ? Qt.alpha(Theme.text, 0.05) : "transparent"
                    border.color: row.highlighted ? Qt.alpha(Theme.accent, 0.45) : "transparent"
                }
                contentItem: RowLayout {
                    spacing: 10
                    Rectangle {
                        width: 12
                        height: 12
                        radius: 6
                        color: row.strength > 0 ? Theme.positive : row.strength < 0 ? Theme.negative : Theme.textMuted
                    }
                    ColumnLayout {
                        spacing: 1
                        Layout.fillWidth: true
                        Label {
                            text: row.name
                            color: Theme.text
                            font.pixelSize: Theme.bodyText
                            font.bold: row.highlighted
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                        }
                        Label {
                            text: row.typeName
                            color: Theme.textMuted
                            font.pixelSize: Theme.smallText
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                        }
                    }
                    Label {
                        text: row.strengthText
                        color: Theme.text
                        font.pixelSize: Theme.smallText
                        font.family: "Consolas"
                    }
                }
            }

            Label {
                anchors.centerIn: parent
                width: parent.width - 20
                visible: list.count === 0
                wrapMode: Text.WordWrap
                horizontalAlignment: Text.AlignHCenter
                color: Theme.textMuted
                text: qsTr("Aucune source.\nAjoutez-en une, ou chargez un exemple.")
            }
        }
    }

    Rectangle {
        anchors.right: parent.right
        width: 1
        height: parent.height
        color: Theme.border
    }
}
