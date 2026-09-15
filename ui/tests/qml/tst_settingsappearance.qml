import QtQuick
import QtTest
import Isotone

// Settings, Appearance: theme, accent, band colours and custom colours switch
// Theme's tokens; the colour popover; the preview stays off the real session.
Item {
    id: root
    width: 1200
    height: 900

    SettingsAppearance {
        id: page
        width: 1112
    }
    Item {
        id: overlay
        anchors.fill: parent
        z: 100
    }
    Component.onCompleted: UiState.overlay = overlay

    TestCase {
        name: "SettingsAppearance"
        when: windowShown

        function child(name) { return findChild(page, name) }
        function type(text) { for (const c of text) keyClick(c) }
        function options(control) {
            const row = control.children[0]
            const out = []
            for (let i = 0; i < row.children.length; ++i)
                if (row.children[i].modelData !== undefined) out.push(row.children[i])
            return out
        }
        function sameColour(a, b) { return Qt.colorEqual(a, b) }

        function init() {
            findChild(overlay, "colourPopover").close()
            AppSettings.theme = "dark"
            AppSettings.accent = 0
            AppSettings.bandColours = "band"
            for (const key of ["background", "surface", "text", "grid", "spectrum"]) AppSettings.setValue("appearance/custom/" + key, "")
            waitForRendering(page)
        }
        function cleanupTestCase() {
            AppSettings.theme = "system"
            AppSettings.accent = 0
        }

        function test_theme_switches_the_tokens() {
            const theme = child("theme")
            mouseClick(options(theme)[2])   // Light
            compare(AppSettings.theme, "light")
            verify(!Theme.dark)
            verify(sameColour(Theme.background, "#f4f6f8"))
            compare(theme.current, 2)
            mouseClick(options(theme)[1])   // Dark
            compare(AppSettings.theme, "dark")
            verify(Theme.dark)
            verify(sameColour(Theme.background, "#121519"))
            mouseClick(options(theme)[0])
            compare(AppSettings.theme, "system")
            mouseClick(options(theme)[3])
            compare(AppSettings.theme, "custom")
            verify(Theme.custom)
        }

        function test_accent_swatches() {
            mouseClick(child("accent2"))
            compare(AppSettings.accent, 2)
            verify(sameColour(Theme.accent, Theme.accents[2]))
            verify(child("accent2").on)
            verify(!child("accent0").on)
            verify(sameColour(findChild(page, "previewGraph").accent, Theme.accents[2]))
        }

        function test_band_colours() {
            mouseClick(child("bandColours_accent"))
            compare(AppSettings.bandColours, "accent")
            verify(!Theme.perBandColours)
            verify(sameColour(Theme.bandColour(3), Theme.accent))
            verify(!findChild(page, "previewGraph").perBandColours)
            mouseClick(child("bandColours_band"))
            compare(AppSettings.bandColours, "band")
            verify(Theme.perBandColours)
            verify(sameColour(Theme.bandColour(3), Theme.bandColours[3]))
        }

        function test_custom_colours() {
            verify(child("custom_background") === null, "only with the Custom theme")
            AppSettings.theme = "custom"
            tryVerify(() => child("custom_background") !== null)
            const row = child("custom_background")
            waitForItemPolished(row.parent)   // the new rows laid out before one is clicked
            const hex = findChild(row, "hex")
            compare(hex.text, "#121519")   // starts from Dark
            mouseClick(hex)
            verify(hex.editing)
            type("#0C1722")
            keyClick(Qt.Key_Return)
            verify(!hex.editing)
            compare(AppSettings.value("appearance/custom/background"), "#0c1722")
            verify(sameColour(Theme.background, "#0c1722"))
            compare(hex.text, "#0C1722")

            // Not a colour: stays open, nothing changes.
            mouseClick(hex)
            type("#12")
            keyClick(Qt.Key_Return)
            verify(hex.editing)
            keyClick(Qt.Key_Escape)
            verify(sameColour(Theme.background, "#0c1722"))

            const grid = findChild(child("custom_grid"), "hex")
            mouseClick(grid)
            type("1f2b38")
            keyClick(Qt.Key_Return)
            verify(sameColour(Theme.gridMajor, "#1f2b38"))

            mouseClick(child("startFromDark"))
            verify(sameColour(Theme.background, "#121519"))
            verify(sameColour(Theme.gridMajor, "#26292e"))
        }

        function test_custom_accent_popover() {
            mouseClick(child("customAccent"))
            const pop = findChild(overlay, "colourPopover")
            verify(pop.open)
            const hex = findChild(pop, "hex")
            compare(hex.text, AppSettings.customAccent.toUpperCase())
            mouseClick(hex.input)
            hex.input.selectAll()
            type("#00ff80")
            keyClick(Qt.Key_Return)
            compare(AppSettings.customAccent, "#00ff80")
            compare(AppSettings.accent, -1)
            verify(sameColour(Theme.accent, "#00ff80"))
            verify(child("customAccent").on)
            compare(hex.text, "#00FF80")

            hex.input.selectAll()
            type("#12")
            keyClick(Qt.Key_Return)
            compare(hex.input.selectedText, "#12")
            compare(AppSettings.customAccent, "#00ff80")

            // Opened again on a dark colour: the square's knob is where that colour is.
            keyClick(Qt.Key_Escape)
            AppSettings.customAccent = "#203040"
            mouseClick(child("customAccent"))
            verify(pop.open)
            const picker = findChild(pop, "colourPicker")
            const dark = Qt.color("#203040")
            fuzzyCompare(picker.value, dark.hsvValue, 0.01)
            fuzzyCompare(picker.saturation, dark.hsvSaturation, 0.01)
            fuzzyCompare(picker.hue, dark.hsvHue, 0.01)

            // The square's top right corner: full saturation and value of the hue.
            const square = findChild(pop, "saturationValue")
            mousePress(square, square.width - 1, 1)
            mouseRelease(square, square.width - 1, 1)
            const picked = Qt.color(AppSettings.customAccent)
            fuzzyCompare(picked.hsvSaturation, 1, 0.02)
            fuzzyCompare(picked.hsvValue, 1, 0.02)
            fuzzyCompare(picked.hsvHue, dark.hsvHue, 0.02)
            // The hue bar's left end: red.
            const hue = findChild(pop, "hue")
            mousePress(hue, 0, 6)
            mouseRelease(hue, 0, 6)
            const red = Qt.color(AppSettings.customAccent)
            verify(red.hsvHue < 0.01 || red.hsvHue > 0.99, AppSettings.customAccent)
            fuzzyCompare(red.hsvSaturation, 1, 0.02)

            keyClick(Qt.Key_Escape)
            mouseClick(child("accent0"))
            verify(!child("customAccent").on)
        }

        function test_preview_is_a_sample_of_its_own() {
            while (EqSession.count > 0) EqSession.deleteBand(0)
            const preview = findChild(page, "preview")
            compare(preview.session.count, 8)
            compare(preview.session.selectedRow, 3)
            compare(EqSession.count, 0)
            verify(preview.width > 240)
        }
    }
}
