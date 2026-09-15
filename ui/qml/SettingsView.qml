import QtQuick
import Isotone

// Settings: the title, the tabs (General, Outputs, Appearance, Shortcuts, About)
// and the page for UiState.settingsTab. Pages scroll when taller than the window.
Item {
    id: root

    readonly property var tabs: [["general", "General"], ["outputs", "Outputs"], ["appearance", "Appearance"],
                                 ["shortcuts", "Shortcuts"], ["about", "About"]]

    Text {
        id: title
        x: 40
        y: 36
        text: "Settings"
        font.family: Theme.font
        font.pixelSize: 26
        font.weight: Font.DemiBold
        font.letterSpacing: -0.39
        color: Theme.text
    }

    Item {
        id: tabBar
        x: 40
        anchors.top: title.bottom
        anchors.topMargin: 18
        width: 760
        height: 35
        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: Theme.gridMajor }
        Row {
            spacing: 24
            Repeater {
                model: root.tabs
                delegate: Item {
                    id: tab
                    required property var modelData
                    readonly property bool on: UiState.settingsTab === modelData[0]
                    objectName: "tab_" + modelData[0]
                    width: label.implicitWidth
                    height: 35
                    Text {
                        id: label
                        anchors.verticalCenter: parent.verticalCenter
                        text: tab.modelData[1]
                        font.family: Theme.font
                        font.pixelSize: 14
                        font.weight: tab.on ? Font.DemiBold : Font.Normal
                        color: tab.on ? Theme.text : Theme.muted
                    }
                    Rectangle {
                        visible: tab.on
                        anchors.bottom: parent.bottom
                        width: parent.width
                        height: 2
                        radius: 1
                        color: Theme.text
                    }
                    MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: UiState.settingsTab = tab.modelData[0] }
                }
            }
        }
    }

    Flickable {
        id: pageFlick
        objectName: "settingsFlick"
        x: 40
        anchors.top: tabBar.bottom
        anchors.bottom: parent.bottom
        width: parent.width - 80
        contentHeight: page.item ? page.item.implicitHeight + 40 : 0
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        Loader {
            id: page
            width: pageFlick.width
            source: {
                switch (UiState.settingsTab) {
                case "outputs": return "SettingsOutputs.qml"
                case "appearance": return "SettingsAppearance.qml"
                case "shortcuts": return "SettingsShortcuts.qml"
                case "about": return "SettingsAbout.qml"
                default: return "SettingsGeneral.qml"
                }
            }
        }
    }
}
