import QtQuick
import QtTest
import Isotone

// Settings, Shortcuts: rebinding, the conflict state with Replace, Escape, Global,
// and a global hotkey another app holds.
Item {
    id: root
    width: 900
    height: 900

    SettingsShortcuts {
        id: page
        width: 760
    }

    TestCase {
        name: "SettingsShortcuts"
        when: windowShown

        readonly property var defaults: ({ eq: "Ctrl+E", mute: "Ctrl+M", nextPreset: "Ctrl+Right", previousPreset: "Ctrl+Left",
                                           savePreset: "Ctrl+S", undo: "Ctrl+Z", redo: "Ctrl+Y", delete: "Del" })

        function row(id) { return findChild(page, "shortcut_" + id) }
        function part(id, name) { return findChild(row(id), name) }

        function init() {
            for (const id in defaults) ShortcutRegistry.replace(id, defaults[id])
            for (const id of ["eq", "mute", "nextPreset", "previousPreset"]) {
                ShortcutRegistry.setGlobal(id, true)
                ShortcutRegistry.setGlobalFailed(id, false)
            }
            waitForRendering(page)
        }

        function test_rows_show_their_keys() {
            compare(part("eq", "caps").keys, ["Ctrl", "E"])
            compare(part("nextPreset", "caps").keys, ["Ctrl", "→"])
            compare(part("gain", "caps").keys, ["↑", "↓"])
            compare(part("coarse", "caps").keys, ["Shift"])
            verify(part("eq", "global").visible)
            verify(part("eq", "global").checked)
            verify(!part("undo", "global").visible)
            verify(!part("frequency", "global").visible)
        }

        function test_rebinding() {
            mouseClick(part("nextPreset", "keys"))
            verify(part("nextPreset", "pressKeys").visible)
            verify(!part("nextPreset", "caps").visible)
            verify(ShortcutRegistry.capturing)
            keyClick(Qt.Key_Control)   // a modifier alone waits for more
            verify(part("nextPreset", "pressKeys").visible)
            keyClick(Qt.Key_K, Qt.ControlModifier)
            compare(ShortcutRegistry.sequence("nextPreset"), "Ctrl+K")
            verify(!part("nextPreset", "pressKeys").visible)
            compare(part("nextPreset", "caps").keys, ["Ctrl", "K"])
            verify(!ShortcutRegistry.capturing)
        }

        function test_escape_cancels() {
            mouseClick(part("undo", "keys"))
            verify(part("undo", "pressKeys").visible)
            keyClick(Qt.Key_Escape)
            verify(!part("undo", "pressKeys").visible)
            compare(ShortcutRegistry.sequence("undo"), "Ctrl+Z")
            verify(!ShortcutRegistry.capturing)
        }

        function test_a_press_elsewhere_cancels() {
            mouseClick(part("undo", "keys"))
            verify(ShortcutRegistry.capturing)
            page.forceActiveFocus()
            verify(!part("undo", "pressKeys").visible)
            verify(!ShortcutRegistry.capturing)
        }

        function test_conflict_and_replace() {
            mouseClick(part("nextPreset", "keys"))
            keyClick(Qt.Key_M, Qt.ControlModifier)
            verify(part("nextPreset", "conflict").visible)
            verify(!part("nextPreset", "pressKeys").visible)
            compare(part("nextPreset", "conflictName").text, "Mute")
            compare(ShortcutRegistry.sequence("nextPreset"), "Ctrl+Right")
            const replace = part("nextPreset", "replace")
            verify(replace.visible)
            waitForItemPolished(part("nextPreset", "conflict"))   // laid out before it is clicked
            mouseClick(replace)
            compare(ShortcutRegistry.sequence("nextPreset"), "Ctrl+M")
            compare(ShortcutRegistry.sequence("mute"), "")
            compare(part("mute", "caps").keys, [])
            verify(!part("nextPreset", "conflict").visible)
            verify(!ShortcutRegistry.capturing)
        }

        function test_conflict_with_a_fixed_key_has_no_replace() {
            mouseClick(part("redo", "keys"))
            keyClick(Qt.Key_Left)
            verify(part("redo", "conflict").visible)
            compare(part("redo", "conflictName").text, "Frequency")
            verify(!part("redo", "replace").visible)
            // Other keys still bind from the conflict state.
            keyClick(Qt.Key_R, Qt.ControlModifier)
            compare(ShortcutRegistry.sequence("redo"), "Ctrl+R")
            verify(!part("redo", "conflict").visible)
        }

        function test_fixed_rows_do_not_rebind() {
            mouseClick(part("frequency", "keys"))
            verify(!part("frequency", "pressKeys").visible)
            verify(!ShortcutRegistry.capturing)
            // Delete does.
            mouseClick(part("delete", "keys"))
            verify(part("delete", "pressKeys").visible)
            keyClick(Qt.Key_Backspace)
            compare(ShortcutRegistry.sequence("delete"), "Backspace")
        }

        function test_global_toggle() {
            mouseClick(part("mute", "global"))
            verify(!ShortcutRegistry.isGlobal("mute"))
            verify(!part("mute", "global").checked)
            mouseClick(part("mute", "global"))
            verify(ShortcutRegistry.isGlobal("mute"))
        }

        function test_global_keys_another_app_holds() {
            verify(!part("eq", "inUse").visible)
            ShortcutRegistry.setGlobalFailed("eq", true)
            verify(part("eq", "inUse").visible)
            verify(Qt.colorEqual(part("eq", "caps").colour, Theme.danger))
            ShortcutRegistry.setGlobal("eq", false)   // off: nothing to hold
            verify(!part("eq", "inUse").visible)
        }
    }
}
