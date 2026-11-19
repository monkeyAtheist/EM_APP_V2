import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qsig.App

// Recent values at the measurement points, like an oscilloscope: newest on the right.
// series: [{name, values}], sampled at the same instants. With two curves, "A − B" shows their difference.
ColumnLayout {
    id: root
    property var series: []
    property string name
    property string unit
    property real span: 0 // duration of the window [s]
    property bool difference: false

    readonly property var colours: [Theme.accent, "#FFA726"]
    readonly property var curves: {
        if (difference && series.length >= 2) {
            const a = series[0].values, b = series[1].values
            const n = Math.min(a.length, b.length)
            const d = []
            for (let i = 0; i < n; ++i)
                d.push(a[i] - b[i])
            return [{ name: series[0].name + " − " + series[1].name, values: d }]
        }
        return series
    }
    readonly property real peak: {
        let m = 0
        for (let c = 0; c < curves.length; ++c)
            for (let i = 0; i < curves[c].values.length; ++i)
                m = Math.max(m, Math.abs(curves[c].values[i]))
        return m
    }

    spacing: 2
    Layout.fillWidth: true
    onCurvesChanged: canvas.requestPaint()
    Connections {
        target: Theme
        function onDarkChanged() { canvas.requestPaint() }
    }

    Canvas {
        id: canvas
        Layout.fillWidth: true
        Layout.preferredHeight: 120
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
            ctx.beginPath()
            ctx.moveTo(0, h / 2)
            ctx.lineTo(w, h / 2)
            ctx.stroke()
            if (!(root.peak > 0))
                return
            const scale = 0.45 * h / root.peak
            for (let c = 0; c < root.curves.length; ++c) {
                const values = root.curves[c].values
                const n = values.length
                if (n < 2)
                    continue
                ctx.strokeStyle = String(root.difference ? Theme.success : root.colours[c % 2])
                ctx.lineWidth = 1.6
                ctx.beginPath()
                for (let i = 0; i < n; ++i) {
                    const x = i * (w - 1) / (n - 1)
                    const y = h / 2 - values[i] * scale
                    if (i === 0)
                        ctx.moveTo(x, y)
                    else
                        ctx.lineTo(x, y)
                }
                ctx.stroke()
            }
        }
    }
    RowLayout {
        Layout.fillWidth: true
        spacing: 10
        Repeater {
            model: root.curves.length > 1 ? root.curves : []
            delegate: RowLayout {
                required property var modelData
                required property int index
                spacing: 4
                Rectangle { width: 14; height: 3; radius: 1; color: root.colours[index % 2] }
                Label { text: modelData.name; color: Theme.textMuted; font.pixelSize: Theme.smallText }
            }
        }
        Label {
            text: qsTr("crête ±%1").arg(Units.format(root.peak, root.unit, 3))
            color: Theme.textMuted
            font.pixelSize: Theme.smallText
            Layout.fillWidth: true
        }
        Label {
            text: qsTr("sur %1").arg(Units.format(root.span, "s", 3))
            color: Theme.textMuted
            font.pixelSize: Theme.smallText
        }
    }
    CheckBox {
        visible: root.series.length >= 2
        text: qsTr("Afficher A − B")
        checked: root.difference
        onToggled: root.difference = checked
    }
}
