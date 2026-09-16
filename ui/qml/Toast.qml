import QtQuick
import Isotone

// A toast at the bottom of the overlay: text and an optional action, raised by
// UiState.toast(). It fades in, stays 3 s, fades out; a second toast takes over
// the same panel and starts the wait again.
Rectangle {
    id: root
    objectName: "toast"
    property var action: null
    // The window's centre, not the overlay's, with the sidebar beside it.
    property real centreOffset: 0

    anchors.horizontalCenter: parent.horizontalCenter
    anchors.horizontalCenterOffset: centreOffset
    anchors.bottom: parent.bottom
    anchors.bottomMargin: 26
    width: row.implicitWidth + 24
    height: 44
    radius: 10
    color: Theme.pop
    border.color: Theme.border
    // Clickable from the moment it is raised, gone once it has faded out.
    property bool showing: false
    opacity: showing ? 1 : 0
    visible: showing || opacity > 0
    Behavior on opacity { NumberAnimation { duration: 250; easing.type: Easing.InOutQuad } }

    function hide() { showing = false }

    Row {
        id: row
        x: 16
        anchors.verticalCenter: parent.verticalCenter
        spacing: 16
        Text {
            id: label
            objectName: "toastText"
            anchors.verticalCenter: parent.verticalCenter
            font.family: Theme.font
            font.pixelSize: 13
            color: Theme.text
        }
        Button {
            id: actionButton
            objectName: "toastAction"
            visible: text !== ""
            kind: "link"
            anchors.verticalCenter: parent.verticalCenter
            onClicked: {
                root.hide()
                if (root.action) root.action()
            }
        }
    }
    Timer { id: wait; interval: 3000; onTriggered: root.hide() }
    Connections {
        target: UiState
        function onToastRequested(text, actionText, action) {
            label.text = text
            actionButton.text = actionText
            root.action = action
            root.showing = true
            wait.restart()
        }
    }
}
