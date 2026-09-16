import QtQuick
import Isotone

// The window: sidebar, the view for UiState.view, and the overlay that popovers,
// dialogs and toasts open in. 1440 x 900 is the boards' size; 1120 x 760 the
// minimum (docs/decisions.md, prototype decisions).
Window {
    id: window
    width: 1440
    height: 900
    minimumWidth: 1120
    minimumHeight: 760
    visible: true
    title: "Isotone"
    color: Theme.background

    Connections {
        target: Outputs
        function onCurrentChanged() {
            EqSession.useOutput(Outputs)
            if (UiState.view === "speakers" && EqSession.outputChannels <= 2) UiState.view = "eq"
        }
    }
    Component.onCompleted: {
        UiState.overlay = overlay
        EqSession.useOutput(Outputs)
        // Devices work package: first run, until it is done, while no output works.
        const done = AppSettings.value("general/firstRunDone", false)
        if (!(done === true || done === "true") && Outputs.count === 0) showFirstRun()
    }

    // Devices work package. Engine changes raise no device notification: read
    // the outputs again once devicetool has run.
    Connections {
        target: Devicetool
        function onFinished() { Devices.refresh(); Outputs.refresh() }
        // At once, before Settings Outputs removes a block: an output turned Off leaves Outputs and the session.
        function onOutputChoicesChanged() { Outputs.refresh(); Devices.refresh() }
    }
    // The 3 s engine poll, config.txt's include and Off: what Outputs lists.
    Connections {
        target: Devices
        function onOutputsChanged() { Outputs.refresh() }
    }
    // First run over the whole window (--first-run forces it).
    function showFirstRun() { firstRun.active = true }
    Loader {
        id: firstRun
        objectName: "firstRun"
        anchors.fill: parent
        z: 1500
        active: false
        sourceComponent: FirstRun { onFinished: firstRun.active = false }
    }

    // Settings: Delete, shortcuts, the selected band's keys, following the default output.
    WindowKeys {
        overlay: overlay
        firstRun: firstRun
    }
    DefaultOutputFollower {}

    // Settings, General, closing the window (WindowClose).
    WindowClose {
        id: closer
        host: window
        onQuit: Qt.quit()
    }
    onClosing: (close) => {
        close.accepted = false
        closer.requestClose()
    }
    // The tray's Quit (main.cpp).
    function requestQuit() { closer.requestQuit() }

    Row {
        id: content
        anchors.fill: parent

        Sidebar {
            id: sidebar
            height: parent.height
            onOutputsRequested: (x, y) => outputsPopover.openAbove(x, y + 44)
        }

        Item {
            id: viewArea
            width: window.width - sidebar.width
            height: parent.height

            EqualizerView {
                id: equalizer
                anchors.fill: parent
                visible: UiState.view === "eq"
                onBandMenuRequested: (row, x, above, below) => bandMenu.openAt(row, x, above, below)
                onPresetsRequested: (x, y) => presetsMenu.openAt(x, y)
                onOutputsRequested: (x, y) => outputsPopover.openAt(x, y)
            }
            Loader {
                anchors.fill: parent
                active: UiState.view !== "eq"
                source: UiState.view === "speakers" ? "SpeakersView.qml"
                      : UiState.view === "devices" ? "DevicesView.qml"
                      : UiState.view === "settings" ? "SettingsView.qml" : ""
            }
        }
    }

    Item {
        id: overlay
        objectName: "overlay"
        anchors.fill: parent
        z: 100

        BandMenu { id: bandMenu; anchors.fill: parent }
        PresetsMenu { id: presetsMenu; anchors.fill: parent }
        // Presets: the deleted band toast, the unsaved dialog on a load, and a dropped file.
        PresetPrompts {}
        DropOverlay { anchors.fill: parent; z: 950 }

        // The collapsed rail's outputs list (and the top bar's output name).
        Popover {
            id: outputsPopover
            objectName: "outputsPopover"
            panelWidth: 280
            Column {
                width: parent.width
                spacing: 0
                Item {
                    width: parent.width
                    height: 30
                    Text {
                        x: 10
                        text: "Outputs"
                        font.family: Theme.font
                        font.pixelSize: 13
                        font.weight: Font.DemiBold
                        color: Theme.text
                    }
                    Text {
                        anchors.right: parent.right
                        anchors.rightMargin: 10
                        text: "Manage"
                        font.family: Theme.font
                        font.pixelSize: 13
                        color: Theme.muted
                        MouseArea {
                            anchors.fill: parent
                            anchors.margins: -4
                            cursorShape: Qt.PointingHandCursor
                            onClicked: { outputsPopover.close(); UiState.view = "devices" }
                        }
                    }
                }
                OutputList { width: parent.width; onPicked: outputsPopover.close() }
            }
        }

        // A toast: text and an optional action, for a few seconds (Toast.qml).
        Toast { centreOffset: sidebar.width / 2 }
    }

    PressWatch {
        content: content
        overlay: overlay
    }
}
