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
                height: 28
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

        SettingsSection { text: "Spectrum" }
        SettingsRow {
            label: "Resolution"
            Segmented {
                objectName: "resolution"
                height: 28
                options: ["4096", "8192", "16384"]
                current: GeneralSettings.resolutions.indexOf(GeneralSettings.resolution)
                onPicked: (index) => AppSettings.setValue("spectrum/resolution", GeneralSettings.resolutions[index])
            }
        }
        SettingsRow {
            label: "Release"
            SettingBox {
                objectName: "release"
                text: GeneralSettings.releaseMs + " ms"
                accept: (t) => {
                    const v = EqSession.parseValue(t.replace(/\s*ms\s*$/i, ""), EqSession.Plain)
                    return isNaN(v) || v <= 0 ? undefined : v
                }
                onSubmitted: (v) => GeneralSettings.setReleaseMs(v)
            }
        }
        SettingsRow {
            label: "Peak hold"
            Toggle {
                objectName: "peakHold"
                checked: GeneralSettings.peakHold
                onToggled: (on) => AppSettings.setValue("spectrum/peakHold", on)
            }
        }
        SettingsRow {
            label: "Tilt"
            Segmented {
                objectName: "tilt"
                height: 28
                options: ["Off", "3 dB/oct", "4.5 dB/oct"]
                current: GeneralSettings.tilts.indexOf(GeneralSettings.tilt)
                onPicked: (index) => AppSettings.setValue("spectrum/tiltDbPerOct", GeneralSettings.tilts[index])
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
