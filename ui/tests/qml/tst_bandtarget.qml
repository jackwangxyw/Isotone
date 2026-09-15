import QtQuick
import QtTest
import Isotone

// The band popover's Target row on a surround output: group and speaker chips
// set the band's channel mask, and the column shows the target's name.
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
        name: "BandTarget"
        when: windowShown

        readonly property int maskRole: EqSession.ChannelMaskRole
        function role(r) { return EqSession.data(EqSession.index(0, 0), r) }
        function panel() { return findChild(menu, "bandMenuPanel") }
        function chip(name) {
            const c = findChild(panel(), name)
            verify(c !== null, name)
            return c
        }
        function open() {
            mouseClick(findChild(strip, "typeName"))
            verify(menu.open)
            waitForRendering(panel())
        }

        function init() {
            menu.close()
            TestHooks.useLayout(8, 0x63F)
            while (EqSession.count > 0) EqSession.deleteBand(0)
            EqSession.addBand(1000, 4)
            tryVerify(() => findChild(strip, "typeName") !== null && findChild(strip, "typeName").width > 0)
            waitForRendering(strip)
        }
        function cleanupTestCase() { TestHooks.useLayout(2, 0x3) }

        function test_target_row_instead_of_channels() {
            open()
            verify(findChild(panel(), "bandTarget").visible)
            verify(!findChild(panel(), "bandChannels").parent.visible)
            for (const code of ["L", "R", "C", "LFE", "RL", "RR", "SL", "SR"]) verify(chip("targetSpeaker_" + code).visible, code)
            for (const g of ["All", "Front", "Surround", "Sub"]) verify(chip("targetGroup_" + g).visible, g)
            verify(chip("targetGroup_All").lit)
            verify(chip("targetSpeaker_SL").lit)
        }

        function test_a_group_sets_the_mask() {
            open()
            mouseClick(chip("targetGroup_Front"))
            compare(role(maskRole), 0x07)
            compare(role(EqSession.TargetRole), "Front")
            verify(chip("targetGroup_Front").lit)
            verify(!chip("targetGroup_All").lit)
            verify(chip("targetSpeaker_C").lit)
            verify(!chip("targetSpeaker_SL").lit)
            compare(findChild(strip, "typeName").parent.parent.parent.target, "Front")

            mouseClick(chip("targetGroup_Sub"))
            compare(role(maskRole), 0x08)
            compare(role(EqSession.TargetRole), "Sub")
            mouseClick(chip("targetGroup_All"))
            compare(role(maskRole), 0)
            compare(role(EqSession.TargetRole), "All")
        }

        function test_speakers_join_and_leave() {
            open()
            mouseClick(chip("targetGroup_Surround"))
            compare(role(maskRole), 0xF0)
            mouseClick(chip("targetSpeaker_RL"))
            compare(role(maskRole), 0xE0)
            compare(role(EqSession.TargetRole), "RR SL SR")
            verify(!chip("targetSpeaker_RL").lit)
            mouseClick(chip("targetSpeaker_C"))
            compare(role(maskRole), 0xE4)
            mouseClick(chip("targetGroup_Sub"))
            mouseClick(chip("targetSpeaker_LFE"))   // the last one stays
            compare(role(maskRole), 0x08)
            mouseClick(chip("targetSpeaker_C"))
            compare(role(maskRole), 0x0C)
            compare(role(EqSession.TargetRole), "C LFE")
            mouseClick(chip("targetSpeaker_LFE"))
            compare(role(EqSession.TargetRole), "Centre")
        }

        function test_user_groups_are_chips() {
            verify(Speakers.addGroup("Wides", ["SL", "SR"]))
            open()
            mouseClick(chip("targetGroup_Wides"))
            compare(role(maskRole), 0xC0)
            compare(role(EqSession.TargetRole), "Wides")
            Speakers.removeGroup("Wides")
            compare(role(EqSession.TargetRole), "SL SR")
        }

        function test_stereo_keeps_channels() {
            TestHooks.useLayout(2, 0x3)
            open()
            verify(!findChild(panel(), "bandTarget").visible)
            verify(findChild(panel(), "bandChannels").parent.visible)
        }
    }
}
