import QtQuick
import Isotone

// "Save changes to <name>?" (or "... before closing?" with `closing`): Cancel,
// Don't save, Save. Don't save puts the output back to its saved preset (owner's
// decision). Save on an untitled output asks for a name first. Emits exactly one
// of saved(), discarded(), cancelled(), then closes. PresetActions.confirmUnsaved
// opens it.
DialogFrame {
    id: root
    property bool closing: false
    property string name: Presets.currentName
    signal saved()
    signal discarded()
    signal cancelled()

    title: closing ? "Save changes to " + name + " before closing?" : "Save changes to " + name + "?"
    cardWidth: closing ? 480 : 440
    onRejected: root.cancelled()

    actions: [
        Button {
            objectName: "unsavedCancel"
            text: "Cancel"
            onClicked: { root.cancelled(); root.close() }
        },
        Button {
            objectName: "unsavedDiscard"
            text: "Don’t save"
            onClicked: { Presets.revert(); root.discarded(); root.close() }
        },
        Button {
            objectName: "unsavedSave"
            text: "Save"
            kind: "primary"
            onClicked: {
                if (!Presets.untitled) {
                    Presets.save()
                    root.saved()
                    root.close()
                    return
                }
                // Hidden, not closed, until the name is given.
                root.visible = false
                PresetActions.saveAs(() => { root.saved(); root.close() }, () => { root.cancelled(); root.close() })
            }
        }
    ]
}
