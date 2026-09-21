import QtQuick
import Isotone

// A newer release, over the bottom right of the window (owner, 2026-09-20). It
// shows once UpdateCheck has found one and the window is open, so a start in
// the tray stays silent until then; it fades out after 8 s, held while the
// pointer is over it. Later and the close button put it away until the next
// start; View release opens the release's page.
Rectangle {
    id: root
    objectName: "updateNotice"
    // Seconds on screen; the tests shorten it.
    property int seconds: 8
    property bool dismissed: false
    readonly property bool windowOpen: Window.window !== null && Window.window.visible
    readonly property bool wanted: UpdateCheck.latest !== "" && windowOpen && !dismissed

    anchors.right: parent.right
    anchors.bottom: parent.bottom
    anchors.margins: 24
    width: 320
    height: column.implicitHeight + 34
    radius: 12
    color: Theme.pop
    border.color: Theme.border
    property bool showing: false
    opacity: showing ? 1 : 0
    visible: showing || opacity > 0
    Behavior on opacity { NumberAnimation { duration: 250; easing.type: Easing.InOutQuad } }

    onWantedChanged: if (wanted) { showing = true; wait.restart() }
    function dismiss() { dismissed = true; showing = false; wait.stop() }

    Timer { id: wait; interval: root.seconds * 1000; onTriggered: root.dismiss() }
    // Under the buttons: hovering anywhere on the card holds it.
    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        onContainsMouseChanged: if (root.showing) { if (containsMouse) wait.stop(); else wait.restart() }
    }

    Column {
        id: column
        x: 20
        y: 18
        width: parent.width - 38
        Text {
            objectName: "updateTitle"
            width: parent.width - 28
            text: "Isotone " + UpdateCheck.latest + " is available"
            font.family: Theme.font
            font.pixelSize: 14
            font.weight: Font.DemiBold
            color: Theme.text
            elide: Text.ElideRight
        }
        Item { width: 1; height: 4 }
        Text {
            objectName: "updateCurrent"
            text: "Current version: " + UpdateCheck.current
            font.family: Theme.font
            font.pixelSize: 13
            color: Theme.muted
        }
        Item { width: 1; height: 16 }
        Row {
            anchors.right: parent.right
            spacing: 8
            Button { objectName: "updateLater"; text: "Later"; kind: "ghost"; onClicked: root.dismiss() }
            Button {
                objectName: "updateView"
                text: "View release"
                kind: "primary"
                onClicked: {
                    Qt.openUrlExternally(UpdateCheck.releaseUrl)
                    root.dismiss()
                }
            }
        }
    }

    Rectangle {
        objectName: "updateClose"
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.margins: 14
        width: 24
        height: 24
        radius: 6
        color: closeArea.containsMouse ? Theme.surface : "transparent"
        Icon { anchors.centerIn: parent; name: "close"; size: 13; colour: Theme.muted }
        MouseArea {
            id: closeArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: root.dismiss()
        }
    }
}
