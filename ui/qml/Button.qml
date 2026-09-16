import QtQuick
import Isotone

// The prototype's .btn: 32 px, radius 8, surface; "primary" on the accent,
// "ghost" transparent, "danger" red text. Optional leading icon.
Rectangle {
    id: root
    property string text
    property string icon
    property string kind: "normal"   // normal, primary, ghost, danger, link (ghost in the accent)
    property bool active: true   // false greys it and ignores clicks
    property bool busy: false    // a Spinner in place of the icon
    signal clicked()

    readonly property color foreground: kind === "primary" ? Theme.textOnAccent : kind === "danger" ? Theme.danger : kind === "link" ? Theme.accent : Theme.text

    implicitWidth: row.implicitWidth + 28
    implicitHeight: 32
    radius: 8
    color: kind === "primary" ? Theme.accent
         : kind === "ghost" || kind === "link" ? (area.containsMouse && active ? Theme.surface : "transparent")
         : area.containsMouse && active ? Qt.lighter(Theme.surface, Theme.dark ? 1.25 : 0.96) : Theme.surface
    opacity: active ? 1 : 0.45
    activeFocusOnTab: true
    Keys.onReturnPressed: if (active) root.clicked()
    Keys.onSpacePressed: if (active) root.clicked()

    Row {
        id: row
        anchors.centerIn: parent
        spacing: 8
        Item {
            visible: root.icon !== "" || root.busy
            width: 15
            height: 15
            anchors.verticalCenter: parent.verticalCenter
            Icon {
                visible: !root.busy
                anchors.centerIn: parent
                name: root.icon
                size: 15
                colour: root.foreground
            }
            Spinner {
                objectName: "buttonSpinner"
                visible: root.busy
                anchors.centerIn: parent
            }
        }
        Text {
            text: root.text
            font.family: Theme.font
            font.pixelSize: 13
            font.weight: Font.Medium
            color: root.foreground
            anchors.verticalCenter: parent.verticalCenter
        }
    }
    Rectangle {
        visible: root.activeFocus
        anchors.fill: parent
        anchors.margins: -3
        radius: root.radius + 3
        color: "transparent"
        border.width: 2
        border.color: Theme.accent
    }
    MouseArea {
        id: area
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: root.active ? Qt.PointingHandCursor : Qt.ArrowCursor
        onClicked: if (root.active) root.clicked()
    }
}
