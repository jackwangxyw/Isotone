import QtQuick
import Isotone

// The graph card: the response graph with draggable numbered handles and the
// hover readout. Drag a handle for frequency and gain, scroll on it for Q.
Rectangle {
    id: root
    // Where the readout is; negative for none.
    property real hoverFrequency: -1
    signal menuRequested(int row, real x, real above, real below)
    // The handle of a row, for tests: they all share an objectName.
    function handleItem(row) { return handles.itemAt(row) }

    radius: 18
    color: Theme.plot
    height: 14 + 404 + 4   // the 1440 x 900 board; the view sets it from the window

    // A wheel spin on a handle is one edit: each notch live, committed once the
    // wheel has been still for 300 ms, or when a band is pressed or picked. On an
    // Equalizer APO output every commit rewrites Isotone.txt.
    Timer {
        id: wheelCommit
        interval: 300
        onTriggered: EqSession.finishEdit()
    }
    function commitWheel() {
        if (!wheelCommit.running) return
        wheelCommit.stop()
        EqSession.finishEdit()
    }
    Connections {
        target: EqSession
        function onSelectionChanged() { root.commitWheel() }
    }

    ResponseGraph {
        id: graph
        objectName: "responseGraph"
        x: 10
        y: 14
        width: parent.width - 20
        height: parent.height - 18
        session: EqSession
        fontFamily: Theme.font
        spectrumVisible: AppSettings.spectrumOn
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
        // Settings, General: graph ranges and peak hold.
        rangeDb: GeneralSettings.gainRange
        minHz: GeneralSettings.minHz
        maxHz: GeneralSettings.maxHz
        peakHoldVisible: GeneralSettings.peakHold

        MouseArea {
            anchors.fill: parent
            onDoubleClicked: (mouse) => {
                if (mouse.x >= graph.plotLeft && mouse.x <= graph.plotLeft + graph.plotWidth)
                    EqSession.addBand(graph.frequencyAt(mouse.x), graph.dbAt(mouse.y))
            }
        }
        MouseArea {
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.NoButton
            onPositionChanged: (mouse) => {
                const inside = mouse.x >= graph.plotLeft && mouse.x <= graph.plotLeft + graph.plotWidth
                root.hoverFrequency = inside ? graph.frequencyAt(mouse.x) : -1
            }
            onExited: root.hoverFrequency = -1
        }

        // Hover readout: a dashed line and the composite at that frequency.
        Item {
            id: readout
            visible: root.hoverFrequency > 0
            // xOf and compositeAt are not properties: the conditions on revision and
            // plotWidth make the bindings re-run when the curve or the size changes.
            // (A comma expression does not: the compiled binding drops the unused read.)
            readonly property real lineX: graph.revision >= 0 && graph.plotWidth > 0 ? graph.xOf(root.hoverFrequency) : 0
            readonly property real db: graph.revision >= 0 ? graph.compositeAt(root.hoverFrequency) : 0
            anchors.fill: parent
            Repeater {
                model: Math.max(0, Math.ceil(graph.plotHeight / 7))   // 0 before the graph has a size
                delegate: Rectangle {
                    required property int index
                    x: Math.round(readout.lineX)
                    y: graph.plotTop + index * 7
                    width: 1
                    height: Math.min(3, graph.plotTop + graph.plotHeight - y)
                    color: Theme.muted
                }
            }
            Rectangle {
                x: readout.lineX + 10
                y: graph.plotTop + 10
                width: chipText.implicitWidth + 20
                height: 26
                radius: 6
                color: Theme.surface
                border.color: Theme.gridMajor
                Text {
                    id: chipText
                    x: 10
                    anchors.verticalCenter: parent.verticalCenter
                    textFormat: Text.StyledText
                    font.family: Theme.font
                    font.pixelSize: 12
                    color: Theme.text
                    text: {
                        const f = root.hoverFrequency
                        const hz = f >= 1000 ? Number((f / 1000).toPrecision(2)) + " kHz" : Math.round(f) + " Hz"
                        return hz + "&nbsp;&nbsp;<font color='" + Theme.muted + "'>" + Theme.signed(readout.db, 1) + " dB</font>"
                    }
                }
            }
        }

        Repeater {
            id: handles
            model: EqSession
            delegate: Item {
                id: handle
                objectName: "handle"
                required property int index
                required property real frequency
                required property real gain
                required property real q
                required property int position
                required property int colorIndex
                required property bool selected
                required property bool bandEnabled
                readonly property color colour: Theme.bandColour(colorIndex)
                // Slightly smaller than the boards (owner, 2026-09-15).
                readonly property real radius: selected ? 12 : 10
                // Re-evaluated when the curve or the size changes, as the readout's.
                readonly property real cx: graph.revision >= 0 && graph.plotWidth > 0 ? graph.xOf(frequency) : 0
                readonly property real cy: graph.revision >= 0 && graph.plotHeight > 0 ? graph.yOf(graph.handleDb(index)) : 0
                readonly property bool onView: graph.revision >= 0 && graph.onView(index)

                // Settings, General: a band outside the frequency range has no handle,
                // and one beyond the gain range sits on the plot's edge.
                visible: frequency >= graph.minHz && frequency <= graph.maxHz
                x: cx - 20
                y: Math.max(graph.plotTop, Math.min(graph.plotTop + graph.plotHeight, cy)) - 20
                width: 40
                height: 40
                z: selected ? 2 : 1
                opacity: EqSession.eqOn && !EqSession.muted && onView && bandEnabled ? 1 : 0.4

                Rectangle {
                    visible: handle.selected
                    anchors.centerIn: parent
                    width: 36
                    height: 36
                    radius: 18
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
                        anchors.verticalCenterOffset: 0.5
                        text: handle.position
                        font.family: Theme.font
                        font.pixelSize: handle.position < 10 ? 11 : 10
                        font.weight: Font.DemiBold
                        color: Theme.textOnAccent
                    }
                }

                MouseArea {
                    anchors.centerIn: parent
                    width: handle.radius * 2 + 4
                    height: width
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    cursorShape: pressed ? Qt.ClosedHandCursor : Qt.OpenHandCursor
                    property real startDb
                    property real startGain
                    onPressed: (mouse) => {
                        root.commitWheel()
                        EqSession.select(handle.index)
                        if (mouse.button === Qt.RightButton) {
                            // Centred on the handle, above it, or below where there is no room.
                            const c = handle.mapToItem(null, handle.width / 2, handle.height / 2)
                            root.menuRequested(handle.index, c.x, c.y - 18, c.y + 18)
                            return
                        }
                        const p = mapToItem(graph, mouse.x, mouse.y)
                        startDb = graph.dbAt(p.y)
                        startGain = handle.gain
                    }
                    onPositionChanged: (mouse) => {
                        if (!pressed || (pressedButtons & Qt.RightButton)) return
                        const p = mapToItem(graph, mouse.x, mouse.y)
                        EqSession.setFrequency(handle.index, graph.frequencyAt(p.x))
                        EqSession.setGain(handle.index, startGain + graph.dbAt(p.y) - startDb)
                    }
                    onReleased: EqSession.finishEdit()
                    // As a double-click on the band's gain slider does.
                    onDoubleClicked: (mouse) => {
                        if (mouse.button !== Qt.LeftButton) return
                        EqSession.resetGain(handle.index)
                        EqSession.finishEdit()
                    }
                    onWheel: (wheel) => {
                        if (!handle.selected) root.commitWheel()   // before the selection moves: undo selects that band
                        EqSession.select(handle.index)
                        EqSession.setWidth(handle.index, handle.q * (wheel.angleDelta.y > 0 ? 1.08 : 1 / 1.08), false)
                        wheelCommit.restart()
                    }
                }
            }
        }
    }
}
