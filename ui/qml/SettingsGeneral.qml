import QtQuick
import Isotone

// Settings, General (SettingsGeneral board): startup, presets, graph and
// spectrum. Launch at sign-in shows and sets the Run value itself (Startup);
// everything else is AppSettings, read back through GeneralSettings.
Item {
    implicitHeight: root.implicitHeight

    Column {
        id: root
        width: 760

        SettingsSection { text: "Startup" }
        SettingsRow {
            label: "Launch at sign-in"
            Toggle {
                objectName: "launchAtSignIn"
                checked: Startup.launchAtSignIn
                onToggled: (on) => {
                    Startup.setLaunchAtSignIn(on, GeneralSettings.startInTray)
                    AppSettings.setValue("general/launchAtSignIn", Startup.launchAtSignIn)
                }
            }
        }
        SettingsRow {
            label: "Start in the tray"
            Toggle {
                objectName: "startInTray"
                checked: GeneralSettings.startInTray
                onToggled: (on) => {
                    AppSettings.setValue("general/startInTray", on)
                    Startup.setStartInTray(on)
                }
            }
        }
        SettingsRow {
            label: "Keep running in the tray when closed"
            Toggle {
                objectName: "keepInTray"
                checked: GeneralSettings.keepInTray
                onToggled: (on) => AppSettings.setValue("general/keepInTray", on)
            }
        }

        SettingsSection { text: "Window" }
        SettingsRow {
            label: "Always on top"
            Toggle {
                objectName: "alwaysOnTop"
                checked: GeneralSettings.alwaysOnTop
                onToggled: (on) => AppSettings.setValue("window/alwaysOnTop", on)
            }
        }

        // Linux: the layout is Isotone's own sink's, one for every output, and is
        // chosen here, where Windows chooses an output's in its sound settings.
        SettingsSection { visible: Qt.platform.os === "linux"; text: "Speakers" }
        SettingsRow {
            visible: Qt.platform.os === "linux"
            label: "Layout"
            Segmented {
                objectName: "speakerLayout"
                options: ["Stereo", "2.1", "5.1", "7.1"]
                current: options.indexOf(Speakers.layoutName)
                enabled: Outputs.count > 0
                onPicked: (index) => {
                    const to = options[index]
                    if (to === Speakers.layoutName) return
                    UiState.openDialog(layoutDialog, {
                        outputName: "Isotone", from: Speakers.layoutName, to: to,
                        fromChannels: Speakers.channels, toChannels: Speakers.layoutChannels(to)
                    })
                }
            }
        }
        Component {
            id: layoutDialog
            LayoutDialog {
                onConfirmed: if (Speakers.setLayout(to) !== 0) UiState.toast("Speaker setup not changed")
            }
        }

        SettingsSection { text: "Presets" }
        SettingsRow {
            label: "Switch preset when the default output changes"
            Toggle {
                objectName: "switchOnDefaultOutput"
                checked: GeneralSettings.switchOnDefaultOutput
                onToggled: (on) => AppSettings.setValue("general/switchOnDefaultOutput", on)
            }
        }
        SettingsRow {
            label: "Auto preamp for new presets"
            Toggle {
                objectName: "autoPreampForNew"
                checked: GeneralSettings.autoPreampForNew
                onToggled: (on) => AppSettings.setValue("general/autoPreampForNew", on)
            }
        }

        SettingsSection { text: "Graph" }
        SettingsRow {
            label: "Gain range"
            Segmented {
                objectName: "gainRange"
                options: ["±12 dB", "±15 dB", "±24 dB"]
                current: GeneralSettings.gainRanges.indexOf(GeneralSettings.gainRange)
                onPicked: (index) => AppSettings.setValue("graph/gainRange", GeneralSettings.gainRanges[index])
            }
        }
        SettingsRow {
            label: "Frequency range"
            Rectangle {
                id: rangeButton
                objectName: "frequencyRange"
                width: rangeText.implicitWidth + 20
                height: 26
                radius: 6
                color: Theme.segmentedSelected
                Text {
                    id: rangeText
                    anchors.centerIn: parent
                    text: GeneralSettings.hertz(GeneralSettings.minHz) + " – " + GeneralSettings.hertz(GeneralSettings.maxHz)
                    font.family: Theme.font
                    font.pixelSize: 13
                    font.weight: Font.DemiBold
                    color: Theme.text
                }
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        if (rangePopover.open) { rangePopover.close(); return }
                        fromBox.text = GeneralSettings.hertz(GeneralSettings.minHz)
                        toBox.text = GeneralSettings.hertz(GeneralSettings.maxHz)
                        const p = rangeButton.mapToItem(rangePopover, rangeButton.width - rangePopover.panelWidth, rangeButton.height + 8)
                        rangePopover.openAt(p.x, p.y)
                    }
                }
            }
        }

        SettingsRow {
            label: "Short window"
            Segmented {
                objectName: "shortWindow"
                options: ["Graph", "Bands"]
                current: GeneralSettings.shortWindow === "bands" ? 1 : 0
                onPicked: (index) => GeneralSettings.setShortWindow(index === 1 ? "bands" : "graph")
            }
        }

        SettingsSection { text: "Spectrum" }
        SettingsRow {
            label: "Decay"
            SettingSlider {
                objectName: "decay"
                from: 50
                to: 500
                step: 10
                value: GeneralSettings.decayMs
                format: (v) => Math.round(v) + " ms"
                onMoved: (v) => GeneralSettings.setDecayMs(v)
            }
        }
        SettingsRow {
            label: "Smoothing"
            SettingSlider {
                objectName: "smoothing"
                from: 0
                to: 1
                step: 0.05
                value: GeneralSettings.smoothing
                format: (v) => Math.round(v * 100) + "%"
                onMoved: (v) => GeneralSettings.setSmoothing(v)
            }
        }

        // In the window's overlay, so the page's scrolling does not clip it.
        Popover {
            id: rangePopover
            objectName: "frequencyRangePopover"
            parent: UiState.overlay
            panelWidth: 260
            Row {
                width: parent.width
                spacing: 10
                function submit(box) {
                    const lo = EqSession.parseValue(fromBox.text, EqSession.Hertz)
                    const hi = EqSession.parseValue(toBox.text, EqSession.Hertz)
                    if (isNaN(lo) || isNaN(hi) || !GeneralSettings.setFrequencyRange(lo, hi)) {
                        box.input.selectAll()
                        return
                    }
                    fromBox.text = GeneralSettings.hertz(GeneralSettings.minHz)
                    toBox.text = GeneralSettings.hertz(GeneralSettings.maxHz)
                    box.input.selectAll()
                }
                TextBox {
                    id: fromBox
                    objectName: "rangeFrom"
                    width: (parent.width - 10) / 2
                    label: "From"
                    onAccepted: parent.submit(fromBox)
                }
                TextBox {
                    id: toBox
                    objectName: "rangeTo"
                    width: (parent.width - 10) / 2
                    label: "To"
                    onAccepted: parent.submit(toBox)
                }
            }
        }
    }
}
