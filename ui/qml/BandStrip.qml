import QtQuick
import Isotone

// The band strip: header with the order control, the scrolling columns with a
// right-edge fade and a thumb, Add band, and the Channels panel, pinned.
Item {
    id: root
    signal menuRequested(int row, real x, real above, real below)

    Row {
        x: 32
        y: 12
        spacing: 12
        height: parent.height - 12

        Column {
            id: scroller
            width: root.width - 64 - 92 - (speakersPanel.visible ? speakersPanel.width : panel.width) - 24
            spacing: 6

            Item {
                width: parent.width
                height: 32
                Row {
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 14
                    Text { text: "Bands"; font.family: Theme.font; font.pixelSize: 13; font.weight: Font.DemiBold; color: Theme.text }
                    Text { text: EqSession.count; font.family: Theme.font; font.pixelSize: 13; color: Theme.muted }
                }
                Segmented {
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    options: ["Manual", "By frequency"]
                    current: EqSession.byFrequency ? 1 : 0
                    onPicked: (index) => EqSession.byFrequency = index === 1
                }
            }

            Item {
                width: parent.width
                height: flick.height
                Flickable {
                    id: flick
                    objectName: "bandFlick"
                    width: parent.width
                    height: columns.height
                    contentWidth: columns.width
                    clip: true
                    boundsBehavior: Flickable.StopAtBounds
                    flickableDirection: Flickable.HorizontalFlick
                    // Dragging the columns scrolls; a gain slider keeps its own
                    // drag (preventStealing in GainSlider).
                    interactive: contentWidth > width
                    Row {
                        id: columns
                        spacing: 6
                        Repeater {
                            model: EqSession
                            delegate: BandColumn {
                                onMenuRequested: (x, above, below) => root.menuRequested(index, x, above, below)
                            }
                        }
                    }
                    // Where a wheel is taking the columns; contentX eases there.
                    property real scrollTarget: 0
                    NumberAnimation {
                        id: glide
                        target: flick
                        property: "contentX"
                        duration: 260
                        easing.type: Easing.OutCubic
                    }
                    function clampX(x) { return Math.max(0, Math.min(contentWidth - width, x)) }
                    // A press on the columns or the thumb takes over from a glide.
                    function scrollTo(x) {
                        glide.stop()
                        scrollTarget = clampX(x)
                        contentX = scrollTarget
                    }
                    // A wheel adds to the target and restarts the ease from where
                    // the columns are, so many small deltas glide instead of step.
                    function scrollBy(dx) {
                        scrollTarget = clampX((glide.running ? scrollTarget : contentX) + dx)
                        glide.stop()
                        glide.from = contentX
                        glide.to = scrollTarget
                        glide.start()
                    }
                    onDragStarted: glide.stop()
                }
                // Both wheels scroll sideways. On top of the columns, taking no
                // buttons, so presses and drags still reach them.
                MouseArea {
                    anchors.fill: flick
                    acceptedButtons: Qt.NoButton
                    // A high-resolution wheel (the MX Master's side wheel) sends
                    // many small deltas: pixels where the event has them, else one
                    // notch (120) moves a column and its spacing.
                    onWheel: (wheel) => {
                        const pixels = wheel.pixelDelta.x !== 0 ? wheel.pixelDelta.x : wheel.pixelDelta.y
                        const angle = wheel.angleDelta.x !== 0 ? wheel.angleDelta.x : wheel.angleDelta.y
                        flick.scrollBy(-(pixels !== 0 ? pixels : angle / 120 * 118))
                    }
                }
                // The fade marks the side with more bands out of view.
                Rectangle {
                    visible: flick.contentX + flick.width < flick.contentWidth - 1
                    anchors.right: parent.right
                    width: 72
                    height: flick.height
                    gradient: Gradient {
                        orientation: Gradient.Horizontal
                        GradientStop { position: 0; color: Qt.rgba(Theme.background.r, Theme.background.g, Theme.background.b, 0) }
                        GradientStop { position: 1; color: Theme.background }
                    }
                }
            }

            Rectangle {
                id: track
                x: 4
                width: parent.width - 8
                height: 4
                radius: 2
                color: Theme.track
                visible: flick.contentWidth > flick.width
                Rectangle {
                    id: thumb
                    objectName: "bandThumb"
                    height: 4
                    radius: 2
                    color: Theme.zero
                    width: parent.width * flick.width / Math.max(1, flick.contentWidth)
                    x: parent.width * flick.contentX / Math.max(1, flick.contentWidth)
                }
                // Drag the thumb, or press the track to jump there.
                MouseArea {
                    anchors.fill: parent
                    anchors.topMargin: -8
                    anchors.bottomMargin: -8
                    property real grab: 0
                    onPressed: (mouse) => {
                        const onThumb = mouse.x >= thumb.x && mouse.x <= thumb.x + thumb.width
                        if (onThumb) flick.scrollTo(flick.contentX)   // ends a glide, keeping the columns where they are
                        else flick.scrollTo((mouse.x - thumb.width / 2) * flick.contentWidth / track.width)
                        grab = mouse.x - thumb.x
                    }
                    onPositionChanged: (mouse) => {
                        if (!pressed) return
                        flick.scrollTo((mouse.x - grab) * flick.contentWidth / track.width)
                    }
                }
            }
        }

        // Add band.
        Item {
            id: addBand
            objectName: "addBand"
            width: 92
            height: parent.height
            opacity: EqSession.canAddBand ? 1 : 0.35
            MouseArea {
                anchors.fill: parent
                enabled: EqSession.canAddBand
                cursorShape: Qt.PointingHandCursor
                onClicked: EqSession.addBand(1000, 0)
            }
            Rectangle { width: 1; height: parent.height; color: Theme.gridMinor }
            // Centred between this line and the panel's: the Row's spacing after
            // the item is part of the same gap.
            Column {
                objectName: "addBandContent"
                anchors.centerIn: parent
                anchors.horizontalCenterOffset: addBand.parent.spacing / 2
                spacing: 10
                Rectangle {
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: 40
                    height: 40
                    radius: 20
                    color: "transparent"
                    border.color: Theme.gridMajor
                    Icon { name: "plus"; size: 18; anchors.centerIn: parent }
                }
                Text { text: "Add band"; font.family: Theme.font; font.pixelSize: 12; color: Theme.muted }
            }
        }

        // The right-hand panel: Channels on stereo outputs.
        ChannelsPanel {
            id: panel
            height: parent.height
            visible: !speakersPanel.visible
        }
        // Speakers on outputs with more than two channels.
        SpeakersPanel {
            id: speakersPanel
            height: parent.height
            visible: EqSession.outputChannels > 2
        }
    }
}
