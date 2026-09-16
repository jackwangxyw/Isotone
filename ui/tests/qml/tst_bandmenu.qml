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
        function column() { return findChild(strip, "typeName").parent.parent.parent }   // Row, Column, BandColumn
        function panelCentre() { return panel().x + panel().width / 2 }
        function columnCentre() {
            const c = column()
            return c.mapToItem(root, c.width / 2, 0).x
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
            verify(Math.abs(panelCentre() - Math.max(8 + p.width / 2, columnCentre())) < 1, "centred on the column, or held inside the window")
            verify(p.y + p.height <= findChild(strip, "typeName").mapToItem(root, 0, 0).y, "above the type name")
        }

        // Right-click anywhere in the column, not only on the type name.
        function test_a_right_click_in_the_column_opens_it_where_the_type_name_does() {
            const c = column()
            mouseClick(c, c.width / 2, c.height - 10, Qt.RightButton)
            verify(menu.open, "a right click low in the column opens the popover")
            const p = panel()
            verify(Math.abs(panelCentre() - Math.max(8 + p.width / 2, columnCentre())) < 1, "centred on the column")
            verify(p.y + p.height <= findChild(strip, "typeName").mapToItem(root, 0, 0).y, "above the type name")
            compare(EqSession.selectedRow, 0)
        }

        function test_a_right_click_on_the_gain_slider_opens_it() {
            const c = column()
            const slider = findChild(strip, "gainSlider")
            const p = slider.mapToItem(c, slider.width / 2, slider.height / 2)
            const gain = role(0, EqSession.GainRole)
            mouseClick(c, p.x, p.y, Qt.RightButton)
            verify(menu.open, "a right click on the slider opens the popover")
            compare(role(0, EqSession.GainRole), gain, "and leaves the gain alone")
        }

        function test_a_left_click_in_the_column_selects_without_opening_it() {
            const c = column()
            mouseClick(c, c.width / 2, c.height - 10)
            verify(!menu.open)
            compare(EqSession.selectedRow, 0)
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
            const column = findChild(strip, "typeName").parent.parent.parent   // Row, Column, BandColumn
            compare(column.opacity, 0.5, "a disabled band's column is faded")
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
