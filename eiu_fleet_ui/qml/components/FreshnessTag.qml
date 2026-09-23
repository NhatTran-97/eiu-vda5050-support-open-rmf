import QtQuick
import QtQuick.Controls
import "Format.js" as Format

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
    readonly property color stateColor: !hasData ? Theme.textDim : (online ? Theme.success : Theme.err)
    readonly property string detailText: {
        if (!hasData) return "Never received"
        return (online ? "Updated " : "Last seen ") + Format.formatAgo(nowTick, lastRx)
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
        ToolTip.text: Format.formatAbsolute(root.lastRx)
    }
}
