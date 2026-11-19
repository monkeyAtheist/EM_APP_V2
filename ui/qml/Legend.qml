import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Qsig.App

// Colour bar of the field map, with its quantity, unit and limits.
Rectangle {
    id: root
    required property var view
    required property var scene

    readonly property bool potential: view.quantity === 1
    // Reading quantityNames re-evaluates the texts when the scene changes them (polarisation...).
    readonly property string unit: scene.quantityNames.length > 0 ? scene.quantityUnit(view.quantity) : ""
    readonly property bool smallVariation: view.legendLogarithmic && Math.pow(10, view.legendMax - view.legendMin) < 2
    // Reads 'potential', the scene, the view and its ground (light or dark theme) so that the gradient
    // bindings re-evaluate when they change.
    function colour(t) {
        const dependencies = [root.potential, root.scene, root.view, root.view.lightGround]
        return root.view.colourAt(t)
    }
    function valueAt(t) {
        const x = view.legendMin + t * (view.legendMax - view.legendMin)
        return Units.format(view.legendLogarithmic ? Math.pow(10, x) : x, root.unit, 3)
    }

    implicitWidth: 150
    implicitHeight: (root.potential && root.view.potentialReferenced) || root.smallVariation ? 244 : 210
    radius: Theme.radius
    color: Qt.alpha(Theme.panel, 0.88)
    border.color: Theme.border

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 10
        spacing: 6

        Label {
            text: root.scene.quantityNames.length > 0 ? root.scene.quantityTitle(root.view.quantity) : ""
            font.pixelSize: Theme.smallText
            font.bold: true
            color: Theme.text
        }
        Label {
            // The colour scale adapts to the view: warn when a tiny variation fills the whole palette.
            readonly property real ratio: Math.pow(10, root.view.legendMax - root.view.legendMin)
            visible: root.smallVariation
            text: qsTr("faible variation : ×%1\nsur toute la vue").arg(ratio.toFixed(2).replace(".", ","))
            font.pixelSize: Theme.smallText - 1
            color: Theme.textMuted
        }
        Label {
            visible: root.potential && root.view.potentialReferenced
            text: qsTr("relatif : V = 0 à 1 m\nde l'axe d'une source infinie")
            font.pixelSize: Theme.smallText - 1
            color: Theme.textMuted
        }
        RowLayout {
            Layout.fillHeight: true
            spacing: 8
            Rectangle {
                Layout.fillHeight: true
                width: 14
                radius: 3
                gradient: Gradient {
                    GradientStop { position: 0.0; color: root.colour(1.0) }
                    GradientStop { position: 0.25; color: root.colour(0.75) }
                    GradientStop { position: 0.5; color: root.colour(0.5) }
                    GradientStop { position: 0.75; color: root.colour(0.25) }
                    GradientStop { position: 1.0; color: root.colour(0.0) }
                }
            }
            ColumnLayout {
                Layout.fillHeight: true
                Label { text: root.valueAt(1.0); color: Theme.text; font.pixelSize: Theme.smallText }
                Item { Layout.fillHeight: true }
                Label { text: root.valueAt(0.5); color: Theme.textMuted; font.pixelSize: Theme.smallText }
                Item { Layout.fillHeight: true }
                Label { text: root.valueAt(0.0); color: Theme.text; font.pixelSize: Theme.smallText }
            }
        }
    }
}
