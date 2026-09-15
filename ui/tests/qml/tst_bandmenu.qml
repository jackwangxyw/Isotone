import QtQuick
import QtTest
import Isotone

// The band popover, opened from a column's type name as the app does.
Item {
    id: root
    width: 1192
    height: 900

    BandStrip {
        id: strip
        y: 498
        width: parent.width
        height: 402
        onMenuRequested: (row, x, above, below) => menu.openAt(row, x, above, below)
    }
    BandMenu {
        id: menu
        anchors.fill: parent
        z: 10
    }

    TestCase {
        name: "BandMenu"
        when: windowShown

        function role(row, r) { return EqSession.data(EqSession.index(row, 0), r) }
        function panel() { return findChild(menu, "bandMenuPanel") }
        function openFromColumn() {
            mouseClick(findChild(strip, "typeName"))
            verify(menu.open)
        }
        function clickChild(name) {
            const item = findChild(panel(), name)
            verify(item !== null, name)
            mouseClick(item)
        }

        function init() {
            menu.close()
            while (EqSession.count > 0) EqSession.deleteBand(0)
            EqSession.byFrequency = false
            EqSession.addBand(1000, 4)
            tryVerify(() => findChild(strip, "typeName") !== null && findChild(strip, "typeName").width > 0)
            waitForRendering(strip)
        }

        function test_opens_above_the_column_centred_on_it() {
            openFromColumn()
            const p = panel()
            const column = findChild(strip, "typeName").parent.parent.parent   // Row, Column, BandColumn
            const centre = column.mapToItem(root, column.width / 2, 0).x
            const panelCentre = p.x + p.width / 2
            verify(Math.abs(panelCentre - Math.max(8 + p.width / 2, centre)) < 1, "centred on the column, or held inside the window")
            verify(p.y + p.height <= findChild(strip, "typeName").mapToItem(root, 0, 0).y, "above the type name")
        }

        function test_opens_below_where_there_is_no_room_above() {
            menu.openAt(0, 600, 40, 60)
            verify(panel().y >= 60)
        }

        function test_a_tile_sets_the_type() {
            openFromColumn()
            clickChild("typeTile1")   // low pass
            compare(role(0, EqSession.TypeRole), 1)
            compare(role(0, EqSession.TypeNameRole), "Low pass")
            verify(menu.open, "stays open")
            verify(findChild(panel(), "typeTile1").current)
            verify(!findChild(panel(), "typeTile0").current)
        }

        function test_reset_gain_is_greyed_for_a_type_without_gain() {
            openFromColumn()
            verify(findChild(panel(), "menuResetGain").active)
            clickChild("typeTile4")   // notch
            verify(!findChild(panel(), "menuResetGain").active)
        }

        function test_reset_gain() {
            openFromColumn()
            clickChild("menuResetGain")
            compare(role(0, EqSession.GainRole), 0)
        }

        function test_channels() {
            openFromColumn()
            const seg = findChild(panel(), "bandChannels")
            verify(seg !== null)
            compare(seg.current, 2)
            seg.picked(0)
            compare(role(0, EqSession.ChannelsRole), 0)
            compare(role(0, EqSession.TargetRole), "L")
            compare(seg.current, 0)
        }

        function test_enabled() {
            openFromColumn()
            const toggle = findChild(panel(), "bandEnabled")
            verify(toggle.checked)
            mouseClick(toggle)
            compare(role(0, EqSession.EnabledRole), false)
            verify(!toggle.checked)
        }

        function test_duplicate_closes_and_adds_a_band() {
            openFromColumn()
            clickChild("menuDuplicate")
            verify(!menu.open)
            compare(EqSession.count, 2)
            compare(EqSession.selectedRow, 1)
        }

        function test_delete_closes_and_removes_the_band() {
            openFromColumn()
            clickChild("menuDelete")
            verify(!menu.open)
            compare(EqSession.count, 0)
        }

        function test_escape_closes() {
            openFromColumn()
            keyClick(Qt.Key_Escape)
            verify(!menu.open)
        }

        function test_a_press_outside_closes_and_changes_nothing() {
            openFromColumn()
            mouseClick(root, 5, 5)
            verify(!menu.open)
            compare(EqSession.count, 1)
            compare(role(0, EqSession.TypeRole), 0)
        }
    }
}
