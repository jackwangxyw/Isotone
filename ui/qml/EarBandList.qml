import QtQuick
import Isotone

// EQ by ear's bands: a card with the count and Add band, then a row per band
// (number, type, frequency, gain, Q, channels, enable), values click-to-edit.
// It scrolls; the selected band is scrolled into view. Geometry from the board's
// generator (gen_screens.py, band_list).
Rectangle {
    id: root
    signal menuRequested(int row, real x, real above, real below)

    radius: 18
    color: Theme.plot

    // 28 190 120 100 80 90 1fr 40, 10 apart, 12 in from the row's sides.
    readonly property var columns: [28, 190, 120, 100, 80, 90]
    readonly property var columnX: [0, 38, 238, 368, 478, 568]

    component HeaderText: Text {
        required property int column
        x: root.columnX[column]
        font.family: Theme.font
        font.pixelSize: 12
        color: Theme.muted
    }

    Item {
        id: header
        x: 20
        y: 12
        width: parent.width - 40
        height: 28
        Row {
            anchors.verticalCenter: parent.verticalCenter
            spacing: 10
            Text { text: "Bands"; font.family: Theme.font; font.pixelSize: 13; font.weight: Font.DemiBold; color: Theme.text }
            Text { text: EqSession.count; font.family: Theme.font; font.pixelSize: 13; color: Theme.muted }
        }
        Rectangle {
            objectName: "earAddBand"
            anchors.right: parent.right
            width: 28
            height: 28
            radius: 8
            color: addArea.containsMouse && EqSession.canAddBand ? Theme.surface : Theme.track
            opacity: EqSession.canAddBand ? 1 : 0.45
            Icon { name: "plus"; size: 15; colour: Theme.text; anchors.centerIn: parent }
            MouseArea {
                id: addArea
                anchors.fill: parent
                hoverEnabled: true
                enabled: EqSession.canAddBand
                cursorShape: Qt.PointingHandCursor
                onClicked: EqSession.addBand(1000, 0)
            }
        }
    }

    Item {
        id: labels
        x: 20
        y: header.y + header.height + 8
        width: parent.width - 40
        height: 19
        HeaderText { column: 1; text: "Type" }
        HeaderText { column: 2; text: "Freq" }
        HeaderText { column: 3; text: "Gain" }
        HeaderText { column: 4; text: "Q" }
        HeaderText { column: 5; text: "Channels" }
    }

    ListView {
        id: list
        objectName: "earBandList"
        x: 8
        y: labels.y + labels.height
        width: parent.width - 16 - 14
        height: parent.height - y - 8
        clip: true
        spacing: 2
        boundsBehavior: Flickable.StopAtBounds
        model: EqSession
        // After the model has rebuilt its rows: an added band resets it, and selects itself.
        function showSelected() { if (EqSession.selectedRow >= 0) positionViewAtIndex(EqSession.selectedRow, ListView.Contain) }
        Connections {
            target: EqSession
            function onSelectionChanged() { Qt.callLater(list.showSelected) }
        }

        delegate: Rectangle {
            id: row
            objectName: "earBandRow"
            required property int index
            required property int position
            required property string typeName
            required property int type
            required property real frequency
            required property real gain
            required property real q
            required property string widthLabel
            required property int widthUnit
            required property bool hasGain
            required property bool bandEnabled
            required property string target
            required property int colorIndex
            required property bool selected
            readonly property color colour: Theme.bandColour(colorIndex)

            width: ListView.view.width
            height: 38
            radius: 9
            color: selected ? Theme.surface : "transparent"

            function openMenu() {
                const c = typeText.mapToItem(null, typeText.width / 2, 0)
                root.menuRequested(row.index, c.x, c.y - 4, c.y + typeText.height + 8)
            }

            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.LeftButton | Qt.RightButton
                onPressed: (mouse) => {
                    EqSession.select(row.index)
                    if (mouse.button === Qt.RightButton) row.openMenu()
                }
            }

            Item {
                x: 12
                width: parent.width - 24
                height: parent.height
                opacity: row.bandEnabled ? 1 : 0.5

                Rectangle {
                    x: root.columnX[0]
                    anchors.verticalCenter: parent.verticalCenter
                    width: 22
                    height: 22
                    radius: 11
                    color: row.selected ? row.colour : Theme.track
                    Text {
                        anchors.centerIn: parent
                        text: row.position
                        font.family: Theme.font
                        font.pixelSize: 11
                        font.weight: Font.DemiBold
                        color: row.selected ? Theme.textOnAccent : Theme.text
                    }
                }
                Row {
                    x: root.columnX[1]
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 10
                    FilterGlyph {
                        anchors.verticalCenter: parent.verticalCenter
                        width: 30
                        height: 18
                        type: row.type
                        selected: row.selected
                        stroke: row.selected ? Theme.accent : Theme.muted
                        zeroLine: Theme.gridMajor
                        background: "transparent"
                    }
                    Text {
                        id: typeText
                        objectName: "earTypeName"
                        anchors.verticalCenter: parent.verticalCenter
                        text: row.typeName
                        font.family: Theme.font
                        font.pixelSize: 13
                        color: row.selected || typeArea.containsMouse ? Theme.text : Theme.muted
                        MouseArea {
                            id: typeArea
                            anchors.fill: parent
                            anchors.margins: -4
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: { EqSession.select(row.index); row.openMenu() }
                        }
                    }
                }
                ValueField {
                    objectName: "earFrequencyValue"
                    x: root.columnX[2]
                    anchors.verticalCenter: parent.verticalCenter
                    horizontalAlignment: Text.AlignLeft
                    text: Theme.frequency(row.frequency)
                    unit: EqSession.Hertz
                    onStarted: EqSession.select(row.index)
                    onSubmitted: (v) => { EqSession.setFrequency(row.index, v); EqSession.finishEdit() }
                }
                ValueField {
                    objectName: "earGainValue"
                    x: root.columnX[3]
                    anchors.verticalCenter: parent.verticalCenter
                    horizontalAlignment: Text.AlignLeft
                    text: Theme.signed(row.hasGain ? row.gain : 0, 1) + " dB"
                    unit: EqSession.Decibels
                    editable: row.hasGain
                    opacity: row.hasGain ? 1 : 0.35
                    weight: Font.DemiBold
                    onStarted: EqSession.select(row.index)
                    onSubmitted: (v) => { EqSession.setGain(row.index, v); EqSession.finishEdit() }
                }
                ValueField {
                    objectName: "earWidthValue"
                    x: root.columnX[4]
                    anchors.verticalCenter: parent.verticalCenter
                    horizontalAlignment: Text.AlignLeft
                    text: row.widthUnit === EqSession.Q ? row.q.toFixed(2) : row.widthLabel
                    unit: row.widthUnit
                    colour: Theme.muted
                    onStarted: EqSession.select(row.index)
                    onSubmitted: (v) => EqSession.setWidth(row.index, v)
                }
                Text {
                    x: root.columnX[5]
                    anchors.verticalCenter: parent.verticalCenter
                    text: row.target
                    font.family: Theme.font
                    font.pixelSize: 13
                    color: Theme.muted
                }
                Toggle {
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    width: 28
                    height: 16
                    checked: row.bandEnabled
                    colour: row.colour
                    onToggled: (on) => EqSession.setEnabled(row.index, on)
                }
            }
        }
    }

    // Rows scrolled up under the column names fade out.
    Rectangle {
        visible: list.contentY > list.originY
        x: list.x
        y: list.y
        width: list.width
        height: 36
        gradient: Gradient {
            GradientStop { position: 0; color: Theme.plot }
            GradientStop { position: 1; color: Qt.alpha(Theme.plot, 0) }
        }
    }

    // The thumb, while the list scrolls.
    Rectangle {
        visible: list.contentHeight > list.height
        x: parent.width - 8 - 2 - 4
        y: list.y + 4
        width: 4
        height: list.height - 8
        radius: 2
        color: Theme.track
        Rectangle {
            y: parent.height * list.visibleArea.yPosition
            width: 4
            height: parent.height * list.visibleArea.heightRatio
            radius: 2
            color: Theme.zero
        }
    }
}
