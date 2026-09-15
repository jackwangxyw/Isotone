import QtQuick
import Isotone

// Settings, Shortcuts (SettingsShortcuts board and the prototype's rebinding
// states): each action's keys and, for app-wide ones, Global. Click the keys,
// press new ones; keys another action holds show that action, with Replace when
// it can be rebound. Escape, or a press elsewhere, cancels. Global keys another
// app holds show in red on their row.
Item {
    implicitHeight: root.implicitHeight

    Column {
        id: root
        width: 760

        // The action waiting for keys, and keys pressed that another action holds.
        property string rebinding: ""
        property string pending: ""
        property string conflictWith: ""

        function start(id) {
            rebinding = id
            pending = ""
            conflictWith = ""
            ShortcutRegistry.capturing = true
        }
        function cancel() {
            rebinding = ""
            pending = ""
            conflictWith = ""
            ShortcutRegistry.capturing = false
        }
        function pressed(key, modifiers) {
            if (key === Qt.Key_Escape) { cancel(); return }
            const sequence = ShortcutRegistry.sequenceFor(key, modifiers)
            if (sequence === "") return
            const other = ShortcutRegistry.conflict(rebinding, sequence)
            if (other === "") {
                ShortcutRegistry.rebind(rebinding, sequence)
                cancel()
            } else {
                pending = sequence
                conflictWith = other
            }
        }
        Component.onDestruction: ShortcutRegistry.capturing = false

        component ShortcutRow: Item {
            id: row
            required property string action
            readonly property bool waiting: root.rebinding === action
            readonly property bool rebindable: ShortcutRegistry.rebindable(action)
            readonly property bool failed: ShortcutRegistry.revision >= 0 && ShortcutRegistry.globalFailed(action)
            objectName: "shortcut_" + action
            width: root.width
            height: 48

            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: ShortcutRegistry.label(row.action)
                font.family: Theme.font
                font.pixelSize: 14
                color: Theme.text
            }

            // The keys column: 220 px, at the table's 1fr 220px 60px.
            FocusScope {
                id: keysCell
                objectName: "keys"
                x: parent.width - 280
                width: 220
                height: parent.height
                Keys.onPressed: (event) => {
                    if (!row.waiting) return
                    event.accepted = true
                    root.pressed(event.key, event.modifiers)
                }
                onActiveFocusChanged: if (!activeFocus && row.waiting) root.cancel()

                KeyCaps {
                    objectName: "caps"
                    visible: !row.waiting
                    anchors.verticalCenter: parent.verticalCenter
                    keys: ShortcutRegistry.revision >= 0 ? ShortcutRegistry.keyCaps(row.action) : []
                    colour: row.failed ? Theme.danger : Theme.text
                    edge: row.failed ? Theme.danger : Theme.gridMajor
                }
                Text {
                    objectName: "inUse"
                    visible: !row.waiting && row.failed
                    x: 110
                    anchors.verticalCenter: parent.verticalCenter
                    text: "In use"
                    font.family: Theme.font
                    font.pixelSize: 12
                    color: Theme.danger
                }
                MouseArea {
                    anchors.fill: parent
                    visible: !row.waiting && row.rebindable
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        root.start(row.action)
                        keysCell.forceActiveFocus()
                    }
                }

                // Waiting for keys.
                Rectangle {
                    objectName: "pressKeys"
                    visible: row.waiting && root.pending === ""
                    anchors.verticalCenter: parent.verticalCenter
                    width: pressText.implicitWidth + 20
                    height: 24
                    radius: 5
                    color: "transparent"
                    border.width: 1.5
                    border.color: Theme.accent
                    Text {
                        id: pressText
                        anchors.centerIn: parent
                        text: "Press keys"
                        font.family: Theme.font
                        font.pixelSize: 12
                        font.weight: Font.DemiBold
                        color: Theme.accent
                    }
                }
                // Keys another action holds.
                Row {
                    objectName: "conflict"
                    visible: row.waiting && root.pending !== ""
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 8
                    KeyCaps {
                        anchors.verticalCenter: parent.verticalCenter
                        keys: root.pending !== "" ? ShortcutRegistry.keyCapsFor(root.pending) : []
                    }
                    Text {
                        objectName: "conflictName"
                        anchors.verticalCenter: parent.verticalCenter
                        text: ShortcutRegistry.label(root.conflictWith)
                        font.family: Theme.font
                        font.pixelSize: 12
                        color: Theme.danger
                    }
                    Button {
                        objectName: "replace"
                        visible: ShortcutRegistry.rebindable(root.conflictWith)
                        anchors.verticalCenter: parent.verticalCenter
                        height: 26
                        text: "Replace"
                        onClicked: {
                            ShortcutRegistry.replace(root.rebinding, root.pending)
                            root.cancel()
                        }
                    }
                }
            }

            Toggle {
                objectName: "global"
                visible: ShortcutRegistry.globalCapable(row.action)
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                checked: ShortcutRegistry.revision >= 0 && ShortcutRegistry.isGlobal(row.action)
                onToggled: (on) => ShortcutRegistry.setGlobal(row.action, on)
            }

            Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.gridMinor }
        }

        // Column heads.
        Item {
            width: root.width
            height: 20 + 16
            Text {
                x: root.width - 280
                y: 20
                text: "Keys"
                font.family: Theme.font
                font.pixelSize: 12
                color: Theme.muted
            }
            Text {
                anchors.right: parent.right
                y: 20
                text: "Global"
                font.family: Theme.font
                font.pixelSize: 12
                color: Theme.muted
            }
        }
        SettingsSection { text: "App"; topPadding: 4 }
        Repeater {
            model: ShortcutRegistry.ids("app")
            delegate: ShortcutRow { required property string modelData; action: modelData }
        }
        SettingsSection { text: "Selected band" }
        Repeater {
            model: ShortcutRegistry.ids("band")
            delegate: ShortcutRow { required property string modelData; action: modelData }
        }
    }
}
