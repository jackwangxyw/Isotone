import QtQuick
import QtTest
import Isotone

// The status mark is a bar, and the two states that mean "nothing is here"
// draw nothing.
Item {
    id: root
    width: 200
    height: 200

    StatusDot { id: ok; status: "ok" }
    StatusDot { id: warn; status: "warn"; y: 20 }
    StatusDot { id: bad; status: "bad"; y: 40 }
    StatusDot { id: idle; status: ""; y: 60 }
    StatusDot { id: off; status: "off"; y: 80 }

    function mark(item) {
        // The one Rectangle inside is the bar itself.
        for (let i = 0; i < item.children.length; ++i)
            if (item.children[i] instanceof Rectangle) return item.children[i]
        return null
    }

    TestCase {
        name: "StatusDot"

        function test_it_is_a_bar_not_a_circle() {
            // Taller than it is wide is the whole point: a circle of 6 is what
            // the Devices table had eleven of.
            compare(ok.height, 14)
            const bar = root.mark(ok)
            verify(bar !== null)
            compare(bar.width, 3)
            compare(bar.height, 14)
            verify(bar.height > bar.width * 3)
        }

        function test_the_box_still_reserves_the_old_width() {
            // Six wide, so no call site reflows.
            compare(ok.width, 6)
        }

        function test_each_live_state_carries_its_colour() {
            compare(root.mark(ok).color, Theme.ok)
            compare(root.mark(warn).color, Theme.warning)
            compare(root.mark(bad).color, Theme.danger)
            for (const live of [ok, warn, bad]) {
                verify(root.mark(live).visible)
                compare(root.mark(live).opacity, 1)
            }
        }

        function test_idle_is_the_same_bar_darkened() {
            // An output that is not playing: the mark stays, the light goes.
            const bar = root.mark(idle)
            verify(bar.visible)
            compare(bar.color, Theme.muted)
            verify(bar.opacity < 1)
        }

        function test_off_draws_nothing() {
            // not_installed and unplugged (devicestatus.cpp). A mark for
            // "there is nothing here" is worse than no mark.
            verify(off.blank)
            verify(!root.mark(off).visible)
        }
    }
}
