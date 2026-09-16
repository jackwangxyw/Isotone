import QtQuick
import QtTest
import Isotone

// The graph's handles: their size, and double-click resetting the gain as a
// double-click on a band's slider does (owner, 2026-09-15).
Item {
    id: root
    width: 1192
    height: 440

    GraphCard {
        id: card
        anchors.fill: parent
    }

    TestCase {
        name: "GraphHandle"
        when: windowShown

        readonly property int gainRole: 261

        function handle(row) { return card.handleItem(row) }
        function gain(row) { return EqSession.data(EqSession.index(row, 0), gainRole) }

        function init() {
            while (EqSession.count > 0) EqSession.deleteBand(0)
            EqSession.byFrequency = false
            EqSession.addBand(1000, 6)
            tryVerify(() => handle(0) !== null && handle(0).width > 0)
            waitForRendering(card)
        }

        function test_a_handle_is_smaller_than_the_first_boards() {
            // 12 px selected, 10 unselected; the boards had 14 and 12.
            compare(handle(0).radius, 12)
            EqSession.addBand(4000, 0)
            tryVerify(() => handle(1) !== null)
            verify(handle(1).selected)
            compare(handle(0).radius, 10)
            compare(handle(1).radius, 12)
        }

        function test_double_click_resets_the_gain() {
            const h = handle(0)
            compare(gain(0), 6)
            mouseDoubleClickSequence(h, h.width / 2, h.height / 2)
            tryCompare(EqSession, "count", 1, 1000, "no band is added on a handle")
            compare(gain(0), 0)
        }

        function test_double_click_beside_a_handle_still_adds_a_band() {
            mouseDoubleClickSequence(card, 200, 60)
            tryCompare(EqSession, "count", 2)
        }
    }
}
