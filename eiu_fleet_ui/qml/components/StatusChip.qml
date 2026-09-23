import QtQuick

// A rounded chip with a state dot and a short label; level is "ok", "warn", "err" or "idle".
Rectangle {
    id: chip

    property string label: ""
    property string level: "idle"
    // Show a pointing hand and emit clicked() when the chip leads somewhere.
    property bool clickable: false

    signal clicked()

    readonly property color levelColor: level === "ok" ? Theme.success
                                        : (level === "warn" ? Theme.warn : (level === "err" ? Theme.err : Theme.textDim))

    implicitWidth: content.implicitWidth + 30
    implicitHeight: 34
    radius: height / 2
    color: Theme.surface
    border.color: level === "ok" ? Theme.successBorder
                  : (level === "warn" ? Theme.warnBorder : (level === "err" ? Theme.errBorder : Theme.border))
    border.width: 1

    Row {
        id: content
        anchors.centerIn: parent
        spacing: 7
        Rectangle {
            width: 7
            height: 7
            radius: 4
            color: chip.levelColor
            anchors.verticalCenter: parent.verticalCenter
        }
        Text {
            text: chip.label
            color: Theme.text
            font.family: fontMono
            font.pixelSize: 10
            font.bold: true
            anchors.verticalCenter: parent.verticalCenter
        }
    }

    MouseArea {
        anchors.fill: parent
        enabled: chip.clickable
        cursorShape: chip.clickable ? Qt.PointingHandCursor : Qt.ArrowCursor
        onClicked: chip.clicked()
    }
}
