import QtQuick
import QtTest
import Isotone

// The band strip scrolls sideways: horizontal wheel, vertical wheel, dragging
// the columns and dragging the thumb. A drag on a gain slider still sets gain.
Item {
    id: root
    width: 1192
    height: 402

    BandStrip {
        id: strip
        anchors.fill: parent
    }

    TestCase {
        name: "BandStrip"
        when: windowShown

        function flick() { return findChild(strip, "bandFlick") }
        function maxX(f) { return f.contentWidth - f.width }

        // No output in a test: the session edits in memory only.
        function initTestCase() {
            while (EqSession.count < 12) EqSession.addBand(100 * (EqSession.count + 1), 0)
            tryVerify(() => flick().contentWidth > flick().width, 1000)
        }

        function init() {
            flick().contentX = 0
        }

        function test_bands_overflow() {
            const f = flick()
            tryVerify(() => f.contentWidth > f.width, 1000, "12 columns do not fit, so there is something to scroll")
        }

        function test_horizontal_wheel_scrolls() {
            const f = flick()
            mouseWheel(f, 200, 150, -240, 0)
            tryVerify(() => f.contentX > 0, 1000, "a horizontal wheel moves the columns")
        }

        function test_vertical_wheel_scrolls() {
            const f = flick()
            mouseWheel(f, 200, 150, 0, -240)
            tryVerify(() => f.contentX > 0, 1000, "a vertical wheel over the strip moves the columns")
        }

        function test_wheel_stops_at_the_ends() {
            const f = flick()
            mouseWheel(f, 200, 150, 240, 0)
            wait(50)
            compare(f.contentX, 0)
            for (let i = 0; i < 40; ++i) mouseWheel(f, 200, 150, -240, 0)
            tryCompare(f, "contentX", maxX(f))
        }

        function test_dragging_the_columns_scrolls() {
            const f = flick()
            // Start on a column's frequency text, away from its slider.
            const x = 300, y = 225
            mousePress(f, x, y)
            for (let i = 1; i <= 10; ++i) mouseMove(f, x - i * 20, y)
            mouseRelease(f, x - 200, y)
            tryVerify(() => f.contentX > 100, 1000, "dragging the columns left moves them left, contentX " + f.contentX)
        }

        function test_dragging_the_thumb_scrolls() {
            const thumb = findChild(strip, "bandThumb")
            const f = flick()
            verify(thumb !== null)
            const start = thumb.mapToItem(strip, thumb.width / 2, thumb.height / 2)
            mousePress(strip, start.x, start.y)
            for (let i = 1; i <= 10; ++i) mouseMove(strip, start.x + i * 15, start.y)
            mouseRelease(strip, start.x + 150, start.y)
            tryVerify(() => f.contentX > 100, 1000, "dragging the thumb right moves the columns left, contentX " + f.contentX)
        }

        function test_dragging_a_slider_sets_gain_not_scroll() {
            const f = flick()
            const slider = findChild(strip, "gainSlider")
            verify(slider !== null)
            const before = EqSession.data(EqSession.index(0, 0), 261)   // GainRole
            const p = slider.mapToItem(f, slider.width / 2, slider.height / 2)
            mousePress(f, p.x, p.y)
            for (let i = 1; i <= 5; ++i) mouseMove(f, p.x - i * 2, p.y + i * 8)
            mouseRelease(f, p.x - 10, p.y + 40)
            compare(f.contentX, 0, "a slider drag does not scroll")
            verify(EqSession.data(EqSession.index(0, 0), 261) !== before, "the slider drag changed the gain")
        }
    }
}
