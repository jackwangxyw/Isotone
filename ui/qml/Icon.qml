import QtQuick
import QtQuick.Shapes

// An icon, scaled to `size`.
//
// The paths in `filled` below are Phosphor Icons, vendored unmodified:
//
//   Copyright (c) 2023 Phosphor Icons
//
//   Permission is hereby granted, free of charge, to any person obtaining a copy
//   of this software and associated documentation files (the "Software"), to deal
//   in the Software without restriction, including without limitation the rights
//   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
//   copies of the Software, and to permit persons to whom the Software is
//   furnished to do so, subject to the following conditions:
//
//   The above copyright notice and this permission notice shall be included in all
//   copies or substantial portions of the Software.
//
//   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
//   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
//   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
//   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
//   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
//   OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
//   SOFTWARE.
//
// The notice is here rather than in a file of its own because that is how this
// repository already carries vendored code: core/tests/third_party/doctest.h
// does the same. @phosphor-icons/core 2.1.1, the `bold` weight.
//
// Most of the set is Phosphor Bold (phosphoricons.com, MIT), used as published:
// filled geometry on a 256 box, not hairline strokes on a 24 one. The owner's
// reviewer called the old set generic and named the sidebar toggle as identical
// to ChatGPT's and Claude's, which it was, stroke for stroke (docs/decisions.md,
// "The icons"). Phosphor is the pack that is actually drawn differently, and its
// Bold weight already speaks the mark's language: its `waveform` is five
// round-capped bars of varying heights about a centre line, which is the logo.
//
// Two icons are ours and stay stroked on the 24 box:
//   logo   the mark itself, locked 2026-09-19
//   panel  a hairline panel with the rail as a bar beside it, at two weights,
//          which is the shape the owner picked from six
// So `strokes` is the 24-box stroked set and `filled` is the 256-box Phosphor
// one; a name in `filled` wins. A stroked path is either an SVG string at
// `strokeWidth` or { d, w } with a width of its own, which is what makes the
// two-weight contrast in `panel` possible.
Item {
    id: root
    property string name
    property color colour: Theme.muted
    property real size: 20
    property real strokeWidth: 1.6

    readonly property var strokes: ({
        // The mark (docs/design/logo, locked 2026-09-19): five bars about the
        // zero line, centred on the ink rather than on the line. Stroked at width
        // 3 with the round caps this renderer already uses, each segment is one
        // pill bar; the call sites pass strokeWidth 3.
        logo: ["M4.6 13.11V14.94M8.3 7.21V13.11M12 13.11V16.79M15.7 9.43V13.11M19.4 13.11V14.94"],
        // The rail is outside the panel, not a line inside it, which is the
        // shape the owner picked from six (2026-09-20, "Sidebar Toggle
        // Options"). Every outline set draws a box divided by a hairline, and
        // the version before this one put the bar inside: at 18 px a 3.1 stroke
        // against a 1.6 outline has too little contrast to read as its own
        // object, so it came out looking like a thick divider. Two separate
        // shapes cannot be read that way.
        panel: ["M11.4 4.4h7.6a2.4 2.4 0 0 1 2.4 2.4v10.4a2.4 2.4 0 0 1 -2.4 2.4h-7.6a2.4 2.4 0 0 1 -2.4 -2.4v-10.4a2.4 2.4 0 0 1 2.4 -2.4z",
                { d: "M4.6 6.4V17.6", w: 3.2 }]
    })

    // Phosphor Bold, one filled path each, on the 256 box they are drawn in.
    readonly property var filled: ({
        eq: "M140,124v92a12,12,0,0,1-24,0V124a12,12,0,0,1,24,0Zm60,68a12,12,0,0,0-12,12v12a12,12,0,0,0,24,0V204A12,12,0,0,0,200,192Zm24-40H212V40a12,12,0,0,0-24,0V152H176a12,12,0,0,0,0,24h48a12,12,0,0,0,0-24ZM56,160a12,12,0,0,0-12,12v44a12,12,0,0,0,24,0V172A12,12,0,0,0,56,160Zm24-40H68V40a12,12,0,0,0-24,0v80H32a12,12,0,0,0,0,24H80a12,12,0,0,0,0-24Zm72-48H140V40a12,12,0,0,0-24,0V72H104a12,12,0,0,0,0,24h48a12,12,0,0,0,0-24Z",
        ear: "M220,104a12,12,0,0,1-24,0,68,68,0,0,0-136,0c0,25,7.58,32.3,16.35,40.76S96,163.71,96,188a32,32,0,0,0,32,32c9,0,16.19-3.7,22.75-11.64a12,12,0,0,1,18.5,15.28C158.09,237.15,144.21,244,128,244a56.06,56.06,0,0,1-56-56c0-14.09-4.63-18.56-12.31-26C49.13,151.86,36,139.19,36,104a92,92,0,0,1,184,0Zm-40.13,53.61a12,12,0,0,0-16.4,4.38,4,4,0,0,1-7.47-2c0-7.61,3.65-12.86,9.6-20.8C172,130.65,180,120,180,104a52,52,0,0,0-104,0,12,12,0,0,0,24,0,28,28,0,0,1,56,0c0,7.61-3.65,12.86-9.6,20.8C140,133.35,132,144,132,160a28,28,0,0,0,52.25,14A12,12,0,0,0,179.87,157.61Z",
        devices: "M192,20H64A20,20,0,0,0,44,40V216a20,20,0,0,0,20,20H192a20,20,0,0,0,20-20V40A20,20,0,0,0,192,20Zm-4,192H68V44H188ZM112,76a16,16,0,1,1,16,16A16,16,0,0,1,112,76Zm16,120a44,44,0,1,0-44-44A44.05,44.05,0,0,0,128,196Zm0-64a20,20,0,1,1-20,20A20,20,0,0,1,128,132Z",
        settings: "M40,92H70.06a36,36,0,0,0,67.88,0H216a12,12,0,0,0,0-24H137.94a36,36,0,0,0-67.88,0H40a12,12,0,0,0,0,24Zm64-24A12,12,0,1,1,92,80,12,12,0,0,1,104,68Zm112,96H201.94a36,36,0,0,0-67.88,0H40a12,12,0,0,0,0,24h94.06a36,36,0,0,0,67.88,0H216a12,12,0,0,0,0-24Zm-48,24a12,12,0,1,1,12-12A12,12,0,0,1,168,188Z",
        speakers: "M157.27,21.22a12,12,0,0,0-12.64,1.31L75.88,76H32A20,20,0,0,0,12,96v64a20,20,0,0,0,20,20H75.88l68.75,53.47A12,12,0,0,0,164,224V32A12,12,0,0,0,157.27,21.22ZM36,100H68v56H36Zm104,99.46L92,162.13V93.87l48-37.33ZM212,128a44,44,0,0,1-11,29.11,12,12,0,1,1-18-15.88,20,20,0,0,0,0-26.43,12,12,0,0,1,18-15.86A43.94,43.94,0,0,1,212,128Zm40,0a83.87,83.87,0,0,1-21.39,56,12,12,0,0,1-17.89-16,60,60,0,0,0,0-80,12,12,0,1,1,17.88-16A83.87,83.87,0,0,1,252,128Z",
        // The Outputs button in the collapsed rail: the mark's own shape, five
        // bars about a centre line, rather than a second speaker glyph.
        output: "M60,96v64a12,12,0,0,1-24,0V96a12,12,0,0,1,24,0ZM88,20A12,12,0,0,0,76,32V224a12,12,0,0,0,24,0V32A12,12,0,0,0,88,20Zm40,32a12,12,0,0,0-12,12V192a12,12,0,0,0,24,0V64A12,12,0,0,0,128,52Zm40,32a12,12,0,0,0-12,12v64a12,12,0,0,0,24,0V96A12,12,0,0,0,168,84Zm40-16a12,12,0,0,0-12,12v96a12,12,0,0,0,24,0V80A12,12,0,0,0,208,68Z",
        chevron: "M216.49,104.49l-80,80a12,12,0,0,1-17,0l-80-80a12,12,0,0,1,17-17L128,159l71.51-71.52a12,12,0,0,1,17,17Z",
        chevronRight: "M184.49,136.49l-80,80a12,12,0,0,1-17-17L159,128,87.51,56.49a12,12,0,1,1,17-17l80,80A12,12,0,0,1,184.49,136.49Z",
        chevronLeft: "M168.49,199.51a12,12,0,0,1-17,17l-80-80a12,12,0,0,1,0-17l80-80a12,12,0,0,1,17,17L97,128Z",
        plus: "M228,128a12,12,0,0,1-12,12H140v76a12,12,0,0,1-24,0V140H40a12,12,0,0,1,0-24h76V40a12,12,0,0,1,24,0v76h76A12,12,0,0,1,228,128Z",
        check: "M232.49,80.49l-128,128a12,12,0,0,1-17,0l-56-56a12,12,0,1,1,17-17L96,183,215.51,63.51a12,12,0,0,1,17,17Z",
        close: "M208.49,191.51a12,12,0,0,1-17,17L128,145,64.49,208.49a12,12,0,0,1-17-17L111,128,47.51,64.49a12,12,0,0,1,17-17L128,111l63.51-63.52a12,12,0,0,1,17,17L145,128Z",
        play: "M234.49,111.07,90.41,22.94A20,20,0,0,0,60,39.87V216.13a20,20,0,0,0,30.41,16.93l144.08-88.13a19.82,19.82,0,0,0,0-33.86ZM84,208.85V47.15L216.16,128Z",
        pause: "M200,28H160a20,20,0,0,0-20,20V208a20,20,0,0,0,20,20h40a20,20,0,0,0,20-20V48A20,20,0,0,0,200,28Zm-4,176H164V52h32ZM96,28H56A20,20,0,0,0,36,48V208a20,20,0,0,0,20,20H96a20,20,0,0,0,20-20V48A20,20,0,0,0,96,28ZM92,204H60V52H92Z",
        refresh: "M228,48V96a12,12,0,0,1-12,12H168a12,12,0,0,1,0-24h19l-7.8-7.8a75.55,75.55,0,0,0-53.32-22.26h-.43A75.49,75.49,0,0,0,72.39,75.57,12,12,0,1,1,55.61,58.41a99.38,99.38,0,0,1,69.87-28.47H126A99.42,99.42,0,0,1,196.2,59.23L204,67V48a12,12,0,0,1,24,0ZM183.61,180.43a75.49,75.49,0,0,1-53.09,21.63h-.43A75.55,75.55,0,0,1,76.77,179.8L69,172H88a12,12,0,0,0,0-24H40a12,12,0,0,0-12,12v48a12,12,0,0,0,24,0V189l7.8,7.8A99.42,99.42,0,0,0,130,226.06h.56a99.38,99.38,0,0,0,69.87-28.47,12,12,0,0,0-16.78-17.16Z",
        warning: "M240.26,186.1,152.81,34.23h0a28.74,28.74,0,0,0-49.62,0L15.74,186.1a27.45,27.45,0,0,0,0,27.71A28.31,28.31,0,0,0,40.55,228h174.9a28.31,28.31,0,0,0,24.79-14.19A27.45,27.45,0,0,0,240.26,186.1Zm-20.8,15.7a4.46,4.46,0,0,1-4,2.2H40.55a4.46,4.46,0,0,1-4-2.2,3.56,3.56,0,0,1,0-3.73L124,46.2a4.77,4.77,0,0,1,8,0l87.44,151.87A3.56,3.56,0,0,1,219.46,201.8ZM116,136V104a12,12,0,0,1,24,0v32a12,12,0,0,1-24,0Zm28,40a16,16,0,1,1-16-16A16,16,0,0,1,144,176Z",
        shield: "M208,36H48A20,20,0,0,0,28,56v56c0,54.29,26.32,87.22,48.4,105.29,23.71,19.39,47.44,26,48.44,26.29a12.1,12.1,0,0,0,6.32,0c1-.28,24.73-6.9,48.44-26.29,22.08-18.07,48.4-51,48.4-105.29V56A20,20,0,0,0,208,36Zm-4,76c0,35.71-13.09,64.69-38.91,86.15A126.28,126.28,0,0,1,128,219.38a126.14,126.14,0,0,1-37.09-21.23C65.09,176.69,52,147.71,52,112V60H204ZM79.51,144.49a12,12,0,1,1,17-17L112,143l47.51-47.52a12,12,0,0,1,17,17l-56,56a12,12,0,0,1-17,0Z",
        trash: "M216,48H180V36A28,28,0,0,0,152,8H104A28,28,0,0,0,76,36V48H40a12,12,0,0,0,0,24h4V208a20,20,0,0,0,20,20H192a20,20,0,0,0,20-20V72h4a12,12,0,0,0,0-24ZM100,36a4,4,0,0,1,4-4h48a4,4,0,0,1,4,4V48H100Zm88,168H68V72H188ZM116,104v64a12,12,0,0,1-24,0V104a12,12,0,0,1,24,0Zm48,0v64a12,12,0,0,1-24,0V104a12,12,0,0,1,24,0Z",
        copy: "M216,28H88A12,12,0,0,0,76,40V76H40A12,12,0,0,0,28,88V216a12,12,0,0,0,12,12H168a12,12,0,0,0,12-12V180h36a12,12,0,0,0,12-12V40A12,12,0,0,0,216,28ZM156,204H52V100H156Zm48-48H180V88a12,12,0,0,0-12-12H100V52H204Z",
        search: "M232.49,215.51,185,168a92.12,92.12,0,1,0-17,17l47.53,47.54a12,12,0,0,0,17-17ZM44,112a68,68,0,1,1,68,68A68.07,68.07,0,0,1,44,112Z",
        pen: "M230.14,70.54,185.46,25.85a20,20,0,0,0-28.29,0L33.86,149.17A19.85,19.85,0,0,0,28,163.31V208a20,20,0,0,0,20,20H92.69a19.86,19.86,0,0,0,14.14-5.86L230.14,98.82a20,20,0,0,0,0-28.28ZM91,204H52V165l84-84,39,39ZM192,103,153,64l18.34-18.34,39,39Z",
        file: "M216.49,79.52l-56-56A12,12,0,0,0,152,20H56A20,20,0,0,0,36,40V216a20,20,0,0,0,20,20H200a20,20,0,0,0,20-20V88A12,12,0,0,0,216.49,79.52ZM160,57l23,23H160ZM60,212V44h76V92a12,12,0,0,0,12,12h48V212Z",
        external: "M228,104a12,12,0,0,1-24,0V69l-59.51,59.51a12,12,0,0,1-17-17L187,52H152a12,12,0,0,1,0-24h64a12,12,0,0,1,12,12Zm-44,24a12,12,0,0,0-12,12v64H52V84h64a12,12,0,0,0,0-24H48A20,20,0,0,0,28,80V208a20,20,0,0,0,20,20H176a20,20,0,0,0,20-20V140A12,12,0,0,0,184,128Z"
    })

    readonly property string filledPath: filled[name] || ""

    implicitWidth: size
    implicitHeight: size

    // Phosphor: one filled path on the 256 box.
    Shape {
        visible: root.filledPath !== ""
        width: 256
        height: 256
        Component.onCompleted: {
            if ("preferredRendererType" in this) preferredRendererType = Shape.CurveRenderer
            else { layer.samples = 4; layer.enabled = true }
        }
        transform: Scale { xScale: root.size / 256; yScale: root.size / 256 }
        ShapePath {
            fillColor: root.colour
            strokeWidth: -1
            fillRule: ShapePath.WindingFill
            PathSvg { path: root.filledPath }
        }
    }

    // Ours: stroked on the 24 box.
    Repeater {
        model: root.filledPath === "" ? (root.strokes[root.name] || []) : []
        delegate: Shape {
            required property var modelData
            readonly property string d: typeof modelData === "string" ? modelData : modelData.d
            readonly property real w: typeof modelData === "string" ? root.strokeWidth : modelData.w
            width: 24
            height: 24
            // Shape.CurveRenderer (Qt 6.6) where there is one; Qt 6.4, as Linux distributions
            // ship it, has only the geometry renderer, smoothed here by multisampling.
            Component.onCompleted: {
                if ("preferredRendererType" in this) preferredRendererType = Shape.CurveRenderer
                else { layer.samples = 4; layer.enabled = true }
            }
            transform: Scale { xScale: root.size / 24; yScale: root.size / 24 }
            ShapePath {
                strokeColor: root.colour
                strokeWidth: w
                fillColor: "transparent"
                capStyle: ShapePath.RoundCap
                joinStyle: ShapePath.RoundJoin
                PathSvg { path: d }
            }
        }
    }
}
