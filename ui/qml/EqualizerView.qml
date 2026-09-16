import QtQuick
import Isotone

// The Equalizer view: top bar, graph, band strip with the right-hand panel. The
// graph takes the height the strip (402 px) leaves: 422 px at 900, 282 at 760.
Item {
    id: root
    signal bandMenuRequested(int row, real x, real above, real below)
    signal presetsRequested(real x, real y)

    Column {
        anchors.fill: parent

        TopBar {
            width: parent.width
            onPresetsRequested: (x, y) => root.presetsRequested(x, y)
        }
        GraphCard {
            x: 32
            width: parent.width - 64
            height: Math.max(200, root.height - 76 - 402)
            onMenuRequested: (row, x, above, below) => root.bandMenuRequested(row, x, above, below)
        }
        BandStrip {
            width: parent.width
            height: 402
            onMenuRequested: (row, x, above, below) => root.bandMenuRequested(row, x, above, below)
        }
    }
}
