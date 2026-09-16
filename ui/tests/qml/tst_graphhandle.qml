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

    FontMetrics { id: metrics }
    TextMetrics { id: ink }

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

        // The owner, 2026-09-15: the numbers in the circles are not centred. The
        // digits' ink, not the text's box (which carries the descent and rounds to
        // a whole pixel), has to sit on the circle's middle.
        function test_the_number_is_centred_in_its_circle() {
            for (const hz of [4000, 120, 8000, 300, 600, 900, 2000, 5000, 15000]) EqSession.addBand(hz, 0)
            compare(EqSession.count, 10)
            tryVerify(() => handle(9) !== null && handle(9).width > 0)
            waitForRendering(card)
            for (let row = 0; row < 10; ++row) {
                const number = findChild(handle(row), "handleNumber")
                verify(number !== null, "row " + row)
                const circle = number.parent
                ink.font = number.font
                ink.text = number.text
                metrics.font = number.font
                const box = ink.tightBoundingRect
                verify(box.width > 0 && box.height > 0, "row " + row + " has ink")
                // The ink box is measured from the text's origin, its top from the
                // baseline. The number sits on the whole pixel nearest the middle, so
                // the glyphs stay crisp: half a pixel is the most it can be out.
                const exactX = circle.width / 2 - box.x - box.width / 2
                const exactY = circle.height / 2 - metrics.ascent - box.y - box.height / 2
                compare(number.x, Math.round(exactX), "row " + row + " horizontally")
                compare(number.y, Math.round(exactY), "row " + row + " vertically")
                verify(Math.abs(number.x + box.x + box.width / 2 - circle.width / 2) <= 0.5, "row " + row + " x")
                verify(Math.abs(number.y + metrics.ascent + box.y + box.height / 2 - circle.height / 2) <= 0.5,
                       "row " + row + " y")
            }
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
