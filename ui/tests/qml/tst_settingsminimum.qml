import QtQuick
import QtTest
import Isotone

// Settings at the minimum window, 1120 x 760 with the sidebar collapsed (72 px):
// every page fits across and scrolls down to its last row.
Item {
    id: root
    width: 1120 - 72
    height: 760

    SettingsView {
        id: view
        anchors.fill: parent
    }
    Item {
        id: overlay
        anchors.fill: parent
        z: 100
    }
    Component.onCompleted: UiState.overlay = overlay

    TestCase {
        name: "SettingsMinimum"
        when: windowShown

        function cleanupTestCase() { UiState.settingsTab = "general" }

        function test_every_page_fits_and_scrolls() {
            const flick = findChild(view, "settingsFlick")
            for (const tab of ["general", "appearance", "shortcuts", "about"]) {
                UiState.settingsTab = tab
                const page = flick.contentItem.children[0].item   // the Loader's page
                tryVerify(() => page !== null && page.implicitHeight > 0, 5000, tab)
                waitForRendering(view)
                verify(page.childrenRect.x + page.childrenRect.width <= flick.width, tab + " fits across")
                if (flick.contentHeight > flick.height) {
                    flick.contentY = flick.contentHeight - flick.height
                    verify(page.mapToItem(flick, 0, page.implicitHeight).y <= flick.height, tab + " scrolls to its end")
                    flick.contentY = 0
                }
            }
            UiState.settingsTab = "general"
            tryVerify(() => flick.contentHeight > flick.height, 5000, "General is taller than the window, and scrolls")
            verify(flick.interactive)
        }
    }
}
