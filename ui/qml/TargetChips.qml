import QtQuick
import Isotone

// The band popover's Target row on a surround output: group chips (All, Front,
// Surround, Sub and the output's own groups) and a chip per speaker. A group
// sets the band's channels to it; a speaker adds itself or leaves (a band keeps
// at least one). A group is lit when the band is exactly on it, a speaker when
// the band is on it.
Item {
    id: root
    property int row: -1
    property int mask: 0   // the band's ChannelMask, 0 for every channel
    readonly property int allMask: (1 << Speakers.channels) - 1
    readonly property int effective: mask === 0 ? allMask : mask & allMask

    height: chips.height + 14

    function pickGroup(groupMask) { EqSession.setChannelMask(root.row, groupMask) }
    function toggleSpeaker(channel) {
        const next = root.effective ^ (1 << channel)
        if (next !== 0) EqSession.setChannelMask(root.row, next)
    }

    // Right-aligned lines of chips that fit `width`.
    FontMetrics { id: metrics; font.family: Theme.font; font.pixelSize: 11; font.weight: Font.DemiBold }
    function chipWidth(label) { return Math.max(28, Math.ceil(metrics.advanceWidth(label)) + 14) }
    function lines(items, width) {
        const out = []
        let line = [], used = 0
        for (const item of items) {
            const w = chipWidth(item.label)
            if (line.length > 0 && used + 3 + w > width) {
                out.push(line)
                line = []
                used = 0
            }
            used += (line.length > 0 ? 3 : 0) + w
            line.push(item)
        }
        if (line.length > 0) out.push(line)
        return out
    }

    Text {
        id: label
        x: 8
        y: 8 + 4
        text: "Target"
        font.family: Theme.font
        font.pixelSize: 13
        color: Theme.text
    }

    Column {
        id: chips
        anchors.right: parent.right
        anchors.rightMargin: 8
        y: 8
        width: available
        spacing: 6
        readonly property real available: root.width - 8 - (label.x + label.implicitWidth + 10)

        Repeater {
            model: root.lines(Speakers.groups.map(g => ({label: g.name, mask: g.mask, group: true})), chips.available)
                .concat(root.lines(Speakers.speakers.map(s => ({label: s.code, channel: s.channel, group: false})), chips.available))
            delegate: Row {
                required property var modelData
                anchors.right: parent.right
                spacing: 3
                Repeater {
                    model: parent.modelData
                    delegate: Rectangle {
                        id: chip
                        required property var modelData
                        readonly property bool lit: modelData.group ? root.effective === modelData.mask
                                                                    : (root.effective & (1 << modelData.channel)) !== 0
                        objectName: (modelData.group ? "targetGroup_" : "targetSpeaker_") + modelData.label
                        width: root.chipWidth(modelData.label)
                        height: 24
                        radius: 6
                        color: lit ? Theme.accent : Theme.track
                        Text {
                            anchors.centerIn: parent
                            text: chip.modelData.label
                            font.family: Theme.font
                            font.pixelSize: 11
                            font.weight: Font.DemiBold
                            color: chip.lit ? Theme.textOnAccent : Theme.muted
                        }
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: chip.modelData.group ? root.pickGroup(chip.modelData.mask) : root.toggleSpeaker(chip.modelData.channel)
                        }
                    }
                }
            }
        }
    }
}
