import QtQuick
import QtQuick.Dialogs
import Isotone

// The presets popover (PresetsMenu board), from the preset name: search, the
// presets with a check on the current output's and the outputs assigned each,
// rename, duplicate and delete on the hovered row (rename and delete inline),
// then New, Save, Save as, Import, Export. Main opens it with openAt(x, y), the
// popover's top left in window coordinates.
Popover {
    id: root
    objectName: "presetsMenu"
    panelWidth: 400

    property string search: ""
    property string renaming: ""
    property string confirmingDelete: ""
    // The preset whose output is being picked (the row's speaker icon).
    property string scoping: ""
    readonly property var outputs: Presets.outputChoices()

    onClosed: {
        renaming = ""
        confirmingDelete = ""
        scoping = ""
        searchInput.text = ""
    }

    function matches(name) {
        return search === "" || name.toLowerCase().indexOf(search.toLowerCase()) >= 0
    }
    // Here, not in the row: renaming rebuilds the rows.
    function commitRename(name, to) {
        renaming = ""
        Presets.rename(name, to)
    }
    function scopingThisRow(name) { return root.scoping === name ? "" : name }
    function loadPreset(name) {
        root.close()
        Presets.load(name)   // asks first with unsaved changes (UnsavedPrompt)
    }

    // A 28 px icon button on a row.
    component RowIcon: Rectangle {
        id: rowIcon
        property string icon
        signal clicked()
        width: 28
        height: 28
        radius: 6
        color: iconArea.containsMouse ? Theme.segmentedSelected : "transparent"
        Icon {
            anchors.centerIn: parent
            name: rowIcon.icon
            size: 16
            colour: iconArea.containsMouse ? Theme.text : Theme.muted
        }
        MouseArea {
            id: iconArea
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: rowIcon.clicked()
        }
    }

    FileDialog {
        id: importFile
        title: "Import"
        nameFilters: ["Equalizer APO (*.txt)", "All files (*)"]
        onAccepted: PresetActions.showImport(selectedFile)
    }

    Column {
        width: parent.width

        Rectangle {
            width: parent.width
            height: 34
            radius: 8
            color: Theme.surface
            Icon {
                x: 12
                anchors.verticalCenter: parent.verticalCenter
                name: "search"
                size: 15
            }
            TextInput {
                id: searchInput
                objectName: "presetSearch"
                x: 35
                width: parent.width - 47
                anchors.verticalCenter: parent.verticalCenter
                font.family: Theme.font
                font.pixelSize: 13
                color: Theme.text
                selectionColor: Theme.accent
                selectedTextColor: Theme.textOnAccent
                selectByMouse: true
                clip: true
                onTextChanged: root.search = text
                Text {
                    visible: searchInput.text === ""
                    text: "Search presets"
                    font: searchInput.font
                    color: Theme.muted
                }
            }
        }
        Item { width: 1; height: 6 }

        Flickable {
            id: list
            width: parent.width
            height: Math.min(rows.implicitHeight, 44 * 8)
            contentHeight: rows.implicitHeight
            interactive: contentHeight > height
            clip: true
            boundsBehavior: Flickable.StopAtBounds

            Column {
                id: rows
                width: list.width
                Repeater {
                    model: Presets
                    delegate: Rectangle {
                        id: row
                        required property string name
                        required property string assigned
                        required property string forOutput
                        required property bool current
                        readonly property bool renamingThis: root.renaming === name
                        readonly property bool confirming: root.confirmingDelete === name
                        readonly property bool scopingThis: root.scoping === name
                        readonly property bool showIcons: hover.hovered && !renamingThis && !confirming
                        objectName: "presetRow_" + name
                        visible: root.matches(name)
                        width: rows.width
                        height: row.scopingThis ? 44 + scopeColumn.implicitHeight + 8
                                                : Math.max(44, labels.implicitHeight + 16)
                        radius: 8
                        color: hover.hovered || renamingThis || confirming ? Theme.surface : "transparent"

                        HoverHandler { id: hover }
                        MouseArea {
                            width: parent.width
                            height: 44
                            enabled: !row.renamingThis && !row.confirming && !row.scopingThis
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.loadPreset(row.name)
                        }

                        // Hidden while renaming: that row already shows the save check.
                        Icon {
                            objectName: "presetCurrentCheck"
                            x: 12
                            anchors.verticalCenter: parent.verticalCenter
                            visible: row.current && !row.renamingThis
                            name: "check"
                            size: 16
                            colour: Theme.accent
                        }

                        Column {
                            id: labels
                            x: 38
                            width: (row.showIcons ? icons.x - 8 : row.confirming ? confirm.x - 8 : row.width - 10) - x
                            y: row.scopingThis ? (44 - implicitHeight) / 2 : (row.height - implicitHeight) / 2
                            visible: !row.renamingThis
                            spacing: 1
                            Text {
                                width: parent.width
                                text: row.name
                                elide: Text.ElideRight
                                font.family: Theme.font
                                font.pixelSize: 13
                                font.weight: row.current ? Font.DemiBold : Font.Normal
                                color: Theme.text
                            }
                            Text {
                                width: parent.width
                                visible: row.assigned !== "" && !row.confirming
                                text: "Only " + row.assigned
                                elide: Text.ElideRight
                                font.family: Theme.font
                                font.pixelSize: 12
                                color: Theme.muted
                            }
                        }

                        Row {
                            id: icons
                            anchors.right: parent.right
                            anchors.rightMargin: 10
                            y: (44 - height) / 2
                            spacing: 4
                            visible: row.showIcons
                            RowIcon {
                                objectName: "presetRename"
                                icon: "pen"
                                onClicked: {
                                    root.confirmingDelete = ""
                                    root.renaming = row.name
                                }
                            }
                            RowIcon {
                                objectName: "presetDuplicate"
                                icon: "copy"
                                onClicked: Presets.duplicate(row.name)
                            }
                            RowIcon {
                                objectName: "presetScope"
                                icon: "devices"
                                onClicked: {
                                    root.renaming = ""
                                    root.confirmingDelete = ""
                                    root.scoping = root.scopingThisRow(row.name)
                                }
                            }
                            RowIcon {
                                objectName: "presetDelete"
                                icon: "trash"
                                onClicked: {
                                    root.renaming = ""
                                    root.confirmingDelete = row.name
                                }
                            }
                        }

                        // What the preset is for, picked in the row so nothing is
                        // clipped by the list's own scrolling.
                        Column {
                            id: scopeColumn
                            objectName: "presetScopeList"
                            visible: row.scopingThis
                            x: 30
                            y: 44
                            width: row.width - x - 10
                            Repeater {
                                model: row.scopingThis ? [{ guid: "", name: "All outputs" }].concat(root.outputs) : []
                                delegate: Rectangle {
                                    required property var modelData
                                    width: scopeColumn.width
                                    height: 32
                                    radius: 6
                                    color: scopeArea.containsMouse ? Theme.plot : "transparent"
                                    Icon {
                                        x: 8
                                        anchors.verticalCenter: parent.verticalCenter
                                        visible: modelData.guid === row.forOutput
                                        name: "check"
                                        size: 14
                                        colour: Theme.accent
                                    }
                                    Text {
                                        x: 30
                                        width: parent.width - 40
                                        anchors.verticalCenter: parent.verticalCenter
                                        text: modelData.name
                                        elide: Text.ElideRight
                                        font.family: Theme.font
                                        font.pixelSize: 13
                                        color: Theme.text
                                    }
                                    MouseArea {
                                        id: scopeArea
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: {
                                            Presets.setPresetOutput(row.name, modelData.guid)
                                            root.scoping = ""
                                        }
                                    }
                                }
                            }
                        }

                        // Rename, inline.
                        Loader {
                            x: 38
                            width: row.width - x - 10
                            anchors.verticalCenter: parent.verticalCenter
                            active: row.renamingThis
                            sourceComponent: Row {
                                spacing: 4
                                function commit() { root.commitRename(row.name, renameInput.text) }
                                Rectangle {
                                    width: parent.parent.width - 32
                                    height: 32
                                    radius: 8
                                    color: Theme.surface
                                    border.width: 1.5
                                    border.color: Theme.accent
                                    TextInput {
                                        id: renameInput
                                        objectName: "presetRenameInput"
                                        anchors.fill: parent
                                        anchors.leftMargin: 12
                                        anchors.rightMargin: 12
                                        verticalAlignment: TextInput.AlignVCenter
                                        text: row.name
                                        font.family: Theme.font
                                        font.pixelSize: 14
                                        color: Theme.text
                                        selectionColor: Theme.accent
                                        selectedTextColor: Theme.textOnAccent
                                        selectByMouse: true
                                        clip: true
                                        onAccepted: parent.parent.commit()
                                        Keys.onEscapePressed: root.renaming = ""
                                        Component.onCompleted: { forceActiveFocus(); selectAll() }
                                    }
                                }
                                RowIcon {
                                    objectName: "presetRenameDone"
                                    icon: "check"
                                    anchors.verticalCenter: parent.verticalCenter
                                    onClicked: parent.commit()
                                }
                            }
                        }

                        // Delete, confirmed inline.
                        Row {
                            id: confirm
                            anchors.right: parent.right
                            anchors.rightMargin: 10
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 6
                            visible: row.confirming
                            Button {
                                objectName: "presetDeleteCancel"
                                text: "Cancel"
                                kind: "ghost"
                                implicitHeight: 28
                                onClicked: root.confirmingDelete = ""
                            }
                            Button {
                                objectName: "presetDeleteConfirm"
                                text: "Delete"
                                kind: "danger"
                                implicitHeight: 28
                                onClicked: {
                                    root.confirmingDelete = ""
                                    Presets.remove(row.name)
                                }
                            }
                        }
                    }
                }
            }
        }

        Item { width: 1; height: 8 }
        Rectangle { width: parent.width; height: 1; color: Theme.border }
        Item { width: 1; height: 10 }
        Row {
            spacing: 6
            Button {
                objectName: "presetNew"
                text: "New"
                icon: "plus"
                onClicked: { root.close(); PresetActions.newPreset() }
            }
            Button {
                objectName: "presetSave"
                text: "Save"
                active: Presets.modified
                onClicked: {
                    if (Presets.untitled) {
                        root.close()
                        PresetActions.saveAs()
                    } else {
                        Presets.save()
                    }
                }
            }
            Button {
                objectName: "presetSaveAs"
                text: "Save as"
                onClicked: { root.close(); PresetActions.saveAs() }
            }
            Button {
                objectName: "presetImport"
                text: "Import"
                onClicked: { root.close(); importFile.open() }
            }
            Button {
                objectName: "presetExport"
                text: "Export"
                onClicked: { root.close(); PresetActions.openExport() }
            }
        }
    }
}
