import QtQuick
import Isotone

// The Equalizer view at the 1440 x 900 reference size (docs/design/screens/Main.png).
Window {
    id: window
    width: 1440
    height: 900
    minimumWidth: 1120
    minimumHeight: 760
    visible: true
    title: "Isotone"
    color: Theme.background

    property bool spectrumOn: true

    Connections {
        target: Outputs
        function onCurrentChanged() { EqSession.useOutput(Outputs) }
    }
    Component.onCompleted: EqSession.useOutput(Outputs)

    Row {
        anchors.fill: parent
        focus: true
        Keys.onDeletePressed: EqSession.deleteBand(EqSession.selectedRow)

        Sidebar {
            id: sidebar
            height: parent.height
        }

        Column {
            width: window.width - sidebar.width
            height: parent.height

            TopBar {
                width: parent.width
                spectrumOn: window.spectrumOn
                onSpectrumPicked: (on) => window.spectrumOn = on
            }
            GraphCard {
                x: 32
                width: parent.width - 64
                spectrumOn: window.spectrumOn
            }
            BandStrip {
                width: parent.width
                height: parent.height - 76 - 422
            }
        }
    }
}
