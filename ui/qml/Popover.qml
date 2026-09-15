import QtQuick
import Isotone

// A popover: fills its parent (put it in the window's overlay), and shows the
// prototype's .pop panel (radius 14, padding 10, pop colour, border) at x, y.
// A press outside or Escape closes it. Content goes in the default property and
// sets the panel's height through implicitHeight.
Item {
    id: root
    property real panelX: 0
    property real panelY: 0
    property real panelWidth: 280
    property real padding: 10
    default property alias content: body.data
    readonly property bool open: panel.visible
    readonly property alias panel: panel
    signal closed()

    anchors.fill: parent
    z: 900

    function openAt(x, y) {
        root.panelX = Math.max(8, Math.min(root.width - panel.width - 8, x))
        root.panelY = Math.max(8, Math.min(root.height - panel.height - 8, y))
        panel.visible = true
        panel.forceActiveFocus()
    }
    // With the panel's bottom edge at `bottom`.
    function openAbove(x, bottom) { openAt(x, bottom - panel.height) }
    function close() {
        if (!panel.visible) return
        panel.visible = false
        root.closed()
    }

    MouseArea {
        anchors.fill: parent
        visible: panel.visible
        acceptedButtons: Qt.AllButtons
        onPressed: root.close()
    }

    Rectangle {
        id: panel
        visible: false
        x: root.panelX
        y: root.panelY
        width: root.panelWidth
        height: body.childrenRect.height + root.padding * 2
        radius: 14
        color: Theme.pop
        border.color: Theme.border
        Keys.onEscapePressed: root.close()
        MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons }
        Item {
            id: body
            x: root.padding
            y: root.padding
            width: parent.width - root.padding * 2
            height: childrenRect.height
        }
    }
}
