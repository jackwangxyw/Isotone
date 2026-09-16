import QtQuick
import Isotone

// Import (ImportDialog board): the curve of the file as it reads for the chosen
// output, Preamp, Filters, Skipped lines, the lines skipped, Name and For.
// Import creates the preset and loads it on that output; it is greyed when no
// filter imports. PresetActions.showImport opens it with `preview`.
DialogFrame {
    id: root
    property ImportPreview preview
    // Every output unless one is picked: a preset is not a device's (owner,
    // 2026-09-15).
    readonly property var choices: [{ guid: "", name: "All outputs" }].concat(Presets.outputChoices())
    readonly property string outputName: {
        for (const c of choices)
            if (preview && c.guid === preview.outputGuid) return c.name
        return ""
    }

    cardWidth: 640
    onClosed: if (preview) preview.destroy()

    component Stat: Column {
        property string label
        property string value
        property color colour: Theme.text
        spacing: 2
        Text {
            text: parent.label
            font.family: Theme.font
            font.pixelSize: 12
            color: Theme.muted
        }
        Text {
            text: parent.value
            font.family: Theme.font
            font.pixelSize: 13
            font.weight: Font.DemiBold
            color: parent.colour
        }
    }

    function doImport() {
        if (!preview || !preview.usable || nameField.text.trim() === "") return
        const run = () => {
            Presets.importPreset(preview, nameField.text.trim())
            root.close()
        }
        // Loaded on the current output (which is what "All outputs" does), it
        // replaces unsaved changes: ask first.
        const current = choices.find((c) => c.current)
        if (!current || preview.outputGuid === "" || preview.outputGuid === current.guid) {
            root.visible = false
            PresetActions.confirmUnsaved(false, run, () => { root.visible = true })
        } else {
            run()
        }
    }

    Item {
        width: parent.width
        height: 26
        Text {
            anchors.verticalCenter: parent.verticalCenter
            text: "Import"
            font.family: Theme.font
            font.pixelSize: 18
            font.weight: Font.DemiBold
            font.letterSpacing: -0.18
            color: Theme.text
        }
        Row {
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            spacing: 8
            Icon { name: "file"; size: 15; anchors.verticalCenter: parent.verticalCenter }
            Text {
                objectName: "importFileName"
                text: root.preview ? root.preview.fileName : ""
                font.family: Theme.font
                font.pixelSize: 13
                color: Theme.muted
                anchors.verticalCenter: parent.verticalCenter
            }
        }
    }

    Item { width: 1; height: 18 }
    Rectangle {
        width: parent.width
        height: 210
        radius: 10
        color: Theme.plot
        CurvePreview {
            objectName: "importCurve"
            anchors.fill: parent
            anchors.margins: 8
            preview: root.preview
            accent: Theme.accent
            gridMajor: Theme.gridMajor
            gridMinor: Theme.gridMinor
            zeroLine: Theme.zero
            labelColour: Theme.muted
            bell: Theme.bell
            fillEdgeAlpha: Theme.fillEdgeAlpha
            fillMidAlpha: Theme.fillMidAlpha
            fontFamily: Theme.font
        }
    }

    Item { width: 1; height: 18 }
    Row {
        spacing: 36
        Stat {
            objectName: "importPreamp"
            label: "Preamp"
            value: root.preview ? Theme.signed(root.preview.preampDb, 1) + " dB" : ""
        }
        Stat {
            objectName: "importFilters"
            label: "Filters"
            value: root.preview ? String(root.preview.filterCount) : ""
        }
        Stat {
            objectName: "importFit"
            visible: root.preview !== null && root.preview.curvePoints > 0
            label: "Curve fit"
            value: root.preview ? root.preview.curvePoints + " points, ±"
                                  + root.preview.fitWorstDb.toFixed(1) + " dB" : ""
        }
        Stat {
            objectName: "importSkipped"
            label: "Skipped"
            readonly property int lines: root.preview ? root.preview.skipped.length : 0
            value: lines + (lines === 1 ? " line" : " lines")
            colour: root.preview && !root.preview.usable ? Theme.warning : Theme.text
        }
    }

    Item { width: 1; height: 14; visible: skippedList.visible }
    Rectangle {
        id: skippedList
        objectName: "importSkippedList"
        visible: root.preview !== null && root.preview.skipped.length > 0
        width: parent.width
        height: Math.min(skippedLines.implicitHeight, 26 * 6) + 16
        radius: 8
        color: Theme.plot
        Flickable {
            x: 14
            y: 8
            width: parent.width - 28
            height: parent.height - 16
            contentHeight: skippedLines.implicitHeight
            interactive: contentHeight > height
            clip: true
            Column {
                id: skippedLines
                width: parent.width
                Repeater {
                    model: root.preview ? root.preview.skipped : []
                    delegate: Item {
                        required property var modelData
                        width: skippedLines.width
                        height: 26
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: "Line " + modelData.line
                            font.family: Theme.font
                            font.pixelSize: 12
                            color: Theme.muted
                        }
                        Text {
                            x: 64
                            width: parent.width - 64
                            anchors.verticalCenter: parent.verticalCenter
                            text: modelData.text
                            elide: Text.ElideRight
                            font.family: Theme.font
                            font.pixelSize: 12
                            color: Theme.text
                        }
                    }
                }
            }
        }
    }

    Item { width: 1; height: 18 }
    Row {
        width: parent.width
        spacing: 14
        z: 2
        TextBox {
            id: nameField
            objectName: "importName"
            width: (parent.width - 14) / 2
            label: "Name"
            text: root.preview ? root.preview.suggestedName : ""
            onAccepted: root.doImport()
        }
        Column {
            width: (parent.width - 14) / 2
            spacing: 6
            Text {
                text: "For"
                font.family: Theme.font
                font.pixelSize: 12
                color: Theme.muted
            }
            Rectangle {
                id: assignBox
                objectName: "importAssign"
                width: parent.width
                height: 36
                radius: 8
                color: Theme.surface
                Text {
                    x: 12
                    width: parent.width - 44
                    anchors.verticalCenter: parent.verticalCenter
                    text: root.outputName
                    elide: Text.ElideRight
                    font.family: Theme.font
                    font.pixelSize: 14
                    color: Theme.text
                }
                Icon {
                    anchors.right: parent.right
                    anchors.rightMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    name: "chevron"
                    size: 15
                }
                MouseArea {
                    anchors.fill: parent
                    enabled: root.choices.length > 1
                    cursorShape: Qt.PointingHandCursor
                    onClicked: assignList.visible = !assignList.visible
                }
                // The outputs, above the box (the actions are below).
                Rectangle {
                    id: assignList
                    objectName: "importAssignList"
                    visible: false
                    y: -height - 4
                    width: parent.width
                    height: assignRows.implicitHeight + 8
                    radius: 10
                    color: Theme.pop
                    border.color: Theme.border
                    Column {
                        id: assignRows
                        x: 4
                        y: 4
                        width: parent.width - 8
                        Repeater {
                            model: root.choices
                            delegate: Rectangle {
                                required property var modelData
                                width: assignRows.width
                                height: 34
                                radius: 6
                                color: choiceArea.containsMouse ? Theme.surface : "transparent"
                                Text {
                                    x: 10
                                    width: parent.width - 20
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: modelData.name
                                    elide: Text.ElideRight
                                    font.family: Theme.font
                                    font.pixelSize: 13
                                    color: Theme.text
                                }
                                MouseArea {
                                    id: choiceArea
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: {
                                        root.preview.outputGuid = modelData.guid
                                        assignList.visible = false
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    actions: [
        Button {
            objectName: "importCancel"
            text: "Cancel"
            onClicked: { root.rejected(); root.close() }
        },
        Button {
            objectName: "importConfirm"
            text: "Import"
            kind: "primary"
            active: root.preview !== null && root.preview.usable && nameField.text.trim() !== ""
            onClicked: root.doImport()
        }
    ]
}
