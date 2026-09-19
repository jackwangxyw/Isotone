import QtQuick
import QtQuick.Shapes
import Isotone

// A file dragged over the window: a dashed accent border, "Import" and the file
// name; dropped, the import dialog. Fills its parent.
DropArea {
    id: root
    property string fileName: ""
    readonly property bool showing: fileName !== ""

    function nameOf(url) {
        const s = decodeURIComponent(url.toString())
        return s.substring(Math.max(s.lastIndexOf("/"), s.lastIndexOf("\\")) + 1)
    }
    // The drag's steps, apart from its event (a test drives them).
    function enter(urls) {
        fileName = urls.length > 0 ? nameOf(urls[0]) : ""
        return fileName !== ""
    }
    function leave() { fileName = "" }
    function dropFiles(urls) {
        fileName = ""
        return urls.length > 0 ? PresetActions.showImport(urls[0]) : null
    }

    onEntered: (drag) => {
        if (!drag.hasUrls || !enter(drag.urls)) {
            drag.accepted = false
            return
        }
        drag.accept(Qt.CopyAction)
    }
    onExited: leave()
    onDropped: (drop) => {
        if (!drop.hasUrls) {
            leave()
            return
        }
        drop.accept(Qt.CopyAction)
        dropFiles(drop.urls)
    }

    Item {
        objectName: "dropOverlay"
        anchors.fill: parent
        anchors.margins: 16
        visible: root.showing

        Rectangle {
            anchors.fill: parent
            radius: 16
            color: Qt.alpha(Theme.background, 0.7)
        }
        Shape {
            anchors.fill: parent
            // Shape.CurveRenderer (Qt 6.6) where there is one; Qt 6.4, as Linux distributions
            // ship it, has only the geometry renderer, smoothed here by multisampling.
            Component.onCompleted: {
                if ("preferredRendererType" in this) preferredRendererType = Shape.CurveRenderer
                else { layer.samples = 4; layer.enabled = true }
            }
            ShapePath {
                strokeColor: Theme.accent
                strokeWidth: 2
                strokeStyle: ShapePath.DashLine
                dashPattern: [3, 2]
                fillColor: "transparent"
                // A rounded rectangle at (1, 1) with radius 15. PathSvg rather than
                // PathRectangle, which is Qt 6.8 and Linux distributions ship 6.4.
                PathSvg {
                    readonly property real w: root.width - 34
                    readonly property real h: root.height - 34
                    readonly property real r: 15
                    path: 'M ' + (1 + r) + ' 1 L ' + (1 + w - r) + ' 1 A ' + r + ' ' + r + ' 0 0 1 ' + (1 + w) + ' ' + (1 + r)
                          + ' L ' + (1 + w) + ' ' + (1 + h - r) + ' A ' + r + ' ' + r + ' 0 0 1 ' + (1 + w - r) + ' ' + (1 + h)
                          + ' L ' + (1 + r) + ' ' + (1 + h) + ' A ' + r + ' ' + r + ' 0 0 1 1 ' + (1 + h - r)
                          + ' L 1 ' + (1 + r) + ' A ' + r + ' ' + r + ' 0 0 1 ' + (1 + r) + ' 1 Z'
                }
            }
        }
        Column {
            anchors.centerIn: parent
            spacing: 10
            Icon {
                anchors.horizontalCenter: parent.horizontalCenter
                name: "file"
                size: 30
                colour: Theme.text
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: "Import"
                font.family: Theme.font
                font.pixelSize: 20
                font.weight: Font.DemiBold
                color: Theme.text
            }
            Text {
                objectName: "dropFileName"
                anchors.horizontalCenter: parent.horizontalCenter
                text: root.fileName
                font.family: Theme.font
                font.pixelSize: 13
                color: Theme.muted
            }
        }
    }
}
