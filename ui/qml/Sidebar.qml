import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Module navigation: one entry per study, the same layout behind each one.
Rectangle {
    id: root
    property int current: 0
    signal moduleSelected(int index)

    implicitWidth: 84
    color: Theme.panel

    readonly property var modules: [
        { glyph: "E", label: qsTr("Électro-\nstatique"), ready: true },
        { glyph: "B", label: qsTr("Magnéto-\nstatique"), ready: true },
        { glyph: "∿", label: qsTr("Ondes\nFDTD 2D"), ready: true },
        { glyph: "Ψ", label: qsTr("Antennes"), ready: true },
        { glyph: "RF", label: qsTr("Outils RF"), ready: false }
    ]

    Rectangle {
        anchors.right: parent.right
        width: 1
        height: parent.height
        color: Theme.border
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.topMargin: 10
        spacing: 4

        Repeater {
            model: root.modules
            delegate: ItemDelegate {
                id: entry
                required property var modelData
                required property int index
                readonly property bool active: index === root.current
                Layout.alignment: Qt.AlignHCenter
                Layout.preferredWidth: 72
                Layout.preferredHeight: 72
                hoverEnabled: true
                onClicked: if (modelData.ready) root.moduleSelected(index)
                ToolTip.visible: hovered && !modelData.ready
                ToolTip.text: qsTr("Arrive dans une prochaine phase de la refonte")

                background: Rectangle {
                    radius: Theme.radius
                    color: entry.active ? Qt.alpha(Theme.accent, 0.16)
                                        : entry.hovered && entry.modelData.ready ? Qt.alpha(Theme.text, 0.06) : "transparent"
                    border.color: entry.active ? Qt.alpha(Theme.accent, 0.5) : "transparent"
                }
                contentItem: ColumnLayout {
                    spacing: 2
                    opacity: entry.modelData.ready ? 1.0 : 0.38
                    Label {
                        Layout.alignment: Qt.AlignHCenter
                        text: entry.modelData.glyph
                        font.pixelSize: 20
                        font.bold: true
                        color: entry.active ? Theme.accent : Theme.text
                    }
                    Label {
                        Layout.alignment: Qt.AlignHCenter
                        horizontalAlignment: Text.AlignHCenter
                        text: entry.modelData.label
                        font.pixelSize: 10
                        lineHeight: 0.9
                        color: entry.active ? Theme.accent : Theme.textMuted
                    }
                }
            }
        }
        Item { Layout.fillHeight: true }
    }
}
