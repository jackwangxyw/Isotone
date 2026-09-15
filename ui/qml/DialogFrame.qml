import QtQuick
import Isotone

// A modal dialog: the scrim over the window, and the prototype's .dlg card
// (radius 16, padding 24/24/20) with a title, content and a right-aligned
// action row. Escape and a press on the scrim close it (rejected()).
//
// Open one with UiState.openDialog(component, properties); the dialog calls
// close() when done. Content goes in the default property; buttons in `actions`.
Item {
    id: root
    property string title
    property real cardWidth: 640
    default property alias content: body.data
    property alias actions: actionRow.data
    property bool closeOnScrim: true
    signal rejected()
    signal closed()

    anchors.fill: parent
    z: 1000

    function close() {
        root.closed()
        root.destroy()
    }

    Component.onCompleted: card.forceActiveFocus()

    Rectangle {
        anchors.fill: parent
        color: Theme.scrim
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
            onPressed: if (root.closeOnScrim) { root.rejected(); root.close() }
            onWheel: (wheel) => wheel.accepted = true
        }
    }

    Rectangle {
        id: card
        objectName: "dialogCard"
        anchors.centerIn: parent
        width: root.cardWidth
        height: column.implicitHeight + 44
        radius: 16
        color: Theme.pop
        border.color: Theme.border
        Keys.onEscapePressed: { root.rejected(); root.close() }

        MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons }

        Column {
            id: column
            x: 24
            y: 24
            width: parent.width - 48
            spacing: 0
            Text {
                visible: root.title !== ""
                width: parent.width
                text: root.title
                wrapMode: Text.Wrap
                font.family: Theme.font
                font.pixelSize: 18
                font.weight: Font.DemiBold
                font.letterSpacing: -0.18
                color: Theme.text
            }
            Column {
                id: body
                width: parent.width
                spacing: 0
            }
            Item { width: 1; height: actionRow.children.length > 0 ? 22 : 0 }
            Row {
                id: actionRow
                anchors.right: parent.right
                spacing: 8
            }
        }
    }
}
