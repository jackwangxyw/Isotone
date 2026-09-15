pragma Singleton
import QtQuick
import Isotone

// The preset flows that open dialogs: unsaved changes, Save as, New, import and
// export. Dialogs open in UiState.overlay.
QtObject {
    id: actions

    readonly property Component unsavedDialog: Component { UnsavedDialog {} }
    readonly property Component saveAsDialog: Component { SaveAsDialog {} }
    readonly property Component importDialog: Component { ImportDialog {} }
    readonly property Component exportDialog: Component { ExportDialog {} }

    // general/autoPreampForNew, default on; settings.ini reads a bool back as text.
    function autoPreampForNew() {
        const v = AppSettings.value("general/autoPreampForNew", true)
        return v !== false && String(v) !== "false"
    }

    // Runs proceed() once the current output has no unsaved changes: at once, or
    // after Save or Don't save in the unsaved dialog; cancelled() on Cancel.
    // `closing` asks "before closing?". Returns the dialog, or null when none opened.
    function confirmUnsaved(closing, proceed, cancelled) {
        if (!Presets.modified) {
            if (proceed) proceed()
            return null
        }
        const d = UiState.openDialog(unsavedDialog, {closing: closing})
        if (!d) return null
        d.saved.connect(() => { if (proceed) proceed() })
        d.discarded.connect(() => { if (proceed) proceed() })
        d.cancelled.connect(() => { if (cancelled) cancelled() })
        return d
    }

    function newPreset() {
        confirmUnsaved(false, () => Presets.newPreset(autoPreampForNew()))
    }

    // Save, or Save as while the output is untitled; done() once saved.
    function save(done, cancelled) {
        if (Presets.untitled) {
            saveAs(done, cancelled)
            return
        }
        Presets.save()
        if (done) done()
    }

    function saveAs(done, cancelled) {
        const d = UiState.openDialog(saveAsDialog, {})
        if (!d) return null
        d.accepted.connect((name) => {
            if (Presets.saveAs(name) !== "") { if (done) done() }
            else if (cancelled) cancelled()
        })
        d.rejected.connect(() => { if (cancelled) cancelled() })
        return d
    }

    // The import dialog for a file; null when it cannot be read.
    function showImport(url) {
        const preview = Presets.openImport(url)
        if (!preview) return null
        return UiState.openDialog(importDialog, {preview: preview})
    }

    function openExport() {
        return UiState.openDialog(exportDialog, {})
    }
}
