import QtQuick
import QtQuick.Shapes

// A 24 x 24 stroked icon, scaled to `size` with its stroke, as the SVG viewBox
// did.
//
// A path is either an SVG string at `strokeWidth`, or { d, w } with a width of
// its own. The second is what lets an icon borrow the mark's vocabulary: the
// logo is five pill bars, drawn as round-capped segments at width 3, so a
// segment at a heavy width inside a hairline outline reads as the same family
// rather than as a stock outline set (docs/decisions.md, "The icons").
Item {
    id: root
    property string name
    property color colour: Theme.muted
    property real size: 20
    property real strokeWidth: 1.6

    readonly property var paths: ({
        eq: ["M5 4v16M12 4v16M19 4v16", "M7.2 14a2.2 2.2 0 1 1 -4.4 0a2.2 2.2 0 1 1 4.4 0",
             "M14.2 8a2.2 2.2 0 1 1 -4.4 0a2.2 2.2 0 1 1 4.4 0", "M21.2 15a2.2 2.2 0 1 1 -4.4 0a2.2 2.2 0 1 1 4.4 0"],
        ear: ["M3 12c1.5-5 3-5 4.5 0s3 5 4.5 0 3-5 4.5 0 3 5 4.5 0"],
        devices: ["M6 3h6a2 2 0 0 1 2 2v14a2 2 0 0 1 -2 2h-6a2 2 0 0 1 -2 -2v-14a2 2 0 0 1 2 -2z",
                  "M11.5 15a2.5 2.5 0 1 1 -5 0a2.5 2.5 0 1 1 5 0", "M9 7h.01M17 8a5 5 0 0 1 0 8M19.5 5.5a8.5 8.5 0 0 1 0 13"],
        settings: ["M19.17 10.17 L21.90 10.60 L21.90 13.40 L19.17 13.83 L18.84 14.81 L18.36 15.78 L19.99 18.02 L18.02 19.99 L15.78 18.36 L14.85 18.83 L13.83 19.17 L13.40 21.90 L10.60 21.90 L10.17 19.17 L9.19 18.84 L8.22 18.36 L5.98 19.99 L4.01 18.02 L5.64 15.78 L5.17 14.85 L4.83 13.83 L2.10 13.40 L2.10 10.60 L4.83 10.17 L5.16 9.19 L5.64 8.22 L4.01 5.98 L5.98 4.01 L8.22 5.64 L9.15 5.17 L10.17 4.83 L10.60 2.10 L13.40 2.10 L13.83 4.83 L14.81 5.16 L15.78 5.64 L18.02 4.01 L19.99 5.98 L18.36 8.22 L18.83 9.15Z",
                   "M15 12a3 3 0 1 1 -6 0a3 3 0 1 1 6 0"],
        chevron: ["M6 9l6 6 6-6"],
        chevronRight: ["M9 6l6 6-6 6"],
        chevronLeft: ["M15 6l-6 6 6 6"],
        plus: ["M12 5v14M5 12h14"],
        // The rail is a pill bar, as the mark's bars are, not the hairline
        // division every outline set draws: that one was Lucide's panel-left
        // stroke for stroke, and it is the icon the owner's reviewer picked out
        // as identical to ChatGPT's and Claude's.
        panel: ["M6 4.5h12a3.2 3.2 0 0 1 3.2 3.2v8.6a3.2 3.2 0 0 1 -3.2 3.2h-12a3.2 3.2 0 0 1 -3.2 -3.2v-8.6a3.2 3.2 0 0 1 3.2 -3.2z",
                { d: "M7.7 9.1V14.9", w: 2.9 }],
        output: ["M4 9v6h4l5 4V5L8 9H4z", "M16.5 8.5a5 5 0 0 1 0 7"],
        // The mark (docs/design/logo, locked 2026-09-19): five bars about the
        // zero line, centred on the ink rather than on the line. Stroked at width
        // 3 with the round caps this renderer already uses, each segment is one
        // pill bar; the call sites pass strokeWidth 3.
        logo: ["M4.6 13.11V14.94M8.3 7.21V13.11M12 13.11V16.79M15.7 9.43V13.11M19.4 13.11V14.94"],
        // The prototype's set (docs/design/prototype/views-eq.js, IC), circles and rects as paths.
        speakers: ["M8 3h8a2 2 0 0 1 2 2v14a2 2 0 0 1 -2 2h-8a2 2 0 0 1 -2 -2v-14a2 2 0 0 1 2 -2z",
                   "M15 14a3 3 0 1 1 -6 0a3 3 0 1 1 6 0", "M13 7.5a1 1 0 1 1 -2 0a1 1 0 1 1 2 0"],
        search: ["M17 11a6 6 0 1 1 -12 0a6 6 0 1 1 12 0", "M20 20l-4.5-4.5"],
        pen: ["M4 20h4L19 9l-4-4L4 16z"],
        copy: ["M10 8h8a2 2 0 0 1 2 2v8a2 2 0 0 1 -2 2h-8a2 2 0 0 1 -2 -2v-8a2 2 0 0 1 2 -2z",
               "M16 8V6a2 2 0 0 0-2-2H6a2 2 0 0 0-2 2v8a2 2 0 0 0 2 2h2"],
        trash: ["M4 7h16M10 11v6M14 11v6M6 7l1 13h10l1-13M9 7V4h6v3"],
        play: ["M8 5l11 7-11 7z"],
        pause: ["M8 5v14M16 5v14"],
        refresh: ["M20 11a8 8 0 0 0-14.8-3M4 5v4h4M4 13a8 8 0 0 0 14.8 3M20 19v-4h-4"],
        check: ["M5 12.5l4.5 4.5L19 7.5"],
        file: ["M14 3H6v18h12V7z", "M14 3v4h4"],
        close: ["M6 6l12 12M18 6L6 18"],
        shield: ["M12 3l8 3v6c0 5-3.5 8-8 9-4.5-1-8-4-8-9V6z"],
        warning: ["M12 4l9 16H3z", "M12 10v4M12 17v.5"],
        external: ["M14 4h6v6M20 4l-9 9M18 14v6H4V6h6"]
    })

    implicitWidth: size
    implicitHeight: size

    Repeater {
        model: root.paths[root.name] || []
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
