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
    required property bool bandEnabled
    required property string target
    required property int colorIndex
    required property bool selected
    readonly property color colour: Theme.bandColour(colorIndex)

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
                anchors.verticalCenter: parent.verticalCenter
                text: root.typeName
                font.family: Theme.font
                font.pixelSize: 12
                color: Theme.muted
            }
        }
        GainSlider {
            objectName: "gainSlider"
            anchors.horizontalCenter: parent.horizontalCenter
            gain: root.gain
            colour: root.colour
            onMoved: (g) => { EqSession.select(root.index); EqSession.setGain(root.index, g) }
            onReleased: EqSession.finishEdit()
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: Theme.signed(root.gain, 1) + " dB"
            font.family: Theme.font
            font.pixelSize: 15
            font.weight: Font.DemiBold
            color: Theme.text
        }
        Column {
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: 4
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: Theme.frequency(root.frequency)
                font.family: Theme.font
                font.pixelSize: 12
                color: Theme.muted
            }
            Text {
                anchors.horizontalCenter: parent.horizontalCenter
                text: root.widthLabel
                font.family: Theme.font
                font.pixelSize: 12
                color: Theme.muted
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
