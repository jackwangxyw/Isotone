import QtQuick
import Isotone

// First run (prototype wizardView), over the whole window: Outputs (pick an
// engine per output), Install (approval, then every change, restart, test),
// Done. Skip and Open Isotone set general/firstRunDone and emit finished().
Rectangle {
    id: root
    signal finished()

    // 0 Outputs, 1 Installing, 2 Ready.
    property int step: 0
    property var choices: ({})
    property bool installing: false

    color: Theme.background

    Connections {
        target: Devicetool
        function onChanged() {
            if (!root.installing || Devicetool.kind !== "apply") return
            if (Devicetool.phase === "declined" || (Devicetool.phase === "failed" && root.step === 0)) {
                root.installing = false
                root.step = 0
            } else if (Devicetool.phase === "running" || Devicetool.phase === "restarting" || Devicetool.phase === "testing") {
                root.step = 1
            } else if (!Devicetool.working && Devicetool.phase !== "") {
                root.step = 2
            }
        }
    }

    function install() {
        if (Devicetool.working) return
        installing = true
        Devicetool.apply(table.plans())
    }
    function close() {
        AppSettings.setValue("general/firstRunDone", true)
        Devicetool.clear()
        Outputs.refresh()
        root.finished()
    }

    // Swallows input meant for the window underneath.
    MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons; onWheel: (wheel) => wheel.accepted = true }

    Column {
        width: 920
        anchors.horizontalCenter: parent.horizontalCenter
        y: 64

        Row {
            spacing: 12
            height: 32
            Rectangle {
                width: 28
                height: 28
                radius: 7
                anchors.verticalCenter: parent.verticalCenter
                color: Theme.dark ? Theme.knob : "#1b2025"
                Icon { name: "logo"; size: 16; strokeWidth: 3; colour: Theme.dark ? "#121519" : "#fcfdff"; anchors.centerIn: parent }
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: "Isotone"
                font.family: Theme.font
                font.pixelSize: 17
                font.weight: Font.DemiBold
                font.letterSpacing: -0.17
                color: Theme.text
            }
        }

        Item { width: 1; height: 36 }
        Row {
            objectName: "wizardSteps"
            spacing: 8
            Repeater {
                model: ["Outputs", "Install", "Done"]
                delegate: Row {
                    required property string modelData
                    required property int index
                    spacing: 8
                    Rectangle {
                        width: 20
                        height: 20
                        radius: 10
                        anchors.verticalCenter: parent.verticalCenter
                        color: index <= root.step ? Theme.accent : Theme.surface
                        Text {
                            anchors.centerIn: parent
                            text: index + 1
                            font.family: Theme.font
                            font.pixelSize: 11
                            font.weight: Font.DemiBold
                            color: index <= root.step ? Theme.textOnAccent : Theme.text
                        }
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: modelData
                        font.family: Theme.font
                        font.pixelSize: 13
                        font.weight: index === root.step ? Font.DemiBold : Font.Normal
                        color: index === root.step ? Theme.text : Theme.muted
                    }
                    Item {
                        visible: index < 2
                        width: 52
                        height: 1
                        anchors.verticalCenter: parent.verticalCenter
                        Rectangle { x: 6; width: 40; height: 1; color: Theme.gridMajor }
                    }
                }
            }
        }

        Item { width: 1; height: 26 }
        Text {
            objectName: "wizardTitle"
            text: ["Outputs", "Installing", "Ready"][root.step]
            font.family: Theme.font
            font.pixelSize: 26
            font.weight: Font.DemiBold
            font.letterSpacing: -0.39
            color: Theme.text
        }

        Item { width: 1; height: 14 }
        OutputSetupTable {
            id: table
            objectName: "wizardTable"
            width: parent.width
            firstRun: true
            phase: ["edit", "applying", "done"][root.step]
            choices: root.choices
            onPicked: (guid, engine) => {
                const c = Object.assign({}, root.choices)
                c[guid] = engine
                root.choices = c
            }
        }

        Item { width: 1; height: 26 }
        Item {
            width: parent.width
            height: 34
            Row {
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                spacing: 10

                Row {
                    visible: root.step === 0 && Devicetool.phase === "uac"
                    spacing: 8
                    anchors.verticalCenter: parent.verticalCenter
                    Icon { name: "shield"; size: 15; anchors.verticalCenter: parent.verticalCenter }
                    Text { objectName: "wizardWaiting"; text: "Waiting for administrator approval"; font.family: Theme.font; font.pixelSize: 13; color: Theme.muted; anchors.verticalCenter: parent.verticalCenter }
                }
                Row {
                    // Ready with a failure, or an approval that failed on Outputs (a decline shows nothing).
                    visible: Devicetool.kind === "apply" && (root.step === 2 && (Devicetool.phase === "reboot" || Devicetool.phase === "failed" || Devicetool.phase === "busy")
                                                             || root.step === 0 && Devicetool.phase === "failed")
                    spacing: 8
                    anchors.verticalCenter: parent.verticalCenter
                    Icon {
                        name: "warning"
                        size: 15
                        colour: Devicetool.phase === "failed" ? Theme.danger : Theme.warning
                        anchors.verticalCenter: parent.verticalCenter
                    }
                    Text {
                        objectName: "wizardResult"
                        anchors.verticalCenter: parent.verticalCenter
                        text: Devicetool.phase === "reboot" ? "Audio did not restart" : Devicetool.phase === "busy" ? "Another install is running" : Devicetool.reason
                        font.family: Theme.font
                        font.pixelSize: 13
                        color: Theme.text
                    }
                }
                Button { objectName: "wizardRestart"; visible: root.step === 2 && Devicetool.phase === "reboot"; text: "Restart Windows"; onClicked: Devicetool.restartWindows() }
                Button { objectName: "wizardRetry"; visible: root.step === 2 && Devicetool.phase === "busy"; text: "Retry"; onClicked: Devicetool.retry() }

                Button { objectName: "wizardSkip"; visible: root.step === 0; kind: "ghost"; text: "Skip"; onClicked: root.close() }
                Button {
                    objectName: "wizardInstall"
                    visible: root.step === 0
                    kind: "primary"
                    icon: "shield"
                    text: "Install"
                    active: table.changeCount > 0 && !Devicetool.working
                    onClicked: root.install()
                }
                Button { objectName: "wizardOpenWaiting"; visible: root.step === 1; text: "Open Isotone"; active: false }
                Button { objectName: "wizardOpen"; visible: root.step === 2; kind: "primary"; text: "Open Isotone"; onClicked: root.close() }
            }
        }
    }
}
