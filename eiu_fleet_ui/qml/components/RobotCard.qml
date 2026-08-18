import QtQuick
import QtQuick.Layouts

// A card describing a single robot. Data is passed in via properties (from ros.robotsJson).
Rectangle {
    id: card

    property string robotName: ""
    property string fleet:     ""
    property string status:    "—"
    property string level:     "L1"
    property real   battery:   0
    property string task:      ""

    Layout.fillWidth: true
    implicitHeight:   col.implicitHeight + 24
    radius:           10
    color:            C.surface
    border.color:     C.border
    border.width:     1

    // Color based on the robot's status
    function statusColor(s) {
        if (s === "MOVING" || s === "DOCKING" || s === "GOING_HOME") return C.blue
        if (s === "CHARGING")                                        return C.accent
        if (s === "EMERGENCY" || s === "ERROR")                      return C.err
        if (s === "PAUSED" || s === "WAITING")                       return C.warn
        return C.textDim   // IDLE / unknown
    }

    Column {
        id: col
        anchors { left: parent.left; right: parent.right; top: parent.top; margins: 12 }
        spacing: 8

        // Header: status dot + name + fleet
        RowLayout {
            width: parent.width
            spacing: 8
            Rectangle {
                width: 9; height: 9; radius: 4.5
                color: card.statusColor(card.status)
                Layout.alignment: Qt.AlignVCenter
            }
            Text {
                text: card.robotName
                font.pixelSize: 14; font.bold: true; color: C.text
            }
            Item { Layout.fillWidth: true }
            Text { text: card.fleet; font.pixelSize: 11; color: C.textDim }
        }

        // Info grid: Status / Level / Task
        Grid {
            columns: 2; columnSpacing: 12; rowSpacing: 4
            width: parent.width

            Text { text: "Status"; font.pixelSize: 11; color: C.textDim }
            Text {
                text: card.status; font.pixelSize: 11; font.bold: true
                color: card.statusColor(card.status)
            }
            Text { text: "Level"; font.pixelSize: 11; color: C.textDim }
            Text { text: card.level; font.pixelSize: 11; color: C.text }
            Text { text: "Task"; font.pixelSize: 11; color: C.textDim }
            Text {
                text: card.task !== "" ? card.task : "—"
                font.pixelSize: 11; color: C.text
                elide: Text.ElideRight; width: 130
            }
        }

        // Battery bar
        RowLayout {
            width: parent.width; spacing: 8
            Text { text: "Battery"; font.pixelSize: 11; color: C.textDim
                   Layout.preferredWidth: 46 }
            Rectangle {
                Layout.fillWidth: true
                height: 8; radius: 4; color: C.surfaceAlt
                Rectangle {
                    width:  parent.width * Math.max(0, Math.min(1, card.battery / 100))
                    height: parent.height; radius: 4
                    color:  card.battery < 20 ? C.err : C.accent
                }
            }
            Text { text: card.battery.toFixed(0) + "%"; font.pixelSize: 11; color: C.text }
        }
    }
}
