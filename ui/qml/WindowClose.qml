import QtQuick
import Isotone

// Settings, General, closing the window: to the tray, or quit; and the tray's
// Quit. With unsaved changes "Save changes to <name> before closing?" asks
// first, over the window; Cancel keeps the window open. While that dialog is
// open another close or Quit brings it forward instead of asking again, and a
// Quit makes it quit.
QtObject {
    id: root
    // Main's window: show, raise, requestActivate, hide.
    required property var host
    signal quit()

    property var pending: null
    property bool quitting: false

    function requestClose() { ask(false) }
    function requestQuit() { ask(true) }

    function ask(andQuit) {
        if (andQuit) quitting = true
        if (pending || Presets.modified) UiState.showWindow(host)
        if (pending) return
        const d = PresetActions.confirmUnsaved(true, finish, () => { quitting = false })
        if (d) {
            pending = d
            d.closed.connect(() => { pending = null })
        }
    }
    function finish() {
        const q = quitting
        quitting = false
        if (q || !GeneralSettings.keepInTray) root.quit()
        else host.hide()
    }
}
