import QtQuick
import QtTest
import Isotone

// Settings, General: toggles persist, launch at sign-in writes the test key (the
// test runner sets ISOTONE_RUN_KEY), the graph follows the ranges, and the
// frequency range popover.
Item {
    id: root
    width: 1400
    height: 900

    GraphCard {
        id: card
        x: 780
        width: 600
        height: 420
    }
    SettingsGeneral {
        id: page
        width: 760
    }
    Item {
        id: overlay
        anchors.fill: parent
        z: 100
    }
    Component.onCompleted: UiState.overlay = overlay

    TestCase {
        name: "SettingsGeneral"
        when: windowShown

        function child(name) { return findChild(page, name) }
        function type(text) { for (const c of text) keyClick(c) }
        function graph() { return findChild(card, "responseGraph") }
        function flag(key) { const v = AppSettings.value(key, ""); return v === true || v === "true" }
        function segment(control, index) {
            // The option's own rectangle: Segmented's row holds its delegates and the Repeater.
            const row = control.children[0]
            const options = []
            for (let i = 0; i < row.children.length; ++i)
                if (row.children[i].modelData !== undefined) options.push(row.children[i])
            mouseClick(options[index])
        }

        function init() {
            AppSettings.setValue("graph/gainRange", 15)
            GeneralSettings.setFrequencyRange(20, 20000)
            AppSettings.setValue("spectrum/releaseMs", 300)
            if (Startup.launchAtSignIn) Startup.setLaunchAtSignIn(false, false)
            while (EqSession.count > 0) EqSession.deleteBand(0)
            EqSession.addBand(1000, 6)
            waitForRendering(page)
        }

        function test_toggles_persist() {
            const toggles = [["startInTray", "general/startInTray"], ["keepInTray", "general/keepInTray"],
                             ["switchOnDefaultOutput", "general/switchOnDefaultOutput"],
                             ["autoPreampForNew", "general/autoPreampForNew"], ["peakHold", "spectrum/peakHold"]]
            for (const [name, key] of toggles) {
                const t = child(name)
                verify(t !== null, name)
                const before = t.checked
                mouseClick(t)
                compare(flag(key), !before, key)
                compare(t.checked, !before, name)
                mouseClick(t)
                compare(flag(key), before, key)
            }
            // The defaults are the board's.
            compare(GeneralSettings.keepInTray, true)
            compare(GeneralSettings.peakHold, false)
            // settings.ini gives a saved flag back as text.
            AppSettings.setValue("general/keepInTray", "false")
            compare(GeneralSettings.keepInTray, false)
            verify(!child("keepInTray").checked)
            AppSettings.setValue("general/keepInTray", "true")
            compare(GeneralSettings.keepInTray, true)
        }

        function test_peak_hold_reaches_the_graph() {
            const before = GeneralSettings.peakHold
            mouseClick(child("peakHold"))
            compare(graph().peakHoldVisible, !before)
            mouseClick(child("peakHold"))
            compare(graph().peakHoldVisible, before)
        }

        function test_launch_at_sign_in_writes_the_run_value() {
            verify(Startup.command() === "")
            if (!GeneralSettings.startInTray) mouseClick(child("startInTray"))
            mouseClick(child("launchAtSignIn"))
            verify(Startup.launchAtSignIn)
            verify(child("launchAtSignIn").checked)
            verify(flag("general/launchAtSignIn"))
            const command = Startup.command()
            verify(/^".*ui_qml_tests\.exe" --tray$/.test(command), command)

            mouseClick(child("startInTray"))   // off: the value loses --tray
            verify(/^".*ui_qml_tests\.exe"$/.test(Startup.command()), Startup.command())
            mouseClick(child("startInTray"))
            verify(Startup.command().endsWith(" --tray"))

            mouseClick(child("launchAtSignIn"))
            verify(!Startup.launchAtSignIn)
            compare(Startup.command(), "")
            verify(!flag("general/launchAtSignIn"))
            // Start in the tray alone writes nothing.
            mouseClick(child("startInTray"))
            compare(Startup.command(), "")
            mouseClick(child("startInTray"))
        }

        function test_gain_range_changes_the_graph() {
            const g = graph()
            compare(g.rangeDb, 15)
            const handle = findChild(card, "handle")
            const at15 = handle.y
            segment(child("gainRange"), 2)
            compare(AppSettings.value("graph/gainRange"), 24)
            compare(GeneralSettings.gainRange, 24)
            compare(g.rangeDb, 24)
            // The handle follows: +6 dB is closer to the middle on a wider range.
            tryVerify(() => handle.y > at15)
            fuzzyCompare(handle.y + 20, g.yOf(6), 0.5)
            fuzzyCompare(g.dbAt(g.plotTop), 24, 1e-9)
            segment(child("gainRange"), 0)
            compare(g.rangeDb, 12)
            fuzzyCompare(g.dbAt(g.plotTop), 12, 1e-9)
            compare(child("gainRange").current, 0)
        }

        function test_frequency_range_popover() {
            const g = graph()
            const button = child("frequencyRange")
            compare(button.children[0].text, "20 Hz – 20 kHz")
            mouseClick(button)
            const pop = findChild(overlay, "frequencyRangePopover")
            verify(pop.open)
            const from = findChild(pop, "rangeFrom"), to = findChild(pop, "rangeTo")
            compare(from.text, "20 Hz")
            compare(to.text, "20 kHz")
            // The popover's right edge is the button's.
            fuzzyCompare(pop.panel.x + pop.panel.width, button.mapToItem(overlay, button.width, 0).x, 1)

            mouseClick(from.input)
            from.input.selectAll()
            type("50")
            keyClick(Qt.Key_Return)
            compare(GeneralSettings.minHz, 50)
            compare(Number(AppSettings.value("graph/minHz")), 50)
            compare(g.minHz, 50)
            compare(from.text, "50 Hz")
            compare(button.children[0].text, "50 Hz – 20 kHz")
            // A band below the range has no handle; the handle is where the range puts it.
            const handle = findChild(card, "handle")
            verify(handle.visible)
            fuzzyCompare(handle.x + 20, g.xOf(1000), 0.5)
            EqSession.setFrequency(0, 30)
            verify(!handle.visible)
            EqSession.setFrequency(0, 1000)
            verify(handle.visible)

            // To at or under From is refused, and stays selected.
            mouseClick(to.input)
            to.input.selectAll()
            type("40")
            keyClick(Qt.Key_Return)
            compare(GeneralSettings.maxHz, 20000)
            compare(to.input.selectedText, "40")

            // Clamped to 10 Hz and 24 kHz.
            to.input.selectAll()
            type("30k")
            keyClick(Qt.Key_Return)
            compare(GeneralSettings.maxHz, 24000)
            compare(g.maxHz, 24000)
            mouseClick(from.input)
            from.input.selectAll()
            type("5")
            keyClick(Qt.Key_Return)
            compare(GeneralSettings.minHz, 10)
            compare(button.children[0].text, "10 Hz – 24 kHz")

            // Text that is not a frequency changes nothing.
            from.input.selectAll()
            type("abc")
            keyClick(Qt.Key_Return)
            compare(GeneralSettings.minHz, 10)

            keyClick(Qt.Key_Escape)
            verify(!pop.open)
        }

        function test_resolution_release_and_tilt() {
            segment(child("resolution"), 2)
            compare(Number(AppSettings.value("spectrum/resolution")), 16384)
            compare(GeneralSettings.resolution, 16384)
            segment(child("resolution"), 1)
            compare(GeneralSettings.resolution, 8192)

            segment(child("tilt"), 2)
            compare(GeneralSettings.tilt, 4.5)
            segment(child("tilt"), 0)
            compare(GeneralSettings.tilt, 0)

            const release = child("release")
            compare(release.text, "300 ms")
            mouseClick(release)
            verify(release.editing)
            type("120 ms")
            keyClick(Qt.Key_Return)
            verify(!release.editing)
            compare(GeneralSettings.releaseMs, 120)
            compare(release.text, "120 ms")
            mouseClick(release)
            type("fast")
            keyClick(Qt.Key_Return)
            verify(release.editing)
            keyClick(Qt.Key_Escape)
            compare(GeneralSettings.releaseMs, 120)
        }
    }
}
