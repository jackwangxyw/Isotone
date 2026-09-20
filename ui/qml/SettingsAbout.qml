import QtQuick
import Isotone

// Settings, About (SettingsAbout board): the mark, name and version, Copy
// diagnostics, and the engines with protected audio (About).
Item {
    implicitHeight: root.implicitHeight

    Column {
        id: root
        width: 760

        Component.onCompleted: About.refresh()

        Item { width: 1; height: 26 }
        Item {
            width: root.width
            height: 56
            Rectangle {
                id: mark
                width: 56
                height: 56
                radius: 14
                color: Theme.knob
                Icon { name: "logo"; size: 30; strokeWidth: 3; colour: "#121519"; anchors.centerIn: parent }
            }
            Column {
                anchors.left: mark.right
                anchors.leftMargin: 16
                anchors.verticalCenter: parent.verticalCenter
                Text {
                    text: "Isotone"
                    font.family: Theme.font
                    font.pixelSize: 20
                    font.weight: Font.DemiBold
                    color: Theme.text
                }
                Text {
                    objectName: "version"
                    text: "Version " + About.version
                    font.family: Theme.font
                    font.pixelSize: 13
                    color: Theme.muted
                }
            }
            Button {
                id: copy
                objectName: "copyDiagnostics"
                property bool copied: false
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                text: copied ? "Copied" : "Copy diagnostics"
                icon: copied ? "check" : ""
                onClicked: {
                    About.copyDiagnostics()
                    copied = true
                    copiedTimer.restart()
                }
                Timer { id: copiedTimer; interval: 1600; onTriggered: copy.copied = false }
            }
        }

        SettingsSection { text: "Engine" }
        // Linux: the daemon and PipeWire in place of the two APOs and protected audio.
        readonly property bool linux: Qt.platform.os === "linux"
        SettingsRow {
            visible: root.linux
            label: "Daemon"
            Text {
                objectName: "daemon"
                text: About.daemon
                font.family: Theme.font
                font.pixelSize: 13
                color: Theme.muted
            }
        }
        SettingsRow {
            visible: root.linux
            label: "PipeWire"
            Text {
                objectName: "pipewire"
                text: About.pipewire
                font.family: Theme.font
                font.pixelSize: 13
                color: Theme.muted
            }
        }
        SettingsRow {
            visible: !root.linux
            label: "IsoAPO"
            Text {
                objectName: "isoapo"
                text: About.isoapo
                font.family: Theme.font
                font.pixelSize: 13
                color: Theme.muted
            }
        }
        SettingsRow {
            visible: !root.linux
            label: "Equalizer APO"
            Text {
                objectName: "equalizerApo"
                text: About.equalizerApo
                font.family: Theme.font
                font.pixelSize: 13
                color: Theme.muted
            }
        }
        SettingsRow {
            visible: !root.linux
            label: "Protected audio"
            Row {
                spacing: 8
                StatusDot {
                    anchors.verticalCenter: parent.verticalCenter
                    status: About.protectedAudioDisabled ? "warn" : "ok"
                }
                Text {
                    objectName: "protectedAudio"
                    anchors.verticalCenter: parent.verticalCenter
                    text: About.protectedAudioDisabled ? "Disabled" : "Enabled"
                    font.family: Theme.font
                    font.pixelSize: 14
                    color: Theme.text
                }
            }
        }
    }
}
