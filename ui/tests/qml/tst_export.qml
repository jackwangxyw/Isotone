import QtQuick
import QtCore
import QtTest
import Isotone

// The export dialog writes the current EQ as Equalizer APO text. No output in a
// test, so no layout choice (stereo).
Item {
    id: root
    width: 1440
    height: 900

    Item {
        id: overlay
        anchors.fill: parent
    }

    TestCase {
        name: "ExportDialog"
        when: windowShown

        readonly property url scratch: StandardPaths.writableLocation(StandardPaths.TempLocation) + "/isotone-export-test.txt"

        function child(name) { return findChild(overlay, name) }
        function readText(url) {
            const xhr = new XMLHttpRequest()
            xhr.open("GET", url, false)
            xhr.send()
            return xhr.responseText
        }

        function initTestCase() { UiState.overlay = overlay }
        function init() {
            while (Presets.count > 0) Presets.remove(Presets.names[0])
            while (EqSession.count > 0) EqSession.deleteBand(0)
            Presets.assign("", "")
        }

        function test_writes_the_eq_as_equalizer_apo_text() {
            EqSession.addBand(105, 5.5)
            EqSession.addBand(3100, -2.5)
            EqSession.setChannels(1, 1)
            EqSession.preampDb = -6.1
            EqSession.finishEdit()
            Presets.saveAs("HD 650 · tuned")

            const d = PresetActions.openExport()
            verify(d !== null)
            compare(child("exportName").text, "HD 650 · tuned")
            verify(!child("exportLayout").parent.visible)   // stereo
            compare(d.fileName(), "HD 650 · tuned.txt")
            child("exportName").text = "a/b:c"
            compare(d.fileName(), "a-b-c.txt")

            verify(d.writeTo(scratch))
            const text = readText(scratch)
            compare(text, Presets.exportText(0, 0))
            verify(text.indexOf("Preamp: -6.1 dB") >= 0)
            verify(text.indexOf("Channel: R") >= 0)
            verify(text.indexOf("Fc 3100 Hz Gain -2.5 dB") >= 0)

            // What was written imports back as it was.
            const back = Presets.openImport(scratch)
            compare(back.filterCount, 2)
            fuzzyCompare(back.preampDb, -6.1, 1e-9)
            compare(back.skipped.length, 0)
            back.destroy()
            d.close()
        }

        function test_writes_for_the_layout_picked() {
            EqSession.addBand(1000, -3)
            EqSession.setChannels(0, 1)   // right only: not on a mono layout
            EqSession.finishEdit()
            const d = UiState.openDialog(PresetActions.exportDialog,
                                         {layouts: [{label: "Stereo", channels: 2, mask: 3}, {label: "Mono", channels: 1, mask: 4}]})
            const picker = child("exportLayout")
            verify(picker.parent.visible)
            compare(picker.options, ["Stereo", "Mono"])
            picker.picked(1)
            verify(d.writeTo(scratch))
            const text = readText(scratch)
            compare(text, Presets.exportText(1, 4))
            verify(text !== Presets.exportText(2, 3))
            d.close()
        }
    }
}
