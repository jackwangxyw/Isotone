import QtQuick
import Isotone

// Settings, Appearance (Appearance board, and the prototype's "Custom theme and
// accent"): theme, accent swatches and a custom accent, band colours, the custom
// theme's five colours, and a live preview on the right.
Item {
    id: root
    implicitHeight: Math.max(leftColumn.implicitHeight, previewColumn.y + previewColumn.implicitHeight)

    readonly property var themes: ["system", "dark", "light", "custom"]
    // Custom starts from Dark (ui-spec.md, "Themes").
    readonly property var customColours: [["background", "Background", "#121519"], ["surface", "Surface", "#0e1115"],
                                          ["text", "Text", "#e8ebf1"], ["grid", "Grid", "#26292e"],
                                          ["spectrum", "Spectrum", "#7e8792"]]

    function customValue(key, fallback) {
        AppSettings.themeRevision   // re-read when a colour changes, as Theme.customColour
        const v = AppSettings.value("appearance/custom/" + key, "")
        return v !== "" ? v : fallback
    }

    // What the colour popover is editing: "accent", or a custom colour's key.
    property string editing: ""
    function openPicker(target, colour, anchor, dx, dy) {
        editing = target
        picker.load(colour)
        const p = anchor.mapToItem(colourPopover, dx, dy)
        colourPopover.openAt(p.x, p.y)
    }

    Column {
        id: leftColumn
        width: 480

        SettingsSection { text: "Theme" }
        Segmented {
            objectName: "theme"
            options: ["System", "Dark", "Light", "Custom"]
            current: root.themes.indexOf(AppSettings.theme)
            onPicked: (index) => AppSettings.theme = root.themes[index]
        }

        SettingsSection { text: "Accent" }
        Row {
            id: accentRow
            leftPadding: 4   // room for the selected swatch's ring inside the page's clip
            spacing: 12
            Repeater {
                model: Theme.accents
                delegate: Item {
                    id: swatch
                    required property string modelData
                    required property int index
                    readonly property bool on: AppSettings.accent === index
                    objectName: "accent" + index
                    width: 34
                    height: 34
                    Rectangle {
                        visible: swatch.on
                        anchors.centerIn: parent
                        width: 42
                        height: 42
                        radius: 21
                        color: "transparent"
                        border.width: 2
                        border.color: swatch.modelData
                    }
                    Rectangle {
                        anchors.fill: parent
                        radius: 17
                        color: swatch.modelData
                        Icon { visible: swatch.on; name: "check"; size: 16; strokeWidth: 2; colour: Theme.textOnAccent; anchors.centerIn: parent }
                    }
                    MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: AppSettings.accent = swatch.index }
                }
            }
            Rectangle {
                id: customAccent
                objectName: "customAccent"
                readonly property bool on: AppSettings.accent < 0
                width: customRow.implicitWidth + 28
                height: 34
                radius: 17
                color: "transparent"
                border.width: on ? 2 : 1
                border.color: on ? Theme.text : Theme.gridMajor
                Row {
                    id: customRow
                    anchors.centerIn: parent
                    spacing: 8
                    Rectangle {
                        visible: customAccent.on
                        width: 12
                        height: 12
                        radius: 6
                        color: AppSettings.customAccent
                        anchors.verticalCenter: parent.verticalCenter
                    }
                    Text {
                        text: "Custom…"
                        font.family: Theme.font
                        font.pixelSize: 13
                        font.weight: Font.Medium
                        color: Theme.text
                        anchors.verticalCenter: parent.verticalCenter
                    }
                }
                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        if (colourPopover.open && root.editing === "accent") { colourPopover.close(); return }
                        root.openPicker("accent", AppSettings.customAccent, customAccent, 0, customAccent.height + 10)
                    }
                }
            }
        }

        SettingsSection { text: "Band colours" }
        Row {
            spacing: 14
            Repeater {
                model: [["accent", "Accent"], ["band", "Per band"]]
                delegate: Item {
                    id: card
                    required property var modelData
                    readonly property bool on: AppSettings.bandColours === modelData[0]
                    objectName: "bandColours_" + modelData[0]
                    width: 150
                    height: cardColumn.height
                    Column {
                        id: cardColumn
                        spacing: 8
                        Rectangle {
                            width: 150
                            height: 56
                            radius: 10
                            color: "transparent"
                            border.width: card.on ? 2 : 1
                            border.color: card.on ? Theme.text : Theme.gridMajor
                            Row {
                                anchors.centerIn: parent
                                spacing: 8
                                Repeater {
                                    model: 5
                                    delegate: Rectangle {
                                        required property int index
                                        width: 12
                                        height: 12
                                        radius: 6
                                        color: card.modelData[0] === "accent" ? Theme.accent : Theme.bandColours[index]
                                    }
                                }
                            }
                        }
                        Text {
                            text: card.modelData[1]
                            font.family: Theme.font
                            font.pixelSize: 13
                            color: Theme.text
                        }
                    }
                    // The whole card, name included.
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: AppSettings.bandColours = card.modelData[0]
                    }
                }
            }
        }

        Item {
            visible: AppSettings.theme === "custom"
            width: parent.width
            height: visible ? customHeader.height : 0
            SettingsSection { id: customHeader; text: "Custom colours" }
            Text {
                objectName: "startFromDark"
                anchors.right: parent.right
                anchors.baseline: customHeader.baseline
                text: "Start from Dark"
                font.family: Theme.font
                font.pixelSize: 13
                color: startArea.containsMouse ? Theme.text : Theme.muted
                MouseArea {
                    id: startArea
                    anchors.fill: parent
                    anchors.margins: -4
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        for (const c of root.customColours) AppSettings.setValue("appearance/custom/" + c[0], c[2])
                    }
                }
            }
        }
        Repeater {
            model: AppSettings.theme === "custom" ? root.customColours : []
            // A .srow with the colour's swatch before its name.
            delegate: Item {
                id: colourRow
                required property var modelData
                readonly property string value: root.customValue(modelData[0], modelData[2])
                objectName: "custom_" + modelData[0]
                width: leftColumn.width
                height: 45
                Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.gridMinor }
                Row {
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 12
                    Rectangle {
                        id: colourSwatch
                        width: 26
                        height: 26
                        radius: 6
                        color: colourRow.value
                        border.width: 1
                        border.color: Theme.gridMajor
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.openPicker(colourRow.modelData[0], colourRow.value, colourSwatch, 0, colourSwatch.height + 8)
                        }
                    }
                    Text {
                        anchors.verticalCenter: parent.verticalCenter
                        text: colourRow.modelData[1]
                        font.family: Theme.font
                        font.pixelSize: 14
                        color: Theme.text
                    }
                }
                SettingBox {
                    objectName: "hex"
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    text: colourRow.value.toUpperCase()
                    accept: (t) => { const c = picker.parseHex(t); return c === undefined ? undefined : c.toString() }
                    onSubmitted: (v) => AppSettings.setValue("appearance/custom/" + colourRow.modelData[0], v)
                }
            }
        }
    }

    Column {
        id: previewColumn
        x: leftColumn.width + 40
        y: 30
        width: Math.max(240, root.width - x)
        spacing: 8
        Text {
            text: "Preview"
            font.family: Theme.font
            font.pixelSize: 13
            color: Theme.muted
        }
        PreviewGraph {
            objectName: "preview"
            width: parent.width
            height: 300
        }
    }

    Popover {
        id: colourPopover
        objectName: "colourPopover"
        parent: UiState.overlay
        panelWidth: 250
        ColourPicker {
            id: picker
            objectName: "colourPicker"
            width: parent.width
            onPicked: (c) => {
                if (root.editing === "accent") {
                    AppSettings.customAccent = c.toString()
                    AppSettings.accent = -1
                } else if (root.editing !== "") {
                    AppSettings.setValue("appearance/custom/" + root.editing, c.toString())
                }
            }
        }
    }
}
