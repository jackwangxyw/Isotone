import QtQuick
import Isotone

// Installing IsoAPO on an output that has Equalizer APO (prototype dialog
// replace): replace it with IsoAPO (Recommended, preselected), or keep Equalizer
// APO and attach Isotone to its config.txt.
DialogFrame {
    id: root
    property var device: ({})
    property string choice: "iso"
    signal keepEqualizerApo(var device)
    objectName: "replaceDialog"
    title: device.name || ""
    cardWidth: 560

    Item { width: 1; height: 18 }
    Row {
        width: parent.width
        spacing: 12
        Repeater {
            model: [["iso", "IsoAPO", ["Live edits", "Engine spectrum", "Removes Equalizer APO from this output"]],
                    ["eapo", "Equalizer APO", ["Edits apply on release", "Loopback spectrum", "Keeps config.txt and its includes"]]]
            delegate: Rectangle {
                id: card
                required property var modelData
                readonly property bool on: root.choice === modelData[0]
                objectName: "choice_" + modelData[0]
                width: (parent.width - 12) / 2
                height: cardColumn.implicitHeight + 32
                radius: 12
                color: on ? Theme.surface : "transparent"
                border.width: on ? 1.5 : 1
                border.color: on ? Theme.accent : Theme.gridMajor
                Column {
                    id: cardColumn
                    x: 16
                    y: 16
                    width: parent.width - 32
                    Item {
                        width: parent.width
                        height: 24
                        Row {
                            spacing: 8
                            anchors.verticalCenter: parent.verticalCenter
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                text: card.modelData[1]
                                font.family: Theme.font
                                font.pixelSize: 15
                                font.weight: Font.DemiBold
                                color: Theme.text
                            }
                            Rectangle {
                                visible: card.modelData[0] === "iso"
                                anchors.verticalCenter: parent.verticalCenter
                                width: recommended.implicitWidth + 18
                                height: 22
                                radius: 6
                                color: Theme.accent
                                Text {
                                    id: recommended
                                    anchors.centerIn: parent
                                    text: "Recommended"
                                    font.family: Theme.font
                                    font.pixelSize: 11
                                    font.weight: Font.DemiBold
                                    color: Theme.textOnAccent
                                }
                            }
                        }
                        Icon {
                            visible: card.on
                            anchors.right: parent.right
                            anchors.verticalCenter: parent.verticalCenter
                            name: "check"
                            size: 16
                            colour: Theme.accent
                        }
                    }
                    Repeater {
                        model: card.modelData[2]
                        delegate: Text {
                            required property string modelData
                            topPadding: 8
                            text: modelData
                            font.family: Theme.font
                            font.pixelSize: 13
                            color: Theme.muted
                        }
                    }
                }
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.choice = card.modelData[0]
                }
            }
        }
    }

    actions: [
        Button { objectName: "replaceCancel"; text: "Cancel"; onClicked: root.close() },
        Button {
            objectName: "replaceContinue"
            text: "Continue"
            kind: "primary"
            onClicked: {
                if (root.choice === "iso") {
                    const op = Devices.operation(root.device.guid, "replaceWithIsoApo")
                    if (op.kind) Devicetool.run(op.kind, root.device.guid, op.args)
                }
                const keep = root.choice === "eapo"
                root.close()
                // The opener shows Attach: an object made in this dialog's context would stop working once it is gone.
                if (keep) root.keepEqualizerApo(root.device)
            }
        }
    ]
}
