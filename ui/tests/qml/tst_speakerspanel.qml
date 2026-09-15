import QtQuick
import QtTest
import Isotone

// The Speakers panel on a surround output, in place of the Channels panel: its
// rows, Speaker setup, Mute, and collapsing.
Item {
    id: root
    width: 1192
    height: 402

    BandStrip {
        id: strip
        anchors.fill: parent
    }

    TestCase {
        name: "SpeakersPanel"
        when: windowShown

        function child(name) { return findChild(strip, name) }
        function value(rowName) {
            const row = child(rowName)
            return row.children[1].text
        }

        function init() {
            AppSettings.panelOpen = true
            UiState.view = "eq"
            TestHooks.useLayout(8, 0x63F)
            EqSession.muted = false
            tryVerify(() => child("layoutRow") !== null && child("layoutRow").visible)
            waitForRendering(strip)
        }
        function cleanupTestCase() { TestHooks.useLayout(2, 0x3) }

        function test_shown_instead_of_channels_on_surround() {
            verify(child("speakerSetup").visible)
            verify(child("balanceValue") === null || !child("balanceValue").visible)
            TestHooks.useLayout(2, 0x3)
            tryVerify(() => child("balanceValue").visible)
            verify(!child("layoutRow").visible)
        }

        function test_rows_show_the_setup() {
            compare(value("layoutRow"), "7.1")
            compare(value("crossoverRow"), "Off")   // no small speakers
            compare(value("upmixRow"), "Off")
            compare(value("lipSyncRow"), "0 ms")
            Speakers.setSmall(0, true)
            Speakers.crossoverHz = 100
            Speakers.upmix = 2
            Speakers.lipSyncMs = 40
            compare(value("crossoverRow"), "100 Hz")
            compare(value("upmixRow"), "No centre")
            compare(value("lipSyncRow"), "40 ms")
            Speakers.setSmall(0, false)
            Speakers.upmix = 0
            Speakers.lipSyncMs = 0
            TestHooks.useLayout(6, 0x60F)
            compare(value("layoutRow"), "5.1")
        }

        function test_speaker_setup_opens_the_speakers_view() {
            mouseClick(child("speakerSetup"))
            compare(UiState.view, "speakers")
        }

        function test_mute() {
            mouseClick(child("speakersMute"))
            verify(EqSession.muted)
            mouseClick(child("speakersMute"))
            verify(!EqSession.muted)
        }

        function test_collapses_to_a_strip_with_the_layout() {
            mouseClick(child("speakersPanelCollapse"))
            verify(!AppSettings.panelOpen)
            const expand = child("speakersPanelExpand")
            tryVerify(() => expand.visible)
            compare(expand.parent.parent.width, 52)
            compare(child("collapsedLayout").text, "7.1")
            verify(child("collapsedLayout").visible)
            mouseClick(expand)
            verify(AppSettings.panelOpen)
            tryVerify(() => child("layoutRow").visible)
        }
    }
}
