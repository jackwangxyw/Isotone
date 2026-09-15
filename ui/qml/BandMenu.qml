import QtQuick
import Isotone

// The band popover (BandMenu board): filter types drawn from their responses,
// Channels, Enabled, Duplicate, Reset gain, Delete. Fills its parent; opens
// centred on a point, above it, or below where there is no room above. A press
// outside or Escape closes it.
Item {
    id: root
    property int row: -1
    readonly property bool open: panel.visible
    readonly property var types: [
        {type: 0, name: "Peak"}, {type: 6, name: "Low shelf"}, {type: 7, name: "High shelf"}, {type: 1, name: "Low pass"},
        {type: 2, name: "High pass"}, {type: 3, name: "Band pass"}, {type: 4, name: "Notch"}, {type: 5, name: "All pass"}]

    component Separator: Item {
        width: parent ? parent.width : 0
        height: 13
        Rectangle { y: 6; width: parent.width; height: 1; color: Theme.gridMinor }
    }
    // A row of the menu: a label, and a hint or a control on the right.
    component MenuRow: Rectangle {
        id: menuRow
        property string label
        property string hint
        property bool danger: false
        property bool active: true
        property bool clickable: true
        default property alias trailing: trailingSlot.data
        signal activated()
        width: parent ? parent.width : 0
        height: 34
        radius: 7
        color: clickable && active && rowArea.containsMouse ? Theme.track : "transparent"
        opacity: active ? 1 : 0.35
        MouseArea {
            id: rowArea
            anchors.fill: parent
            hoverEnabled: menuRow.clickable
            enabled: menuRow.active
            cursorShape: menuRow.clickable ? Qt.PointingHandCursor : Qt.ArrowCursor
            onClicked: if (menuRow.clickable) menuRow.activated()
        }
        Text {
            x: 8
            anchors.verticalCenter: parent.verticalCenter
            text: menuRow.label
            font.family: Theme.font
            font.pixelSize: 13
            color: menuRow.danger ? Theme.danger : Theme.text
        }
        Text {
            anchors.right: parent.right
            anchors.rightMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            text: menuRow.hint
            font.family: Theme.font
            font.pixelSize: 13
            color: Theme.muted
        }
        Item {
            id: trailingSlot
            anchors.right: parent.right
            anchors.rightMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            width: childrenRect.width
            height: childrenRect.height
        }
    }

    // What the band holds; re-read whenever the model changes.
    property int revision: 0
    function value(role) {
        return root.revision >= 0 && root.row >= 0 && root.row < EqSession.count
            ? EqSession.data(EqSession.index(root.row, 0), role) : undefined
    }

    // Centred on x; above `above`, or below `below` where the panel does not fit above.
    function openAt(row, x, above, below) {
        root.row = row
        EqSession.select(row)
        const margin = 8
        const fitsAbove = above - 8 - panel.height >= margin
        panel.pointsDown = fitsAbove
        panel.x = Math.max(margin, Math.min(root.width - panel.width - margin, x - panel.width / 2))
        panel.y = fitsAbove ? above - 8 - panel.height : Math.min(root.height - panel.height - margin, below + 8)
        panel.caretX = x - panel.x
        panel.visible = true
        panel.forceActiveFocus()
    }
    function close() {
        panel.visible = false
        root.row = -1
    }

    Connections {
        target: EqSession
        function onDataChanged() { root.revision++ }
        function onModelReset() {
            // The rows were rebuilt (a band added, deleted or re-sorted): the band
            // is no longer known by its row.
            if (root.open) root.close()
            root.revision++
        }
    }

    MouseArea {
        anchors.fill: parent
        visible: panel.visible
        acceptedButtons: Qt.AllButtons
        onPressed: root.close()
    }

    Rectangle {
        id: panel
        objectName: "bandMenuPanel"
        visible: false
        width: 348
        height: content.implicitHeight + 20
        radius: 14
        color: Theme.surface
        border.color: Theme.gridMajor
        property bool pointsDown: true
        property real caretX: width / 2
        Keys.onEscapePressed: root.close()

        // Presses inside do not close it.
        MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons }

        // The caret: a square turned 45 degrees under the panel, centred on its
        // edge, with the panel's border covered where it crosses the caret.
        readonly property real caretCentre: Math.max(22, Math.min(width - 22, caretX))
        Rectangle {
            x: panel.caretCentre - 7
            y: panel.pointsDown ? panel.height - 7 : -7
            width: 14
            height: 14
            rotation: 45
            color: Theme.surface
            border.color: Theme.gridMajor
            z: -1
        }
        Rectangle {
            x: panel.caretCentre - 10
            y: panel.pointsDown ? panel.height - 8 : 0
            width: 20
            height: 8
            color: Theme.surface
        }

        Column {
            id: content
            x: 10
            y: 10
            width: parent.width - 20
            spacing: 0

            Grid {
                columns: 4
                columnSpacing: 4
                rowSpacing: 4
                Repeater {
                    model: root.types
                    delegate: Rectangle {
                        id: tile
                        required property var modelData
                        readonly property bool current: root.value(EqSession.TypeRole) === modelData.type
                        objectName: "typeTile" + modelData.type
                        width: (content.width - 12) / 4
                        height: glyph.height + name.implicitHeight + 6 + 15
                        radius: 10
                        color: current ? Theme.track : tileArea.containsMouse ? Qt.rgba(Theme.track.r, Theme.track.g, Theme.track.b, 0.5) : "transparent"
                        border.width: 1
                        border.color: current ? Theme.accent : "transparent"
                        FilterGlyph {
                            id: glyph
                            y: 8
                            anchors.horizontalCenter: parent.horizontalCenter
                            width: 56
                            height: 32
                            type: tile.modelData.type
                            selected: tile.current
                            stroke: tile.current ? Theme.accent : Theme.muted
                            zeroLine: Theme.gridMajor
                            background: Theme.plot
                        }
                        Text {
                            id: name
                            anchors.horizontalCenter: parent.horizontalCenter
                            y: glyph.y + glyph.height + 6
                            text: tile.modelData.name
                            font.family: Theme.font
                            font.pixelSize: 11
                            color: tile.current ? Theme.text : Theme.muted
                        }
                        MouseArea {
                            id: tileArea
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: EqSession.setType(root.row, tile.modelData.type)
                        }
                    }
                }
            }

            Separator {}
            MenuRow {
                label: "Channels"
                clickable: false
                visible: EqSession.outputChannels === 2
                Segmented {
                    objectName: "bandChannels"
                    options: ["L", "R", "L+R"]
                    current: root.value(EqSession.ChannelsRole) ?? 2
                    onPicked: (index) => EqSession.setChannels(root.row, index)
                }
            }
            MenuRow {
                label: "Enabled"
                clickable: false
                Toggle {
                    objectName: "bandEnabled"
                    width: 30
                    height: 18
                    checked: root.value(EqSession.EnabledRole) ?? false
                    onToggled: (on) => EqSession.setEnabled(root.row, on)
                }
            }
            Separator {}
            MenuRow {
                objectName: "menuDuplicate"
                label: "Duplicate"
                active: EqSession.canAddBand
                onActivated: { const r = root.row; root.close(); EqSession.duplicateBand(r) }
            }
            MenuRow {
                objectName: "menuResetGain"
                label: "Reset gain"
                active: root.value(EqSession.HasGainRole) ?? false
                onActivated: EqSession.resetGain(root.row)
            }
            MenuRow {
                objectName: "menuDelete"
                label: "Delete"
                hint: "Del"
                danger: true
                onActivated: { const r = root.row; root.close(); EqSession.deleteBand(r) }
            }
        }
    }
}
