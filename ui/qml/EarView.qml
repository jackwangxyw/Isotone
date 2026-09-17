import QtQuick
import Isotone

// EQ by ear (EqByEar board): the top bar, a shorter graph with the tone's cursor
// and the marks, the sweep controls, and the band list filling the rest. New
// starts a fresh preset; the tone plays through whatever the output has
// (owner, 2026-09-16). Leaving the view, or the window hiding, fades the tone out.
Item {
    id: root
    // The graph, the controls and a few rows of bands; a shorter window scrolls (Main).
    readonly property int minimumHeight: 76 + 268 + 246 + 96 + 20
    signal bandMenuRequested(int row, real x, real above, real below)
    signal presetsRequested(real x, real y)

    Component.onDestruction: EqByEar.stop()
    Connections {
        target: root.Window.window
        function onVisibleChanged() { if (!root.Window.window.visible) EqByEar.stop() }
    }
    Connections {
        target: EqByEar
        function onToneFailed() { UiState.toast("Tone stopped") }
    }

    // Play asks to turn the volume down first, once a session.
    function togglePlay() {
        if (EqByEar.playing) { EqByEar.playing = false; return }
        if (UiState.earWarned) { EqByEar.playing = true; return }
        UiState.openDialog(volumeDialog, {})
    }
    Component {
        id: volumeDialog
        VolumeDialog {
            onConfirmed: {
                UiState.earWarned = true
                EqByEar.playing = true
            }
        }
    }

    // The keys, unless a dialog or popover has the focus (WindowKeys); a field
    // being typed in keeps its own.
    readonly property bool held: {
        const f = Window.activeFocusItem
        if (!f || !f.visible) return false
        for (let p = f; p; p = p.parent) if (p === UiState.overlay) return true
        return false
    }
    // A 48th of an octave, a sixth with Shift, an octave a page (EqByEar::kNudgeOctaves, kCoarseNudgeOctaves).
    Shortcut { sequences: ["Left"]; enabled: !root.held; autoRepeat: true; onActivated: EqByEar.nudge(-1 / 48) }
    Shortcut { sequences: ["Right"]; enabled: !root.held; autoRepeat: true; onActivated: EqByEar.nudge(1 / 48) }
    Shortcut { sequences: ["Shift+Left"]; enabled: !root.held; autoRepeat: true; onActivated: EqByEar.nudge(-1 / 6) }
    Shortcut { sequences: ["Shift+Right"]; enabled: !root.held; autoRepeat: true; onActivated: EqByEar.nudge(1 / 6) }
    Shortcut { sequences: ["PgDown"]; enabled: !root.held; autoRepeat: true; onActivated: EqByEar.nudge(-1) }
    Shortcut { sequences: ["PgUp"]; enabled: !root.held; autoRepeat: true; onActivated: EqByEar.nudge(1) }
    Shortcut { sequences: ["Space"]; enabled: !root.held; autoRepeat: false; onActivated: root.togglePlay() }

    // A value in a raised box: the prototype's .sbox.
    component ValueBox: Rectangle {
        id: box
        property alias text: field.text
        property alias unit: field.unit
        signal submitted(real value)
        width: Math.max(56, field.implicitWidth + 18)
        height: 26
        radius: 7
        color: Theme.track
        ValueField {
            id: field
            anchors.centerIn: parent
            weight: Font.DemiBold
            onSubmitted: (v) => box.submitted(v)
        }
    }

    component Label: Text {
        font.family: Theme.font
        font.pixelSize: 13
        color: Theme.muted
    }

    // Start, Top or End: shows its frequency, and a click records the tone's.
    component MarkBox: Rectangle {
        id: markBox
        property int mark
        property string label
        property real hz
        width: 120
        height: 54
        radius: 10
        color: "transparent"
        border.width: 1
        border.color: Theme.gridMajor
        Column {
            x: 14
            anchors.verticalCenter: parent.verticalCenter
            spacing: 3
            Text { text: markBox.label; font.family: Theme.font; font.pixelSize: 12; color: Theme.muted }
            Text {
                text: markBox.hz > 0 ? Theme.frequency(markBox.hz) : "–"
                font.family: Theme.font
                font.pixelSize: 15
                font.weight: markBox.hz > 0 ? Font.DemiBold : Font.Normal
                color: markBox.hz > 0 ? Theme.text : Theme.muted
            }
        }
        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: EqByEar.mark(markBox.mark)
        }
    }

    component NudgeButton: Rectangle {
        id: nudgeButton
        property string icon
        signal clicked()
        width: 36
        height: 36
        radius: 9
        color: nudgeArea.containsMouse ? Theme.surface : Theme.track
        Icon { name: nudgeButton.icon; size: 18; colour: Theme.text; anchors.centerIn: parent }
        MouseArea {
            id: nudgeArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: nudgeButton.clicked()
        }
    }

    Column {
        anchors.fill: parent

        TopBar {
            width: parent.width
            onPresetsRequested: (x, y) => root.presetsRequested(x, y)
        }
        GraphCard {
            id: graphCard
            x: 32
            width: parent.width - 64
            height: 268
            ear: true
            onMenuRequested: (row, x, above, below) => root.bandMenuRequested(row, x, above, below)
        }

        Item {
            id: controls
            x: 32
            width: parent.width - 64
            height: root.height - 76 - graphCard.height

            // Play and the frequency with its nudges; the level and the auto sweep on
            // the same row (owner, 2026-09-16). The channel is the top bar's.
            Item {
                id: playRow
                y: 16
                width: parent.width
                height: 56
                Row {
                    height: parent.height
                    spacing: 18
                    Rectangle {
                        objectName: "earPlay"
                        width: 56
                        height: 56
                        radius: 28
                        color: Theme.accent
                        Icon {
                            name: EqByEar.playing ? "pause" : "play"
                            size: 22
                            strokeWidth: 2.2
                            colour: Theme.textOnAccent
                            anchors.centerIn: parent
                        }
                        MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: root.togglePlay() }
                    }
                    Row {
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 10
                        NudgeButton {
                            objectName: "earNudgeDown"
                            anchors.verticalCenter: parent.verticalCenter
                            icon: "chevronLeft"
                            onClicked: EqByEar.nudge(-1 / 48)
                        }
                        ValueField {
                            objectName: "earFrequency"
                            width: 190
                            anchors.verticalCenter: parent.verticalCenter
                            text: Theme.frequency(EqByEar.frequency)
                            unit: EqSession.Hertz
                            pixelSize: 40
                            weight: Font.DemiBold
                            onSubmitted: (v) => EqByEar.frequency = v
                        }
                        NudgeButton {
                            objectName: "earNudgeUp"
                            anchors.verticalCenter: parent.verticalCenter
                            icon: "chevronRight"
                            onClicked: EqByEar.nudge(1 / 48)
                        }
                    }
                }
                Row {
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 10
                    Label { text: "Level"; anchors.verticalCenter: parent.verticalCenter }
                    // −60 to 0 dBFS, filled from the left.
                    Item {
                        id: level
                        readonly property real low: -60
                        function position(db) { return (Math.max(low, Math.min(0, db)) - low) / -low * width }
                        width: 120
                        height: 18
                        anchors.verticalCenter: parent.verticalCenter
                        Rectangle { y: 7; width: parent.width; height: 4; radius: 2; color: Theme.track }
                        Rectangle { y: 7; width: level.position(EqByEar.levelDb); height: 4; radius: 2; color: Theme.accent }
                        Rectangle {
                            width: 18
                            height: 18
                            radius: 9
                            y: 0
                            x: level.position(EqByEar.levelDb) - 9
                            color: Theme.knob
                        }
                        MouseArea {
                            objectName: "earLevel"
                            anchors.fill: parent
                            anchors.margins: -6
                            cursorShape: Qt.PointingHandCursor
                            function set(mouseX) {
                                const t = Math.max(0, Math.min(1, (mouseX - 6) / level.width))
                                EqByEar.levelDb = Math.round(level.low * (1 - t))
                            }
                            onPressed: (mouse) => set(mouse.x)
                            onPositionChanged: (mouse) => { if (pressed) set(mouse.x) }
                        }
                    }
                    ValueBox {
                        objectName: "earLevelValue"
                        anchors.verticalCenter: parent.verticalCenter
                        text: Theme.dbfs(EqByEar.levelDb)
                        unit: EqSession.Dbfs
                        onSubmitted: (v) => EqByEar.levelDb = v
                    }
                    Item { width: 22; height: 1 }
                    Label { text: "Auto sweep"; anchors.verticalCenter: parent.verticalCenter }
                    Toggle {
                        objectName: "earAutoSweep"
                        anchors.verticalCenter: parent.verticalCenter
                        checked: EqByEar.autoSweep
                        onToggled: (on) => EqByEar.autoSweep = on
                    }
                    ValueBox {
                        objectName: "earSweepRate"
                        anchors.verticalCenter: parent.verticalCenter
                        text: Number(EqByEar.sweepRate.toFixed(2)) + " oct/s"
                        unit: EqSession.OctavesPerSecond
                        onSubmitted: (v) => EqByEar.sweepRate = v
                    }
                }
            }

            SweepSlider {
                id: slider
                objectName: "sweepSlider"
                y: playRow.y + playRow.height + 14 + 30 - 7
                width: parent.width
            }

            // The marks, Peak or Dip, New preset, Clear and Add band.
            Item {
                id: markRow
                y: slider.y + 7 + 44 + 14
                width: parent.width
                height: 54
                Row {
                    height: parent.height
                    spacing: 10
                    MarkBox { objectName: "markStart"; mark: EqByEar.Start; label: "Start"; hz: EqByEar.start }
                    MarkBox { objectName: "markTop"; mark: EqByEar.Top; label: "Top"; hz: EqByEar.top }
                    MarkBox { objectName: "markEnd"; mark: EqByEar.End; label: "End"; hz: EqByEar.end }
                    Item { width: 12; height: 1 }
                    Segmented {
                        objectName: "earPeakDip"
                        anchors.verticalCenter: parent.verticalCenter
                        options: ["Peak", "Dip"]
                        current: EqByEar.dip ? 1 : 0
                        onPicked: (index) => EqByEar.dip = index === 1
                    }
                }
                Row {
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 10
                    // A fresh preset to find by ear (owner, 2026-09-16), highlighted.
                    Button {
                        objectName: "earNewPreset"
                        text: "New preset"
                        kind: "primary"
                        onClicked: PresetActions.newPreset()
                    }
                    Button { objectName: "earClear"; text: "Clear"; onClicked: EqByEar.clearMarks() }
                    Button {
                        objectName: "earAddMarked"
                        text: "Add band"
                        icon: "plus"
                        kind: "primary"
                        active: EqByEar.canAddBand
                        onClicked: EqByEar.addBand()
                    }
                }
            }

            EarBandList {
                y: markRow.y + markRow.height + 18
                width: parent.width
                height: Math.max(96, controls.height - y - 20)
                onMenuRequested: (row, x, above, below) => root.bandMenuRequested(row, x, above, below)
            }
        }
    }
}
