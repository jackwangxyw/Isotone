import QtQuick
import Isotone

// What the window shows when the model asks: "Band N deleted" with Undo when a
// band is deleted, and the unsaved dialog when a preset is loaded over unsaved
// changes (from the popover, a shortcut or the tray). One in Main.
Item {
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
            PresetActions.confirmUnsaved(false, () => Presets.load(name))
        }
    }
}
