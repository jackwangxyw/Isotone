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

        // The owner asked for the output a preset is for to be changeable after it
        // is made (2026-09-15), from the device icon on its row.
        function test_the_output_a_preset_is_for_is_picked_on_its_row() {
            const r = row("Late night")
            compare(r.forOutput, "")            // every output
            compare(r.assigned, "")
            verify(!child(r, "presetScopeList").visible)
            const height = r.height
            hover(r)
            const icon = child(r, "presetScope")
            verify(icon !== null)
            mouseClick(icon)
            const list = child(r, "presetScopeList")
            verify(list.visible)
            // The row grows to hold the choices: click them where they end up.
            waitForItemPolished(r)
            waitForRendering(menu)
            verify(r.height > height)           // the row opens, nothing is clipped
            // The Column holds its delegates and the Repeater itself.
            const choices = []
            for (let i = 0; i < list.children.length; ++i)
                if (list.children[i].modelData !== undefined) choices.push(list.children[i])
            // "All outputs" first, then the machine's own outputs when it has any.
            verify(choices.length >= 1)
            compare(choices[0].modelData.name, "All outputs")
            compare(choices[0].modelData.guid, "")
            mouseClick(choices[0])
            verify(!child(r, "presetScopeList").visible)
            compare(r.forOutput, "")
            compare(Presets.presetOutput("Late night"), "")
            compare(r.height, height)
        }

        // A preset for another output is listed, faded, and does not load when it is
        // clicked (owner, 2026-09-16). No real output here, so one is invented.
        function test_a_preset_for_another_output_is_faded_and_does_not_load() {
            Presets.setPresetOutput("Late night", "{00000000-0000-4000-8000-00000000beef}")
            const r = row("Late night")
            tryVerify(() => r.loadable === false)
            compare(r.assigned, "")          // no such output to name here
            const labels = r.children[2]     // the name and its line
            verify(labels.opacity < 1)
            const before = Presets.currentName
            mouseClick(r, r.width / 2, 22)
            compare(Presets.currentName, before)
            verify(menu.open, "a dead click does not close the popover")

            // Widened from its own row, it loads again. The rows are made again
            // when the model resets, so this one is found and settled first.
            Presets.setPresetOutput("Late night", "")
            tryVerify(() => row("Late night") !== null && row("Late night").loadable === true)
            const again = row("Late night")
            waitForItemPolished(again)
            waitForRendering(menu)
            mouseClick(again, again.width / 2, 22)
            compare(Presets.currentName, "Late night")
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

        function test_rename_hides_the_current_check() {
            const r = row("HD 650 · tuned")
            verify(child(r, "presetCurrentCheck").visible, "the current preset is checked")
            hover(r)
            mouseClick(child(r, "presetRename"))
            tryVerify(() => child(r, "presetRenameInput") !== null)
            verify(!child(r, "presetCurrentCheck").visible, "only the save check shows while renaming")
            verify(child(r, "presetRenameDone").visible)
            keyClick(Qt.Key_Escape)
            tryVerify(() => child(r, "presetCurrentCheck").visible)
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
