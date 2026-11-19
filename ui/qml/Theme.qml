pragma Singleton
import QtQuick

// Design tokens shared by every view (canvas colours included, so the field view follows the theme).
QtObject {
    property bool dark: true

    readonly property color accent: dark ? "#5AAAFF" : "#0B6BD3"
    readonly property color window: dark ? "#14161A" : "#F3F5F8"
    readonly property color panel: dark ? "#1B1E23" : "#FFFFFF"
    readonly property color panelAlt: dark ? "#22262C" : "#F0F2F5"
    readonly property color border: dark ? "#2C3139" : "#DDE1E7"
    readonly property color text: dark ? "#E6E8EC" : "#1E2227"
    readonly property color textMuted: dark ? "#9AA1AC" : "#5F6672"
    readonly property color canvas: dark ? "#0F1114" : "#FAFBFC"
    readonly property color grid: dark ? Qt.rgba(1, 1, 1, 0.09) : Qt.rgba(0, 0, 0, 0.10)
    readonly property color positive: "#FF6060"
    readonly property color negative: "#5AA0FF"
    readonly property color error: "#FF6B6B"
    readonly property color success: "#4CC38A"

    readonly property int radius: 8
    readonly property int gap: 12
    readonly property int smallText: 11
    readonly property int bodyText: 13
    readonly property int titleText: 15
}
