import QtQuick
import QtTest
import Isotone

// The sidebar opens and collapses again and again: the navigation and Settings
// went on sitting where the rail put them after expanding (owner, 2026-09-15).
Item {
    id: root
    width: 248
    height: 900

    Sidebar {
        id: sidebar
        height: parent.height
    }

    TestCase {
        name: "Sidebar"
        when: windowShown

        // Settings is in both the open foot and the rail: look inside the one in use.
        function item(name) { return findChild(sidebar, name) }
        function settingsItem() { return findChild(findChild(sidebar, AppSettings.sidebarOpen ? "sidebarFoot" : "sidebarRail"), "nav_settings") }
        function centre(name) {
            const it = item(name)
            return it.mapToItem(sidebar, it.width / 2, 0).x
        }

        function init() { AppSettings.sidebarOpen = true }
        function cleanupTestCase() { AppSettings.sidebarOpen = true }

        function test_collapsing_and_expanding_puts_everything_back() {
            const open = { eq: item("nav_eq").x, settings: settingsItem().x, width: item("nav_eq").width }
            verify(open.width > 100, "an open item fills the sidebar")

            AppSettings.sidebarOpen = false
            tryCompare(sidebar, "width", 72)
            compare(item("nav_eq").width, 44)
            fuzzyCompare(centre("nav_eq"), 36, 1, "centred on the rail")
            const s = settingsItem()
            fuzzyCompare(s.mapToItem(sidebar, s.width / 2, 0).x, 36, 1, "centred on the rail")

            AppSettings.sidebarOpen = true
            tryCompare(sidebar, "width", 248)
            compare(item("nav_eq").width, open.width)
            compare(item("nav_eq").x, open.eq)
            compare(settingsItem().x, open.settings)
        }

        function test_the_toggle_and_the_logo_move_with_it() {
            const open = { toggle: item("sidebarToggle").x, toggleY: item("sidebarToggle").y }
            AppSettings.sidebarOpen = false
            tryCompare(sidebar, "width", 72)
            fuzzyCompare(item("sidebarToggle").x + item("sidebarToggle").width / 2, 36, 1)
            AppSettings.sidebarOpen = true
            tryCompare(sidebar, "width", 248)
            compare(item("sidebarToggle").x, open.toggle)
            compare(item("sidebarToggle").y, open.toggleY)
        }
    }
}
