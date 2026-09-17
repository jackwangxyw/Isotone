import QtQuick
import QtTest
import Isotone

// A new spectrum frame repaints the spectrum and nothing else: repainting the
// grid and the curves 60 times a second made a maximized window lag (owner,
// 2026-09-16; 15.6 ms a frame at 2228 x 873, of which the spectrum was 1.1 ms).
Item {
    id: root
    width: 1192
    height: 440

    GraphCard {
        id: card
        anchors.fill: parent
    }

    TestCase {
        name: "GraphLayers"
        when: windowShown

        function graphPart(name) {
            const l = findChild(card, name)
            verify(l !== null, name)
            return l
        }

        function test_a_spectrum_frame_repaints_only_the_spectrum() {
            while (EqSession.count > 0) EqSession.deleteBand(0)
            EqSession.addBand(1000, 6)
            waitForRendering(card)
            const grid = graphPart("graphGrid"), spectrum = graphPart("graphSpectrum"), curves = graphPart("responseGraph")
            tryVerify(() => grid.paintCount() > 0 && spectrum.paintCount() > 0 && curves.paintCount() > 0, 2000, "each layer painted")
            wait(100)
            const before = { grid: grid.paintCount(), spectrum: spectrum.paintCount(), curves: curves.paintCount() }

            for (let i = 0; i < 5; ++i) {
                EqSession.spectrumChanged()
                waitForRendering(card)
            }
            tryVerify(() => spectrum.paintCount() > before.spectrum, 2000, "the spectrum repainted")
            compare(grid.paintCount(), before.grid, "the grid did not")
            compare(curves.paintCount(), before.curves, "the curves did not")

            // An edit repaints the curves and the spectrum (which a mute hides), not the grid.
            const edited = { grid: grid.paintCount(), spectrum: spectrum.paintCount(), curves: curves.paintCount() }
            for (let i = 1; i <= 5; ++i) {
                EqSession.setGain(0, 6 - i)
                waitForRendering(card)
            }
            EqSession.finishEdit()
            tryVerify(() => curves.paintCount() > edited.curves, 2000, "an edit repaints the curves")
            compare(grid.paintCount(), edited.grid, "an edit does not repaint the grid")

            // The range does.
            grid.rangeDb = 24
            tryVerify(() => grid.paintCount() > edited.grid, 2000, "a new range repaints the grid")
            grid.rangeDb = 15
        }
    }
}
