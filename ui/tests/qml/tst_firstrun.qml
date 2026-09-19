import QtQuick
import QtTest
import Isotone

// First run on the prototype's outputs with devicetool scripted: Outputs,
// approval, Installing, Ready, and Skip.
Item {
    id: root
    width: 1440
    height: 900

    Loader { id: loader; anchors.fill: parent; sourceComponent: FirstRun {} }
    SignalSpy { id: finished; target: loader.item; signalName: "finished" }

    TestCase {
        name: "FirstRun"
        when: windowShown

        // After layout: a button just shown is placed on the next polish.
        function click(item) {
            verify(item !== null)
            wait(30)
            // A button a phase change has just shown is placed on the next polish,
            // which offscreen can take longer than the wait; mouseClick maps the
            // item where it is now, so without this it clicks the old layout.
            for (let i = item; i; i = i.parent) waitForItemPolished(i)
            mouseClick(item)
        }

        function guid(n) { return "{" + String(n).padStart(8, "0") + "-0000-4000-8000-000000000000}" }
        function script(s) { verify(Devicetool.loadScript(JSON.stringify(s))) }
        function wizard() { return loader.item }
        function child(name) { return findChild(wizard(), name) }
        function row(n) { return findChild(wizard(), "setupRow_" + guid(n)) }

        function init() {
            // devicetool, IsoAPO and Equalizer APO: Linux has none of them (devicesmodel_posix.cpp).
            if (Qt.platform.os === "linux") skip("Windows only")
            tryVerify(() => !Devicetool.working, 5000)
            script({ started: false })
            loader.active = false
            loader.active = true
            finished.clear()
            AppSettings.setValue("general/firstRunDone", false)
            waitForRendering(loader.item)
        }

        function test_outputs_step_proposes_isoapo_everywhere() {
            compare(child("wizardTitle").text, "Outputs")
            const choice = findChild(row(8), "engineChoice")
            verify(choice.visible)
            compare(choice.options[choice.current], "IsoAPO")
            verify(child("wizardInstall").active)
            compare(child("wizardInstall").icon, "shield")
            // No Now column.
            compare(findChild(wizard(), "wizardTable").columns.length, 4)
        }

        function test_install_goes_through_approval_installing_and_ready() {
            script({ start: { delay_ms: 300 }, commands: { install: [{ delay_ms: 150 }], "restart-audio": [{ delay_ms: 300 }] } })
            click(child("wizardInstall"))
            tryVerify(() => child("wizardWaiting").visible, 1000)
            compare(child("wizardTitle").text, "Outputs")
            tryCompare(child("wizardTitle"), "text", "Installing", 2000)
            verify(child("wizardOpenWaiting").visible)
            verify(!child("wizardOpenWaiting").active)
            tryCompare(child("wizardTitle"), "text", "Ready", 5000)
            compare(findChild(row(8), "setupStatusText").text, "Installed")
            click(child("wizardOpen"))
            compare(finished.count, 1)
            const done = AppSettings.value("general/firstRunDone", false)
            verify(done === true || done === "true")
        }

        function test_nothing_chosen_nothing_to_install() {
            const rows = findChild(wizard(), "wizardTable").rows
            const choices = {}
            for (const d of rows) choices[d.guid] = "Off"
            wizard().choices = choices
            verify(!child("wizardInstall").active)
        }

        function test_declined_stays_on_outputs() {
            script({ start: { error: 1223 } })
            click(child("wizardInstall"))
            tryVerify(() => Devicetool.phase === "declined", 2000)
            compare(child("wizardTitle").text, "Outputs")
            verify(child("wizardInstall").visible)
        }

        function test_an_approval_that_fails_says_why() {
            // ERROR_FILE_NOT_FOUND from starting serve (devicetool missing).
            script({ start: { error: 2 } })
            click(child("wizardInstall"))
            tryVerify(() => !Devicetool.working && Devicetool.phase === "failed", 3000)
            compare(child("wizardTitle").text, "Outputs")
            verify(child("wizardResult").visible)
            compare(child("wizardResult").text, "The system cannot find the file specified.")
            verify(child("wizardInstall").visible)
        }

        function test_skip_finishes() {
            click(child("wizardSkip"))
            compare(finished.count, 1)
            compare(Devicetool.kind, "")
        }
    }
}
