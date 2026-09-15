import QtQuick
import Isotone

// The prototype's .opbox: the phase of the operation Devicetool runs (uac,
// running, restarting, testing, done, reboot, failed, busy, declined), with
// Later / Restart Windows, Copy details / Retry, or Retry.
Rectangle {
    id: root
    readonly property string phase: Devicetool.phase
    readonly property string kind: Devicetool.kind
    readonly property var verbs: ({ install: "Installing", repair: "Repairing", uninstall: "Uninstalling",
                                    replace: "Replacing Equalizer APO", test: "Testing" })
    readonly property var dones: ({ install: "Installed", repair: "Repaired", uninstall: "Uninstalled",
                                    replace: "IsoAPO installed", test: "Test passed" })
    readonly property var failures: ({ install: "Install failed", repair: "Repair failed", uninstall: "Uninstall failed",
                                       replace: "Replace failed", test: "Test failed" })

    readonly property string icon: phase === "uac" || phase === "declined" ? "shield"
                                 : phase === "done" ? "check"
                                 : phase === "reboot" || phase === "failed" || phase === "busy" ? "warning" : ""
    readonly property color iconColour: phase === "done" ? Theme.ok
                                      : phase === "failed" ? Theme.danger
                                      : phase === "reboot" || phase === "busy" ? Theme.warning : Theme.muted
    readonly property string headline: {
        switch (phase) {
        case "uac": return "Waiting for administrator approval"
        case "running": return verbs[kind] || ""
        case "restarting": return "Restarting audio"
        case "testing": return "Testing"
        case "done": return dones[kind] || ""
        case "reboot": return (dones[kind] || "") + " · audio did not restart"
        case "failed": return failures[kind] || ""
        case "busy": return "Another install is running"
        case "declined": return "Approval declined"
        }
        return ""
    }

    implicitHeight: column.implicitHeight + 24
    radius: 10
    color: Theme.surface

    Column {
        id: column
        x: 14
        y: 12
        width: parent.width - 28
        spacing: 8

        Row {
            spacing: 10
            Spinner {
                visible: root.phase === "running" || root.phase === "restarting" || root.phase === "testing"
                anchors.verticalCenter: parent.verticalCenter
            }
            Icon {
                visible: root.icon !== ""
                name: root.icon
                size: 16
                colour: root.iconColour
                anchors.verticalCenter: parent.verticalCenter
            }
            Text {
                objectName: "operationText"
                anchors.verticalCenter: parent.verticalCenter
                text: root.headline
                font.family: Theme.font
                font.pixelSize: 13
                color: Theme.text
            }
        }
        Text {
            objectName: "operationReason"
            visible: root.phase === "failed" && Devicetool.reason !== ""
            width: parent.width
            wrapMode: Text.Wrap
            lineHeight: 1.5
            text: Devicetool.reason
            font.family: Theme.font
            font.pixelSize: 13
            color: Theme.muted
        }
        Row {
            visible: root.phase === "reboot" || root.phase === "failed" || root.phase === "busy" || root.phase === "declined"
            anchors.right: parent.right
            spacing: 8
            Button { objectName: "operationLater"; visible: root.phase === "reboot"; kind: "ghost"; text: "Later"; onClicked: Devicetool.clear() }
            Button { objectName: "operationRestart"; visible: root.phase === "reboot"; kind: "primary"; text: "Restart Windows"; onClicked: Devicetool.restartWindows() }
            Button { objectName: "operationCopy"; visible: root.phase === "failed"; kind: "ghost"; text: "Copy details"; onClicked: Devicetool.copyDetails() }
            Button {
                objectName: "operationRetry"
                visible: root.phase === "failed" || root.phase === "busy" || root.phase === "declined"
                text: "Retry"
                onClicked: Devicetool.retry()
            }
        }
    }
}
