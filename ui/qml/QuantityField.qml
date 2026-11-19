import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import Qsig.App

// Text field for a physical quantity: shows "1,5 nC", accepts "2 nC", "10 cm", "3,5e-9 C" or a bare
// number (read in the unit currently shown). Emits edited(si) only for a valid, changed value.
ColumnLayout {
    id: root
    property string label
    property real value: 0
    property string unit: "m"
    property bool readOnly: false
    signal edited(real value)

    spacing: 2
    Layout.fillWidth: true

    Label {
        text: root.label
        color: Theme.textMuted
        font.pixelSize: Theme.smallText
        visible: root.label.length > 0
    }
    TextField {
        id: field
        property bool invalid: false
        Layout.fillWidth: true
        readOnly: root.readOnly
        selectByMouse: true
        font.pixelSize: Theme.bodyText
        text: Units.format(root.value, root.unit, 5)
        Material.accent: invalid ? Theme.error : Theme.accent
        ToolTip.delay: 600
        ToolTip.visible: hovered && !invalid && !root.readOnly
        ToolTip.text: qsTr("Exemples : 2 nC, 10 cm, 3,5e-9 C. Un nombre seul est lu en %1.")
                      .arg(Units.preferredUnit(root.value, root.unit))

        function restore() {
            text = Qt.binding(function() { return Units.format(root.value, root.unit, 5) })
        }
        // Show the beginning of the value (its sign) when the field is too narrow for all of it.
        onTextChanged: if (!activeFocus) cursorPosition = 0
        onActiveFocusChanged: if (!activeFocus) cursorPosition = 0
        onEditingFinished: {
            if (readOnly)
                return
            const r = Units.parse(text, root.unit, Units.preferredUnit(root.value, root.unit))
            if (r.ok) {
                invalid = false
                if (r.value !== root.value)
                    root.edited(r.value)
                restore()
            } else {
                invalid = true
                errorTip.text = r.error
                errorTip.open()
            }
        }
        Keys.onEscapePressed: {
            invalid = false
            restore()
            focus = false
        }
        ToolTip {
            id: errorTip
            timeout: 3500
            y: field.height + 2
        }
    }
}
