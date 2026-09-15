import QtQuick
import QtTest
import Isotone

// The Speakers view on a 7.1 session with no output behind it: the table's
// edits, test tones, the New group dialog and the layout dialog (its setter
// replaced, so no layout changes).
Item {
    id: root
    width: 1192
    height: 900

    property var layoutCalls: []

    Loader {
        id: loader
        anchors.fill: parent
        sourceComponent: SpeakersView {
            layoutSetter: (name) => { root.layoutCalls.push(name); return 0 }
        }
    }
    Item {
        id: overlay
        anchors.fill: parent
        z: 100
    }

    TestCase {
        name: "SpeakersView"
        when: windowShown

        function view() { return loader.item }
        function row(code) {
            const r = findChild(view(), "speakerRow_" + code)
            verify(r !== null, code)
            return r
        }
        function cell(code, name) { return findChild(row(code), name) }
        function type(text) { for (const c of text) keyClick(c) }
        function speaker(code, role) {
            for (let i = 0; i < Speakers.count; ++i)
                if (Speakers.data(Speakers.index(i, 0), Speakers.CodeRole) === code)
                    return Speakers.data(Speakers.index(i, 0), role)
            return undefined
        }
        function dialog() {
            for (let i = 0; i < overlay.children.length; ++i)
                if (overlay.children[i].title !== undefined) return overlay.children[i]
            return null
        }
        function edit(box, text) {
            const field = box.field
            mouseClick(field)
            verify(field.editing)
            type(text)
            keyClick(Qt.Key_Return)
            verify(!field.editing)
        }

        function initTestCase() {
            UiState.overlay = overlay
            TestHooks.useLayout(8, 0x63F)
            loader.active = true
            tryVerify(() => view() !== null && Speakers.count === 8)
            waitForRendering(view())
        }
        function cleanupTestCase() { TestHooks.useLayout(2, 0x3) }
        function init() {
            Speakers.distanceMode = true
            Speakers.testTones = false
            if (Speakers.soloRow >= 0) Speakers.toggleSolo(Speakers.soloRow)
            if (dialog()) dialog().close()
        }

        function test_a_row_per_speaker() {
            for (const code of ["L", "R", "C", "LFE", "RL", "RR", "SL", "SR"]) verify(row(code).visible, code)
            compare(row("LFE").name, "Subwoofer")
            compare(cell("SL", "level").text, "+0.0 dB")
        }

        function test_typing_a_level() {
            edit(cell("SL", "level"), "-2")
            compare(speaker("SL", Speakers.LevelRole), -2)
            compare(cell("SL", "level").text, "−2.0 dB")
        }

        function test_distance_and_delay_switch() {
            verify(cell("C", "distance").visible)
            verify(!cell("C", "delay").visible)
            edit(cell("LFE", "distance"), "3.40 m")
            fuzzyCompare(speaker("LFE", Speakers.DistanceRole), 3.4, 1e-9)
            fuzzyCompare(speaker("L", Speakers.DelayRole), 0.4 / 343 * 1000, 1e-6)

            mouseClick(findChild(findChild(view(), "timeMode"), "option_Delay"))
            verify(!Speakers.distanceMode)
            verify(!cell("C", "distance").visible)
            verify(cell("C", "delay").visible)
            edit(cell("C", "delay"), "1.5")
            fuzzyCompare(speaker("C", Speakers.DelayRole), 1.5, 1e-9)
            compare(cell("C", "delay").text, "1.50 ms")

            mouseClick(findChild(findChild(view(), "timeMode"), "option_Distance"))
            verify(Speakers.distanceMode)
            fuzzyCompare(speaker("C", Speakers.DistanceRole), 3.4 - 1.5 * 0.343, 1e-9)
            compare(cell("C", "distance").text, speaker("C", Speakers.DistanceRole).toFixed(2) + " m")
        }

        function test_polarity() {
            mouseClick(cell("LFE", "polarity"))
            verify(speaker("LFE", Speakers.InvertedRole))
            mouseClick(cell("LFE", "polarity"))
            verify(!speaker("LFE", Speakers.InvertedRole))
        }

        function test_mute() {
            const m = cell("RR", "mute")
            mouseClick(m)
            verify(speaker("RR", Speakers.MutedRole))
            verify(m.lit)
            mouseClick(m)
            verify(!speaker("RR", Speakers.MutedRole))
            verify(!m.lit)
        }

        function test_solo_mutes_the_others() {
            mouseClick(cell("C", "solo"))
            compare(Speakers.soloRow, 2)
            verify(cell("L", "mute").lit)
            verify(cell("LFE", "mute").lit)
            verify(!cell("C", "mute").lit)
            verify(!speaker("L", Speakers.MutedRole), "solo is not the speaker's own mute")
            mouseClick(cell("C", "solo"))
            compare(Speakers.soloRow, -1)
            verify(!cell("L", "mute").lit)
        }

        function test_test_tones() {
            verify(!findChild(view(), "tonesInfo").visible)
            mouseClick(findChild(view(), "testTones"))
            verify(Speakers.testTones)
            verify(findChild(view(), "tonesInfo").visible)
            compare(Speakers.playingRow, 0)
            verify(!findChild(view(), "upmix").enabled)
            verify(!findChild(view(), "swapFrontRear").enabled)
            mouseClick(cell("LFE", "tone"))
            compare(Speakers.playingRow, 3)
            verify(row("LFE").playing)
            mouseClick(cell("LFE", "tone"))
            compare(Speakers.playingRow, -1)
            mouseClick(findChild(view(), "testTones"))
            verify(!Speakers.testTones)
            verify(findChild(view(), "upmix").enabled)
            mouseClick(cell("LFE", "tone"))   // no tones: nothing plays
            compare(Speakers.playingRow, -1)
        }

        function test_routing_and_bass_management() {
            mouseClick(findChild(findChild(view(), "upmix"), "option_No centre"))
            compare(Speakers.upmix, 2)
            mouseClick(findChild(view(), "swapLeftRight"))
            verify(Speakers.swapLeftRight)
            mouseClick(findChild(view(), "swapLeftRight"))
            mouseClick(findChild(view(), "small_SL"))
            compare(Speakers.smallSpeakers, 0x40)
            verify(Speakers.bassManagement)
            mouseClick(findChild(view(), "small_SL"))
            edit(findChild(view(), "crossover"), "95")
            compare(Speakers.crossoverHz, 100)
            edit(findChild(view(), "lfeLowpass"), "300 Hz")
            compare(Speakers.lfeLowpassHz, 250)
            edit(findChild(view(), "lipSync"), "25 ms")
            compare(Speakers.lipSyncMs, 25)
            Speakers.upmix = 0
        }

        function test_new_group() {
            const before = Speakers.groups.length
            mouseClick(findChild(view(), "newGroup"))
            tryVerify(() => dialog() !== null)
            const d = dialog()
            compare(d.title, "New group")
            waitForRendering(d)
            verify(!findChild(d, "groupCreate").active)
            mouseClick(findChild(d, "groupName"))
            type("Heights")
            mouseClick(findChild(d, "groupSpeaker_SL"))
            mouseClick(findChild(d, "groupSpeaker_SR"))
            verify(findChild(d, "groupCreate").active)
            mouseClick(findChild(d, "groupCreate"))
            tryVerify(() => dialog() === null)
            compare(Speakers.groups.length, before + 1)
            compare(Speakers.groups[before].name, "Heights")
            compare(Speakers.groups[before].codes, "SL SR")
            tryVerify(() => findChild(view(), "group_Heights") !== null)
            Speakers.removeGroup("Heights")
        }

        function test_change_layout_asks_then_calls_the_setter() {
            root.layoutCalls = []
            // No output here supports another layout: the picker's other options are off.
            mouseClick(findChild(findChild(view(), "layoutPicker"), "option_5.1"))
            verify(dialog() === null)

            view().requestLayout("5.1")
            tryVerify(() => dialog() !== null)
            const d = dialog()
            verify(d.title.endsWith(" to 5.1?"))
            compare(d.from, "7.1")
            compare(d.fromChannels, 8)
            compare(d.toChannels, 6)
            waitForRendering(d)
            mouseClick(findChild(d, "layoutCancel"))
            tryVerify(() => dialog() === null)
            compare(root.layoutCalls.length, 0)

            view().requestLayout("Stereo")
            tryVerify(() => dialog() !== null)
            waitForRendering(dialog())
            mouseClick(findChild(dialog(), "layoutChange"))
            tryVerify(() => dialog() === null)
            compare(root.layoutCalls, ["Stereo"])
        }

        function test_leaving_the_view_ends_tones_and_solo() {
            Speakers.testTones = true
            Speakers.toggleSolo(1)
            loader.active = false
            verify(!Speakers.testTones)
            compare(Speakers.soloRow, -1)
            loader.active = true
            tryVerify(() => view() !== null)
            waitForRendering(view())
        }
    }
}
