import QtQuick
import QtQuick.Shapes

// A 24 x 24 stroked icon from the generator's set (gen_mockups3.py, ICONS),
// scaled to `size` with its stroke, as the SVG viewBox did.
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
        panel: ["M6 4h12a3 3 0 0 1 3 3v10a3 3 0 0 1 -3 3h-12a3 3 0 0 1 -3 -3v-10a3 3 0 0 1 3 -3z", "M9 4v16"],
        output: ["M4 9v6h4l5 4V5L8 9H4z", "M16.5 8.5a5 5 0 0 1 0 7"],
        logo: ["M3 14c3 0 3-6 6-6s3 8 6 8 3-4 6-4"]
    })

    implicitWidth: size
    implicitHeight: size

    Repeater {
        model: root.paths[root.name] || []
        delegate: Shape {
            required property string modelData
            width: 24
            height: 24
            preferredRendererType: Shape.CurveRenderer
            transform: Scale { xScale: root.size / 24; yScale: root.size / 24 }
            ShapePath {
                strokeColor: root.colour
                strokeWidth: root.strokeWidth
                fillColor: "transparent"
                capStyle: ShapePath.RoundCap
                joinStyle: ShapePath.RoundJoin
                PathSvg { path: modelData }
            }
        }
    }
}
