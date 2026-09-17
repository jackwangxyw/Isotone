import QtQuick
import QtTest
import Isotone

// EQ by ear on a stereo session with no output behind it (no audio plays): the
// marks and Add band, play and the volume dialog, the keys, the slider, the
// channel and level, and the tone stopping when the view goes.
Item {
    id: root
    width: 1192
    height: 900

    Loader {
        id: loader
        anchors.fill: parent
        sourceComponent: EarView {}
    }
    Item {
        id: overlay
        anchors.fill: parent
        z: 100
    }

    TestCase {
        name: "EarView"
        when: windowShown

        function view() { return loader.item }
        function item(name) {
            const it = findChild(view(), name)
            verify(it !== null, name)
            return it
        }
        function dialog() {
            for (let i = 0; i < overlay.children.length; ++i)
                if (overlay.children[i].title !== undefined) return overlay.children[i]
            return null
        }
        function type(text) { for (const c of text) keyClick(c) }
        function editIn(container, text) {
            const click = findChild(container, "valueClick")
            verify(click !== null)
            mouseClick(click)
            type(text)
            keyClick(Qt.Key_Return)
        }
        function flat() { while (EqSession.count > 0) EqSession.deleteBand(0) }

        function initTestCase() {
            UiState.overlay = overlay
            TestHooks.useLayout(2, 0x3)
            tryVerify(() => view() !== null)
            waitForRendering(view())
        }
        function init() {
            if (dialog()) dialog().close()
            loader.active = true
            tryVerify(() => view() !== null)
            EqByEar.playing = false
            EqByEar.clearMarks()
            EqByEar.frequency = 1000
            EqSession.viewChannel = 2
            EqByEar.levelDb = -30
            EqByEar.dip = false
            UiState.earWarned = false
            flat()
            view().forceActiveFocus()
        }

        function test_marks_then_add_band() {
            const add = item("earAddMarked")
            verify(!add.active)
            EqByEar.frequency = 2600
            mouseClick(item("markStart"))
            EqByEar.frequency = 3400
            mouseClick(item("markTop"))
            verify(!add.active)
            EqByEar.frequency = 4400
            mouseClick(item("markEnd"))
            compare(EqByEar.start, 2600)
            compare(EqByEar.top, 3400)
            compare(EqByEar.end, 4400)
            verify(add.active)

            mouseClick(add)
            compare(EqSession.count, 1)
            compare(EqSession.selectedRow, 0)
            compare(EqSession.data(EqSession.index(0, 0), EqSession.FrequencyRole), 3400)
            compare(EqSession.data(EqSession.index(0, 0), EqSession.GainRole), -3)
            fuzzyCompare(EqSession.data(EqSession.index(0, 0), EqSession.QRole), 1.89, 0.001)
            compare(EqByEar.start, 0)
            verify(!add.active)
            tryVerify(() => findChild(item("earBandList"), "earBandRow") !== null)
        }

        function test_a_band_added_last_is_scrolled_into_view() {
            for (let i = 0; i < 12; ++i) EqSession.addBand(100 + i * 100, 0)
            EqSession.select(0)
            const list = item("earBandList")
            tryCompare(list, "contentY", list.originY)
            // Clear of the bands, or it adds to the one there.
            EqByEar.frequency = 5000; EqByEar.mark(EqByEar.Start)
            EqByEar.frequency = 6000; EqByEar.mark(EqByEar.Top)
            EqByEar.frequency = 7000; EqByEar.mark(EqByEar.End)
            mouseClick(item("earAddMarked"))
            compare(EqSession.selectedRow, 12)
            tryVerify(() => list.contentY + list.height >= list.contentHeight + list.originY - 1, 2000, "scrolled to the end")
        }

        function test_play_asks_to_turn_the_volume_down_once() {
            mouseClick(item("earPlay"))
            verify(dialog() !== null)
            verify(!EqByEar.playing)
            mouseClick(findChild(dialog(), "volumeCancel"))
            tryVerify(() => dialog() === null)   // destroyed on the next turn of the event loop
            verify(!EqByEar.playing)
            verify(!UiState.earWarned)

            mouseClick(item("earPlay"))
            mouseClick(findChild(dialog(), "volumeStart"))
            tryVerify(() => dialog() === null)
            verify(EqByEar.playing)
            mouseClick(item("earPlay"))
            verify(!EqByEar.playing)
            mouseClick(item("earPlay"))
            verify(dialog() === null)
            verify(EqByEar.playing)
        }

        function test_keys_move_the_tone() {
            keyClick(Qt.Key_Right)
            fuzzyCompare(EqByEar.frequency, 1000 * Math.pow(2, 1 / 48), 0.01)
            keyClick(Qt.Key_Left)
            fuzzyCompare(EqByEar.frequency, 1000, 0.01)
            keyClick(Qt.Key_Right, Qt.ShiftModifier)
            fuzzyCompare(EqByEar.frequency, 1000 * Math.pow(2, 1 / 6), 0.01)
            keyClick(Qt.Key_Left, Qt.ShiftModifier)
            keyClick(Qt.Key_PageUp)
            fuzzyCompare(EqByEar.frequency, 2000, 0.01)
            keyClick(Qt.Key_PageDown)
            fuzzyCompare(EqByEar.frequency, 1000, 0.01)

            UiState.earWarned = true
            keyClick(Qt.Key_Space)
            verify(EqByEar.playing)
            keyClick(Qt.Key_Space)
            verify(!EqByEar.playing)
        }

        function test_a_field_being_typed_in_keeps_its_keys() {
            const field = item("earFrequency")
            mouseClick(findChild(field, "valueClick"))
            verify(field.editing)
            keyClick(Qt.Key_Left)
            compare(EqByEar.frequency, 1000)
            keyClick(Qt.Key_A, Qt.ControlModifier)
            type("250")
            keyClick(Qt.Key_Return)
            compare(EqByEar.frequency, 250)
        }

        function test_the_slider_is_logarithmic() {
            const slider = item("sweepSlider")
            mouseClick(slider, slider.width / 3, 9)   // a third of three decades: 200 Hz
            fuzzyCompare(EqByEar.frequency, 200, 2)
            mouseClick(slider, slider.width, 9)
            compare(EqByEar.frequency, 20000)
        }

        // The owner, 2026-09-16: the channel is the top bar's; level and auto sweep share
        // a row with play; New preset sits left of Clear and Add band.
        function test_the_layout() {
            verify(findChild(view(), "earChannel") === null, "no channel picker of its own")
            const centre = (it) => it.mapToItem(view(), 0, it.height / 2).y
            fuzzyCompare(centre(item("earLevelValue")), centre(item("earAutoSweep")), 1)
            fuzzyCompare(centre(item("earLevelValue")), centre(item("earPlay")), 1)
            const level = item("earLevelValue").mapToItem(view(), 0, 0).x
            verify(level < item("earAutoSweep").mapToItem(view(), 0, 0).x, "level, then auto sweep")
            const button = item("earNewPreset")
            compare(button.text, "New preset")
            const clear = item("earClear")
            fuzzyCompare(centre(button), centre(clear), 1)
            verify(button.mapToItem(view(), button.width, 0).x < clear.mapToItem(view(), 0, 0).x, "left of Clear")
            verify(findChild(view(), "earNew") === null)
        }

        function test_channel_level_and_rate() {
            EqSession.viewChannel = 0
            compare(EqByEar.channel, EqByEar.Left)
            editIn(item("earLevelValue"), "-20 dBFS")
            compare(EqByEar.levelDb, -20)
            editIn(item("earSweepRate"), "2 oct/s")
            compare(EqByEar.sweepRate, 2)
            mouseClick(item("earAutoSweep"))
            verify(EqByEar.autoSweep)
            mouseClick(item("earAutoSweep"))
            verify(!EqByEar.autoSweep)
        }

        function test_leaving_the_view_stops_the_tone() {
            UiState.earWarned = true
            mouseClick(item("earPlay"))
            verify(EqByEar.playing)
            loader.active = false
            verify(!EqByEar.playing)
        }

        // The owner, 2026-09-16: a press anywhere on the graph moves the tone there, as
        // the slider does, and a drag from there carries on; a handle is still a handle.
        function test_a_press_on_the_graph_moves_the_tone_there() {
            const graph = item("responseGraph")
            const y = graph.plotTop + graph.plotHeight * 0.8
            mouseClick(graph, graph.xOf(5000), y)
            fuzzyCompare(EqByEar.frequency, 5000, 5000 * 0.005)
            mousePress(graph, graph.xOf(200), y)
            fuzzyCompare(EqByEar.frequency, 200, 2)
            mouseMove(graph, graph.xOf(400), y)
            mouseMove(graph, graph.xOf(800), y)
            fuzzyCompare(EqByEar.frequency, 800, 8)
            mouseRelease(graph, graph.xOf(800), y)

            EqSession.addBand(2000, 6)
            waitForRendering(graph)
            const handle = findChild(view(), "handle")
            verify(handle !== null)
            EqByEar.frequency = 1000
            mouseClick(handle, handle.width / 2, handle.height / 2)
            compare(EqByEar.frequency, 1000, "a press on a handle leaves the tone")

            wait(400)   // not a double click with the last press
            mouseDoubleClickSequence(graph, graph.xOf(300), graph.yOf(-4))
            compare(EqSession.count, 2, "a double click still adds a band")
        }

        function test_the_graph_shows_the_cursor_and_marks() {
            const cursor = item("earCursor")
            verify(cursor.visible)
            const graph = item("responseGraph")
            fuzzyCompare(cursor.x + 1, graph.xOf(1000), 1)
            EqByEar.frequency = 5000
            fuzzyCompare(cursor.x + 1, graph.xOf(5000), 1)
        }
    }
}
