import QtQuick
import QtTest
import Isotone

// The update notice: nothing until a newer release is found, then over the
// bottom right while the window is open; it fades out on its own, held while
// the pointer is over it, and Later or the close button put it away.
Item {
    id: root
    width: 1000
    height: 700

    UpdateNotice { id: notice; seconds: 1 }

    SignalSpy { id: finished; target: UpdateCheck; signalName: "finished" }

    TestCase {
        name: "UpdateNotice"
        when: windowShown

        function child(name) { return findChild(notice, name) }
        // Put away, then brought back as a new start would: shown by itself once
        // a release is found and the window is open.
        function reset() {
            notice.dismiss()
            tryVerify(() => !notice.visible, 1000)
        }
        function show() {
            notice.dismissed = false
            tryVerify(() => notice.visible && notice.opacity === 1, 1000)
        }
        // A release newer than any this build could be, from a file.
        function find() {
            if (UpdateCheck.latest !== "") return
            finished.clear()
            UpdateCheck.check(Qt.resolvedUrl("data/update-latest.json"))
            finished.wait(5000)
            compare(UpdateCheck.latest, "99.0.0")
        }

        function test_a_nothing_found_shows_nothing() {
            // First: nothing has been checked yet.
            compare(UpdateCheck.latest, "")
            wait(300)
            verify(!notice.visible)
        }

        function test_b_it_shows_the_version_found_and_the_current_one() {
            find()
            tryVerify(() => notice.visible && notice.opacity === 1, 1000, "shown as soon as it is found")
            compare(child("updateTitle").text, "Isotone 99.0.0 is available")
            compare(child("updateCurrent").text, "Current version: " + UpdateCheck.current)
            // Bottom right, 24 px in.
            compare(notice.x + notice.width, root.width - 24)
            compare(notice.y + notice.height, root.height - 24)
        }

        function test_c_it_waits_for_the_window() {
            find()
            reset()
            const w = root.Window.window
            w.visible = false
            notice.dismissed = false
            wait(300)
            verify(!notice.visible, "silent while the window is hidden, as in the tray")
            w.visible = true
            tryVerify(() => notice.visible && notice.opacity === 1, 2000, "shown once it opens")
        }

        function test_d_it_fades_out_on_its_own() {
            find()
            reset()
            show()
            // seconds: 1, then a 250 ms fade.
            tryVerify(() => !notice.visible, 3000)
            verify(notice.dismissed, "and stays away until the next start")
        }

        function test_e_the_pointer_holds_it() {
            find()
            reset()
            show()
            mouseMove(notice, 60, 20)
            wait(1800)
            compare(notice.opacity, 1, "held under the pointer")
            mouseMove(root, 10, 10)
            tryVerify(() => !notice.visible, 3000, "and goes once it leaves")
        }

        function test_f_later_and_close_put_it_away() {
            find()
            for (const name of ["updateLater", "updateClose"]) {
                reset()
                show()
                mouseClick(child(name))
                tryVerify(() => !notice.visible, 1000, name)
                verify(notice.dismissed, name)
            }
        }
    }
}
