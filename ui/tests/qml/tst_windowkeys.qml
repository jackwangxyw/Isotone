import QtQuick
import QtTest
import Isotone

// Main's keys, presses and closing, on the pieces Main uses (WindowKeys,
// PressWatch, WindowClose, PresetPrompts) laid out as Main lays them out. The
// window is a stand-in, so a close hides nothing and a quit ends nothing. No
// output in a test.
Item {
    id: root
    width: 1440
    height: 900

    property int quits: 0
    function dialogs() {
        const out = []
        for (const d of Array.from(overlay.children)) if (d.cardWidth !== undefined) out.push(d)
        return out
    }

    QtObject {
        id: fakeWindow
        property bool visible: true
        property int shows: 0
        property int hides: 0
        property int dialogsAtShow: -1
        function show() { visible = true; shows++; dialogsAtShow = root.dialogs().length }
        function raise() {}
        function requestActivate() {}
        function hide() { visible = false; hides++ }
    }

    WindowKeys {
        overlay: overlay
        firstRun: firstRun
    }
    WindowClose {
        id: closer
        host: fakeWindow
        onQuit: root.quits++
    }

    Item {
        id: content
        anchors.fill: parent
        TextInput { id: typing; x: 20; y: 700; width: 200; height: 20; text: "some text" }
    }

    Item {
        id: overlay
        anchors.fill: parent
        z: 100
        BandMenu { id: bandMenu; anchors.fill: parent }
        PresetsMenu { id: presetsMenu; anchors.fill: parent }
        PresetPrompts { host: fakeWindow }
        Popover {
            id: outputsPopover
            panelWidth: 280
            Item { width: 260; height: 80 }
        }
    }
    Loader {
        id: firstRun
        anchors.fill: parent
        z: 1500
        active: false
        sourceComponent: FirstRun {}
    }

    PressWatch {
        content: content
        overlay: overlay
    }

    TestCase {
        name: "WindowKeys"
        when: windowShown

        function gain(row) { return EqSession.data(EqSession.index(row, 0), EqSession.GainRole) }
        function within(item, ancestor) {
            for (let p = item; p; p = p.parent) if (p === ancestor) return true
            return false
        }
        function focusItem() { return root.Window.activeFocusItem }

        // Delete, Shift+Up and Ctrl+Z change nothing on the band.
        function expectBandUntouched(what) {
            const row = EqSession.selectedRow
            const before = gain(row)
            keyClick(Qt.Key_Delete)
            compare(EqSession.count, 2, "Delete deleted a band under " + what)
            keyClick(Qt.Key_Up, Qt.ShiftModifier)
            fuzzyCompare(gain(row), before, 1e-9, "Up moved the band under " + what)
            keyClick(Qt.Key_Z, Qt.ControlModifier)
            fuzzyCompare(gain(0), -8, 1e-9, "Ctrl+Z undid under " + what)
        }

        function initTestCase() {
            UiState.overlay = overlay
            UiState.view = "eq"
            AppSettings.setValue("general/keepInTray", true)
        }

        function init() {
            for (const d of root.dialogs()) d.close()
            tryVerify(() => root.dialogs().length === 0)
            presetsMenu.close()
            bandMenu.close()
            outputsPopover.close()
            firstRun.active = false
            while (Presets.count > 0) Presets.remove(Presets.names[0])
            while (EqSession.count > 0) EqSession.deleteBand(0)
            Presets.assign("", "")
            EqSession.addBand(1000, -3)
            EqSession.addBand(2000, -4)
            Presets.saveAs("A")
            EqSession.setGain(0, -8)
            EqSession.finishEdit()
            verify(Presets.modified)
            EqSession.select(1)
            fakeWindow.visible = true
            fakeWindow.shows = 0
            fakeWindow.hides = 0
            fakeWindow.dialogsAtShow = -1
            root.quits = 0
            content.forceActiveFocus()
        }

        function test_keys_act_on_the_band_with_the_focus_in_the_window() {
            keyClick(Qt.Key_Up)
            fuzzyCompare(gain(1), -3.9, 1e-9)
            keyClick(Qt.Key_Z, Qt.ControlModifier)
            fuzzyCompare(gain(1), -4, 1e-9)
            keyClick(Qt.Key_Delete)
            compare(EqSession.count, 1)
        }

        function test_not_under_the_unsaved_dialog() {
            const d = PresetActions.confirmUnsaved(false, () => {})
            verify(d !== null)
            verify(within(focusItem(), d), "focus in the dialog")
            expectBandUntouched("the unsaved dialog")
            // Global hotkeys and the tray still act.
            ShortcutRegistry.activate("eq")
            verify(!EqSession.eqOn)
            ShortcutRegistry.activate("eq")
            verify(EqSession.eqOn)
        }

        function test_keys_come_back_when_the_dialog_closes() {
            PresetActions.confirmUnsaved(false, () => {})
            keyClick(Qt.Key_Escape)
            tryVerify(() => root.dialogs().length === 0)
            keyClick(Qt.Key_Delete)
            compare(EqSession.count, 1)
        }

        function test_not_under_the_presets_popover() {
            presetsMenu.openAt(280, 58)
            verify(presetsMenu.open)
            expectBandUntouched("the presets popover")
        }

        function test_not_under_the_band_menu() {
            bandMenu.openAt(1, 600, 500, 600)
            verify(bandMenu.open)
            expectBandUntouched("the band menu")
        }

        function test_not_under_the_outputs_popover() {
            outputsPopover.openAt(100, 500)
            verify(outputsPopover.open)
            expectBandUntouched("the outputs popover")
        }

        function test_not_under_first_run() {
            firstRun.active = true
            tryVerify(() => firstRun.item !== null)
            expectBandUntouched("first run")
            firstRun.active = false
        }

        function test_typing_keeps_its_keys() {
            typing.forceActiveFocus()
            typing.cursorPosition = 2
            keyClick(Qt.Key_Z, Qt.ControlModifier)
            fuzzyCompare(gain(0), -8, 1e-9, "Ctrl+Z undid while typing")
            keyClick(Qt.Key_Delete)
            compare(EqSession.count, 2, "Delete deleted while typing")
            keyClick(Qt.Key_Up)
            fuzzyCompare(gain(1), -4, 1e-9)
        }

        function test_ctrl_s_on_an_untitled_output_opens_save_as() {
            Presets.assign("", "")
            verify(Presets.untitled)
            keyClick(Qt.Key_S, Qt.ControlModifier)
            verify(findChild(overlay, "saveAsName") !== null, "Ctrl+S opened no Save as")
            keyClick(Qt.Key_Escape)
            tryVerify(() => root.dialogs().length === 0)
        }

        // A press in a dialog, off the field being typed in, keeps the focus in it.
        function test_a_press_inside_a_dialog_keeps_its_keys() {
            const d = PresetActions.saveAs()
            verify(d !== null)
            const field = findChild(d, "saveAsName")
            tryVerify(() => field.input.activeFocus)
            mouseClick(findChild(d, "dialogCard"), 40, 30)   // the title
            verify(within(focusItem(), d), "focus left the dialog: " + focusItem())
            keyClick(Qt.Key_Delete)
            compare(EqSession.count, 2)
            keyClick(Qt.Key_Escape)
            tryVerify(() => root.dialogs().length === 0)
        }

        function test_a_press_inside_a_popover_keeps_its_escape() {
            presetsMenu.openAt(280, 58)
            const search = findChild(presetsMenu, "presetSearch")
            mouseClick(search)
            tryVerify(() => within(focusItem(), search))
            const panel = presetsMenu.panel
            mouseClick(panel, panel.width - 6, 6)   // the panel's corner, off the field
            verify(presetsMenu.open)
            verify(within(focusItem(), presetsMenu), "focus left the popover: " + focusItem())
            keyClick(Qt.Key_Escape)
            verify(!presetsMenu.open)
        }

        function test_keys_come_back_when_a_popover_closes() {
            presetsMenu.openAt(280, 58)
            keyClick(Qt.Key_Escape)
            verify(!presetsMenu.open)
            keyClick(Qt.Key_Up)
            fuzzyCompare(gain(1), -3.9, 1e-9)

            presetsMenu.openAt(280, 58)
            mouseClick(findChild(presetsMenu, "presetSearch"))
            mouseClick(content, 900, 700)   // outside the panel
            verify(!presetsMenu.open)
            keyClick(Qt.Key_Delete)
            compare(EqSession.count, 1)
        }

        function test_a_press_elsewhere_ends_typing() {
            typing.forceActiveFocus()
            mouseClick(content, 700, 400)
            verify(!typing.activeFocus)
            verify(content.activeFocus)
        }

        function test_a_second_close_raises_the_one_dialog() {
            closer.requestClose()
            compare(root.dialogs().length, 1)
            closer.requestClose()   // the window's X again
            compare(root.dialogs().length, 1)
            verify(fakeWindow.shows > 0)
            mouseClick(findChild(root.dialogs()[0], "unsavedDiscard"))
            compare(fakeWindow.hides, 1)
            tryVerify(() => root.dialogs().length === 0)
            compare(root.quits, 0)
        }

        function test_quit_while_closing_asks_once_and_quits() {
            closer.requestClose()
            fakeWindow.hide()
            closer.requestQuit()   // the tray's Quit, with the dialog left in the hidden window
            verify(fakeWindow.visible)
            compare(root.dialogs().length, 1)
            mouseClick(findChild(root.dialogs()[0], "unsavedDiscard"))
            compare(root.quits, 1)
            compare(fakeWindow.hides, 1)
        }

        function test_cancel_then_close_asks_again() {
            closer.requestClose()
            mouseClick(findChild(root.dialogs()[0], "unsavedCancel"))
            tryVerify(() => root.dialogs().length === 0)
            compare(fakeWindow.hides, 0)
            closer.requestClose()
            compare(root.dialogs().length, 1)
            mouseClick(findChild(root.dialogs()[0], "unsavedDiscard"))
            compare(fakeWindow.hides, 1)
            compare(root.quits, 0)
        }

        function test_nothing_unsaved_closes_at_once() {
            Presets.revert()
            closer.requestClose()
            compare(fakeWindow.hides, 1)
            compare(root.dialogs().length, 0)
            closer.requestQuit()
            compare(root.quits, 1)
        }

        // The tray's Preset menu and the Next and Previous preset hotkeys call Presets.load.
        function test_a_preset_picked_from_the_tray_shows_the_window_first() {
            Presets.revert()
            EqSession.addBand(4000, 1)
            Presets.saveAs("B")
            Presets.load("A")
            EqSession.setGain(0, -8)
            EqSession.finishEdit()
            verify(Presets.modified)
            fakeWindow.hide()
            Presets.load("B")
            verify(fakeWindow.visible, "the window was not shown")
            compare(fakeWindow.dialogsAtShow, 0)
            compare(root.dialogs().length, 1)
            Presets.next()   // a hotkey again: the same dialog
            compare(root.dialogs().length, 1)
            mouseClick(findChild(root.dialogs()[0], "unsavedDiscard"))
            compare(Presets.currentName, "B")
            tryVerify(() => root.dialogs().length === 0)
        }
    }
}
