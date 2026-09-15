import QtQuick
import Isotone

// A press anywhere else ends typing in a field. In a dialog or popover in the
// overlay the focus goes to the nearest item around the field that holds the
// press (its card or panel), so its Escape and Enter keep working and its keys
// stay out of the window's; otherwise to `content`. Passes every press on.
MouseArea {
    id: root
    required property Item content
    required property Item overlay

    anchors.fill: parent
    z: 1000
    acceptedButtons: Qt.AllButtons
    onPressed: (mouse) => {
        mouse.accepted = false
        const f = root.Window.activeFocusItem
        if (!f || f.contains(f.mapFromItem(root, mouse.x, mouse.y))) return
        let holder = null
        for (let p = f.parent; p && p !== root.overlay; p = p.parent) {
            // The dialog or popover itself fills the window: a press outside its
            // card or panel (the scrim, or closing the popover) ends in content.
            if (p.parent === root.overlay) {
                if (holder) {
                    holder.forceActiveFocus()
                    return
                }
                break
            }
            if (!holder && p.contains(p.mapFromItem(root, mouse.x, mouse.y))) holder = p
        }
        root.content.forceActiveFocus()
    }
}
