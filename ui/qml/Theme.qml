pragma Singleton
import QtQuick

// Colour and type tokens, docs/ui-spec.md "Themes" (dark). Light and custom come
// with Appearance.
QtObject {
    readonly property string font: "Instrument Sans"

    readonly property color background: "#121519"
    readonly property color plot: "#0e1115"          // also the sidebar
    readonly property color surface: "#1b1e23"       // selected nav, rows, popovers
    readonly property color gridMajor: "#26292e"
    readonly property color gridMinor: "#1a1d22"
    readonly property color zero: "#4b4f54"
    readonly property color text: "#e8ebf1"
    readonly property color muted: "#90969d"
    readonly property color track: "#23272b"
    readonly property color segmentedSelected: "#383c41"
    readonly property color selectedColumn: "#191c20"
    readonly property color spectrumFill: Qt.rgba(126 / 255, 135 / 255, 146 / 255, 0.10)
    readonly property color spectrumEdge: Qt.rgba(156 / 255, 165 / 255, 177 / 255, 0.22)
    readonly property color bell: Qt.rgba(213 / 255, 223 / 255, 235 / 255, 0.17)
    readonly property color textOnAccent: "#080e16"
    readonly property color knob: "#f3f5f8"
    readonly property color running: "#7ccd8e"
    readonly property color warning: "#d8953d"
    readonly property color danger: "#f47b74"        // gen_screens.py RED, oklch(0.72 0.15 25)
    readonly property real fillEdgeAlpha: 0.30
    readonly property real fillMidAlpha: 0.04

    readonly property color accent: "#6aa7f4"
    readonly property bool perBandColours: true
    readonly property var bandColours: ["#7cb4fc", "#30c8cf", "#76c788", "#c9b04f", "#ec9c63", "#f49191",
                                        "#dc95d5", "#b0a4f8", "#51bfee", "#4acaad", "#a7bc61", "#dda552"]

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
