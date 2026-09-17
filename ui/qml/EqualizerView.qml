import QtQuick
import Isotone

// The Equalizer view: top bar, graph, band strip with the right-hand panel. The
// graph takes the height the strip (402 px) leaves: 422 px at 900, 282 at 760.
// Where the graph would get less than 200, the view keeps the graph or the strip
// alone, as Settings, General, Short window picks (owner, 2026-09-16).
Item {
    id: root
    readonly property bool compact: height < GeneralSettings.bothHeight
    readonly property bool graphShown: !compact || GeneralSettings.shortWindow === "graph"
    readonly property bool stripShown: !compact || GeneralSettings.shortWindow === "bands"
    signal bandMenuRequested(int row, real x, real above, real below)
    signal presetsRequested(real x, real y)

    Column {
        anchors.fill: parent

        TopBar {
            width: parent.width
            onPresetsRequested: (x, y) => root.presetsRequested(x, y)
        }
        GraphCard {
            objectName: "equalizerGraph"
            visible: root.graphShown
            x: 32
            width: parent.width - 64
            height: root.stripShown ? Math.max(GeneralSettings.leastGraphHeight, root.height - GeneralSettings.topBarHeight - GeneralSettings.stripHeight)
                                    : Math.max(GeneralSettings.leastGraphHeight, root.height - GeneralSettings.topBarHeight - GeneralSettings.graphAloneMargin)
            onMenuRequested: (row, x, above, below) => root.bandMenuRequested(row, x, above, below)
        }
        BandStrip {
            objectName: "equalizerStrip"
            visible: root.stripShown
            width: parent.width
            height: GeneralSettings.stripHeight
            onMenuRequested: (row, x, above, below) => root.bandMenuRequested(row, x, above, below)
        }
    }
}
