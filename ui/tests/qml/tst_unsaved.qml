import QtQuick
import QtTest
import Isotone

// Unsaved changes: switching presets asks, and so does the close path through
// PresetActions.confirmUnsaved. No output in a test.
Item {
    id: root
    width: 1440
    height: 900

    Item {
        id: overlay
        anchors.fill: parent
        PresetPrompts {}
    }

    TestCase {
        name: "UnsavedDialog"
        when: windowShown

        property int proceeded: 0
        property int cancelled: 0

        function dialogButton(name) { return findChild(overlay, name) }
        function noDialog() { return findChild(overlay, "dialogCard") === null }
        function title() {
            for (const d of overlay.children) if (d.cardWidth !== undefined && d.visible) return d.title
            return ""
        }
        function type(text) { for (const c of text) keyClick(c) }
        function gain(row) { return EqSession.data(EqSession.index(row, 0), EqSession.GainRole) }

        function initTestCase() { UiState.overlay = overlay }

        function init() {
            while (Presets.count > 0) Presets.remove(Presets.names[0])
            while (EqSession.count > 0) EqSession.deleteBand(0)
            Presets.assign("", "")
            EqSession.addBand(1000, -3)
            Presets.saveAs("A")
            EqSession.addBand(2000, -4)
            Presets.saveAs("B")
            Presets.load("A")
            compare(EqSession.count, 1)
            EqSession.setGain(0, -8)
            EqSession.finishEdit()
            verify(Presets.modified)
            proceeded = 0
            cancelled = 0
        }
        function cleanup() {
            for (const d of overlay.children) if (d.cardWidth !== undefined) d.close()
            tryVerify(noDialog)
        }

        function test_switching_asks_and_cancel_changes_nothing() {
            Presets.load("B")
            verify(dialogButton("unsavedCancel") !== null)
            compare(title(), "Save changes to A?")
            mouseClick(dialogButton("unsavedCancel"))
            tryVerify(noDialog)
            compare(Presets.currentName, "A")
            verify(Presets.modified)
            compare(gain(0), -8)
        }

        function test_escape_cancels() {
            Presets.load("B")
            verify(dialogButton("unsavedCancel") !== null)
            keyClick(Qt.Key_Escape)
            tryVerify(noDialog)
            compare(Presets.currentName, "A")
            verify(Presets.modified)
        }

        function test_dont_save_puts_the_preset_back_then_switches() {
            Presets.next()   // B
            mouseClick(dialogButton("unsavedDiscard"))
            tryVerify(noDialog)
            compare(Presets.currentName, "B")
            compare(EqSession.count, 2)
            verify(!Presets.modified)
            Presets.load("A")
            compare(gain(0), -3)   // A as it was saved
        }

        function test_save_saves_then_switches() {
            Presets.load("B")
            mouseClick(dialogButton("unsavedSave"))
            tryVerify(noDialog)
            compare(Presets.currentName, "B")
            Presets.load("A")
            compare(gain(0), -8)
        }

        function test_closing_asks_before_closing() {
            const d = PresetActions.confirmUnsaved(true, () => proceeded++, () => cancelled++)
            verify(d !== null)
            compare(title(), "Save changes to A before closing?")
            compare(d.cardWidth, 480)
            mouseClick(dialogButton("unsavedCancel"))
            compare(proceeded, 0)
            compare(cancelled, 1)
            tryVerify(noDialog)

            PresetActions.confirmUnsaved(true, () => proceeded++, () => cancelled++)
            mouseClick(dialogButton("unsavedDiscard"))
            compare(proceeded, 1)
            compare(gain(0), -3)
        }

        function test_nothing_unsaved_proceeds_at_once() {
            Presets.revert()
            verify(!Presets.modified)
            verify(PresetActions.confirmUnsaved(true, () => proceeded++) === null)
            compare(proceeded, 1)
            verify(noDialog())
        }

        function test_save_on_an_untitled_output_asks_for_a_name() {
            Presets.revert()
            Presets.newPreset(true)
            EqSession.addBand(500, -1)
            verify(Presets.untitled)
            PresetActions.confirmUnsaved(true, () => proceeded++, () => cancelled++)
            compare(title(), "Save changes to Untitled before closing?")
            mouseClick(dialogButton("unsavedSave"))
            const name = findChild(overlay, "saveAsName")
            verify(name !== null)
            compare(name.text, "")
            verify(!dialogButton("saveAsSave").active)
            tryVerify(() => name.input.activeFocus)
            type("Mine")
            mouseClick(dialogButton("saveAsSave"))
            compare(proceeded, 1)
            compare(cancelled, 0)
            compare(Presets.currentName, "Mine")
            verify(!Presets.modified)
            tryVerify(noDialog)
        }

        function test_cancelling_the_name_cancels() {
            Presets.revert()
            Presets.newPreset(true)
            PresetActions.confirmUnsaved(false, () => proceeded++, () => cancelled++)
            mouseClick(dialogButton("unsavedSave"))
            verify(findChild(overlay, "saveAsCancel") !== null)
            mouseClick(dialogButton("saveAsCancel"))
            compare(proceeded, 0)
            compare(cancelled, 1)
            compare(Presets.currentName, "Untitled")
            tryVerify(noDialog)
        }
    }
}
