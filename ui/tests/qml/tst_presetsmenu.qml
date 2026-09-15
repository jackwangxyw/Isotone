import QtQuick
import QtTest
import Isotone

// The presets popover: search, load, rename, duplicate, delete, and the footer.
// No output in a test: presets load into the session in memory only.
Item {
    id: root
    width: 1440
    height: 900

    Item {
        id: overlay
        anchors.fill: parent
        PresetsMenu { id: menu; anchors.fill: parent }
        PresetPrompts {}
    }

    TestCase {
        name: "PresetsMenu"
        when: windowShown

        function row(name) { return findChild(menu, "presetRow_" + name) }
        function child(item, name) { return findChild(item, name) }
        function type(text) { for (const c of text) keyClick(c) }
        function hover(item) { mouseMove(item, item.width / 2, item.height / 2) }

        function clearAll() {
            for (const d of overlay.children) if (d.cardWidth !== undefined) d.close()   // dialogs left open
            while (Presets.count > 0) Presets.remove(Presets.names[0])
            while (EqSession.count > 0) EqSession.deleteBand(0)
            Presets.assign("", "")   // untitled, with nothing to save
        }

        function initTestCase() { UiState.overlay = overlay }

        function init() {
            menu.close()
            clearAll()
            EqSession.addBand(1000, -3)
            compare(Presets.saveAs("Late night"), "Late night")
            EqSession.addBand(3000, -2)
            compare(Presets.saveAs("HD 650 · tuned"), "HD 650 · tuned")
            verify(!Presets.modified)
            menu.openAt(280, 58)
            tryVerify(() => row("Late night") !== null && row("Late night").width > 0)
            waitForRendering(menu)
        }
        function cleanup() {
            menu.close()
            clearAll()
        }

        function test_lists_every_preset_with_the_current_checked_and_assigned_outputs() {
            verify(row("Late night").visible)
            verify(row("HD 650 · tuned").visible)
            verify(row("HD 650 · tuned").current)
            verify(!row("Late night").current)
            // No output here; assigned outputs are the ones Presets knows by GUID.
            compare(row("Late night").assigned, "")
        }

        function test_search_filters_the_list() {
            mouseClick(child(menu, "presetSearch"))
            type("NIGHT")
            verify(row("Late night").visible)
            verify(!row("HD 650 · tuned").visible)
            keyClick(Qt.Key_Backspace)
            for (let i = 0; i < 5; ++i) keyClick(Qt.Key_Backspace)
            verify(row("HD 650 · tuned").visible)
        }

        function test_a_row_loads_its_preset_and_closes() {
            mouseClick(row("Late night"))
            verify(!menu.open)
            compare(Presets.currentName, "Late night")
            compare(EqSession.count, 1)
            verify(!Presets.modified)
        }

        function test_icons_show_on_the_hovered_row_only() {
            hover(row("Late night"))
            verify(child(row("Late night"), "presetRename").visible)
            verify(!child(row("HD 650 · tuned"), "presetRename").visible)
        }

        function test_rename_inline() {
            hover(row("Late night"))
            mouseClick(child(row("Late night"), "presetRename"))
            const input = child(row("Late night"), "presetRenameInput")
            verify(input !== null)
            tryVerify(() => input.activeFocus)
            compare(input.selectedText, "Late night")
            type("Night")
            keyClick(Qt.Key_Return)
            compare(Presets.names.indexOf("Night") >= 0, true)
            compare(Presets.names.indexOf("Late night"), -1)
            verify(menu.open)
        }

        function test_rename_escape_keeps_the_name() {
            hover(row("Late night"))
            mouseClick(child(row("Late night"), "presetRename"))
            const input = child(row("Late night"), "presetRenameInput")
            tryVerify(() => input.activeFocus)
            type("Other")
            keyClick(Qt.Key_Escape)
            verify(Presets.names.indexOf("Late night") >= 0)
            tryVerify(() => child(row("Late night"), "presetRenameInput") === null)
        }

        function test_rename_current_renames_the_title() {
            hover(row("HD 650 · tuned"))
            mouseClick(child(row("HD 650 · tuned"), "presetRename"))
            const input = child(row("HD 650 · tuned"), "presetRenameInput")
            tryVerify(() => input.activeFocus)
            type("Tuned")
            mouseClick(child(row("HD 650 · tuned"), "presetRenameDone"))
            compare(Presets.currentName, "Tuned")
        }

        function test_duplicate() {
            hover(row("Late night"))
            mouseClick(child(row("Late night"), "presetDuplicate"))
            compare(Presets.names, ["HD 650 · tuned", "Late night", "Late night 2"])
            compare(Presets.currentName, "HD 650 · tuned")
        }

        function test_delete_asks_inline() {
            hover(row("Late night"))
            mouseClick(child(row("Late night"), "presetDelete"))
            verify(row("Late night").confirming)
            mouseClick(child(row("Late night"), "presetDeleteCancel"))
            verify(!row("Late night").confirming)
            compare(Presets.count, 2)

            hover(row("Late night"))
            mouseClick(child(row("Late night"), "presetDelete"))
            mouseClick(child(row("Late night"), "presetDeleteConfirm"))
            compare(Presets.names, ["HD 650 · tuned"])
        }

        function test_save_is_greyed_until_there_is_something_to_save() {
            const save = child(menu, "presetSave")
            verify(!save.active)
            mouseClick(save)
            verify(!Presets.modified)
            EqSession.setGain(0, -9)
            EqSession.finishEdit()
            verify(Presets.modified)
            verify(save.active)
            mouseClick(save)
            verify(!Presets.modified)
            verify(!save.active)
        }

        function test_new_is_untitled_and_flat() {
            mouseClick(child(menu, "presetNew"))
            verify(!menu.open)
            compare(Presets.currentName, "Untitled")
            compare(EqSession.count, 0)
            verify(Presets.modified)
            verify(EqSession.autoPreamp)
        }

        function test_a_load_over_unsaved_changes_asks_first() {
            EqSession.setGain(0, -9)
            EqSession.finishEdit()
            mouseClick(row("Late night"))
            verify(!menu.open)
            compare(Presets.currentName, "HD 650 · tuned")
            const discard = findChild(overlay, "unsavedDiscard")
            verify(discard !== null)
            mouseClick(discard)
            compare(Presets.currentName, "Late night")
        }
    }
}
