import QtQuick
import QtTest
import Isotone

// Click-to-edit values in a band column: click, type, Enter.
Item {
    id: root
    width: 1192
    height: 402

    property int deletes: 0
    Shortcut {
        sequence: StandardKey.Delete
        onActivated: root.deletes++
    }

    BandStrip {
        id: strip
        anchors.fill: parent
    }
    Item { id: elsewhere; focus: true }

    TestCase {
        name: "ValueField"
        when: windowShown

        readonly property int gainRole: 261
        readonly property int frequencyRole: 260
        readonly property int qRole: 262

        function value(role) { return EqSession.data(EqSession.index(0, 0), role) }
        function field(name) { return findChild(strip, name) }
        function type(text) { for (const c of text) keyClick(c) }
        function input(f) { return findChild(f, "valueInput") }

        // No output in a test: the session edits in memory only.
        function init() {
            while (EqSession.count > 0) EqSession.deleteBand(0)
            EqSession.byFrequency = false
            EqSession.addBand(1000, 2)
            // The new column is laid out before it is clicked.
            tryVerify(() => field("gainValue") !== null && field("gainValue").width > 0)
            waitForRendering(strip)
            elsewhere.forceActiveFocus()
            root.deletes = 0
        }

        function test_typing_a_gain_and_enter_sets_it() {
            const f = field("gainValue")
            mouseClick(f)
            verify(f.editing)
            type("-4.5")
            keyClick(Qt.Key_Return)
            verify(!f.editing)
            fuzzyCompare(value(gainRole), -4.5, 1e-9)
            compare(f.text, "−4.5 dB")
        }

        function test_escape_keeps_the_value() {
            const f = field("gainValue")
            mouseClick(f)
            type("9")
            keyClick(Qt.Key_Escape)
            verify(!f.editing)
            fuzzyCompare(value(gainRole), 2, 1e-9)
        }

        function test_text_that_is_not_a_value_stays_open() {
            const f = field("gainValue")
            mouseClick(f)
            type("abc")
            keyClick(Qt.Key_Return)
            verify(f.editing)
            compare(input(f).selectedText, "abc")
            fuzzyCompare(value(gainRole), 2, 1e-9)
            keyClick(Qt.Key_Escape)
            verify(!f.editing)
        }

        function test_enter_on_the_shown_text_changes_nothing() {
            EqSession.setFrequency(0, 1234.5)
            const f = field("frequencyValue")
            compare(f.text, "1.23 kHz")
            mouseClick(f)
            keyClick(Qt.Key_Return)
            verify(!f.editing)
            fuzzyCompare(value(frequencyRole), 1234.5, 1e-9)
        }

        function test_frequency_in_kilohertz() {
            const f = field("frequencyValue")
            mouseClick(f)
            type("2.5k")
            keyClick(Qt.Key_Enter)
            fuzzyCompare(value(frequencyRole), 2500, 1e-9)
        }

        function test_width_in_the_band_unit() {
            const f = field("widthValue")
            mouseClick(f)
            type("q 3")
            keyClick(Qt.Key_Return)
            fuzzyCompare(value(qRole), 3, 1e-9)
            compare(f.text, "Q 3.00")
        }

        function test_leaving_the_field_applies_a_value() {
            const f = field("gainValue")
            mouseClick(f)
            type("5")
            elsewhere.forceActiveFocus()
            verify(!f.editing)
            fuzzyCompare(value(gainRole), 5, 1e-9)
        }

        function test_leaving_with_text_that_is_not_a_value_keeps_the_value() {
            const f = field("gainValue")
            mouseClick(f)
            type("x")
            elsewhere.forceActiveFocus()
            verify(!f.editing)
            fuzzyCompare(value(gainRole), 2, 1e-9)
        }

        function test_delete_while_typing_edits_the_text_not_the_bands() {
            const f = field("gainValue")
            mouseClick(f)
            type("12")
            keyClick(Qt.Key_Home)
            keyClick(Qt.Key_Delete)
            compare(input(f).text, "2")
            compare(root.deletes, 0)
            keyClick(Qt.Key_Escape)
            elsewhere.forceActiveFocus()
            keyClick(Qt.Key_Delete)
            compare(root.deletes, 1, "the shortcut still works outside a field")
        }
    }
}
