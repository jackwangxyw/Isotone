pragma Singleton
import QtQuick
import Isotone

// Settings, General, as properties that follow AppSettings (settings.ini gives
// strings back, so each is read as its type and held to its choices), and the
// setters the page calls. The spectrum's options go on to EqSession.
QtObject {
    id: root

    readonly property var gainRanges: [12, 15, 24]
    readonly property real lowestHz: 10
    readonly property real highestHz: 24000

    property bool startInTray: true
    property bool keepInTray: true
    property bool switchOnDefaultOutput: true
    property bool autoPreampForNew: true
    // The window stays over other windows (owner, 2026-09-16).
    property bool alwaysOnTop: false
    property int gainRange: 15
    property real minHz: 20
    property real maxHz: 20000
    // Settings, General, Spectrum: how long a level takes to fall, and how much
    // the drawn curve is smoothed (0 spiky, 1 smoothest).
    property real decayMs: 150
    property real smoothing: 0.35
    // Graph: what the Equalizer view keeps in a window too short for the graph
    // and the band strip both, "graph" or "bands" (owner, 2026-09-16).
    property string shortWindow: "graph"
    // The heights that follow: the top bar, the strip, the least graph, and the
    // margin under a graph shown alone.
    readonly property int topBarHeight: 76
    readonly property int stripHeight: 402
    readonly property int leastGraphHeight: 200
    readonly property int graphAloneMargin: 24
    // Under this the Equalizer view shows one of them.
    readonly property int bothHeight: topBarHeight + stripHeight + leastGraphHeight
    // The window's least height: what is kept, at its least.
    readonly property int shortWindowMinimumHeight: shortWindow === "bands" ? topBarHeight + stripHeight
                                                                           : topBarHeight + leastGraphHeight + graphAloneMargin

    function flag(key, fallback) {
        const v = AppSettings.value(key, fallback)
        return v === true || v === "true"
    }
    function number(key, fallback) {
        const v = Number(AppSettings.value(key, fallback))
        return isFinite(v) ? v : fallback
    }
    function oneOf(v, choices, fallback) { return choices.indexOf(v) >= 0 ? v : fallback }
    function clampHz(hz) { return Math.max(lowestHz, Math.min(highestHz, hz)) }

    function read() {
        startInTray = flag("general/startInTray", true)
        keepInTray = flag("general/keepInTray", true)
        switchOnDefaultOutput = flag("general/switchOnDefaultOutput", true)
        autoPreampForNew = flag("general/autoPreampForNew", true)
        alwaysOnTop = flag("window/alwaysOnTop", false)
        gainRange = oneOf(number("graph/gainRange", 15), gainRanges, 15)
        const lo = clampHz(number("graph/minHz", 20)), hi = clampHz(number("graph/maxHz", 20000))
        minHz = lo < hi ? lo : 20
        maxHz = lo < hi ? hi : 20000
        decayMs = Math.max(50, Math.min(500, number("spectrum/decayMs", 150)))
        smoothing = Math.max(0, Math.min(1, number("spectrum/smoothing", 0.35)))
        shortWindow = oneOf(String(AppSettings.value("graph/shortWindow", "graph")), ["graph", "bands"], "graph")
    }

    // A frequency range, clamped to 10 Hz to 24 kHz; false (and nothing saved) unless from < to.
    function setFrequencyRange(from, to) {
        const lo = clampHz(from), hi = clampHz(to)
        if (!(lo < hi)) return false
        AppSettings.setValue("graph/minHz", lo)
        AppSettings.setValue("graph/maxHz", hi)
        return true
    }
    function setDecayMs(ms) { AppSettings.setValue("spectrum/decayMs", Math.round(Math.max(50, Math.min(500, ms)))) }
    function setShortWindow(v) { AppSettings.setValue("graph/shortWindow", v === "bands" ? "bands" : "graph") }
    function setSmoothing(v) { AppSettings.setValue("spectrum/smoothing", Math.round(Math.max(0, Math.min(1, v)) * 100) / 100) }

    // "20 Hz", "1.5 kHz", "20 kHz".
    function hertz(hz) {
        if (hz < 1000) return Math.round(hz) + " Hz"
        return Number((hz / 1000).toFixed(hz >= 10000 ? 1 : 2)) + " kHz"
    }

    function applySpectrum() { EqSession.setSpectrumDecayMs(decayMs) }
    onDecayMsChanged: applySpectrum()

    readonly property Connections watch: Connections {
        target: AppSettings
        function onValueChanged(key) { root.read() }
    }
    Component.onCompleted: { read(); applySpectrum() }
}
