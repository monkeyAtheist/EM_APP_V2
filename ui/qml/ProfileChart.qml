import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qsig.App

// A quantity along the straight path from the measurement point A (left) to B (right).
// values: samples evenly spaced on the path; NaN where the quantity is not defined (on a source).
ColumnLayout {
    id: root
    property string name
    property string unit
    property var values: []
    property real length: 0 // of the path [m]

    readonly property var range: {
        let lo = Infinity, hi = -Infinity
        for (let i = 0; i < values.length; ++i) {
            const v = values[i]
            if (isFinite(v)) {
                lo = Math.min(lo, v)
                hi = Math.max(hi, v)
            }
        }
        return lo <= hi ? { min: lo, max: hi } : { min: 0, max: 0 }
    }

    spacing: 2
    Layout.fillWidth: true
    onValuesChanged: canvas.requestPaint()
    Connections {
        target: Theme
        function onDarkChanged() { canvas.requestPaint() }
    }

    Label {
        text: qsTr("%1 de A à B").arg(root.name)
        color: Theme.accent
        font.pixelSize: Theme.smallText
        font.bold: true
        Layout.topMargin: 4
    }
    Canvas {
        id: canvas
        Layout.fillWidth: true
        Layout.preferredHeight: 110
        onWidthChanged: requestPaint()
        onPaint: {
            const ctx = getContext("2d")
            ctx.reset()
            const w = width, h = height
            ctx.fillStyle = String(Theme.canvas)
            ctx.fillRect(0, 0, w, h)
            ctx.strokeStyle = String(Theme.border)
            ctx.lineWidth = 1
            ctx.strokeRect(0.5, 0.5, w - 1, h - 1)
            const n = root.values.length
            if (n < 2)
                return
            // A flat curve is drawn at mid-height; otherwise the extremes touch the margins.
            let lo = root.range.min, hi = root.range.max
            if (!(hi > lo)) {
                lo -= 1
                hi += 1
            }
            const top = 8, bottom = h - 8
            const yOf = (v) => bottom - (v - lo) / (hi - lo) * (bottom - top)
            // Zero line when the quantity changes sign.
            if (lo < 0 && hi > 0) {
                ctx.beginPath()
                ctx.moveTo(0, yOf(0))
                ctx.lineTo(w, yOf(0))
                ctx.stroke()
            }
            ctx.strokeStyle = String(Theme.accent)
            ctx.lineWidth = 1.6
            ctx.beginPath()
            let drawing = false
            for (let i = 0; i < n; ++i) {
                const v = root.values[i]
                if (!isFinite(v)) {
                    drawing = false
                    continue
                }
                const x = i * (w - 1) / (n - 1)
                if (drawing)
                    ctx.lineTo(x, yOf(v))
                else
                    ctx.moveTo(x, yOf(v))
                drawing = true
            }
            ctx.stroke()
        }
    }
    RowLayout {
        Layout.fillWidth: true
        spacing: 8
        Label { text: "A"; color: Theme.accent; font.pixelSize: Theme.smallText; font.bold: true }
        Label {
            text: qsTr("min %1 · max %2").arg(Units.format(root.range.min, root.unit, 3)).arg(Units.format(root.range.max, root.unit, 3))
            color: Theme.textMuted
            font.pixelSize: Theme.smallText
            horizontalAlignment: Text.AlignHCenter
            elide: Text.ElideRight
            Layout.fillWidth: true
        }
        Label {
            text: Units.format(root.length, "m", 3)
            color: Theme.textMuted
            font.pixelSize: Theme.smallText
        }
        Label { text: "B"; color: "#FFA726"; font.pixelSize: Theme.smallText; font.bold: true }
    }
}
