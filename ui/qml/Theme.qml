pragma Singleton
import QtQuick
import Isotone

// Colour and type tokens: docs/ui-spec.md "Themes" and the prototype's app.css.
// The theme follows AppSettings: System (Windows' app mode), Dark, Light, or
// Custom (Dark with Background, Surface, Text, Grid and Spectrum overridden).
QtObject {
    id: theme
    readonly property string font: "Instrument Sans"

    readonly property string mode: AppSettings.theme
    readonly property bool dark: mode === "dark" || mode === "custom"
                                 || (mode !== "light" && Qt.styleHints.colorScheme !== Qt.ColorScheme.Light)
    readonly property bool custom: mode === "custom"
    function pick(d, l) { return dark ? d : l }
    function customColour(key, fallback) {
        if (!custom) return fallback
        AppSettings.themeRevision   // re-read when a custom colour changes
        const v = AppSettings.value("appearance/custom/" + key, "")
        return v !== "" ? v : fallback
    }

    readonly property color background: customColour("background", pick("#121519", "#f4f6f8"))
    readonly property color plot: customColour("surface", pick("#0e1115", "#fcfdff"))   // also the sidebar
    readonly property color surface: pick("#1b1e23", "#e7eaed")                        // selected nav, rows
    readonly property color gridMajor: customColour("grid", pick("#26292e", "#dbdee2"))
    readonly property color gridMinor: pick("#1a1d22", "#edeff1")
    readonly property color zero: pick("#4b4f54", "#a0a5ab")
    readonly property color text: customColour("text", pick("#e8ebf1", "#1b2025"))
    readonly property color muted: pick("#90969d", "#646970")
    readonly property color track: pick("#23272b", "#e0e3e7")
    readonly property color segmentedSelected: pick("#383c41", "#fcfdff")
    readonly property color selectedColumn: pick("#191c20", "#eef0f3")
    readonly property color spectrumFill: custom ? Qt.alpha(customColour("spectrum", "#7e8792"), 0.10)
                                                 : pick(Qt.rgba(126 / 255, 135 / 255, 146 / 255, 0.10), Qt.rgba(106 / 255, 114 / 255, 125 / 255, 0.08))
    readonly property color spectrumEdge: custom ? Qt.alpha(customColour("spectrum", "#9ca5b1"), 0.22)
                                                 : pick(Qt.rgba(156 / 255, 165 / 255, 177 / 255, 0.22), Qt.rgba(92 / 255, 100 / 255, 111 / 255, 0.18))
    readonly property color bell: pick(Qt.rgba(213 / 255, 223 / 255, 235 / 255, 0.17), Qt.rgba(39 / 255, 46 / 255, 56 / 255, 0.15))
    readonly property color textOnAccent: pick("#080e16", "#fafcfe")
    readonly property color knob: pick("#f3f5f8", "#fcfdff")
    readonly property color ok: pick("#7ccd8e", "#3b9555")
    readonly property color running: ok
    readonly property color warning: pick("#d8953d", "#a45f00")
    readonly property color danger: pick("#ea808a", "#b44957")
    // Popovers and dialogs.
    readonly property color pop: pick("#191c20", "#fcfdff")
    readonly property color border: pick("#2a2e33", "#d6dadf")
    readonly property color scrim: pick(Qt.rgba(0, 0, 0, 0.55), Qt.rgba(30 / 255, 36 / 255, 44 / 255, 0.28))
    readonly property real fillEdgeAlpha: pick(0.30, 0.20)
    readonly property real fillMidAlpha: pick(0.04, 0.02)

    readonly property var accents: pick(["#6aa7f4", "#a495f0", "#00bcc5", "#62bb78", "#d8953d", "#ea808a"],
                                        ["#3072c1", "#7260bd", "#008892", "#1c8742", "#a45f00", "#b44957"])
    readonly property color accent: AppSettings.accent >= 0 && AppSettings.accent < accents.length
                                    ? accents[AppSettings.accent] : AppSettings.customAccent
    readonly property bool perBandColours: AppSettings.bandColours !== "accent"
    readonly property var bandColours: pick(["#7cb4fc", "#30c8cf", "#76c788", "#c9b04f", "#ec9c63", "#f49191",
                                             "#dc95d5", "#b0a4f8", "#51bfee", "#4acaad", "#a7bc61", "#dda552"],
                                            ["#467cc0", "#008e96", "#3d8e53", "#917800", "#b0652a", "#b65a5c",
                                             "#a15e9c", "#7a6cbc", "#0086b3", "#009176", "#728426", "#a36e09"])

    function bandColour(index) { return perBandColours ? bandColours[index % bandColours.length] : accent }

    // U+2212 for negative numbers, as the spec asks.
    function signed(v, decimals) {
        const s = Math.abs(v).toFixed(decimals)
        return (v < 0 && Number(s) !== 0 ? "−" : "+") + s
    }
    function frequency(hz) {
        return hz >= 1000 ? (hz / 1000).toFixed(2) + " kHz" : Math.round(hz) + " Hz"
    }
}
