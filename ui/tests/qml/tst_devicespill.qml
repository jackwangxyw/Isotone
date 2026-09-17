import QtQuick
import QtTest
import Isotone

// The top bar's status pill for an output whose engine stopped working, and the
// Devices view's Equalizer APO card when it is installed on no output. Each test
// loads its own outputs.
Item {
    id: root
    width: 1192
    height: 900

    TopBar { id: bar; width: parent.width }
    DevicesView { id: view; y: 80; width: parent.width; height: 820 }

    TestCase {
        name: "DevicesPill"
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

        // The output the top bar looks at: the current one, or with none working the default.
        readonly property string target: Outputs.currentGuid !== "" ? Outputs.currentGuid : "{00000099-0000-4000-8000-000000000000}"

        function status(guid, state, backend, extra) {
            return Object.assign({
                command: "status", guid: guid,
                device: { name: "Adapter", connection: "Output", default_device: true, disabled: false, unplugged: false,
                          channels: 2, sample_rate: 48000, enhancements_disabled: false },
                backend: backend, effect_slots: [], isoapo: { state: state, remedies: [] }
            }, extra || {})
        }
        function load(devices, eapo) {
            verify(Devices.loadScript(JSON.stringify({ devices: devices, equalizer_apo: eapo || { installed: false } })))
            waitForRendering(bar)
        }
        function pill() { return findChild(bar, "engineStatus") }

        function init() {
            UiState.view = "eq"
            UiState.devicesSelection = ""
        }

        // The output's name left the top bar (2026-09-15), and its width was still
        // read with the sidebar collapsed: a ReferenceError, and no place for the pill.
        function test_the_pill_follows_the_preset_name_with_the_sidebar_collapsed() {
            failOnWarning(/ReferenceError/)
            load([status(target, "detached", "none")])
            const name = findChild(bar, "presetName")
            for (const open of [true, false]) {
                AppSettings.sidebarOpen = open
                waitForRendering(bar)
                compare(pill().x, 32 + name.width + 10, open ? "open" : "collapsed")
            }
            AppSettings.sidebarOpen = true
        }

        function test_a_detached_output_shows_its_status_and_repair() {
            load([status(target, "detached", "none")])
            verify(pill().visible)
            compare(findChild(bar, "engineStatusAction").text, "Repair")
            click(findChild(bar, "engineStatusAction"))
            compare(UiState.view, "devices")
            compare(UiState.devicesSelection, target)
        }

        function test_an_output_without_isoapo_shows_install() {
            load([status(target, "not_installed", "none")])
            verify(pill().visible)
            compare(findChild(bar, "engineStatusAction").text, "Install")
        }

        function test_a_working_output_shows_nothing() {
            verify(TestHooks.setCompatConfig("Include: Isotone.txt\r\n"))
            load([status(target, "installed", "native")])
            verify(!pill().visible)
            load([status(target, "not_installed", "equalizerapo")])
            verify(!pill().visible)
        }

        function test_equalizer_apo_without_the_include_shows_not_attached_and_attach() {
            verify(TestHooks.setCompatConfig("Include: peace.txt\r\n"))
            load([status(target, "not_installed", "equalizerapo")])
            verify(pill().visible)
            compare(findChild(bar, "engineStatusAction").text, "Attach")
        }

        function test_equalizer_apo_on_no_output_offers_its_uninstaller() {
            load([status(target, "installed", "native")], { installed: true, version: "1.4.2", uninstaller: "x" })
            const card = findChild(view, "equalizerApoCard")
            verify(card.visible)
            verify(findChild(card, "equalizerApoUninstall").visible)
            const spy = createTemporaryObject(signalSpy, root, { target: Devices, signalName: "equalizerApoUninstallerRequested" })
            click(findChild(card, "equalizerApoUninstall"))
            compare(spy.count, 1)
            compare(spy.signalArguments[0][0], "x")

            load([status(target, "not_installed", "equalizerapo")], { installed: true, version: "1.4.2" })
            verify(!card.visible)
            load([status(target, "installed", "native")], { installed: false })
            verify(!card.visible)
        }

        Component { id: signalSpy; SignalSpy {} }
    }
}
