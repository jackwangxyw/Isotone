import QtQuick
import QtTest
import Isotone

// Deleting a band shows "Band N deleted" with Undo, which brings that band back.
Item {
    id: root
    width: 800
    height: 600

    PresetPrompts {}

    property string toastText
    property string toastAction
    property var action: null
    property int toasts: 0
    Connections {
        target: UiState
        function onToastRequested(text, actionText, action) {
            root.toastText = text
            root.toastAction = actionText
            root.action = action
            root.toasts++
        }
    }

    TestCase {
        name: "UndoToast"
        when: windowShown

        function id(row) { return EqSession.data(EqSession.index(row, 0), EqSession.BandIdRole) }

        function init() {
            while (EqSession.count > 0) EqSession.deleteBand(0)
            EqSession.addBand(100, -1)
            EqSession.addBand(1000, -2)
            EqSession.addBand(5000, -3)
            root.toasts = 0
            root.action = null
        }

        function test_undo_brings_the_deleted_band_back() {
            const deleted = id(1)
            EqSession.select(1)
            EqSession.deleteBand(1)
            compare(root.toasts, 1)
            compare(root.toastText, "Band 2 deleted")
            compare(root.toastAction, "Undo")
            compare(EqSession.count, 2)
            root.action()
            compare(EqSession.count, 3)
            compare(id(1), deleted)
            compare(EqSession.selectedRow, 1)
        }

        function test_undo_does_nothing_after_another_edit() {
            EqSession.deleteBand(0)
            const undo = root.action
            EqSession.setGain(0, -7)
            EqSession.finishEdit()
            undo()
            compare(EqSession.count, 2)
            EqSession.undo()
            undo()
            compare(EqSession.count, 3)
        }
    }
}
