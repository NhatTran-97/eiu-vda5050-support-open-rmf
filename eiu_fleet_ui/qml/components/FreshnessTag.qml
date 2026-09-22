import QtQuick
import QtQuick.Controls

// Shows how fresh a value is in one of three states, with the exact timestamp on hover.
Item {
    id: root

    property bool online: false
    property bool hasData: false
    property real lastRx: 0     // epoch seconds
    property real nowTick: 0    // ms, driven by the caller so this ticks without new data
    property real fontSize: 11
    property bool bold: false

    readonly property string stateText: !hasData ? "NO DATA" : (online ? "LIVE" : "OFFLINE")
    readonly property color stateColor: !hasData ? C.textDim : (online ? C.success : C.err)
    readonly property string detailText: {
        var _ = nowTick
        if (!hasData) return "Never received"
        return (online ? "Updated " : "Last seen ") + formatAgo(lastRx)
    }

    function formatAgo(epochSec) {
        if (!epochSec) return ""
        var diff = Math.max(0, nowTick / 1000 - epochSec)
        if (diff < 60) return Math.floor(diff) + "s ago"
        if (diff < 3600) return Math.floor(diff / 60) + "m " + Math.floor(diff % 60) + "s ago"
        if (diff < 86400) return Math.floor(diff / 3600) + "h " + Math.floor((diff % 3600) / 60) + "m ago"
        return Math.floor(diff / 86400) + "d ago"
    }

    function formatAbsolute(epochSec) {
        if (!epochSec) return ""
        return Qt.formatDateTime(new Date(epochSec * 1000), "d MMM yyyy, HH:mm:ss")
    }

    implicitWidth: row.implicitWidth
    implicitHeight: row.implicitHeight

    Row {
        id: row
        anchors.verticalCenter: parent.verticalCenter
        spacing: 6

        Rectangle {
            width: 7; height: 7; radius: 3.5
            color: root.stateColor
            anchors.verticalCenter: parent.verticalCenter
        }
        Text {
            text: root.stateText + (root.detailText ? " · " + root.detailText : "")
            color: root.stateColor
            font.pixelSize: root.fontSize
            font.bold: root.bold
            elide: Text.ElideRight
        }
    }

    MouseArea {
        anchors.fill: parent
        visible: root.hasData
        hoverEnabled: true
        cursorShape: Qt.WhatsThisCursor
        ToolTip.visible: containsMouse
        ToolTip.delay: 400
        ToolTip.text: root.formatAbsolute(root.lastRx)
    }
}
