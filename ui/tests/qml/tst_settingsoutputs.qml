import QtQuick
import QtTest
import Isotone

// Settings, Outputs on the prototype's outputs with devicetool scripted: Change
// asks for approval, the engine choices and the Apply count, applying and done.
Item {
    id: root
    width: 1100
    height: 900

    SettingsOutputs { id: page; width: parent.width }

    TestCase {
        name: "SettingsOutputs"
        when: windowShown

        // After layout: a button just shown is placed on the next polish.
        function click(item) {
            verify(item !== null)
            wait(30)
            mouseClick(item)
        }

        function guid(n) { return "{" + String(n).padStart(8, "0") + "-0000-4000-8000-000000000000}" }
        function script(s) { verify(Devicetool.loadScript(JSON.stringify(s))) }
        function child(name) { return findChild(page, name) }
        function row(n) { return findChild(page, "setupRow_" + guid(n)) }
        function statusText(n) { return findChild(row(n), "setupStatusText").text }
        function choice(n) { return findChild(row(n), "engineChoice") }

        function init() {
            tryVerify(() => !Devicetool.working, 5000)
            script({ started: false })
            page.localPhase = "locked"
            waitForRendering(page)
        }

        function test_locked_lists_the_active_outputs_with_their_engine_now() {
            verify(child("outputsChange").visible)
            verify(row(9) === null, "unplugged outputs are not in the table")
            verify(row(1) !== null)
            verify(!choice(1).visible)
            compare(statusText(4), "Detached")
            compare(statusText(1), "")
        }

        function test_change_asks_for_approval_then_edits() {
            script({ start: { delay_ms: 300 } })
            click(child("outputsChange"))
            verify(child("outputsWaiting").visible)
            tryVerify(() => child("outputsApply").visible, 2000)
            verify(choice(1).visible)
            // CABLE Input has both: IsoAPO is proposed, which is a change.
            compare(choice(5).options[choice(5).current], "IsoAPO")
            compare(child("outputsApply").text, "Apply 1")
            verify(row(5).color.a > 0)
            verify(row(1).color.a === 0)
        }

        function test_declined_approval_stays_locked() {
            script({ start: { error: 1223 } })
            click(child("outputsChange"))
            tryVerify(() => child("outputsChange").visible, 2000)
            verify(!child("outputsApply").visible)
        }

        function test_equalizer_apo_is_offered_only_where_it_is() {
            script({ started: true })
            click(child("outputsChange"))
            compare(choice(3).options, ["IsoAPO", "Equalizer APO", "Off"])
            compare(choice(8).options, ["IsoAPO", "Off"])
        }

        function test_picks_count_and_cancel_puts_them_back() {
            script({ started: true })
            click(child("outputsChange"))
            choice(8).picked(0)   // TV: IsoAPO
            choice(5).picked(2)   // CABLE Input: Off
            compare(child("outputsApply").text, "Apply 2")
            choice(5).picked(0)
            compare(child("outputsApply").text, "Apply 2")
            choice(8).picked(1)
            compare(child("outputsApply").text, "Apply 1")
            click(child("outputsCancel"))
            verify(child("outputsChange").visible)
        }

        function test_apply_shows_each_row_then_done() {
            script({ started: true, commands: { install: [{ delay_ms: 200 }], "restart-audio": [{ delay_ms: 200 }] } })
            click(child("outputsChange"))
            choice(8).picked(0)
            compare(child("outputsApply").text, "Apply 2")
            click(child("outputsApply"))
            tryVerify(() => child("outputsApplying").visible, 1000)
            tryCompare(findChild(row(5), "setupStatusText"), "text", "Installing", 1000)
            compare(statusText(8), "Queued")
            compare(statusText(1), String.fromCharCode(0x2014))   // unchanged
            tryVerify(() => child("outputsResult").visible, 3000)
            compare(child("outputsResult").text, "Applied · audio restarted")
            compare(statusText(5), "Installed")
            compare(statusText(8), "Installed")
            click(child("outputsDone"))
            verify(child("outputsChange").visible)
        }

        function test_a_failed_row_is_marked_and_the_reason_shown() {
            script({ started: true, commands: { install: [{ exit: 1, json: { reason: "registration checks failed" } }] } })
            click(child("outputsChange"))
            click(child("outputsApply"))
            tryVerify(() => child("outputsResult").visible, 3000)
            compare(child("outputsResult").text, "Registration checks failed.")
            compare(statusText(5), "Failed")
        }
    }
}
