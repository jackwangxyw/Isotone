import QtQuick
import Isotone

// The prototype's .dot: 6 px; "ok", "warn", "bad", "off" (a hollow ring), or
// anything else muted.
Rectangle {
    id: root
    property string status: ""
    width: 6
    height: 6
    radius: 3
    color: status === "ok" ? Theme.ok : status === "warn" ? Theme.warning : status === "bad" ? Theme.danger
         : status === "off" ? "transparent" : Theme.muted
    opacity: status === "ok" || status === "warn" || status === "bad" ? 1 : 0.7
    border.width: status === "off" ? 1 : 0
    border.color: Theme.muted
}
