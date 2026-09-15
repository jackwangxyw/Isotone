import QtQuick
import Isotone

// What the window shows when the model asks: "Band N deleted" with Undo when a
// band is deleted, and the unsaved dialog when a preset is loaded over unsaved
// changes (from the popover, a shortcut or the tray). One in Main.
Item {
    id: root
    // The window to show before asking (a pick from the tray or a global hotkey
    // comes while it is hidden); a test gives a stand-in.
    property var host: Window.window
    // The one unsaved dialog, and the preset to load after it: a later pick
    // while it is open changes the preset and asks no more.
    property var unsaved: null
    property string loading: ""

    Connections {
        target: EqSession
        function onBandDeleted(position, step) {
            // Undo undoes that deletion, and only while it is the last edit.
            UiState.toast("Band " + position + " deleted", "Undo", () => EqSession.undoStep(step))
        }
    }
    Connections {
        target: Presets
        function onUnsavedChanges(name) {
            root.loading = name
            UiState.showWindow(root.host)
            if (root.unsaved) return
            const d = PresetActions.confirmUnsaved(false, () => Presets.load(root.loading))
            if (d) {
                root.unsaved = d
                d.closed.connect(() => { root.unsaved = null })
            }
        }
    }
}
