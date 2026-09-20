import QtQuick
import Isotone

// The status mark beside an output or a device: a bar, not a dot.
//
// It was the prototype's .dot, a 6 px circle, and a column of those down the
// Devices table (eleven of them) is the single most generic thing in the app
// (owner, 2026-09-20). A bar is the same information in a channel-strip mark
// rather than a dashboard LED.
//
// "ok", "warn" and "bad" carry their colour; anything else is muted and dimmed,
// which is an output that is idle. "off" draws nothing at all: it is
// not_installed and unplugged (devicestatus.cpp), and a mark that says "there
// is nothing here" was the wrong idea to begin with. The dimmed row already
// says it.
//
// The box stays 6 wide so no call site reflows; only the mark inside changed.
Item {
    id: root
    property string status: ""
    readonly property bool blank: status === "off"

    width: 6
    height: 14

    Rectangle {
        anchors.centerIn: parent
        width: 3
        height: parent.height
        radius: 1.5
        visible: !root.blank
        color: root.status === "ok" ? Theme.ok
             : root.status === "warn" ? Theme.warning
             : root.status === "bad" ? Theme.danger
             : Theme.muted
        opacity: root.status === "ok" || root.status === "warn" || root.status === "bad" ? 1 : 0.55
    }
}
