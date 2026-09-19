import QtQuick
import QtTest
import Isotone

// The import dialog on the sample files in data/, and a file dropped on the
// window. No output in a test: an import loads into the session in memory only.
Item {
    id: root
    width: 1440
    height: 900

    Item {
        id: overlay
        anchors.fill: parent
        PresetPrompts {}
        DropOverlay { id: drop; anchors.fill: parent }
    }

    TestCase {
        name: "ImportDialog"
        when: windowShown

        readonly property url parametric: Qt.resolvedUrl("data/Sennheiser HD 650 ParametricEQ.txt")
        readonly property url graphic: Qt.resolvedUrl("data/HD 650 GraphicEQ.txt")
        readonly property url filterCurve: Qt.resolvedUrl("data/TC8FD05-04 FilterCurve.txt")
        readonly property url convolution: Qt.resolvedUrl("data/Room Convolution.txt")

        function child(name) { return findChild(overlay, name) }
        function noDialog() { return child("dialogCard") === null }

        function initTestCase() { UiState.overlay = overlay }
        function init() {
            while (Presets.count > 0) Presets.remove(Presets.names[0])
            while (EqSession.count > 0) EqSession.deleteBand(0)
            Presets.assign("", "")
        }
        function cleanup() {
            for (const d of Array.from(overlay.children)) if (d.cardWidth !== undefined) d.close()
            tryVerify(noDialog)
        }

        function test_a_file_with_filters_and_lines_it_skips() {
            const d = PresetActions.showImport(parametric)
            verify(d !== null)
            waitForRendering(d)
            compare(child("importFileName").text, "Sennheiser HD 650 ParametricEQ.txt")
            compare(child("importPreamp").value, "−6.1 dB")
            compare(child("importFilters").value, "6")
            compare(child("importSkipped").value, "3 lines")
            compare(child("importSkipped").colour, Theme.text)
            verify(child("importSkippedList").visible)
            const lines = d.preview.skipped
            compare(lines.length, 3)
            compare(lines[0].line, 7)
            compare(lines[0].text, "Include: room.txt")
            compare(lines[1].line, 9)
            compare(lines[2].text, "Convolution: room.wav")
            compare(child("importName").text, "Sennheiser HD 650 ParametricEQ")
            fuzzyCompare(child("importCurve").compositeAt(20), 5.5, 0.3)   // the low shelf

            const confirm = child("importConfirm")
            verify(confirm.active)
            child("importName").text = "HD 650 · oratory1990"
            mouseClick(confirm)
            tryVerify(noDialog)
            compare(Presets.names, ["HD 650 · oratory1990"])
            compare(Presets.currentName, "HD 650 · oratory1990")
            compare(EqSession.count, 6)
            fuzzyCompare(EqSession.preampDb, -6.1, 1e-9)
            verify(!Presets.modified)
        }

        // A curve file holds a magnitude curve, not filters: bands are fitted to it
        // (the owner imported a FilterCurve from Peace, 2026-09-15).
        function test_a_graphic_eq_file_is_fitted() {
            const d = PresetActions.showImport(graphic)
            verify(d !== null)
            waitForRendering(d)
            verify(Number(child("importFilters").value) > 0)
            verify(child("importFit").visible)
            compare(child("importFit").value, "5 points, ±" + d.preview.fitWorstDb.toFixed(1) + " dB")
            verify(d.preview.fitWorstDb < 1.0)
            compare(child("importSkipped").value, "0 lines")
            verify(!child("importSkippedList").visible)
            // The curve it drew: about -2 dB across the few points the file has.
            fuzzyCompare(child("importCurve").compositeAt(21), -2.0, 0.6)
            verify(child("importConfirm").active)
            mouseClick(child("importConfirm"))
            tryVerify(noDialog)
            compare(Presets.count, 1)
        }

        function test_a_filter_curve_file_is_fitted() {
            const d = PresetActions.showImport(filterCurve)
            verify(d !== null)
            waitForRendering(d)
            compare(child("importFileName").text, "TC8FD05-04 FilterCurve.txt")
            // A handful of filters, not one per third of an octave (owner, 2026-09-15).
            const filters = Number(child("importFilters").value)
            verify(filters > 0 && filters <= 12, "filters " + filters)
            verify(child("importFit").visible)
            verify(d.preview.curvePoints === 50)
            // The shape of the owner's curve: a deep bass cut, a lift around 137 Hz,
            // a dip at 250, and up again at 10 kHz.
            const curve = child("importCurve")
            verify(curve.compositeAt(25) < -20)   // the high pass the file was made with
            fuzzyCompare(curve.compositeAt(137), 2.558, 0.6)
            fuzzyCompare(curve.compositeAt(253.6), -2.299, 0.6)
            fuzzyCompare(curve.compositeAt(10211), 4.323, 0.5)
            verify(child("importConfirm").active)
        }

        function test_a_file_with_nothing_usable_cannot_be_imported() {
            const d = PresetActions.showImport(convolution)
            verify(d !== null)
            compare(child("importFilters").value, "0")
            compare(child("importPreamp").value, "+0.0 dB")
            compare(child("importSkipped").value, "1 line")
            compare(child("importSkipped").colour, Theme.warning)
            verify(!child("importFit").visible)
            const confirm = child("importConfirm")
            verify(!confirm.active)
            mouseClick(confirm)
            verify(!noDialog())
            compare(Presets.count, 0)
            mouseClick(child("importCancel"))
            tryVerify(noDialog)
            compare(Presets.count, 0)
        }

        function test_an_empty_name_cannot_be_imported() {
            PresetActions.showImport(parametric)
            child("importName").text = "  "
            verify(!child("importConfirm").active)
        }

        function test_importing_over_unsaved_changes_asks_first() {
            EqSession.addBand(700, -2)
            verify(Presets.modified)
            PresetActions.showImport(parametric)
            mouseClick(child("importConfirm"))
            const cancel = child("unsavedCancel")
            verify(cancel !== null)
            mouseClick(cancel)
            compare(Presets.count, 0)
            compare(EqSession.count, 1)
            tryVerify(() => child("importConfirm") !== null && child("importConfirm").visible)
            mouseClick(child("importConfirm"))
            mouseClick(child("unsavedDiscard"))
            tryVerify(noDialog)
            compare(Presets.count, 1)
            compare(EqSession.count, 6)
        }

        function test_a_file_that_cannot_be_read_opens_nothing() {
            verify(PresetActions.showImport(Qt.resolvedUrl("data/missing.txt")) === null)
            verify(noDialog())
        }

        // The platform's drag events cannot be sent from here: the overlay's steps are.
        function test_dropping_a_file_shows_the_overlay_then_the_dialog() {
            verify(!findChild(drop, "dropOverlay").visible)
            verify(drop.enter([parametric]))
            compare(findChild(drop, "dropFileName").text, "Sennheiser HD 650 ParametricEQ.txt")
            verify(findChild(drop, "dropOverlay").visible)
            drop.leave()
            verify(!findChild(drop, "dropOverlay").visible)
            verify(!drop.enter([]))
            verify(!findChild(drop, "dropOverlay").visible)

            drop.enter([parametric])
            verify(drop.dropFiles([parametric]) !== null)
            verify(!findChild(drop, "dropOverlay").visible)
            compare(child("importFilters").value, "6")
        }
    }
}
