import QtQuick
import Isotone

// "Uninstall IsoAPO from <output>?" (prototype dialog uninstall). Uninstall puts
// back what the output had before IsoAPO: the driver's effects, or Equalizer APO
// where it holds the output. The saved state stays.
DialogFrame {
    id: root
    property var device: ({})
    objectName: "uninstallDialog"
    title: "Uninstall IsoAPO from " + (device.name || "") + "?"
    cardWidth: 460

    component FactRow: Item {
        property string label
        property string value
        width: parent.width
        height: 39
        Text { anchors.verticalCenter: parent.verticalCenter; text: parent.label; font.family: Theme.font; font.pixelSize: 13; color: Theme.muted }
        Text { anchors.right: parent.right; anchors.verticalCenter: parent.verticalCenter; text: parent.value; font.family: Theme.font; font.pixelSize: 13; color: Theme.text }
        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.gridMinor }
    }

    Item { width: 1; height: 16 }
    FactRow { objectName: "restores"; label: "Restores"; value: root.device.hasEqualizerApo ? "Equalizer APO" : "Driver effects" }
    FactRow { label: "Saved state"; value: "Kept" }

    actions: [
        Button { objectName: "uninstallCancel"; text: "Cancel"; onClicked: root.close() },
        Button {
            objectName: "uninstallConfirm"
            text: "Uninstall"
            kind: "primary"
            onClicked: {
                const op = Devices.operation(root.device.guid, "uninstall")
                if (op.kind) Devicetool.run(op.kind, root.device.guid, op.args)
                root.close()
            }
        }
    ]
}
