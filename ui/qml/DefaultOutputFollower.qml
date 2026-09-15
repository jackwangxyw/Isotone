import QtQuick
import Isotone

// Settings, General, "Switch preset when the default output changes": when
// Windows' default output moves to a working output, it becomes the current
// one, which loads what it plays (its preset lives on the endpoint).
QtObject {
    id: root
    // Outputs; a test gives a stand-in.
    property var outputs: Outputs
    property bool enabled: GeneralSettings.switchOnDefaultOutput

    readonly property Connections watch: Connections {
        target: root.outputs
        function onDefaultOutputChanged() {
            if (root.enabled) root.outputs.selectDefault()
        }
    }
}
