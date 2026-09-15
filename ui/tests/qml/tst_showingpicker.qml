import QtQuick
import QtTest
import Isotone

// The top bar's Showing picker on a surround output: its list, and what the
// graph draws after a pick.
Item {
    id: root
    width: 1192
    height: 900

    Column {
        anchors.fill: parent
        TopBar {
            id: bar
            width: parent.width
        }
        GraphCard {
            id: card
            x: 32
            width: parent.width - 64
            height: 422
        }
    }
    Item {
        id: overlay
        anchors.fill: parent
        z: 100
    }

    TestCase {
        name: "ShowingPicker"
        when: windowShown

        function graph() {
            for (let i = 0; i < card.children.length; ++i)
                if (card.children[i].compositeAt !== undefined) return card.children[i]
            return null
        }
        function picker() { return findChild(bar, "showingPicker") }
        function menu() { return findChild(overlay, "showingMenu") }

        function initTestCase() {
            UiState.overlay = overlay
            TestHooks.useLayout(8, 0x63F)
            while (EqSession.count > 0) EqSession.deleteBand(0)
            EqSession.addBand(1000, 3)
            EqSession.setChannelMask(0, 0x07)   // Front
            EqSession.addBand(3000, -2)
            EqSession.setChannelMask(1, 0x07)
            EqSession.addBand(60, 6)
            EqSession.setChannelMask(2, 0x08)   // Sub
        }
        function cleanupTestCase() { TestHooks.useLayout(2, 0x3) }
        function init() {
            Speakers.showing = "all"
            if (menu()) menu().close()
            tryVerify(() => picker().visible && picker().width > 0)
            waitForRendering(bar)
        }

        function test_shown_instead_of_left_and_right() {
            verify(picker().visible)
            verify(!findChild(bar, "viewChannel").visible)
            compare(findChild(picker(), "showingLabel").text, "All speakers")
        }

        function test_lists_all_speakers_groups_and_speakers() {
            mouseClick(picker())
            tryVerify(() => menu() !== null && menu().open)
            for (const key of ["all", "group:Front", "group:Surround", "group:Sub", "speaker:0", "speaker:3", "speaker:7"])
                verify(findChild(menu(), "showing_" + key) !== null, key)
            const panelItem = menu().panel
            const p = picker().mapToItem(overlay, 0, picker().height)
            verify(panelItem.y >= p.y, "under the picker")
        }

        function test_picking_a_speaker_draws_its_channel() {
            fuzzyCompare(graph().compositeAt(60), 0, 0.5)   // All speakers: the front channels have the most bands
            mouseClick(picker())
            tryVerify(() => menu() !== null && menu().open)
            const item = findChild(menu(), "showing_speaker:3")
            waitForRendering(item)
            mouseClick(item)
            compare(Speakers.showing, "speaker:3")
            compare(EqSession.showingMask, 0x08)
            verify(!menu().open)
            compare(findChild(picker(), "showingLabel").text, "Subwoofer")
            fuzzyCompare(graph().compositeAt(60), 6, 0.1)
            verify(!graph().onView(0))
            verify(graph().onView(2))
        }

        function test_picking_a_group() {
            mouseClick(picker())
            tryVerify(() => menu() !== null && menu().open)
            const item = findChild(menu(), "showing_group:Sub")
            waitForRendering(item)
            mouseClick(item)
            compare(findChild(picker(), "showingLabel").text, "Sub")
            compare(EqSession.showingMask, 0x08)
            fuzzyCompare(graph().compositeAt(60), 6, 0.1)
        }
    }
}
