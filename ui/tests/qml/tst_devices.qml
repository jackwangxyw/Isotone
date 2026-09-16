import QtQuick
import QtTest
import Isotone

// The Devices view on the prototype's outputs (tests/devices/fake-devicetool.json)
// with devicetool scripted: selection, the actions each status shows, the
// operation box's phases, and the Uninstall, Replace and Attach dialogs. The
// Attach dialog writes a sandbox config.txt (qml_main.cpp).
Item {
    id: root
    width: 1192
    height: 900

    DevicesView { id: view; anchors.fill: parent }
    Item { id: overlay; anchors.fill: parent; z: 100 }

    TestCase {
        name: "Devices"
        when: windowShown

        // After layout: a button just shown is placed on the next polish, which
        // runs with the render loop, not on the change. mouseClick maps the
        // item's position when it is called, so without waiting for the polish
        // it clicks where the button was before the row was positioned again.
        function click(item) {
            verify(item !== null)
            wait(30)
            for (let i = item; i; i = i.parent) waitForItemPolished(i)
            mouseClick(item)
        }

        function guid(n) { return "{" + String(n).padStart(8, "0") + "-0000-4000-8000-000000000000}" }
        function script(s) { verify(Devicetool.loadScript(JSON.stringify(s))) }
        function all(item, prefix, out) {
            out = out || []
            for (let i = 0; i < item.children.length; ++i) {
                const c = item.children[i]
                if (c.objectName && c.objectName.startsWith(prefix) && c.visible) out.push(c)
                all(c, prefix, out)
            }
            return out
        }
        function visibleIn(item) {
            for (let i = item; i; i = i.parent) if (!i.visible) return false
            return true
        }
        function actionNames() {
            const actions = findChild(view, "detailActions")
            if (!actions.visible) return []
            return all(actions, "action_").map((b) => b.objectName.substring(7))
        }
        function select(n) {
            const row = findChild(view, "deviceRow_" + guid(n))
            verify(row !== null, "row " + n)
            click(row)
            compare(UiState.devicesSelection, guid(n))
        }
        function text(name) { return findChild(view, name).text }
        function dialog(name) { return findChild(overlay, name) }
        function closeDialogs() {
            for (let i = overlay.children.length - 1; i >= 0; --i)
                if (overlay.children[i].objectName.endsWith("Dialog")) overlay.children[i].destroy()
        }

        // The sandbox config.txt, with Isotone.txt included or not.
        readonly property string peaceConfig: "Preamp: -3 dB\r\nInclude: peace.txt\r\nGraphicEQ: 25 0; 40 -1.5; 100 0\r\n"
        function config(text) {
            verify(TestHooks.setCompatConfig(text))
            Devices.refresh()
        }

        function initTestCase() { UiState.overlay = overlay }
        function init() {
            tryVerify(() => !Devicetool.working, 5000)
            closeDialogs()
            wait(0)
            script({ started: false, commands: {} })
            config(peaceConfig + "Include: Isotone.txt\r\n")
            UiState.devicesSelection = ""
            waitForRendering(view)
        }

        function test_the_default_output_is_selected_first() {
            compare(text("detailName"), "Headphones (USB DAC)")
            compare(text("detailStatus"), "Installed")
        }

        function test_a_click_selects_a_row() {
            select(4)
            compare(text("detailName"), "Monitor (DisplayPort)")
            compare(text("detailStatus"), "Detached")
        }

        function test_unplugged_outputs_are_dimmed() {
            compare(findChild(view, "deviceRow_" + guid(9)).opacity, 0.55)
            compare(findChild(view, "deviceRow_" + guid(1)).opacity, 1)
        }

        function test_actions_per_state_data() {
            return [
                { tag: "installed", n: 1, actions: ["test", "uninstall"] },
                { tag: "active", n: 3, actions: ["replace", "test"] },
                { tag: "detached", n: 4, actions: ["repair", "test", "uninstall"] },
                { tag: "conflict", n: 5, actions: ["removeEapo", "uninstall"] },
                { tag: "replaced", n: 6, actions: ["takeBack", "keepEapo"] },
                { tag: "enhancements off", n: 7, actions: ["enableEnhancements"] },
                { tag: "not installed", n: 8, actions: ["install"] },
                { tag: "unplugged", n: 9, actions: [] },
                { tag: "interrupted", n: 10, actions: ["undo"] },
                { tag: "unrecorded", n: 11, actions: ["copyDiagnostics"] },
            ]
        }
        function test_actions_per_state(data) {
            select(data.n)
            compare(actionNames().sort(), data.actions.slice().sort())
        }

        function test_button_labels_and_kinds() {
            select(5)
            compare(findChild(view, "action_uninstall").text, "Uninstall IsoAPO")
            compare(findChild(view, "action_uninstall").kind, "danger")
            compare(findChild(view, "action_removeEapo").text, "Remove Equalizer APO")
            compare(findChild(view, "action_removeEapo").kind, "primary")
            select(1)
            compare(findChild(view, "action_uninstall").text, "Uninstall")
            compare(findChild(view, "action_test").kind, "normal")
        }

        function test_equalizer_apo_outputs_show_their_config_and_slots() {
            select(3)
            verify(findChild(view, "detailConfig").visible)
            verify(findChild(view, "detailConfig").value.startsWith("config.txt"))
            select(1)
            verify(!findChild(view, "detailConfig").visible)
        }

        function test_preset_row_hidden_for_unplugged_and_unrecorded() {
            select(9)
            verify(!findChild(view, "detailPreset").visible)
            select(11)
            verify(!findChild(view, "detailPreset").visible)
            select(8)
            verify(findChild(view, "detailPreset").visible)
        }

        function test_install_asks_approval_then_runs_restarts_and_tests() {
            script({ start: { delay_ms: 300 }, commands: { install: [{ delay_ms: 300 }], "restart-audio": [{ delay_ms: 300 }] } })
            select(8)
            click(findChild(view, "action_install"))
            tryCompare(findChild(view, "operationText"), "text", "Waiting for administrator approval", 1000)
            verify(!findChild(view, "detailActions").visible)
            tryCompare(findChild(view, "operationText"), "text", "Installing", 1000)
            tryCompare(findChild(view, "operationText"), "text", "Restarting audio", 1000)
            tryCompare(findChild(view, "operationText"), "text", "Installed", 2000)
            compare(Devicetool.target, guid(8))
            compare(Devicetool.kind, "install")
        }

        function test_the_operation_shows_only_on_its_output_and_clears_on_selection() {
            script({ started: true, commands: {} })
            select(8)
            click(findChild(view, "action_install"))
            tryCompare(findChild(view, "operationText"), "text", "Installed", 2000)
            select(1)
            verify(findChild(view, "detailActions").visible)
            compare(Devicetool.phase, "")
        }

        function test_a_failure_shows_the_reason_copy_and_retry() {
            script({ started: true, commands: { install: [{ exit: 1, json: { reason: "registration checks failed" } }, { exit: 0 }] } })
            select(8)
            click(findChild(view, "action_install"))
            tryCompare(findChild(view, "operationText"), "text", "Install failed", 2000)
            compare(text("operationReason"), "Registration checks failed.")
            verify(findChild(view, "operationCopy").visible)
            click(findChild(view, "operationRetry"))
            tryCompare(findChild(view, "operationText"), "text", "Installed", 2000)
        }

        function test_busy_offers_retry() {
            script({ started: true, commands: { repair: [{ exit: 4, json: { error: "busy" } }] } })
            select(4)
            click(findChild(view, "action_repair"))
            tryCompare(findChild(view, "operationText"), "text", "Another install is running", 2000)
            verify(findChild(view, "operationRetry").visible)
            verify(!findChild(view, "operationCopy").visible)
        }

        function test_declined_approval_offers_retry() {
            script({ start: { error: 1223 } })
            select(4)
            click(findChild(view, "action_repair"))
            tryCompare(findChild(view, "operationText"), "text", "Approval declined", 2000)
            verify(findChild(view, "operationRetry").visible)
        }

        function test_audio_that_does_not_restart_offers_a_windows_restart() {
            script({ started: true, commands: { "restart-audio": [{ exit: 1, json: { reason: "timeout" } }] } })
            select(8)
            click(findChild(view, "action_install"))
            tryCompare(findChild(view, "operationText"), "text", "Installed · audio did not restart", 2000)
            verify(findChild(view, "operationRestart").visible)
            click(findChild(view, "operationLater"))
            compare(Devicetool.phase, "")
            verify(findChild(view, "detailActions").visible)
        }

        function test_repair_on_a_record_that_predates_the_mode_and_enhancements() {
            script({ started: true })
            select(7)
            click(findChild(view, "action_enableEnhancements"))
            tryCompare(findChild(view, "operationText"), "text", "Repaired", 2000)
            compare(Devicetool.kind, "repair")
        }

        function test_uninstall_asks_first() {
            script({ started: true })
            select(1)
            click(findChild(view, "action_uninstall"))
            const d = dialog("uninstallDialog")
            verify(d !== null)
            compare(d.title, "Uninstall IsoAPO from Headphones (USB DAC)?")
            compare(findChild(d, "restores").value, "Driver effects")
            click(findChild(d, "uninstallCancel"))
            wait(0)
            verify(dialog("uninstallDialog") === null)
            compare(Devicetool.phase, "")

            click(findChild(view, "action_uninstall"))
            click(findChild(dialog("uninstallDialog"), "uninstallConfirm"))
            tryCompare(findChild(view, "operationText"), "text", "Uninstalled", 2000)
            compare(Devicetool.kind, "uninstall")
        }

        function test_uninstall_where_equalizer_apo_holds_the_output_restores_it() {
            select(6)
            click(findChild(view, "action_keepEapo"))
            compare(findChild(dialog("uninstallDialog"), "restores").value, "Equalizer APO")
        }

        function test_replace_preselects_isoapo_and_installs_it() {
            script({ started: true })
            select(3)
            click(findChild(view, "action_replace"))
            const d = dialog("replaceDialog")
            verify(d !== null)
            compare(d.title, "Speakers (Realtek)")
            verify(findChild(d, "choice_iso").on)
            verify(!findChild(d, "choice_eapo").on)
            click(findChild(d, "replaceContinue"))
            tryCompare(findChild(view, "operationText"), "text", "IsoAPO installed", 2000)
            compare(Devicetool.kind, "replace")
        }

        function test_keeping_equalizer_apo_opens_attach() {
            select(3)
            click(findChild(view, "action_replace"))
            const d = dialog("replaceDialog")
            click(findChild(d, "choice_eapo"))
            verify(findChild(d, "choice_eapo").on)
            click(findChild(d, "replaceContinue"))
            wait(0)
            verify(dialog("replaceDialog") === null)
            const a = dialog("attachDialog")
            verify(a !== null)
            compare(Devicetool.phase, "")
        }

        function test_attach_shows_config_txt_and_removes_the_peace_include() {
            config(peaceConfig)
            select(3)
            click(findChild(view, "action_attach"))
            wait(0)
            const a = dialog("attachDialog")
            verify(a !== null, "attach dialog")
            verify(a.preview.directory.length > 0, "directory")
            compare(findChild(a, "configLine0").text, "Preamp: -3 dB")
            compare(findChild(a, "configLine1").text, "Include: peace.txt")
            compare(findChild(a, "configLine1").tag, "Peace")
            compare(findChild(a, "configLine2").tag, "")
            compare(findChild(a, "addedLine2").text, "Include: Isotone.txt")
            compare(findChild(a, "addedLine2").number, "+")
            const peace = findChild(a, "peaceChoice")
            verify(peace.visible)
            compare(peace.current, 0)
            peace.picked(1)
            click(findChild(a, "attachConfirm"))
            const after = EqualizerApoConfig.preview()
            verify(after.attached, "attached: " + findChild(a, "attachFailure").text)
            tryVerify(() => dialog("attachDialog") === null, 1000, "closed")
            verify(!after.lines.some((l) => l.peace), "no Peace include")
            compare(after.lines.length, 5)
        }

        // Review fixes.
        function test_an_equalizer_apo_output_config_txt_does_not_include_is_not_attached() {
            config(peaceConfig)
            select(3)
            compare(text("detailStatus"), "Not attached")
            compare(actionNames().sort(), ["attach", "test"])
            compare(findChild(view, "action_attach").kind, "primary")
            click(findChild(view, "action_attach"))
            verify(dialog("attachDialog") !== null)
        }

        function test_keeping_equalizer_apo_offers_attach_once_isoapo_is_uninstalled() {
            config(peaceConfig)
            script({ started: true, commands: { uninstall: [{ delay_ms: 200 }] } })
            select(6)
            click(findChild(view, "action_keepEapo"))
            click(findChild(dialog("uninstallDialog"), "uninstallConfirm"))
            wait(0)
            verify(dialog("attachDialog") === null, "not before the uninstall is done")
            tryVerify(() => dialog("attachDialog") !== null, 3000)
            compare(Devicetool.phase, "done")
        }

        function test_uninstall_alone_offers_no_attach() {
            config(peaceConfig)
            script({ started: true })
            select(6)
            click(findChild(view, "action_keepEapo"))
            click(findChild(dialog("uninstallDialog"), "uninstallCancel"))
            select(1)
            click(findChild(view, "action_uninstall"))
            click(findChild(dialog("uninstallDialog"), "uninstallConfirm"))
            tryCompare(findChild(view, "operationText"), "text", "Uninstalled", 2000)
            wait(50)
            verify(dialog("attachDialog") === null)
        }

        function test_retry_under_test_failed_runs_the_test() {
            script({ started: true, commands: { test: [{ exit: 1, json: { reason: "Initialize failed" } }, { exit: 0 }] } })
            select(8)
            click(findChild(view, "action_install"))
            tryCompare(findChild(view, "operationText"), "text", "Test failed", 2000)
            click(findChild(view, "operationRetry"))
            compare(Devicetool.kind, "test")
            tryCompare(findChild(view, "operationText"), "text", "Test passed", 2000)
        }

        function test_restart_audio_busy_is_busy_with_retry() {
            script({ started: true, commands: { "restart-audio": [{ exit: 4, json: { error: "busy" } }, { exit: 0 }] } })
            select(8)
            click(findChild(view, "action_install"))
            tryCompare(findChild(view, "operationText"), "text", "Another install is running", 2000)
            verify(!findChild(view, "operationRestart").visible)
            click(findChild(view, "operationRetry"))
            tryCompare(findChild(view, "operationText"), "text", "Installed", 2000)
        }
    }
}
