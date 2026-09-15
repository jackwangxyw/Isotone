import QtQuick
import Isotone

// The Devices view (Devices board, prototype devicesView): every output with its
// engine, status, format and preset; the selected output's detail with its
// actions and the operation running on it; and Equalizer APO's own uninstaller
// when it is installed but on no output.
Item {
    id: root

    // Devices.revision is read in each binding that calls into Devices, so it runs again on a new read.
    readonly property string selected: Devices.revision >= 0 && Devices.indexOf(UiState.devicesSelection) >= 0 ? UiState.devicesSelection
                                     : Devices.defaultGuid !== "" ? Devices.defaultGuid
                                     : Devices.count > 0 ? firstGuid() : ""
    readonly property var device: Devices.revision >= 0 ? Devices.row(selected) : ({})
    // The prototype's empty value.
    readonly property string dash: String.fromCharCode(0x2014)

    function firstGuid() {
        const index = Devices.index(0, 0)
        return Devices.data(index, Qt.UserRole + 1)
    }
    function select(guid) {
        if (guid === UiState.devicesSelection) return
        UiState.devicesSelection = guid
        if (!Devicetool.working && Devicetool.kind !== "apply" && Devicetool.kind !== "approval") Devicetool.clear()
    }
    function runAction(action) {
        const op = Devices.operation(root.selected, action)
        if (op.kind) Devicetool.run(op.kind, root.selected, op.args)
    }
    function textWidth(text, size, weight) {
        const m = size === 11 ? metrics11 : size === 13 ? metrics13 : weight === Font.Medium ? metrics14Medium : metrics14
        return Math.ceil(m.advanceWidth(text))
    }

    FontMetrics { id: metrics11; font.family: Theme.font; font.pixelSize: 11; font.weight: Font.Medium }
    FontMetrics { id: metrics13; font.family: Theme.font; font.pixelSize: 13 }
    FontMetrics { id: metrics14; font.family: Theme.font; font.pixelSize: 14 }
    FontMetrics { id: metrics14Medium; font.family: Theme.font; font.pixelSize: 14; font.weight: Font.Medium }

    Component { id: uninstallDialog; UninstallDialog {} }
    Component { id: replaceDialog; ReplaceDialog { onKeepEqualizerApo: (device) => UiState.openDialog(attachDialog, { device: device }) } }
    Component { id: attachDialog; AttachDialog {} }

    Text {
        id: title
        x: 36
        y: 30
        text: "Devices"
        font.family: Theme.font
        font.pixelSize: 26
        font.weight: Font.DemiBold
        font.letterSpacing: -0.39
        color: Theme.text
    }
    Button {
        objectName: "refreshButton"
        anchors.right: parent.right
        anchors.rightMargin: 36
        anchors.verticalCenter: title.verticalCenter
        text: "Refresh"
        icon: "refresh"
        onClicked: { Devices.refresh(); Outputs.refresh() }
    }

    Item {
        id: body
        x: 36
        anchors.top: title.bottom
        anchors.topMargin: 22
        width: parent.width - 72
        anchors.bottom: parent.bottom

        // The table, and under it Equalizer APO's card.
        Column {
            id: tableColumn
            width: body.width - detail.width - 24
            spacing: 0

            Item {
                id: table
                objectName: "devicesTable"
                width: parent.width
                height: 36 + Devices.count * 48

                // Column widths from their content, and the room left shared in
                // proportion, as the prototype's HTML table lays out; when there
                // is no room the output's column gives way.
                readonly property var natural: {
                    const revision = Devices.revision
                    const w = revision < 0 ? [] : [root.textWidth("Output", 13), root.textWidth("Engine", 13), root.textWidth("Status", 13),
                               root.textWidth("Format", 13), root.textWidth("Preset", 13)]
                    for (let i = 0; i < Devices.count; ++i) {
                        const d = Devices.row(Devices.data(Devices.index(i, 0), Qt.UserRole + 1))
                        w[0] = Math.max(w[0], root.textWidth(d.name, 14, Font.Medium) + (d.isDefault ? 10 + root.textWidth("Default", 11, Font.Medium) + 18 : 0))
                        w[1] = Math.max(w[1], root.textWidth(d.engine, 14))
                        w[2] = Math.max(w[2], 14 + root.textWidth(d.statusLabel, 14))
                        w[3] = Math.max(w[3], root.textWidth(d.format, 14))
                        w[4] = Math.max(w[4], root.textWidth(Presets.assignedName(d.guid) || root.dash, 14))
                    }
                    return w.map((x) => x + 32)
                }
                readonly property var columnWidths: {
                    const total = natural.reduce((a, b) => a + b, 0)
                    if (total <= width) return natural.map((x) => x + (width - total) * x / total)
                    const rest = natural[1] + natural[2] + natural[3] + natural[4]
                    return [Math.max(80, width - rest), natural[1], natural[2], natural[3], natural[4]]
                }
                readonly property real outputWidth: columnWidths[0]
                readonly property var widths: columnWidths.slice(1)

                Row {
                    height: 36
                    Repeater {
                        model: ["Output", "Engine", "Status", "Format", "Preset"]
                        delegate: Text {
                            required property string modelData
                            required property int index
                            width: index === 0 ? table.outputWidth : table.widths[index - 1]
                            height: 36
                            leftPadding: 16
                            verticalAlignment: Text.AlignVCenter
                            text: modelData
                            font.family: Theme.font
                            font.pixelSize: 13
                            color: Theme.muted
                        }
                    }
                }

                Column {
                    y: 36
                    width: parent.width
                    Repeater {
                        model: Devices
                        delegate: Rectangle {
                            id: row
                            required property string guid
                            required property string name
                            required property bool isDefault
                            required property bool present
                            required property string statusLabel
                            required property string dot
                            required property string engine
                            required property string format
                            readonly property bool on: guid === root.selected
                            objectName: "deviceRow_" + guid
                            width: table.width
                            height: 48
                            radius: 10
                            color: on ? Theme.surface : "transparent"
                            opacity: present ? 1 : 0.55

                            Row {
                                height: parent.height
                                Item {
                                    width: table.outputWidth
                                    height: parent.height
                                    Row {
                                        x: 16
                                        anchors.verticalCenter: parent.verticalCenter
                                        spacing: 10
                                        Text {
                                            width: Math.min(implicitWidth, table.outputWidth - 32 - (pill.visible ? pill.width + 10 : 0))
                                            elide: Text.ElideRight
                                            anchors.verticalCenter: parent.verticalCenter
                                            text: row.name
                                            font.family: Theme.font
                                            font.pixelSize: 14
                                            font.weight: Font.Medium
                                            color: Theme.text
                                        }
                                        Rectangle {
                                            id: pill
                                            visible: row.isDefault
                                            anchors.verticalCenter: parent.verticalCenter
                                            width: pillText.implicitWidth + 18
                                            height: 22
                                            radius: 6
                                            color: Theme.surface
                                            Text {
                                                id: pillText
                                                anchors.centerIn: parent
                                                text: "Default"
                                                font.family: Theme.font
                                                font.pixelSize: 11
                                                font.weight: Font.Medium
                                                color: Theme.muted
                                            }
                                        }
                                    }
                                }
                                Text {
                                    width: table.widths[0]
                                    height: parent.height
                                    leftPadding: 16
                                    verticalAlignment: Text.AlignVCenter
                                    text: row.engine
                                    font.family: Theme.font
                                    font.pixelSize: 14
                                    color: Theme.muted
                                }
                                Item {
                                    width: table.widths[1]
                                    height: parent.height
                                    Row {
                                        x: 16
                                        anchors.verticalCenter: parent.verticalCenter
                                        spacing: 8
                                        StatusDot { status: row.dot; anchors.verticalCenter: parent.verticalCenter }
                                        Text {
                                            text: row.statusLabel
                                            font.family: Theme.font
                                            font.pixelSize: 14
                                            color: Theme.text
                                        }
                                    }
                                }
                                Text {
                                    width: table.widths[2]
                                    height: parent.height
                                    leftPadding: 16
                                    verticalAlignment: Text.AlignVCenter
                                    text: row.format
                                    font.family: Theme.font
                                    font.pixelSize: 14
                                    color: Theme.muted
                                }
                                Text {
                                    readonly property string preset: Devices.revision >= 0 ? Presets.assignedName(row.guid) : ""
                                    width: table.widths[3]
                                    height: parent.height
                                    leftPadding: 16
                                    rightPadding: 8
                                    elide: Text.ElideRight
                                    verticalAlignment: Text.AlignVCenter
                                    text: preset !== "" ? preset : root.dash
                                    font.family: Theme.font
                                    font.pixelSize: 14
                                    color: preset !== "" ? Theme.text : Theme.muted
                                }
                            }
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: root.select(row.guid)
                            }
                        }
                    }
                }
            }

            Item { width: 1; height: eapoCard.visible ? 22 : 0 }
            Rectangle {
                id: eapoCard
                objectName: "equalizerApoCard"
                visible: Devices.equalizerApoInstalled && !Devices.equalizerApoUsed
                width: parent.width
                height: 60
                radius: 14
                color: Theme.plot
                Row {
                    x: 18
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 14
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: Devices.equalizerApoVersion !== "" ? "Equalizer APO " + Devices.equalizerApoVersion : "Equalizer APO"
                        font.family: Theme.font
                        font.pixelSize: 14
                        font.weight: Font.DemiBold
                        color: Theme.text
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: "No outputs"
                        font.family: Theme.font
                        font.pixelSize: 14
                        color: Theme.muted
                    }
                }
                Button {
                    objectName: "equalizerApoUninstall"
                    anchors.right: parent.right
                    anchors.rightMargin: 18
                    anchors.verticalCenter: parent.verticalCenter
                    text: "Uninstall"
                    icon: "external"
                    onClicked: Devices.openEqualizerApoUninstaller()
                }
            }
        }

        DeviceDetail {
            id: detail
            anchors.right: parent.right
            width: 340
            visible: root.selected !== ""
            device: root.device
            onAction: (action) => {
                if (action === "uninstall" || action === "keepEapo")
                    UiState.openDialog(uninstallDialog, { device: root.device })
                else if (action === "replace")
                    UiState.openDialog(replaceDialog, { device: root.device })
                else if (action === "copyDiagnostics")
                    Devices.copyDiagnostics(root.selected)
                else
                    root.runAction(action)
            }
        }
    }
}
