import QtQuick
import Isotone

// The app's shortcuts: a window Shortcut for each app action, bound to its keys
// in ShortcutRegistry, and what each action does, for these, global hotkeys and
// anything else that calls ShortcutRegistry.activate. Off while the Shortcuts
// page waits for keys. Delete is Main's Shortcut, for the Equalizer view.
Item {
    id: root

    function perform(id) {
        switch (id) {
        case "eq": EqSession.eqOn = !EqSession.eqOn; break
        case "mute": EqSession.muted = !EqSession.muted; break
        case "nextPreset": Presets.next(); break
        case "previousPreset": Presets.previous(); break
        case "savePreset": Presets.save(); break
        case "undo": EqSession.undo(); break
        case "redo": EqSession.redo(); break
        }
    }

    Connections {
        target: ShortcutRegistry
        function onActivated(id) { root.perform(id) }
    }

    component ActionShortcut: Shortcut {
        required property string action
        sequence: ShortcutRegistry.revision >= 0 ? ShortcutRegistry.sequence(action) : ""
        enabled: !ShortcutRegistry.capturing && sequence !== ""
        onActivated: ShortcutRegistry.activate(action)
    }
    ActionShortcut { action: "eq" }
    ActionShortcut { action: "mute" }
    ActionShortcut { action: "nextPreset" }
    ActionShortcut { action: "previousPreset" }
    ActionShortcut { action: "savePreset" }
    ActionShortcut { action: "undo" }
    ActionShortcut { action: "redo" }
}
