import QtQuick
import Isotone

// The Devices view's detail card (prototype devDetail): name, status, Engine,
// Effect slot, Format, Config for Equalizer APO outputs, Preset, then the
// operation running on this output or its actions.
Rectangle {
    id: root
    property var device: ({})
    signal action(string action)

    readonly property string dash: String.fromCharCode(0x2014)
    readonly property bool showsOperation: Devicetool.phase !== "" && Devicetool.target === device.guid
                                           && Devicetool.kind !== "apply" && Devicetool.kind !== "approval"
    readonly property var labels: ({
        test: "Test", uninstall: "Uninstall", replace: "Replace with IsoAPO", repair: "Repair",
        removeEapo: "Remove Equalizer APO", takeBack: "Take back", keepEapo: "Keep Equalizer APO",
        undo: "Undo", copyDiagnostics: "Copy diagnostics", enableEnhancements: "Turn on enhancements", install: "Install"
    })
    readonly property var primary: ["replace", "repair", "removeEapo", "takeBack", "undo", "enableEnhancements", "install"]

    height: column.implicitHeight + 44
    radius: 14
    color: Theme.plot

    component DetailRow: Item {
        property string label
        property string value
        width: parent.width
        height: 39
        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: parent.label
            font.family: Theme.font
            font.pixelSize: 13
            color: Theme.muted
        }
        Text {
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            text: parent.value
            font.family: Theme.font
            font.pixelSize: 13
            color: Theme.text
        }
        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.gridMinor }
    }

    Column {
        id: column
        x: 20
        y: 22
        width: parent.width - 40

        Text {
            objectName: "detailName"
            width: parent.width
            elide: Text.ElideRight
            text: root.device.name || ""
            font.family: Theme.font
            font.pixelSize: 17
            font.weight: Font.DemiBold
            color: Theme.text
        }
        Item { width: 1; height: 6 }
        Row {
            spacing: 8
            StatusDot { status: root.device.dot || ""; anchors.verticalCenter: parent.verticalCenter }
            Text {
                objectName: "detailStatus"
                text: root.device.statusLabel || ""
                font.family: Theme.font
                font.pixelSize: 13
                color: Theme.text
            }
        }
        Item { width: 1; height: 14 }
        DetailRow { label: "Engine"; value: root.device.engineDetail || "" }
        DetailRow { label: "Effect slot"; value: root.device.slot || root.dash }
        DetailRow { label: "Format"; value: root.device.format || "" }
        DetailRow {
            objectName: "detailConfig"
            visible: root.device.status === "active"
            label: "Config"
            value: root.device.config || "config.txt"
        }
        Item {
            objectName: "detailPreset"
            visible: root.device.status !== "unplugged" && root.device.status !== "unrecorded"
            width: parent.width
            height: 39
            readonly property string preset: Devices.revision >= 0 ? Presets.assignedName(root.device.guid || "") : ""
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: "Preset"
                font.family: Theme.font
                font.pixelSize: 13
                color: Theme.muted
            }
            Row {
                id: presetButton
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                spacing: 4
                Text {
                    text: parent.parent.preset !== "" ? parent.parent.preset : "None"
                    font.family: Theme.font
                    font.pixelSize: 13
                    color: Theme.text
                    anchors.verticalCenter: parent.verticalCenter
                }
                Icon { name: "chevron"; size: 14; anchors.verticalCenter: parent.verticalCenter }
            }
            MouseArea {
                objectName: "presetButton"
                anchors.fill: presetButton
                anchors.margins: -6
                cursorShape: Qt.PointingHandCursor
                onClicked: {
                    const p = presetButton.mapToItem(presetMenu.parent, presetButton.width, presetButton.height + 6)
                    presetMenu.openAt(p.x - presetMenu.panelWidth, p.y)
                }
            }
            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.gridMinor }
        }

        Item { width: 1; height: 18; visible: actions.visible || opBox.visible }
        OperationBox { id: opBox; width: parent.width; visible: root.showsOperation }

        // The actions in the prototype's order; Uninstall on the right.
        Item {
            id: actions
            objectName: "detailActions"
            readonly property var list: root.device.actions || []
            visible: !root.showsOperation && list.length > 0
            width: parent.width
            height: 32
            Row {
                spacing: 8
                Repeater {
                    model: actions.list.filter((a) => a !== "uninstall")
                    delegate: Button {
                        required property string modelData
                        objectName: "action_" + modelData
                        text: root.labels[modelData]
                        kind: root.primary.indexOf(modelData) >= 0 ? "primary" : "normal"
                        active: !Devicetool.working
                        onClicked: root.action(modelData)
                    }
                }
            }
            Button {
                objectName: "action_uninstall"
                visible: actions.list.indexOf("uninstall") >= 0
                anchors.right: parent.right
                text: root.device.status === "conflict" ? "Uninstall IsoAPO" : "Uninstall"
                kind: "danger"
                active: !Devicetool.working
                onClicked: root.action("uninstall")
            }
        }
    }

    // The preset for this output: None or a preset, from the presets list.
    Popover {
        id: presetMenu
        objectName: "presetMenu"
        parent: UiState.overlay ? UiState.overlay : root
        panelWidth: 220
        Column {
            width: parent.width
            Repeater {
                model: [""].concat(Presets.names)
                delegate: Rectangle {
                    required property string modelData
                    width: parent.width
                    height: 34
                    radius: 6
                    color: itemArea.containsMouse ? Theme.surface : "transparent"
                    Text {
                        x: 10
                        anchors.verticalCenter: parent.verticalCenter
                        text: modelData !== "" ? modelData : "None"
                        font.family: Theme.font
                        font.pixelSize: 13
                        color: Theme.text
                    }
                    MouseArea {
                        id: itemArea
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            Presets.assign(root.device.guid, modelData)
                            presetMenu.close()
                            Devices.refresh()
                        }
                    }
                }
            }
        }
    }
}
