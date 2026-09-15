import QtQuick
import Isotone

// Channels panel (stereo), open: balance and mute.
Item {
    id: root

    Rectangle { width: 1; height: parent.height; color: Theme.gridMinor }

    Column {
        x: 22
        y: 4
        width: parent.width - 22
        spacing: 18

        Item {
            width: parent.width
            height: 32
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: "Channels"
                font.family: Theme.font
                font.pixelSize: 13
                font.weight: Font.DemiBold
                color: Theme.text
            }
            Item {
                anchors.right: parent.right
                width: 32
                height: 32
                Icon { name: "chevronRight"; size: 16; anchors.centerIn: parent }
            }
        }

        Column {
            width: parent.width
            spacing: 10
            Item {
                width: parent.width
                height: value.height
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: "Balance"
                    font.family: Theme.font
                    font.pixelSize: 13
                    color: Theme.text
                }
                Rectangle {
                    id: value
                    anchors.right: parent.right
                    width: valueText.implicitWidth + 20
                    height: valueText.implicitHeight + 8
                    radius: 7
                    color: Theme.track
                    Text {
                        id: valueText
                        anchors.centerIn: parent
                        text: EqSession.balance === 0 ? "0.0" : Theme.signed(EqSession.balance, 1)
                        font.family: Theme.font
                        font.pixelSize: 13
                        font.weight: Font.DemiBold
                        color: Theme.text
                    }
                }
            }
            // −1.0 to +1.0, filled from the centre.
            Item {
                id: balance
                width: parent.width
                height: 18
                readonly property real centre: width / 2
                readonly property real knobX: centre + EqSession.balance * centre
                Rectangle { y: 7; width: parent.width; height: 4; radius: 2; color: Theme.track }
                Rectangle { x: balance.centre - 1; y: 3; width: 2; height: 12; radius: 1; color: Theme.zero }
                Rectangle {
                    y: 7
                    height: 4
                    radius: 2
                    color: Theme.accent
                    x: Math.min(balance.knobX, balance.centre)
                    width: Math.abs(balance.knobX - balance.centre)
                }
                Rectangle {
                    x: balance.knobX - 9
                    width: 18
                    height: 18
                    radius: 9
                    color: Theme.knob
                    border.width: 2
                    border.color: Theme.accent
                }
                MouseArea {
                    anchors.fill: parent
                    anchors.margins: -6
                    cursorShape: Qt.PointingHandCursor
                    function set(mouseX) { EqSession.balance = ((mouseX - 6) - balance.centre) / balance.centre }
                    onPressed: (mouse) => set(mouse.x)
                    onPositionChanged: (mouse) => { if (pressed) set(mouse.x) }
                    onReleased: EqSession.finishEdit()
                }
            }
        }

        Item {
            width: parent.width
            height: 20
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: "Mute"
                font.family: Theme.font
                font.pixelSize: 13
                color: Theme.text
            }
            Toggle {
                anchors.right: parent.right
                checked: EqSession.muted
                onToggled: (on) => EqSession.muted = on
            }
        }
    }
}
