import QtQuick
import Isotone

// Settings, Outputs (prototype settingsOutputs): the output setup table. Change
// asks for Windows approval once, then each output's engine can be picked; Apply
// runs every change, restarts audio and tests.
Item {
    id: root
    // locked, uac, edit; applying, done, failed and reboot come from Devicetool.
    property string localPhase: Devicetool.elevated && Devicetool.kind === "apply" && Devicetool.phase !== "" ? "applied" : "locked"
    property var choices: ({})
    readonly property string phase: {
        if (localPhase === "applied" && Devicetool.kind === "apply") {
            if (Devicetool.working) return "applying"
            return Devicetool.phase === "done" ? "done" : Devicetool.phase
        }
        return localPhase === "applied" ? "locked" : localPhase
    }
    readonly property int changes: table.changeCount

    implicitHeight: 24 + 34 + 14 + table.implicitHeight

    Connections {
        target: Devicetool
        function onChanged() {
            if (Devicetool.kind !== "approval" || root.localPhase !== "uac") return
            if (Devicetool.phase === "done") root.edit()
            else if (Devicetool.phase === "declined" || Devicetool.phase === "failed") root.localPhase = "locked"
        }
    }

    function edit() {
        choices = table.defaults()
        localPhase = "edit"
    }
    function change() {
        if (Devicetool.working) return
        if (Devicetool.elevated) { edit(); return }
        localPhase = "uac"
        Devicetool.requestApproval()
    }
    function apply() {
        const plans = table.plans()
        if (plans.length === 0) return
        localPhase = "applied"
        Devicetool.apply(plans)
    }
    function finish() {
        Devicetool.clear()
        localPhase = "locked"
    }

    Item {
        id: bar
        y: 24
        width: Math.min(1000, root.width)
        height: 34

        Row {
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            spacing: 10

            Button { objectName: "outputsChange"; visible: root.phase === "locked"; text: "Change"; icon: "shield"; onClicked: root.change() }

            Row {
                visible: root.phase === "uac"
                spacing: 8
                anchors.verticalCenter: parent.verticalCenter
                Icon { name: "shield"; size: 15; anchors.verticalCenter: parent.verticalCenter }
                Text { objectName: "outputsWaiting"; text: "Waiting for administrator approval"; font.family: Theme.font; font.pixelSize: 13; color: Theme.muted; anchors.verticalCenter: parent.verticalCenter }
            }

            Button { objectName: "outputsCancel"; visible: root.phase === "edit"; kind: "ghost"; text: "Cancel"; onClicked: root.localPhase = "locked" }
            Button {
                objectName: "outputsApply"
                visible: root.phase === "edit"
                kind: "primary"
                text: root.changes > 0 ? "Apply " + root.changes : "Apply"
                active: root.changes > 0
                onClicked: root.apply()
            }

            Row {
                visible: root.phase === "applying"
                spacing: 8
                anchors.verticalCenter: parent.verticalCenter
                Spinner { anchors.verticalCenter: parent.verticalCenter }
                Text { objectName: "outputsApplying"; text: "Applying"; font.family: Theme.font; font.pixelSize: 13; color: Theme.text; anchors.verticalCenter: parent.verticalCenter }
            }

            Row {
                visible: root.phase === "done" || root.phase === "failed" || root.phase === "reboot" || root.phase === "busy"
                spacing: 8
                anchors.verticalCenter: parent.verticalCenter
                Icon {
                    name: root.phase === "done" ? "check" : "warning"
                    size: 15
                    colour: root.phase === "done" ? Theme.ok : root.phase === "failed" ? Theme.danger : Theme.warning
                    anchors.verticalCenter: parent.verticalCenter
                }
                Text {
                    objectName: "outputsResult"
                    anchors.verticalCenter: parent.verticalCenter
                    text: root.phase === "done" ? (Devicetool.restarted ? "Applied · audio restarted" : "Applied")
                        : root.phase === "reboot" ? "Applied · audio did not restart"
                        : root.phase === "busy" ? "Another install is running"
                        : Devicetool.reason
                    font.family: Theme.font
                    font.pixelSize: 13
                    color: Theme.text
                }
            }
            Button { objectName: "outputsLater"; visible: root.phase === "reboot"; kind: "ghost"; text: "Later"; onClicked: root.finish() }
            Button { objectName: "outputsRestart"; visible: root.phase === "reboot"; kind: "primary"; text: "Restart Windows"; onClicked: Devicetool.restartWindows() }
            Button { objectName: "outputsDone"; visible: root.phase === "done" || root.phase === "failed" || root.phase === "busy"; text: "Done"; onClicked: root.finish() }
        }
    }

    OutputSetupTable {
        id: table
        objectName: "outputsTable"
        y: bar.y + bar.height + 14
        width: Math.min(1000, root.width)
        phase: root.phase === "failed" || root.phase === "reboot" || root.phase === "busy" ? "done" : root.phase
        choices: root.choices
        onPicked: (guid, engine) => {
            const c = Object.assign({}, root.choices)
            c[guid] = engine
            root.choices = c
        }
    }
}
