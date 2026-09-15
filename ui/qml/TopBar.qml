import QtQuick
import Isotone

// Top bar (76 px): preset name, then preamp (slider, value, Auto), channel view,
// spectrum and the EQ switch.
Item {
    id: root
    // In window coordinates, where the popover's top left goes.
    signal presetsRequested(real x, real y)
    signal outputsRequested(real x, real y)

    height: 76

    // The preset name (a dot when modified) opens the presets popover; under it,
    // with the sidebar collapsed, the output name opens the outputs popover.
    Column {
        x: 32
        anchors.verticalCenter: parent.verticalCenter
        spacing: 1
        Item {
            objectName: "presetName"
            width: nameRow.implicitWidth
            height: nameRow.implicitHeight
            Row {
                id: nameRow
                spacing: 8
                Text {
                    text: Presets.currentName
                    font.family: Theme.font
                    font.pixelSize: 24
                    font.weight: Font.DemiBold
                    font.letterSpacing: -0.36
                    color: Theme.text
                    anchors.verticalCenter: parent.verticalCenter
                }
                Rectangle {
                    visible: Presets.modified
                    width: 7
                    height: 7
                    radius: 3.5
                    color: Theme.accent
                    anchors.verticalCenter: parent.verticalCenter
                }
                Icon { name: "chevron"; size: 18; anchors.verticalCenter: parent.verticalCenter }
            }
            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: {
                    const p = nameRow.mapToItem(null, 0, nameRow.height + 8)
                    root.presetsRequested(p.x - 12, p.y)
                }
            }
        }
        Item {
            objectName: "topBarOutput"
            visible: !AppSettings.sidebarOpen
            width: outputRow.implicitWidth
            height: outputRow.implicitHeight
            Row {
                id: outputRow
                spacing: 4
                Text {
                    text: Outputs.currentName
                    font.family: Theme.font
                    font.pixelSize: 13
                    color: Theme.muted
                }
                Icon { name: "chevron"; size: 14; anchors.verticalCenter: parent.verticalCenter }
            }
            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: {
                    const p = outputRow.mapToItem(null, 0, outputRow.height + 8)
                    root.outputsRequested(p.x - 12, p.y)
                }
            }
        }
    }

    Row {
        anchors.right: parent.right
        anchors.rightMargin: 32
        anchors.verticalCenter: parent.verticalCenter
        spacing: 24

        Row {
            spacing: 10
            anchors.verticalCenter: parent.verticalCenter
            Text {
                text: "Preamp"
                font.family: Theme.font
                font.pixelSize: 13
                color: Theme.muted
                anchors.verticalCenter: parent.verticalCenter
            }
            // −24 to +6 dB, filled from 0 dB.
            Item {
                id: preamp
                readonly property real low: -24
                readonly property real high: 6
                function position(db) { return (Math.max(low, Math.min(high, db)) - low) / (high - low) * width }
                width: 110
                height: 18
                anchors.verticalCenter: parent.verticalCenter
                Rectangle { y: 7; width: parent.width; height: 4; radius: 2; color: Theme.track }
                Rectangle {
                    y: 7
                    height: 4
                    radius: 2
                    color: Theme.accent
                    x: Math.min(preamp.position(EqSession.preampDb), preamp.position(0))
                    width: Math.abs(preamp.position(0) - preamp.position(EqSession.preampDb))
                }
                Rectangle {
                    width: 16
                    height: 16
                    radius: 8
                    y: 1
                    x: preamp.position(EqSession.preampDb) - 8
                    color: Theme.knob
                    border.color: Theme.accent
                    border.width: 2.5
                }
                MouseArea {
                    anchors.fill: parent
                    anchors.margins: -6
                    cursorShape: Qt.PointingHandCursor
                    function set(mouseX) {
                        const t = Math.max(0, Math.min(1, (mouseX - 6) / preamp.width))
                        EqSession.preampDb = Math.round((preamp.low + t * (preamp.high - preamp.low)) * 10) / 10
                    }
                    onPressed: (mouse) => set(mouse.x)
                    onPositionChanged: (mouse) => { if (pressed) set(mouse.x) }
                    onReleased: EqSession.finishEdit()
                }
            }
            ValueField {
                objectName: "preampValue"
                width: 62
                horizontalAlignment: Text.AlignRight
                text: Theme.signed(EqSession.preampDb, 1) + " dB"
                unit: EqSession.Decibels
                weight: Font.DemiBold
                anchors.verticalCenter: parent.verticalCenter
                onSubmitted: (v) => { EqSession.preampDb = v; EqSession.finishEdit() }
            }
            Rectangle {
                width: autoLabel.implicitWidth + 18
                height: autoLabel.implicitHeight + 6
                radius: height / 2
                anchors.verticalCenter: parent.verticalCenter
                color: EqSession.autoPreamp ? Theme.accent : Theme.segmentedSelected
                Text {
                    id: autoLabel
                    anchors.centerIn: parent
                    text: "Auto"
                    font.family: Theme.font
                    font.pixelSize: 11
                    color: EqSession.autoPreamp ? Theme.textOnAccent : Theme.text
                }
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: EqSession.autoPreamp = !EqSession.autoPreamp
                }
            }
        }

        Segmented {
            objectName: "viewChannel"
            anchors.verticalCenter: parent.verticalCenter
            visible: EqSession.outputChannels === 2
            options: ["L", "R", "L+R"]
            current: EqSession.viewChannel
            onPicked: (index) => EqSession.viewChannel = index
        }
        // Surround: which speakers the graph shows.
        ShowingPicker {
            objectName: "showingPicker"
            anchors.verticalCenter: parent.verticalCenter
            visible: EqSession.outputChannels > 2
        }

        Row {
            spacing: 10
            anchors.verticalCenter: parent.verticalCenter
            Text {
                text: "Spectrum"
                font.family: Theme.font
                font.pixelSize: 13
                color: Theme.muted
                anchors.verticalCenter: parent.verticalCenter
            }
            Segmented {
                options: ["On", "Off"]
                current: AppSettings.spectrumOn ? 0 : 1
                onPicked: (index) => AppSettings.spectrumOn = index === 0
            }
        }

        Row {
            spacing: 10
            anchors.verticalCenter: parent.verticalCenter
            Text {
                text: "EQ"
                font.family: Theme.font
                font.pixelSize: 13
                color: Theme.muted
                anchors.verticalCenter: parent.verticalCenter
            }
            Toggle {
                checked: EqSession.eqOn
                anchors.verticalCenter: parent.verticalCenter
                onToggled: (on) => EqSession.eqOn = on
            }
        }
    }
}
