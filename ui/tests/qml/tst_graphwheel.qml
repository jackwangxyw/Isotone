import QtQuick
import QtTest
import Isotone

// A wheel spin on a handle: each notch live, one commit once the wheel is still,
// or when another band is picked.
Item {
    id: root
    width: 800
    height: 500

    GraphCard {
        id: card
        width: 760
        height: 440
    }

    SignalSpy { id: committed; target: EqSession; signalName: "committed" }

    TestCase {
        name: "GraphWheel"
        when: windowShown

        function handles() {
            const list = []
            const repeaterParent = findChild(card, "responseGraph")
            for (let i = 0; i < repeaterParent.children.length; ++i)
                if (repeaterParent.children[i].objectName === "handle") list.push(repeaterParent.children[i])
            list.sort((a, b) => a.index - b.index)
            return list
        }

        function init() {
            TestHooks.useLayout(2, 3)
            while (EqSession.count > 0) EqSession.deleteBand(0)
            EqSession.addBand(200, 6)
            EqSession.addBand(4000, -6)
            waitForRendering(card)
            wait(400)   // nothing left pending from another test
            committed.clear()
        }

        function test_notches_commit_once_when_still() {
            const h = handles()
            compare(h.length, 2)
            const before = h[0].q
            for (let i = 0; i < 5; ++i) mouseWheel(h[0], 20, 20, 0, 120)
            verify(h[0].q > before * 1.3, "each notch is applied live")
            compare(committed.count, 0)
            tryCompare(committed, "count", 1, 1000)
            wait(500)
            compare(committed.count, 1)
            // The spin is one step.
            EqSession.undo()
            fuzzyCompare(handles()[0].q, before, 1e-9)
        }

        function test_another_band_commits_the_spin() {
            const h = handles()
            for (let i = 0; i < 3; ++i) mouseWheel(h[0], 20, 20, 0, 120)
            compare(committed.count, 0)
            mouseWheel(h[1], 20, 20, 0, -120)
            compare(committed.count, 1)   // band 1's notches, at once
            tryCompare(committed, "count", 2, 1000)
        }

        function test_selecting_another_band_commits_the_spin() {
            const h = handles()
            mouseWheel(h[0], 20, 20, 0, 120)
            compare(committed.count, 0)
            EqSession.select(1)
            compare(committed.count, 1)
            wait(500)
            compare(committed.count, 1)
        }
    }
}
