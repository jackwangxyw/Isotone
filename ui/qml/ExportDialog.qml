import QtQuick
import QtCore
import QtQuick.Dialogs
import Isotone

// Export: Name, Layout (outputs with more than two channels: their own layout,
// 5.1 and stereo below it), Format. Export asks where, then writes the current
// output's EQ as Equalizer APO text for the layout.
DialogFrame {
    id: root
    property var layouts: Presets.exportLayouts()
    property int layoutIndex: 0

    title: "Export"
    cardWidth: 440

    // `file` a URL; true when written.
    function writeTo(file) {
        const l = layouts.length > 0 ? layouts[layoutIndex] : null
        return Presets.exportFile(file, l ? l.channels : 0, l ? l.mask : 0)
    }
    function fileName() {
        const name = nameField.text.trim().replace(/[<>:"\/\\|?*]/g, "-")
        return (name === "" ? "Untitled" : name) + ".txt"
    }

    FileDialog {
        id: saveFile
        title: "Export"
        fileMode: FileDialog.SaveFile
        defaultSuffix: "txt"
        nameFilters: ["Equalizer APO (*.txt)"]
        currentFolder: StandardPaths.writableLocation(StandardPaths.DocumentsLocation)
        onAccepted: {
            root.writeTo(selectedFile)
            root.close()
        }
    }

    Item { width: 1; height: 20 }
    Column {
        width: parent.width
        spacing: 16
        TextBox {
            id: nameField
            objectName: "exportName"
            width: parent.width
            label: "Name"
            text: Presets.currentName
        }
        Column {
            visible: root.layouts.length > 0
            spacing: 6
            Text {
                text: "Layout"
                font.family: Theme.font
                font.pixelSize: 12
                color: Theme.muted
            }
            Segmented {
                objectName: "exportLayout"
                options: root.layouts.map((l) => l.label)
                current: root.layoutIndex
                onPicked: (index) => root.layoutIndex = index
            }
        }
        Item {
            width: parent.width
            height: 28
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: "Format"
                font.family: Theme.font
                font.pixelSize: 13
                color: Theme.muted
            }
            Text {
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                text: "Equalizer APO · .txt"
                font.family: Theme.font
                font.pixelSize: 13
                color: Theme.text
            }
        }
    }

    actions: [
        Button {
            objectName: "exportCancel"
            text: "Cancel"
            onClicked: { root.rejected(); root.close() }
        },
        Button {
            objectName: "exportConfirm"
            text: "Export"
            kind: "primary"
            onClicked: {
                saveFile.selectedFile = saveFile.currentFolder + "/" + root.fileName()
                saveFile.open()
            }
        }
    ]
}
