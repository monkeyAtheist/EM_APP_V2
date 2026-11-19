import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// A panel floating over a view. Its title bar drags it (the pointer shows a hand), folds it to that
// bar ("–", or a double click) and closes it ("×"); whoever owns it offers a way to open it again.
// The position is kept as margins from the top edge and from the left edge (or the right one, with
// 'alignRight'), so that the panel follows its edge when the window is resized.
Rectangle {
    id: root
    property string title
    property bool open: true
    property bool folded: false
    property bool alignRight: false
    property real marginX: 12
    property real marginY: 4
    // Room kept free along the edge the panel is aligned on (a tab of a folded side panel).
    property real keepOut: 0
    // Height of the content, set by the owner (its layout's implicit height and margins).
    property real bodyHeight: 0
    default property alias content: body.data
    readonly property real headerHeight: 28
    // The panel was pressed: the owner brings it to the front.
    signal activated()

    readonly property real availableWidth: parent ? parent.width : width
    readonly property real availableHeight: parent ? parent.height : height
    readonly property real offsetX: Math.max(marginX, keepOut)

    visible: open
    x: alignRight ? Math.max(0, availableWidth - width - offsetX) : Math.max(0, Math.min(offsetX, availableWidth - width))
    y: Math.max(0, Math.min(marginY, availableHeight - headerHeight))
    height: headerHeight + (folded ? 0 : bodyHeight)
    radius: Theme.radius
    color: Qt.alpha(Theme.panel, 0.94)
    border.color: Theme.border
    clip: true

    component HeaderButton: Rectangle {
        id: button
        property string glyph
        property string tip
        signal clicked()
        width: 22
        height: 22
        radius: 4
        color: buttonArea.containsMouse ? Qt.alpha(Theme.text, 0.14) : "transparent"
        Label {
            anchors.centerIn: parent
            text: button.glyph
            color: Theme.textMuted
            font.pixelSize: 15
        }
        MouseArea {
            id: buttonArea
            anchors.fill: parent
            hoverEnabled: true
            onClicked: button.clicked()
        }
        ToolTip.visible: buttonArea.containsMouse && button.tip.length > 0
        ToolTip.delay: 600
        ToolTip.text: button.tip
    }

    // Nothing under the panel receives the clicks that fall between its controls.
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        onPressed: root.activated()
        onWheel: (wheel) => wheel.accepted = true
    }

    Item {
        id: header
        width: parent.width
        height: root.headerHeight
        MouseArea {
            id: dragArea
            anchors.fill: parent
            cursorShape: pressed ? Qt.ClosedHandCursor : Qt.OpenHandCursor
            property real pressX: 0
            property real pressY: 0
            property real startX: 0
            property real startY: 0
            onPressed: (mouse) => {
                root.activated()
                const at = mapToItem(root.parent, mouse.x, mouse.y)
                pressX = at.x
                pressY = at.y
                startX = root.x
                startY = root.y
            }
            onPositionChanged: (mouse) => {
                if (!pressed)
                    return
                const at = mapToItem(root.parent, mouse.x, mouse.y)
                const left = Math.max(0, Math.min(root.availableWidth - root.width, startX + at.x - pressX))
                const top = Math.max(0, Math.min(root.availableHeight - root.headerHeight, startY + at.y - pressY))
                root.marginX = root.alignRight ? root.availableWidth - root.width - left : left
                root.marginY = top
            }
            onDoubleClicked: root.folded = !root.folded
        }
        Label {
            x: 10
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width - 66
            elide: Text.ElideRight
            text: root.title
            color: Theme.textMuted
            font.pixelSize: Theme.smallText
            font.bold: true
        }
        Row {
            anchors.right: parent.right
            anchors.rightMargin: 4
            anchors.verticalCenter: parent.verticalCenter
            spacing: 2
            HeaderButton {
                glyph: root.folded ? "+" : "–"
                tip: root.folded ? qsTr("Déplier") : qsTr("Replier sur la barre de titre")
                onClicked: root.folded = !root.folded
            }
            HeaderButton {
                glyph: "×"
                tip: qsTr("Fermer (bouton Fenêtres pour la rouvrir)")
                onClicked: root.open = false
            }
        }
    }

    Item {
        id: body
        y: root.headerHeight
        width: parent.width
        height: root.bodyHeight
        visible: !root.folded
    }
}
