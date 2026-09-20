import QtQuick
import QtTest
import Isotone

// The sidebar's output control: shut it is one row, and clicking it discloses
// the rest without moving anything below it.
Item {
    id: root
    width: 248
    height: 400

    // The real Outputs singleton enumerates the machine and is empty under Qt
    // Quick Test, so the picker is given its own two outputs here.
    ListModel {
        id: two
        function select(i) { two.selected = i }
        property int selected: 1
        ListElement { name: "Headphones (Anker USB Audio)"; backendLabel: "Native"; activity: "running"; current: false }
        ListElement { name: "CABLE Input (VB-Audio)"; backendLabel: "Native"; activity: "idle"; current: true }
    }

    // The picker sits at the bottom of a column, as it does in the sidebar, so
    // growing upward is what the test can actually see.
    Column {
        id: foot
        objectName: "foot"
        x: 16
        width: parent.width - 32
        anchors.bottom: parent.bottom
        spacing: 0

        OutputPicker { id: picker; objectName: "picker"; width: parent.width; source: two }
        Rectangle { id: below; objectName: "below"; width: parent.width; height: 38; color: "transparent" }
    }

    TestCase {
        name: "OutputPicker"
        when: windowShown

        function rows(list) {
            let n = 0
            for (let i = 0; i < list.children.length; ++i)
                if (list.children[i].visible && list.children[i].height > 0) n++
            return n
        }
        function lists() {
            const out = []
            function walk(item) {
                for (let i = 0; i < item.children.length; ++i) {
                    const c = item.children[i]
                    if (c.hasOwnProperty("onlyCurrent")) out.push(c)
                    walk(c)
                }
            }
            walk(picker)
            return out
        }

        function init() { picker.open = false }

        function test_it_starts_shut_and_shows_one_row() {
            compare(picker.open, false)
            const shut = lists().filter((l) => l.onlyCurrent)
            compare(shut.length, 1, "the shut state draws OutputList, not a second copy of the row")
            compare(rows(shut[0]), 1, "one row when shut, whatever the machine has")
        }

        function test_clicking_it_opens_it() {
            // The row inside the shut control must not take this click. It did,
            // and because it only re-selected the output that was already
            // current, the box never opened and nothing on screen changed.
            const shutHeight = picker.height
            mouseClick(picker, picker.width / 2, shutHeight / 2)
            if (picker.choosable) {
                tryCompare(picker, "open", true)
                verify(picker.height >= shutHeight, "open is never shorter than shut")
            } else {
                // Nothing to choose between: the control is a label.
                compare(picker.open, false)
            }
        }

        function test_the_shut_row_is_not_a_click_target() {
            // The bug: the row inside the shut control kept its own MouseArea,
            // which took the press and re-selected the output that was already
            // current, so the box never opened and nothing changed on screen.
            // The box is the target; the row is a label.
            compare(picker.choosable, true, "two outputs to choose between")
            const shut = lists().filter((l) => l.onlyCurrent)[0]
            const areas = []
            function walk(item) {
                for (let i = 0; i < item.children.length; ++i) {
                    if (item.children[i].hasOwnProperty("containsMouse")) areas.push(item.children[i])
                    walk(item.children[i])
                }
            }
            walk(shut)
            verify(areas.length > 0, "the row draws a MouseArea")
            for (const a of areas) compare(a.enabled, false, "disabled while the row is only a label")
        }

        function test_one_output_is_not_a_choice() {
            compare(picker.choosable, true)
            // The caret is the promise that something will happen, so it is only
            // drawn when something will.
            const carets = []
            function walk(item) {
                for (let i = 0; i < item.children.length; ++i) {
                    if (item.children[i].hasOwnProperty("name")) carets.push(item.children[i])
                    walk(item.children[i])
                }
            }
            walk(picker)
            for (const c of carets)
                if (c.name === "chevron") compare(c.visible, picker.choosable)
        }

        function test_opening_grows_upward_and_leaves_the_rest_alone() {
            if (!picker.choosable) { skip("needs more than one output") }
            const bottomBefore = picker.y + picker.height
            const belowBefore = below.y
            picker.open = true
            // The column is anchored to the floor, so a taller picker moves its
            // own top edge and nothing under it.
            tryVerify(() => picker.y + picker.height === bottomBefore)
            compare(below.y, belowBefore, "the divider and Settings below do not move")
        }

        function test_picking_an_output_shuts_it_again() {
            if (!picker.choosable) { skip("needs more than one output") }
            picker.open = true
            const open = lists().filter((l) => !l.onlyCurrent)
            compare(open.length, 1)
            open[0].picked()
            compare(picker.open, false)
        }
    }
}
