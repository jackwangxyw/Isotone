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

    // A shortcut, not a key handler: a field being typed in keeps Delete for its text.
    Shortcut {
        sequence: ShortcutRegistry.revision >= 0 ? ShortcutRegistry.sequence("delete") : ""   // Settings, Shortcuts
        enabled: UiState.view === "eq" && !ShortcutRegistry.capturing
        onActivated: EqSession.deleteBand(EqSession.selectedRow)
    }

    // Settings: shortcuts, the selected band's keys, following the default output.
    AppShortcuts {}
    BandKeys {
        session: EqSession
        active: UiState.view === "eq"
    }
    DefaultOutputFollower {}

    // Settings, General, closing the window: to the tray, or quit. With unsaved
    // changes the presets package's UnsavedDialog asks first.
    onClosing: (close) => {
        close.accepted = false
        window.requestClose()
    }
    function requestClose() {
        if (Presets.modified) {
            // INTEGRATION (presets package): UnsavedDialog, "Save changes to <name>
            // before closing?" with Cancel / Don't save / Save. It gets `closing: true`
            // and `afterClose`, and calls afterClose() once Save has saved or Don't
            // save has put the output back to its saved preset; Cancel closes only
            // the dialog.
            const unsaved = Qt.createComponent("Isotone", "UnsavedDialog")
            if (unsaved.status === Component.Ready) {
                UiState.openDialog(unsaved, { closing: true, afterClose: window.finishClose })
                return
            }
            console.warn("UnsavedDialog is not in this build; closing without asking")
        }
        finishClose()
    }
    function finishClose() {
        if (GeneralSettings.keepInTray) window.hide()
        else Qt.quit()
    }

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

        // A toast: text and an optional action, for a few seconds.
        Rectangle {
            id: toast
            objectName: "toast"
            property var action: null
            visible: false
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.horizontalCenterOffset: sidebar.width / 2
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 26
            width: toastRow.implicitWidth + 24
            height: 44
            radius: 10
            color: Theme.pop
            border.color: Theme.border
            Row {
                id: toastRow
                x: 16
                anchors.verticalCenter: parent.verticalCenter
                spacing: 16
                Text {
                    id: toastText
                    anchors.verticalCenter: parent.verticalCenter
                    font.family: Theme.font
                    font.pixelSize: 13
                    color: Theme.text
                }
                Button {
                    id: toastAction
                    visible: text !== ""
                    kind: "ghost"
                    anchors.verticalCenter: parent.verticalCenter
                    onClicked: {
                        toast.visible = false
                        if (toast.action) toast.action()
                    }
                }
            }
            Timer { id: toastTimer; interval: 5000; onTriggered: toast.visible = false }
        }
        Connections {
            target: UiState
            function onToastRequested(text, actionText, action) {
                toastText.text = text
                toastAction.text = actionText
                toast.action = action
                toast.visible = true
                toastTimer.restart()
            }
        }
    }

    // A press anywhere else ends typing in a field. Passes every press on.
    MouseArea {
        id: pressWatch
        anchors.fill: parent
        z: 1000
        acceptedButtons: Qt.AllButtons
        onPressed: (mouse) => {
            const f = window.activeFocusItem
            if (f && !f.contains(f.mapFromItem(pressWatch, mouse.x, mouse.y))) content.forceActiveFocus()
            mouse.accepted = false
        }
    }
}
