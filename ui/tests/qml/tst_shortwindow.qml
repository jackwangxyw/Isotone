import QtQuick
import QtTest
import Isotone

// A short window (owner, 2026-09-16): below the height the graph and the band
// strip both fit in, the Equalizer view shows one of them, the one Settings,
// General, Short window picks; and the sidebar scrolls instead of its outputs
// running into its navigation.
Item {
    id: root
    width: 1440
    height: 900

    EqualizerView {
        id: view
        width: 1192
        height: 900
    }
    Sidebar {
        id: sidebar
        x: 1192
        height: 900
    }

    TestCase {
        name: "ShortWindow"
        when: windowShown

        function graph() { return findChild(view, "equalizerGraph") }
        function strip() { return findChild(view, "equalizerStrip") }
        function cleanupTestCase() {
            GeneralSettings.setShortWindow("graph")
            AppSettings.sidebarOpen = true
        }
        function init() {
            GeneralSettings.setShortWindow("graph")
            view.height = 900
            sidebar.height = 900
            AppSettings.sidebarOpen = true
        }

        function test_tall_enough_shows_both() {
            for (const h of [900, 760, 678]) {
                view.height = h
                verify(graph().visible, h + ": graph")
                verify(strip().visible, h + ": strip")
                compare(graph().height, Math.max(200, h - 76 - 402))
            }
        }

        function test_short_shows_the_graph_or_the_bands() {
            view.height = 677
            verify(graph().visible)
            verify(!strip().visible)
            compare(graph().height, 677 - 76 - 24)
            view.height = GeneralSettings.shortWindowMinimumHeight
            compare(GeneralSettings.shortWindowMinimumHeight, 300)
            compare(graph().height, 200)

            GeneralSettings.setShortWindow("bands")
            compare(AppSettings.value("graph/shortWindow", ""), "bands")
            compare(GeneralSettings.shortWindowMinimumHeight, 478)
            view.height = 478
            verify(!graph().visible)
            verify(strip().visible)
            tryCompare(strip(), "y", 76)   // the column lays out on the next polish
            compare(strip().height, 402)
        }

        function test_a_short_sidebar_scrolls_rather_than_overlapping() {
            for (const open of [true, false]) {
                AppSettings.sidebarOpen = open
                for (const h of [900, 478, 300]) {
                    sidebar.height = h
                    waitForRendering(sidebar)
                    const nav = findChild(sidebar, "sidebarNav")
                    const foot = findChild(sidebar, open ? "sidebarFoot" : "sidebarRail")
                    const navBottom = nav.mapToItem(sidebar, 0, nav.height).y
                    const footTop = foot.mapToItem(sidebar, 0, 0).y
                    verify(footTop >= navBottom, (open ? "open" : "rail") + " at " + h + ": foot " + footTop + " under nav " + navBottom)
                    const flick = findChild(sidebar, "sidebarFlick")
                    compare(flick.interactive, flick.contentHeight > flick.height, "scrolls only when it must")
                    if (h === 900) verify(!flick.interactive, "a full height does not scroll")
                }
            }
        }
    }
}
