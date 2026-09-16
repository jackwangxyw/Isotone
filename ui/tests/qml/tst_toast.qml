import QtQuick
import QtTest
import Isotone

// The toast UiState.toast() raises: it fades in, waits about 3 s, fades out,
// and a second one takes over the same panel.
Item {
    id: root
    width: 800
    height: 600

    property int actions: 0

    Toast { id: toast }

    TestCase {
        name: "Toast"
        when: windowShown

        function label() { return findChild(toast, "toastText") }
        function actionButton() { return findChild(toast, "toastAction") }

        function init() {
            root.actions = 0
            toast.hide()
            tryVerify(() => !toast.visible, 1000)
        }

        function test_it_fades_in_rather_than_appearing() {
            UiState.toast("Band 2 deleted", "Undo", () => root.actions++)
            verify(toast.visible)
            verify(toast.opacity < 1, "not there at once, opacity " + toast.opacity)
            tryVerify(() => toast.opacity === 1, 1000, "and reaches full, opacity " + toast.opacity)
            compare(label().text, "Band 2 deleted")
        }

        function test_it_goes_after_about_three_seconds() {
            UiState.toast("Band 2 deleted", "Undo", null)
            wait(2000)
            compare(toast.opacity, 1, "still there at 2 s")
            // Gone by 3.6 s: 3 s of waiting and a 250 ms fade.
            tryVerify(() => !toast.visible, 1600, "gone soon after 3 s")
        }

        function test_it_fades_out_rather_than_vanishing() {
            UiState.toast("Band 2 deleted", "Undo", null)
            tryVerify(() => toast.opacity === 1, 1000)
            tryVerify(() => toast.opacity < 1, 4000, "the wait ends")
            verify(toast.opacity > 0, "still on its way out, opacity " + toast.opacity)
            verify(toast.visible, "and still shown while it fades")
        }

        function test_a_second_toast_starts_the_wait_again() {
            UiState.toast("Band 2 deleted", "Undo", null)
            wait(2000)
            UiState.toast("Band 3 deleted", "Undo", null)
            compare(label().text, "Band 3 deleted")
            wait(2000)
            compare(toast.opacity, 1, "the second toast's own 3 s, not the first's")
            tryVerify(() => !toast.visible, 1600)
        }

        function test_the_action_runs_on_a_click_and_the_toast_goes() {
            UiState.toast("Band 2 deleted", "Undo", () => root.actions++)
            tryVerify(() => toast.opacity === 1, 1000)
            mouseClick(actionButton())
            compare(root.actions, 1)
            tryVerify(() => !toast.visible, 1000)
        }
    }
}
