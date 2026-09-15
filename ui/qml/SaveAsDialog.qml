import QtQuick
import Isotone

// Save as: a name for the current output's EQ as a new preset. A name already
// taken gets a number (Presets.saveAs).
DialogFrame {
    id: root
    signal accepted(string name)

    title: "Save as"
    cardWidth: 440

    function accept() {
        const name = nameField.text.trim()
        if (name === "") return
        root.accepted(name)
        root.close()
    }

    Item { width: 1; height: 18 }
    TextBox {
        id: nameField
        objectName: "saveAsName"
        width: parent.width
        label: "Name"
        text: Presets.untitled ? "" : Presets.currentName
        onAccepted: root.accept()
        // After DialogFrame gives its card the focus.
        Component.onCompleted: Qt.callLater(() => { input.forceActiveFocus(); input.selectAll() })
    }

    actions: [
        Button {
            objectName: "saveAsCancel"
            text: "Cancel"
            onClicked: { root.rejected(); root.close() }
        },
        Button {
            objectName: "saveAsSave"
            text: "Save"
            kind: "primary"
            active: nameField.text.trim() !== ""
            onClicked: root.accept()
        }
    ]
}
