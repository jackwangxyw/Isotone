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

    // A shortcut, not a key handler: a field being typed in keeps Delete for its text.
    Shortcut {
        sequence: StandardKey.Delete
        onActivated: EqSession.deleteBand(EqSession.selectedRow)
    }

    Row {
        id: content
        anchors.fill: parent

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
                onMenuRequested: (row, x, above, below) => bandMenu.openAt(row, x, above, below)
            }
            BandStrip {
                width: parent.width
                height: parent.height - 76 - 422
                onMenuRequested: (row, x, above, below) => bandMenu.openAt(row, x, above, below)
            }
        }
    }

    BandMenu {
        id: bandMenu
        anchors.fill: parent
        z: 900
    }

    // A press anywhere else ends typing in a field. Passes every press on.
    MouseArea {
        id: pressWatch
        anchors.fill: parent
        z: 1000
        acceptedButtons: Qt.AllButtons
        onPressed: (mouse) => {
            const f = window.activeFocusItem
            if (f && !f.contains(f.mapFromItem(pressWatch, mouse.x, mouse.y))) content.forceActiveFocus()
            mouse.accepted = false
        }
    }
}
