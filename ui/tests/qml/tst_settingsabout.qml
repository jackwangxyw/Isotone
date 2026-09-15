import QtQuick
import QtTest
import Isotone

// Settings, About: version, engine rows, and Copy diagnostics on the clipboard
// (the offscreen platform's own clipboard, not the desktop's).
Item {
    id: root
    width: 900
    height: 500

    SettingsAbout {
        id: page
        width: 760
    }
    TextEdit { id: paste; y: 400; width: 100; height: 20 }

    TestCase {
        name: "SettingsAbout"
        when: windowShown

        function child(name) { return findChild(page, name) }

        function test_version_and_engines() {
            compare(child("version").text, "Version 0.1.0")
            verify(/^(Not installed|(\d+\.\d+\.\d+(\.\d+)? · )?\d+ outputs?)$/.test(child("isoapo").text), child("isoapo").text)
            verify(/^(Not installed|(\d+\.\d+\.\d+(\.\d+)? · )?\d+ outputs?)$/.test(child("equalizerApo").text), child("equalizerApo").text)
            verify(["Disabled", "Enabled"].indexOf(child("protectedAudio").text) >= 0)
        }

        function test_copy_diagnostics() {
            const button = child("copyDiagnostics")
            compare(button.text, "Copy diagnostics")
            mouseClick(button)
            compare(button.text, "Copied")
            compare(button.icon, "check")
            paste.text = ""
            paste.paste()
            verify(paste.text.startsWith("Isotone 0.1.0\n"), paste.text.slice(0, 80))
            verify(paste.text.indexOf("\nQt ") > 0)
            verify(paste.text.indexOf("IsoAPO: ") > 0)
            verify(paste.text.indexOf("Equalizer APO: ") > 0)
            verify(paste.text.indexOf("Protected audio: ") > 0)
            verify(paste.text.indexOf("Render endpoints") > 0)
            tryCompare(button, "text", "Copy diagnostics", 2500)
        }
    }
}
