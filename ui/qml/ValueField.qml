import QtQuick
import Isotone

// A value that can be typed: click, type, Enter. Escape keeps the value; leaving
// the field applies what was typed when it reads as a value, and a typed text
// that does not stays open, selected, on Enter.
Item {
    id: root
    property string text
    property int unit: EqSession.Plain
    property bool editable: true
    property color colour: Theme.text
    property int pixelSize: 13
    property int weight: Font.Normal
    property int horizontalAlignment: Text.AlignHCenter
    readonly property bool editing: box.visible
    signal started()
    signal submitted(real value)

    implicitWidth: label.implicitWidth
    implicitHeight: label.implicitHeight

    function edit() {
        input.text = root.text
        box.visible = true
        input.forceActiveFocus()
        input.selectAll()
        root.started()
    }
    function close() {
        box.visible = false
        input.focus = false
    }
    function apply(leaving) {
        if (input.text === root.text) { close(); return }
        const v = EqSession.parseValue(input.text, root.unit)
        if (isNaN(v)) {
            if (leaving) close()
            else input.selectAll()
            return
        }
        close()
        root.submitted(v)
    }

    Text {
        id: label
        anchors.fill: parent
        horizontalAlignment: root.horizontalAlignment
        verticalAlignment: Text.AlignVCenter
        visible: !box.visible
        text: root.text
        color: root.colour
        font.family: Theme.font
        font.pixelSize: root.pixelSize
        font.weight: root.weight
    }
    MouseArea {
        objectName: "valueClick"
        anchors.fill: parent
        anchors.margins: -4
        enabled: root.editable && !box.visible
        cursorShape: Qt.IBeamCursor
        onClicked: root.edit()
    }
    Rectangle {
        id: box
        visible: false
        anchors.centerIn: parent
        width: Math.max(root.width, input.contentWidth) + 16
        height: label.implicitHeight + 8
        radius: 6
        color: Theme.track
        border.width: 1
        border.color: Theme.accent
        TextInput {
            id: input
            objectName: "valueInput"
            anchors.fill: parent
            anchors.leftMargin: 8
            anchors.rightMargin: 8
            verticalAlignment: TextInput.AlignVCenter
            horizontalAlignment: TextInput.AlignHCenter
            color: Theme.text
            selectionColor: Theme.accent
            selectedTextColor: Theme.textOnAccent
            selectByMouse: true
            font: label.font
            onAccepted: root.apply(false)
            Keys.onEscapePressed: root.close()
            onActiveFocusChanged: if (!activeFocus && box.visible) root.apply(true)
        }
    }
}
