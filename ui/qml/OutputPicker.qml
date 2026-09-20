import QtQuick
import Isotone

// The sidebar's output control: the current output, and the rest on demand.
//
// It was a plain list of every working output. One row is enough until you want
// to change it (owner, 2026-09-20), so this is shut by default and discloses
// the others when asked.
//
// It expands in place rather than floating a popover over the app: the foot it
// sits in is anchored to the bottom of the sidebar, so a taller child moves the
// top edge upward and the divider and Settings below it do not move at all.
// Both states draw OutputList's row, so the shut state cannot drift from the
// open one.
Item {
    id: root
    property bool open: false
    // One output is nothing to choose between, and none is nothing to show, so
    // the control is a label in both cases: no caret, and clicking does nothing.
    property var source: Outputs
    readonly property bool choosable: source.count > 1

    implicitHeight: open ? openBox.height : shutBox.height
    onChoosableChanged: if (!choosable) open = false

    // Shut: the current output, and the caret that opens the rest.
    Rectangle {
        id: shutBox
        width: parent.width
        height: 54
        visible: !root.open
        radius: 9
        color: Theme.surface
        border.width: 1
        border.color: Theme.border

        OutputList {
            id: shutRow
            width: parent.width - 26
            y: 1
            source: root.source
            onlyCurrent: true
        }
        Icon {
            name: "chevron"
            size: 15
            visible: root.choosable
            x: parent.width - 21
            anchors.verticalCenter: parent.verticalCenter
        }
        MouseArea {
            anchors.fill: parent
            // The row underneath would otherwise take the press and re-select
            // the output that is already current.
            propagateComposedEvents: false
            enabled: root.choosable
            cursorShape: Qt.PointingHandCursor
            onClicked: root.open = true
        }
    }

    // Open: every working output, the current one marked as the list marks it.
    Rectangle {
        id: openBox
        width: parent.width
        // Never shorter than the control it replaces.
        height: Math.max(list.height + 10, shutBox.height)
        visible: root.open
        radius: 9
        color: Theme.plot
        border.width: 1
        border.color: Theme.border

        OutputList {
            id: list
            width: parent.width - 10
            x: 5
            y: 5
            source: root.source
            onPicked: root.open = false
        }
    }
}
