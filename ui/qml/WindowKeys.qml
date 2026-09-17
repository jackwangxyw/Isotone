import QtQuick
import Isotone

// The window's keys: Delete for the selected band, the app's shortcuts, and the
// selected band's keys. None of them act while first run shows or the focus is
// in the overlay (a dialog or popover has it): those keys belong to it, not to
// the band behind. A closed popover's hidden panel keeps the focus until a press
// moves it, and holds nothing. Global hotkeys and the tray still act.
Item {
    id: root
    property Item overlay
    property Loader firstRun

    readonly property bool held: {
        if (firstRun && firstRun.active) return true
        const f = Window.activeFocusItem
        if (!f || !f.visible) return false
        for (let p = f; p; p = p.parent) if (p === overlay) return true
        return false
    }

    // A shortcut, not a key handler: a field being typed in keeps Delete for its text.
    Shortcut {
        sequence: ShortcutRegistry.revision >= 0 ? ShortcutRegistry.sequence("delete") : ""   // Settings, Shortcuts
        enabled: (UiState.view === "eq" || UiState.view === "ear") && !ShortcutRegistry.capturing && !root.held
        onActivated: EqSession.deleteBand(EqSession.selectedRow)
    }
    AppShortcuts { keysActive: !root.held }
    BandKeys {
        session: EqSession
        active: UiState.view === "eq" && !root.held
    }
}
