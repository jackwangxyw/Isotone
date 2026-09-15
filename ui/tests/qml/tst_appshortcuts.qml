import QtQuick
import QtTest
import Isotone

// The app's shortcuts perform their actions; the selected band's keys edit the
// band and commit once per press or hold; following the default output.
Item {
    id: root
    width: 400
    height: 300

    AppShortcuts { id: shortcuts }
    BandKeys {
        id: bandKeys
        session: EqSession
    }
    TextInput {
        id: field
        width: 100
        height: 20
        text: "text"
    }
    Item { id: elsewhere; focus: true }

    QtObject {
        id: fakeOutputs
        signal defaultOutputChanged()
        property int selects: 0
        function selectDefault() { selects++; return true }
    }
    DefaultOutputFollower {
        id: follower
        outputs: fakeOutputs
    }

    SignalSpy { id: finished; target: bandKeys; signalName: "editFinished" }

    TestCase {
        name: "AppShortcuts"
        when: windowShown

        function role(r) { return EqSession.data(EqSession.index(0, 0), r) }

        function init() {
            for (const [id, keys] of [["eq", "Ctrl+E"], ["mute", "Ctrl+M"], ["nextPreset", "Ctrl+Right"], ["previousPreset", "Ctrl+Left"]])
                ShortcutRegistry.replace(id, keys)
            ShortcutRegistry.capturing = false
            EqSession.eqOn = true
            EqSession.muted = false
            while (EqSession.count > 0) EqSession.deleteBand(0)
            EqSession.byFrequency = false
            EqSession.addBand(1000, 0)
            bandKeys.active = true
            elsewhere.forceActiveFocus()
            finished.clear()
        }

        function test_activate_performs_the_action() {
            ShortcutRegistry.activate("eq")
            verify(!EqSession.eqOn)
            ShortcutRegistry.activate("eq")
            verify(EqSession.eqOn)
            ShortcutRegistry.activate("mute")
            verify(EqSession.muted)
            ShortcutRegistry.activate("mute")
            verify(!EqSession.muted)
            // The foundation's stubs; the presets package implements them.
            ShortcutRegistry.activate("nextPreset")
            ShortcutRegistry.activate("undo")
        }

        function test_keys_in_the_window() {
            keyClick(Qt.Key_E, Qt.ControlModifier)
            verify(!EqSession.eqOn)
            keyClick(Qt.Key_M, Qt.ControlModifier)
            verify(EqSession.muted)

            // Rebound: the old keys do nothing.
            ShortcutRegistry.rebind("eq", "Ctrl+Shift+E")
            keyClick(Qt.Key_E, Qt.ControlModifier)
            verify(!EqSession.eqOn)
            keyClick(Qt.Key_E, Qt.ControlModifier | Qt.ShiftModifier)
            verify(EqSession.eqOn)

            // Not while the Shortcuts page waits for keys.
            ShortcutRegistry.capturing = true
            keyClick(Qt.Key_M, Qt.ControlModifier)
            verify(EqSession.muted)
        }

        function test_selected_band_gain_frequency_width() {
            keyClick(Qt.Key_Up)
            fuzzyCompare(role(EqSession.GainRole), 0.1, 1e-9)
            keyClick(Qt.Key_Up, Qt.ShiftModifier)
            fuzzyCompare(role(EqSession.GainRole), 0.7, 1e-9)
            keyClick(Qt.Key_Down)
            fuzzyCompare(role(EqSession.GainRole), 0.6, 1e-9)

            keyClick(Qt.Key_Right)
            fuzzyCompare(role(EqSession.FrequencyRole), 1000 * Math.pow(2, 1 / 48), 1e-6)
            keyClick(Qt.Key_Left, Qt.ShiftModifier)
            fuzzyCompare(role(EqSession.FrequencyRole), 1000 * Math.pow(2, -5 / 48), 1e-6)

            const q = role(EqSession.QRole)
            keyClick(Qt.Key_BracketRight)
            fuzzyCompare(role(EqSession.QRole), q * 1.05, 1e-9)
            keyClick(Qt.Key_BracketLeft)
            fuzzyCompare(role(EqSession.QRole), q, 1e-9)
            keyClick(Qt.Key_BraceRight, Qt.ShiftModifier)   // Shift+] on a US layout
            fuzzyCompare(role(EqSession.QRole), q * Math.pow(1.05, 6), 1e-9)
        }

        function test_each_press_commits_once_and_a_hold_commits_on_release() {
            keyClick(Qt.Key_Up)
            compare(finished.count, 1)
            keyClick(Qt.Key_Down)
            compare(finished.count, 2)

            // Held: every repeat edits, the release commits once.
            keyPress(Qt.Key_Up)
            keyPress(Qt.Key_Up)
            keyPress(Qt.Key_Up)
            compare(finished.count, 2)
            fuzzyCompare(role(EqSession.GainRole), 0.3, 1e-9)
            keyRelease(Qt.Key_Up)
            compare(finished.count, 3)
        }

        function test_not_while_typing_nor_with_ctrl_nor_elsewhere() {
            field.forceActiveFocus()
            keyClick(Qt.Key_Up)
            keyClick(Qt.Key_Right)
            elsewhere.forceActiveFocus()
            fuzzyCompare(role(EqSession.GainRole), 0, 1e-9)
            fuzzyCompare(role(EqSession.FrequencyRole), 1000, 1e-9)

            keyClick(Qt.Key_Up, Qt.AltModifier)
            fuzzyCompare(role(EqSession.GainRole), 0, 1e-9)

            bandKeys.active = false
            keyClick(Qt.Key_Up)
            fuzzyCompare(role(EqSession.GainRole), 0, 1e-9)
            compare(finished.count, 0)
        }

        function test_following_the_default_output() {
            AppSettings.setValue("general/switchOnDefaultOutput", true)
            fakeOutputs.selects = 0
            fakeOutputs.defaultOutputChanged()
            compare(fakeOutputs.selects, 1)
            AppSettings.setValue("general/switchOnDefaultOutput", false)
            fakeOutputs.defaultOutputChanged()
            compare(fakeOutputs.selects, 1)
            AppSettings.setValue("general/switchOnDefaultOutput", true)
        }
    }
}
