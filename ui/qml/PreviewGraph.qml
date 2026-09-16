import QtQuick
import Isotone

// Appearance's live preview: a response graph of a fixed sample (PreviewSession)
// in the current theme, accent and band colours, with its handles. Not
// interactive, and the real session is untouched.
Rectangle {
    id: root
    readonly property alias session: sample
    readonly property alias graph: graph

    radius: 14
    color: Theme.plot
    border.width: 1
    border.color: Theme.gridMajor

    PreviewSession { id: sample }

    ResponseGraph {
        id: graph
        objectName: "previewGraph"
        x: 12
        y: 12
        width: parent.width - 24
        height: parent.height - 24
        session: sample
        fontFamily: Theme.font
        perBandColours: Theme.perBandColours
        bandColours: Theme.bandColours
        accent: Theme.accent
        gridMajor: Theme.gridMajor
        gridMinor: Theme.gridMinor
        zeroLine: Theme.zero
        labelColour: Theme.muted
        spectrumFill: Theme.spectrumFill
        spectrumEdge: Theme.spectrumEdge
        bell: Theme.bell
        fillEdgeAlpha: Theme.fillEdgeAlpha
        fillMidAlpha: Theme.fillMidAlpha
        spectrumSmoothing: GeneralSettings.smoothing

        Repeater {
            model: sample
            delegate: Item {
                id: handle
                required property int index
                required property real frequency
                required property int position
                required property int colorIndex
                required property bool selected
                readonly property color colour: Theme.bandColour(colorIndex)
                readonly property real radius: selected ? 12 : 10
                x: (graph.revision >= 0 && graph.plotWidth > 0 ? graph.xOf(frequency) : 0) - 17
                y: (graph.revision >= 0 && graph.plotHeight > 0 ? graph.yOf(graph.handleDb(index)) : 0) - 17
                width: 34
                height: 34
                z: selected ? 2 : 1
                Rectangle {
                    visible: handle.selected
                    anchors.fill: parent
                    radius: 17
                    color: "transparent"
                    border.width: 2
                    border.color: Qt.rgba(handle.colour.r, handle.colour.g, handle.colour.b, 0.45)
                }
                Rectangle {
                    anchors.centerIn: parent
                    width: handle.radius * 2 + 2
                    height: width
                    radius: width / 2
                    color: handle.colour
                    border.width: 2
                    border.color: Theme.plot
                    Text {
                        anchors.centerIn: parent
                        text: handle.position
                        font.family: Theme.font
                        font.pixelSize: 11
                        font.weight: Font.DemiBold
                        color: Theme.textOnAccent
                    }
                }
            }
        }
    }
}
