pragma Singleton
import QtQuick
import Isotone

// Settings, General, as properties that follow AppSettings (settings.ini gives
// strings back, so each is read as its type and held to its choices), and the
// setters the page calls. The spectrum's options go on to EqSession.
QtObject {
    id: root

    readonly property var gainRanges: [12, 15, 24]
    readonly property var resolutions: [4096, 8192, 16384]
    readonly property var tilts: [0, 3, 4.5]
    readonly property real lowestHz: 10
    readonly property real highestHz: 24000

    property bool startInTray: true
    property bool keepInTray: true
    property bool switchOnDefaultOutput: true
    property bool autoPreampForNew: true
    property int gainRange: 15
    property real minHz: 20
    property real maxHz: 20000
    property int resolution: 8192
    property real releaseMs: 300
    property bool peakHold: false
    property real tilt: 0

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
        gainRange = oneOf(number("graph/gainRange", 15), gainRanges, 15)
        const lo = clampHz(number("graph/minHz", 20)), hi = clampHz(number("graph/maxHz", 20000))
        minHz = lo < hi ? lo : 20
        maxHz = lo < hi ? hi : 20000
        resolution = oneOf(number("spectrum/resolution", 8192), resolutions, 8192)
        releaseMs = Math.max(10, Math.min(5000, number("spectrum/releaseMs", 300)))
        peakHold = flag("spectrum/peakHold", false)
        tilt = oneOf(number("spectrum/tiltDbPerOct", 0), tilts, 0)
    }

    // A frequency range, clamped to 10 Hz to 24 kHz; false (and nothing saved) unless from < to.
    function setFrequencyRange(from, to) {
        const lo = clampHz(from), hi = clampHz(to)
        if (!(lo < hi)) return false
        AppSettings.setValue("graph/minHz", lo)
        AppSettings.setValue("graph/maxHz", hi)
        return true
    }
    function setReleaseMs(ms) { AppSettings.setValue("spectrum/releaseMs", Math.round(Math.max(10, Math.min(5000, ms)))) }

    // "20 Hz", "1.5 kHz", "20 kHz".
    function hertz(hz) {
        if (hz < 1000) return Math.round(hz) + " Hz"
        return Number((hz / 1000).toFixed(hz >= 10000 ? 1 : 2)) + " kHz"
    }

    function applySpectrum() { EqSession.setSpectrumOptions(resolution, releaseMs, tilt) }
    onResolutionChanged: applySpectrum()
    onReleaseMsChanged: applySpectrum()
    onTiltChanged: applySpectrum()

    readonly property Connections watch: Connections {
        target: AppSettings
        function onValueChanged(key) { root.read() }
    }
    Component.onCompleted: { read(); applySpectrum() }
}
