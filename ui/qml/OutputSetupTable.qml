import QtQuick
import Isotone

// The output setup table shared by Settings, Outputs and first run (prototype
// outputsTable): every active output with its format, what it has now, the
// engine chosen for it, and a status column. phase: locked, uac, edit, applying,
// done. First run has no Now column, and counts every output not Off as a change.
Item {
    id: root
    property string phase: "locked"
    property bool firstRun: false
    // Engine chosen per output GUID: "IsoAPO", "Equalizer APO" or "Off".
    property var choices: ({})
    signal picked(string guid, string engine)

    readonly property string dash: String.fromCharCode(0x2014)
    readonly property var rows: {
        const out = []
        if (Devices.revision < 0) return out
        for (let i = 0; i < Devices.count; ++i) {
            const d = Devices.row(Devices.data(Devices.index(i, 0), Qt.UserRole + 1))
            if (d.present) out.push(d)
        }
        return out
    }

    // Not picked yet: first run proposes IsoAPO, Settings keeps what the output has.
    function want(d) { return choices[d.guid] !== undefined ? choices[d.guid] : firstRun ? "IsoAPO" : d.now }
    function changed(d) { return firstRun ? want(d) !== "Off" : want(d) !== d.now }
    // Settings Outputs' defaults: what each output has, IsoAPO where both are on it.
    function defaults() {
        const c = {}
        for (const d of rows) c[d.guid] = firstRun ? "IsoAPO" : d.now.indexOf("+") >= 0 ? "IsoAPO" : d.now
        return c
    }
    readonly property int changeCount: rows.filter(changed).length
    function plans() { return rows.filter(changed).map((d) => Devices.plan(d.guid, want(d))) }

    readonly property var columns: firstRun ? [["Output", 0], ["Format", 130], ["Engine", 250], ["", 170]]
                                            : [["Output", 0], ["Format", 130], ["Now", 190], ["Engine", 250], ["", 170]]
    readonly property real outputWidth: {
        let w = width
        for (let i = 1; i < columns.length; ++i) w -= columns[i][1]
        return w
    }

    implicitHeight: 36 + rows.length * 48

    Row {
        height: 36
        Repeater {
            model: root.columns
            delegate: Text {
                required property var modelData
                required property int index
                width: index === 0 ? root.outputWidth : modelData[1]
                height: 36
                leftPadding: 16
                verticalAlignment: Text.AlignVCenter
                text: modelData[0]
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
            model: root.rows
            delegate: Rectangle {
                id: row
                required property var modelData
                readonly property var d: modelData
                readonly property bool isChanged: root.changed(d)
                readonly property string rowStatus: Devicetool.rowStatus[d.guid] || ""
                objectName: "setupRow_" + d.guid
                width: root.width
                height: 48
                color: root.phase === "edit" && isChanged ? Qt.alpha(Theme.accent, 0.07) : "transparent"

                Row {
                    height: parent.height
                    Text {
                        width: root.outputWidth
                        height: parent.height
                        leftPadding: 16
                        rightPadding: 8
                        elide: Text.ElideRight
                        verticalAlignment: Text.AlignVCenter
                        text: row.d.name
                        font.family: Theme.font
                        font.pixelSize: 14
                        font.weight: Font.Medium
                        color: Theme.text
                    }
                    Text {
                        width: 130
                        height: parent.height
                        leftPadding: 16
                        verticalAlignment: Text.AlignVCenter
                        text: row.d.format
                        font.family: Theme.font
                        font.pixelSize: 14
                        color: Theme.muted
                    }
                    Text {
                        visible: !root.firstRun
                        width: 190
                        height: parent.height
                        leftPadding: 16
                        verticalAlignment: Text.AlignVCenter
                        text: row.d.now
                        font.family: Theme.font
                        font.pixelSize: 14
                        color: Theme.muted
                    }
                    Item {
                        width: 250
                        height: parent.height
                        Segmented {
                            objectName: "engineChoice"
                            visible: root.phase === "edit"
                            x: 16
                            anchors.verticalCenter: parent.verticalCenter
                            options: row.d.hasEqualizerApo ? ["IsoAPO", "Equalizer APO", "Off"] : ["IsoAPO", "Off"]
                            current: options.indexOf(root.want(row.d))
                            onPicked: (index) => root.picked(row.d.guid, options[index])
                        }
                        Text {
                            visible: root.phase !== "edit"
                            x: 16
                            height: parent.height
                            verticalAlignment: Text.AlignVCenter
                            text: root.firstRun || root.phase === "applying" || root.phase === "done" ? root.want(row.d) : row.d.now
                            font.family: Theme.font
                            font.pixelSize: 14
                            color: Theme.text
                        }
                    }
                    Item {
                        width: 170
                        height: parent.height
                        readonly property bool applying: root.phase === "applying" || root.phase === "done"
                        Row {
                            objectName: "setupStatus"
                            x: 16
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 8
                            readonly property string step: {
                                if (!parent.applying) return row.d.status === "installed" || row.d.status === "active" || row.d.status === "not_installed" ? "" : "device"
                                if (!row.isChanged) return "unchanged"
                                return row.rowStatus
                            }
                            Spinner { visible: ["installing", "removing", "attaching"].indexOf(parent.step) >= 0; anchors.verticalCenter: parent.verticalCenter }
                            Icon {
                                visible: ["installed", "attached", "removed", "failed"].indexOf(parent.step) >= 0
                                name: parent.step === "failed" ? "warning" : "check"
                                size: 15
                                colour: parent.step === "failed" ? Theme.danger : Theme.ok
                                anchors.verticalCenter: parent.verticalCenter
                            }
                            StatusDot { visible: parent.step === "device"; status: row.d.dot; anchors.verticalCenter: parent.verticalCenter }
                            Text {
                                objectName: "setupStatusText"
                                anchors.verticalCenter: parent.verticalCenter
                                width: Math.min(implicitWidth, 170 - 32 - 14)
                                wrapMode: Text.WordWrap
                                lineHeight: 0.95
                                text: {
                                    switch (parent.step) {
                                    case "device": return row.d.statusLabel
                                    case "unchanged": return root.dash
                                    case "queued": return "Queued"
                                    case "installing": return "Installing"
                                    case "removing": return "Removing"
                                    case "attaching": return "Attaching"
                                    case "installed": return "Installed"
                                    case "attached": return "Attached"
                                    case "removed": return "Removed"
                                    case "failed": return "Failed"
                                    }
                                    return ""
                                }
                                font.family: Theme.font
                                font.pixelSize: 14
                                color: parent.step === "unchanged" || parent.step === "queued" ? Theme.muted : Theme.text
                            }
                        }
                    }
                }
            }
        }
    }
}
