import QtQuick
import Isotone

// One band in the strip: number and type, gain slider, gain, frequency and Q,
// target and enable switch.
Rectangle {
    id: root
    required property int index
    required property int position
    required property string typeName
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
    // In window coordinates: centred on x, above `above` or below `below`.
    signal menuRequested(real x, real above, real below)

    width: 112
    height: column.implicitHeight + 26
    radius: 12
    color: selected ? Theme.selectedColumn : "transparent"

    MouseArea { anchors.fill: parent; onPressed: EqSession.select(root.index) }

    Column {
        id: column
        y: 14
        width: parent.width
        spacing: 12

        Row {
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: 7
            Rectangle {
                width: 22
                height: 22
                radius: 11
                color: root.selected ? root.colour : Theme.track
                Text {
                    anchors.centerIn: parent
                    text: root.position
                    font.family: Theme.font
                    font.pixelSize: 11
                    font.weight: Font.DemiBold
                    color: root.selected ? Theme.textOnAccent : Theme.text
                }
            }
            Text {
                id: typeLabel
                objectName: "typeName"
                anchors.verticalCenter: parent.verticalCenter
                text: root.typeName
                font.family: Theme.font
                font.pixelSize: 12
                color: typeArea.containsMouse ? Theme.text : Theme.muted
                MouseArea {
                    id: typeArea
                    anchors.fill: parent
                    anchors.margins: -4
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    // The popover is centred on the column, above the type name.
                    onClicked: {
                        const top = typeLabel.mapToItem(null, 0, -4)
                        const centre = root.mapToItem(null, root.width / 2, 0)
                        root.menuRequested(centre.x, top.y, top.y + typeLabel.height + 8)
                    }
                }
            }
        }
        // A type without gain shows its slider and value greyed, at 0 dB.
        GainSlider {
            objectName: "gainSlider"
            anchors.horizontalCenter: parent.horizontalCenter
            enabled: root.hasGain
            opacity: root.hasGain ? 1 : 0.35
            gain: root.hasGain ? root.gain : 0
            colour: root.colour
            onMoved: (g) => { EqSession.select(root.index); EqSession.setGain(root.index, g) }
            onReleased: EqSession.finishEdit()
        }
        ValueField {
            objectName: "gainValue"
            anchors.horizontalCenter: parent.horizontalCenter
            text: Theme.signed(root.hasGain ? root.gain : 0, 1) + " dB"
            unit: EqSession.Decibels
            editable: root.hasGain
            opacity: root.hasGain ? 1 : 0.35
            pixelSize: 15
            weight: Font.DemiBold
            onStarted: EqSession.select(root.index)
            onSubmitted: (v) => { EqSession.setGain(root.index, v); EqSession.finishEdit() }
        }
        Column {
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: 4
            ValueField {
                objectName: "frequencyValue"
                anchors.horizontalCenter: parent.horizontalCenter
                text: Theme.frequency(root.frequency)
                unit: EqSession.Hertz
                pixelSize: 12
                colour: Theme.muted
                onStarted: EqSession.select(root.index)
                onSubmitted: (v) => { EqSession.setFrequency(root.index, v); EqSession.finishEdit() }
            }
            ValueField {
                objectName: "widthValue"
                anchors.horizontalCenter: parent.horizontalCenter
                text: root.widthLabel
                unit: root.widthUnit
                pixelSize: 12
                colour: Theme.muted
                onStarted: EqSession.select(root.index)
                onSubmitted: (v) => EqSession.setWidth(root.index, v)
            }
        }
        Row {
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: 8
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: root.target
                font.family: Theme.font
                font.pixelSize: 11
                color: Theme.muted
            }
            Toggle {
                width: 28
                height: 16
                checked: root.bandEnabled
                colour: root.colour
                onToggled: (on) => EqSession.setEnabled(root.index, on)
            }
        }
    }
}
