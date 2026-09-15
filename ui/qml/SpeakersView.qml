import QtQuick
import Isotone

// The Speakers view (Speakers board and the prototype's speakersView), for
// outputs with more than two channels: the layout picker and test tones, the
// levels and time alignment table, and the speaker groups, bass management and
// routing cards. Leaving the view ends test tones and solo.
Item {
    id: root
    // Changes the output's layout; an HRESULT. Tests replace it.
    property var layoutSetter: (name) => Speakers.setLayout(name)
    readonly property var layouts: ["Stereo", "2.1", "5.1", "7.1"]
    readonly property bool tones: Speakers.testTones

    Component.onCompleted: Speakers.refreshSupportedLayouts()
    Component.onDestruction: Speakers.endSession()

    Connections {
        target: Speakers
        function onLayoutChanged() { Speakers.refreshSupportedLayouts() }
        function onToneFailed() { UiState.toast("Test tone stopped") }
        function onSaveFailed() { UiState.toast("Speaker settings not saved") }
    }
    Connections {
        target: EqSession
        function onSpeakerSaveFailed() { UiState.toast("Speaker settings not saved") }
    }

    function requestLayout(name) {
        if (name === Speakers.layoutName) return
        UiState.openDialog(layoutDialog, {
            outputName: Outputs.currentName, from: Speakers.layoutName, to: name,
            fromChannels: Speakers.channels, toChannels: Speakers.layoutChannels(name)
        })
    }
    Component {
        id: layoutDialog
        LayoutDialog {
            onConfirmed: {
                const hr = root.layoutSetter(to)
                if (hr !== 0) UiState.toast("Speaker setup not changed")
            }
        }
    }
    Component { id: newGroupDialog; NewGroupDialog {} }

    // A value box (the prototype's .sbox) holding a click-to-edit value.
    component ValueBox: Rectangle {
        id: box
        property alias text: field.text
        property alias unit: field.unit
        property alias field: field
        signal submitted(real value)
        implicitWidth: Math.max(56, field.implicitWidth + 16)
        implicitHeight: 26
        radius: 6
        color: Theme.segmentedSelected
        ValueField {
            id: field
            anchors.centerIn: parent
            weight: Font.DemiBold
            onSubmitted: (v) => box.submitted(v)
        }
    }
    component Header: Text {
        y: 10
        font.family: Theme.font
        font.pixelSize: 12
        color: Theme.muted
    }
    component Label: Text {
        font.family: Theme.font
        font.pixelSize: 13
        color: Theme.text
    }
    component SmallSegmented: Rectangle {
        id: seg
        property var options: []
        property int current: 0
        property var disabled: []   // indexes that cannot be picked
        property int fontSize: 12
        property int itemHeight: 22
        signal picked(int index)
        implicitWidth: segRow.implicitWidth + 6
        implicitHeight: itemHeight + 6
        radius: 8
        color: Theme.track
        Row {
            id: segRow
            x: 3
            y: 3
            spacing: 2
            Repeater {
                model: seg.options
                delegate: Rectangle {
                    required property string modelData
                    required property int index
                    readonly property bool off: seg.disabled.indexOf(index) >= 0
                    objectName: "option_" + modelData
                    width: optionText.implicitWidth + (seg.fontSize > 12 ? 24 : 18)
                    height: seg.itemHeight
                    radius: 6
                    color: index === seg.current ? Theme.segmentedSelected : "transparent"
                    opacity: off ? 0.35 : 1
                    Text {
                        id: optionText
                        anchors.centerIn: parent
                        text: modelData
                        font.family: Theme.font
                        font.pixelSize: seg.fontSize
                        font.weight: index === seg.current ? Font.Medium : Font.Normal
                        color: index === seg.current ? Theme.text : Theme.muted
                    }
                    MouseArea {
                        anchors.fill: parent
                        enabled: !parent.off
                        cursorShape: Qt.PointingHandCursor
                        onClicked: seg.picked(index)
                    }
                }
            }
        }
    }
    component Card: Rectangle {
        default property alias content: cardBody.data
        property string title
        radius: 14
        color: Theme.plot
        height: cardBody.implicitHeight + 36 + 12 + 18
        Text {
            x: 18
            y: 18
            text: parent.title
            font.family: Theme.font
            font.pixelSize: 13
            font.weight: Font.DemiBold
            color: Theme.text
        }
        Column {
            id: cardBody
            x: 18
            y: 18 + 18 + 12
            width: parent.width - 36
        }
    }
    component CardRow: Item {
        default property alias trailing: trailingSlot.data
        property string label
        width: parent ? parent.width : 0
        height: 39
        Label { anchors.verticalCenter: parent.verticalCenter; text: parent.label }
        Item {
            id: trailingSlot
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            width: childrenRect.width
            height: childrenRect.height
        }
    }

    Flickable {
        anchors.fill: parent
        contentHeight: page.height + 30
        boundsBehavior: Flickable.StopAtBounds
        clip: true

        Column {
            id: page
            x: 36
            y: 30
            width: root.width - 72

            // Output, title, layout and test tones.
            Item {
                width: parent.width
                height: titleColumn.height
                Column {
                    id: titleColumn
                    Text {
                        objectName: "outputName"
                        text: Outputs.currentName
                        font.family: Theme.font
                        font.pixelSize: 13
                        color: Theme.muted
                    }
                    Text {
                        text: "Speakers"
                        font.family: Theme.font
                        font.pixelSize: 26
                        font.weight: Font.DemiBold
                        font.letterSpacing: -0.39
                        color: Theme.text
                    }
                }
                Row {
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 2
                    spacing: 12
                    SmallSegmented {
                        id: layoutPicker
                        objectName: "layoutPicker"
                        options: root.layouts
                        fontSize: 13
                        itemHeight: 26
                        current: root.layouts.indexOf(Speakers.layoutName)
                        disabled: root.layouts.map((l, i) => i).filter(i => i !== current && Speakers.supportedLayouts.indexOf(root.layouts[i]) < 0)
                        onPicked: (index) => root.requestLayout(root.layouts[index])
                    }
                    Button {
                        objectName: "testTones"
                        text: root.tones ? "Stop test tones" : "Test tones"
                        icon: root.tones ? "pause" : "play"
                        kind: root.tones ? "primary" : "normal"
                        onClicked: Speakers.testTones = !Speakers.testTones
                    }
                }
            }

            // While test tones play.
            Item { width: 1; height: 14; visible: root.tones }
            Row {
                objectName: "tonesInfo"
                visible: root.tones
                spacing: 10
                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    width: pillRow.implicitWidth + 18
                    height: 22
                    radius: 6
                    color: Theme.surface
                    Row {
                        id: pillRow
                        x: 9
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 7
                        StatusDot { anchors.verticalCenter: parent.verticalCenter; status: "warn" }
                        Text {
                            text: "EQ bypassed · upmix and swaps off"
                            font.family: Theme.font
                            font.pixelSize: 11
                            font.weight: Font.Medium
                            color: Theme.text
                        }
                    }
                }
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: "Pink noise · −30 dBFS RMS"
                    font.family: Theme.font
                    font.pixelSize: 13
                    color: Theme.muted
                }
            }

            // Levels and time alignment.
            Item { width: 1; height: root.tones ? 14 : 30 }
            Item {
                width: parent.width
                height: 28
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    text: "Levels and time alignment"
                    font.family: Theme.font
                    font.pixelSize: 14
                    font.weight: Font.DemiBold
                    color: Theme.text
                }
                SmallSegmented {
                    objectName: "timeMode"
                    anchors.right: parent.right
                    options: ["Distance", "Delay"]
                    current: Speakers.distanceMode ? 0 : 1
                    onPicked: (index) => Speakers.distanceMode = index === 0
                }
            }
            Item { width: 1; height: 8 }

            Item {
                id: table
                width: parent.width
                height: 36 + Speakers.count * 48
                // Column positions from the board: left edges, right edges and centres.
                readonly property real codeX: 14
                readonly property real nameX: 60
                readonly property real levelRight: 454
                readonly property real distanceRight: 601
                readonly property real delayRight: 748
                readonly property real polarityCentre: 827
                readonly property real testCentre: 939
                readonly property real muteCentre: 1045
                readonly property real scale: width / 1120

                Header { x: table.codeX; text: "Speaker" }
                Header { x: table.levelRight * table.scale - width; text: "Level" }
                Header { x: table.distanceRight * table.scale - width; text: "Distance" }
                Header { x: table.delayRight * table.scale - width; text: "Delay" }
                Header { x: table.polarityCentre * table.scale - width / 2; text: "Polarity" }
                Header { x: table.testCentre * table.scale - width / 2; text: "Test" }
                Header { x: table.muteCentre * table.scale - width / 2; text: "Mute · Solo" }

                Repeater {
                    model: Speakers
                    delegate: Rectangle {
                        id: speakerRow
                        required property int index
                        required property string code
                        required property string name
                        required property real level
                        required property real distance
                        required property real delay
                        required property bool inverted
                        required property bool muted
                        required property bool soloMuted
                        required property bool soloed
                        required property bool playing
                        objectName: "speakerRow_" + code
                        y: 36 + index * 48
                        width: table.width
                        height: 48
                        radius: 10
                        color: hover.hovered || playing ? Theme.surface : "transparent"
                        HoverHandler { id: hover }

                        Text {
                            x: table.codeX
                            anchors.verticalCenter: parent.verticalCenter
                            text: speakerRow.code
                            font.family: Theme.font
                            font.pixelSize: 12
                            font.weight: Font.DemiBold
                            color: speakerRow.playing ? Theme.accent : Theme.muted
                        }
                        Text {
                            x: table.nameX
                            anchors.verticalCenter: parent.verticalCenter
                            text: speakerRow.name
                            font.family: Theme.font
                            font.pixelSize: 13
                            color: Theme.text
                        }
                        ValueBox {
                            objectName: "level"
                            x: table.levelRight * table.scale - width
                            anchors.verticalCenter: parent.verticalCenter
                            text: Theme.signed(speakerRow.level, 1) + " dB"
                            unit: EqSession.Decibels
                            onSubmitted: (v) => Speakers.setLevel(speakerRow.index, v)
                        }
                        ValueBox {
                            objectName: "distance"
                            visible: Speakers.distanceMode
                            x: table.distanceRight * table.scale - width
                            anchors.verticalCenter: parent.verticalCenter
                            text: speakerRow.distance.toFixed(2) + " m"
                            unit: EqSession.Metres
                            onSubmitted: (v) => Speakers.setDistance(speakerRow.index, v)
                        }
                        Text {
                            visible: !Speakers.distanceMode
                            x: table.distanceRight * table.scale - width - 8
                            anchors.verticalCenter: parent.verticalCenter
                            text: speakerRow.distance.toFixed(2) + " m"
                            font.family: Theme.font
                            font.pixelSize: 13
                            color: Theme.muted
                        }
                        ValueBox {
                            objectName: "delay"
                            visible: !Speakers.distanceMode
                            x: table.delayRight * table.scale - width
                            anchors.verticalCenter: parent.verticalCenter
                            text: speakerRow.delay.toFixed(2) + " ms"
                            unit: EqSession.Milliseconds
                            onSubmitted: (v) => Speakers.setDelay(speakerRow.index, v)
                        }
                        Text {
                            visible: Speakers.distanceMode
                            x: table.delayRight * table.scale - width - 8
                            anchors.verticalCenter: parent.verticalCenter
                            text: speakerRow.delay.toFixed(2) + " ms"
                            font.family: Theme.font
                            font.pixelSize: 13
                            color: Theme.muted
                        }
                        // Polarity: Normal, or Inverted on the accent.
                        Item {
                            objectName: "polarity"
                            x: table.polarityCentre * table.scale - width / 2
                            anchors.verticalCenter: parent.verticalCenter
                            width: Math.max(64, invertedPill.width)
                            height: 26
                            Rectangle {
                                id: invertedPill
                                visible: speakerRow.inverted
                                anchors.centerIn: parent
                                width: invertedText.implicitWidth + 18
                                height: 22
                                radius: 6
                                color: Theme.accent
                                Text {
                                    id: invertedText
                                    anchors.centerIn: parent
                                    text: "Inverted"
                                    font.family: Theme.font
                                    font.pixelSize: 11
                                    font.weight: Font.Medium
                                    color: Theme.textOnAccent
                                }
                            }
                            Text {
                                visible: !speakerRow.inverted
                                anchors.centerIn: parent
                                text: "Normal"
                                font.family: Theme.font
                                font.pixelSize: 13
                                color: Theme.muted
                            }
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: Speakers.setInverted(speakerRow.index, !speakerRow.inverted)
                            }
                        }
                        // The test tone, one speaker at a time, while test tones are on.
                        Rectangle {
                            objectName: "tone"
                            x: table.testCentre * table.scale - width / 2
                            anchors.verticalCenter: parent.verticalCenter
                            width: 28
                            height: 28
                            radius: 14
                            color: speakerRow.playing ? Theme.accent : "transparent"
                            Icon {
                                anchors.centerIn: parent
                                anchors.horizontalCenterOffset: speakerRow.playing ? 0 : 1
                                name: speakerRow.playing ? "pause" : "play"
                                size: 13
                                colour: speakerRow.playing ? Theme.textOnAccent : Theme.muted
                            }
                            MouseArea {
                                anchors.fill: parent
                                enabled: root.tones
                                cursorShape: Qt.PointingHandCursor
                                onClicked: Speakers.toggleTone(speakerRow.index)
                            }
                        }
                        Row {
                            x: table.muteCentre * table.scale - width / 2
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 6
                            Rectangle {
                                objectName: "mute"
                                readonly property bool lit: speakerRow.muted || speakerRow.soloMuted
                                width: 26
                                height: 22
                                radius: 5
                                color: lit ? Theme.danger : Theme.surface
                                Text {
                                    anchors.centerIn: parent
                                    text: "M"
                                    font.family: Theme.font
                                    font.pixelSize: 11
                                    font.weight: Font.DemiBold
                                    color: parent.lit ? Theme.textOnAccent : Theme.muted
                                }
                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: Speakers.setMuted(speakerRow.index, !speakerRow.muted)
                                }
                            }
                            Rectangle {
                                objectName: "solo"
                                width: 26
                                height: 22
                                radius: 5
                                color: speakerRow.soloed ? Theme.warning : Theme.surface
                                Text {
                                    anchors.centerIn: parent
                                    text: "S"
                                    font.family: Theme.font
                                    font.pixelSize: 11
                                    font.weight: Font.DemiBold
                                    color: speakerRow.soloed ? Theme.textOnAccent : Theme.muted
                                }
                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: Speakers.toggleSolo(speakerRow.index)
                                }
                            }
                        }
                    }
                }
            }

            // Speaker groups, bass management, routing.
            Item { width: 1; height: 22 }
            Row {
                id: cards
                width: parent.width
                spacing: 16
                readonly property real cardWidth: (width - 32) / 3

                Card {
                    objectName: "groupsCard"
                    width: cards.cardWidth
                    title: "Speaker groups"
                    Repeater {
                        model: Speakers.groups
                        delegate: Rectangle {
                            id: groupRow
                            required property var modelData
                            objectName: "group_" + modelData.name
                            x: -10
                            width: parent.width + 20
                            height: 30
                            radius: 6
                            color: groupHover.hovered ? Theme.surface : "transparent"
                            HoverHandler { id: groupHover }
                            Text {
                                x: 10
                                anchors.verticalCenter: parent.verticalCenter
                                text: groupRow.modelData.name
                                font.family: Theme.font
                                font.pixelSize: 13
                                font.weight: Font.DemiBold
                                color: Theme.text
                            }
                            Text {
                                anchors.right: parent.right
                                anchors.rightMargin: removeGroup.visible ? 36 : 10
                                anchors.verticalCenter: parent.verticalCenter
                                text: groupRow.modelData.codes
                                font.family: Theme.font
                                font.pixelSize: 12
                                color: Theme.muted
                            }
                            // A group the user made can be removed.
                            Item {
                                id: removeGroup
                                objectName: "removeGroup"
                                visible: !groupRow.modelData.builtin && groupHover.hovered
                                anchors.right: parent.right
                                anchors.rightMargin: 6
                                anchors.verticalCenter: parent.verticalCenter
                                width: 24
                                height: 24
                                Icon { name: "trash"; size: 15; anchors.centerIn: parent }
                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: Speakers.removeGroup(groupRow.modelData.name)
                                }
                            }
                        }
                    }
                    Item {
                        objectName: "newGroup"
                        width: newGroupRow.implicitWidth
                        height: 32
                        Row {
                            id: newGroupRow
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 8
                            Icon { name: "plus"; size: 14; anchors.verticalCenter: parent.verticalCenter }
                            Text {
                                text: "New group"
                                font.family: Theme.font
                                font.pixelSize: 13
                                color: Theme.muted
                            }
                        }
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: UiState.openDialog(newGroupDialog, {})
                        }
                    }
                }

                Card {
                    objectName: "bassCard"
                    width: cards.cardWidth
                    title: "Bass management"
                    CardRow {
                        label: "Crossover"
                        ValueBox {
                            objectName: "crossover"
                            text: Math.round(Speakers.crossoverHz) + " Hz"
                            unit: EqSession.Hertz
                            onSubmitted: (v) => Speakers.crossoverHz = v
                        }
                    }
                    Label { text: "Small speakers"; topPadding: 6; bottomPadding: 6 }
                    Flow {
                        width: parent.width
                        spacing: 4
                        Repeater {
                            model: Speakers
                            delegate: Rectangle {
                                id: smallChip
                                required property int index
                                required property string code
                                required property bool small
                                required property bool lfe
                                objectName: "small_" + code
                                visible: !lfe
                                width: Math.max(30, smallText.implicitWidth + 14)
                                height: 24
                                radius: 6
                                color: small ? Theme.accent : Theme.track
                                Text {
                                    id: smallText
                                    anchors.centerIn: parent
                                    text: smallChip.code
                                    font.family: Theme.font
                                    font.pixelSize: 11
                                    font.weight: Font.DemiBold
                                    color: smallChip.small ? Theme.textOnAccent : Theme.muted
                                }
                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: Speakers.setSmall(smallChip.index, !smallChip.small)
                                }
                            }
                        }
                    }
                    Item { width: 1; height: 6 }
                    CardRow {
                        label: "LFE low-pass"
                        ValueBox {
                            objectName: "lfeLowpass"
                            text: Math.round(Speakers.lfeLowpassHz) + " Hz"
                            unit: EqSession.Hertz
                            onSubmitted: (v) => Speakers.lfeLowpassHz = v
                        }
                    }
                }

                Card {
                    objectName: "routingCard"
                    width: cards.cardWidth
                    title: "Routing"
                    // Test tones play with routing off; the controls wait until they end.
                    CardRow {
                        label: "Upmix stereo"
                        opacity: root.tones ? 0.45 : 1
                        SmallSegmented {
                            objectName: "upmix"
                            enabled: !root.tones
                            options: ["Off", "All", "No centre"]
                            current: root.tones ? 0 : Speakers.upmix
                            onPicked: (index) => Speakers.upmix = index
                        }
                    }
                    CardRow {
                        label: "Swap front and rear"
                        opacity: root.tones ? 0.45 : 1
                        Toggle {
                            objectName: "swapFrontRear"
                            enabled: !root.tones
                            checked: !root.tones && Speakers.swapFrontRear
                            onToggled: (on) => Speakers.swapFrontRear = on
                        }
                    }
                    CardRow {
                        label: "Swap left and right"
                        opacity: root.tones ? 0.45 : 1
                        Toggle {
                            objectName: "swapLeftRight"
                            enabled: !root.tones
                            checked: !root.tones && Speakers.swapLeftRight
                            onToggled: (on) => Speakers.swapLeftRight = on
                        }
                    }
                    CardRow {
                        label: "Lip sync delay"
                        opacity: root.tones ? 0.45 : 1
                        ValueBox {
                            objectName: "lipSync"
                            enabled: !root.tones
                            field.editable: !root.tones
                            text: Math.round(Speakers.lipSyncMs) + " ms"
                            unit: EqSession.Milliseconds
                            onSubmitted: (v) => Speakers.lipSyncMs = v
                        }
                    }
                }
            }
        }
    }
}
