import QtQuick
import Isotone

// A setting's value in the prototype's .sbox (26 px, radius 6, 600): click, type,
// Enter. `accept` reads the typed text and returns the value, or undefined when
// it is not one: the field then stays open with the text selected. Escape, or
// leaving the field, keeps the value.
Rectangle {
    id: root
    property string text
    property var accept: function(t) { return t }
    readonly property bool editing: input.visible
    signal submitted(var value)

    implicitWidth: Math.max(56, (editing ? input.contentWidth : label.implicitWidth) + 16)
    implicitHeight: 26
    radius: 6
    color: Theme.segmentedSelected
    border.width: editing ? 1 : 0
    border.color: Theme.accent

    function edit() {
        input.text = root.text
        input.visible = true
        input.forceActiveFocus()
        input.selectAll()
    }
    function close() {
        input.visible = false
        input.focus = false
    }

    Text {
        id: label
        anchors.centerIn: parent
        visible: !root.editing
        text: root.text
        font.family: Theme.font
        font.pixelSize: 13
        font.weight: Font.DemiBold
        color: Theme.text
    }
    MouseArea {
        objectName: "settingBoxClick"
        anchors.fill: parent
        enabled: !root.editing
        cursorShape: Qt.IBeamCursor
        onClicked: root.edit()
    }
    TextInput {
        id: input
        objectName: "settingBoxInput"
        visible: false
        anchors.fill: parent
        anchors.leftMargin: 8
        anchors.rightMargin: 8
        verticalAlignment: TextInput.AlignVCenter
        horizontalAlignment: TextInput.AlignHCenter
        font: label.font
        color: Theme.text
        selectionColor: Theme.accent
        selectedTextColor: Theme.textOnAccent
        selectByMouse: true
        Keys.onEscapePressed: root.close()
        onAccepted: {
            if (text === root.text) { root.close(); return }
            const v = root.accept(text)
            if (v === undefined) { selectAll(); return }
            root.close()
            root.submitted(v)
        }
        onActiveFocusChanged: if (!activeFocus && visible) root.close()
    }
}
