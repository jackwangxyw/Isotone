pragma Singleton
import QtQuick

// Window-wide UI state shared by the screens: which view is shown, the Settings
// tab, and the overlay layer dialogs and toasts open in (Main sets `overlay`).
QtObject {
    id: ui
    // "eq", "speakers", "devices", "settings".
    property string view: "eq"
    // "general", "outputs", "appearance", "shortcuts", "about".
    property string settingsTab: "general"
    // The Devices view's selected output (braced GUID), for "Install" and "Repair" from the top bar.
    property string devicesSelection: ""
    property Item overlay: null

    // Creates `component` (a DialogFrame, or anything that fills its parent) in
    // the overlay with `properties`, and returns it.
    function openDialog(component, properties) {
        if (!overlay) return null
        const d = component.createObject(overlay, properties || {})
        if (!d) console.warn("could not open dialog:", component.errorString())
        return d
    }

    // Shows, raises and activates a window (from the tray too), before a dialog asks.
    function showWindow(w) {
        if (w.visibility === Window.Minimized) w.showNormal()
        else w.show()
        w.raise()
        w.requestActivate()
    }

    // A toast at the bottom of the window: text, and an optional action ("Undo").
    signal toastRequested(string text, string actionText, var action)
    function toast(text, actionText, action) { toastRequested(text, actionText || "", action || null) }
}
