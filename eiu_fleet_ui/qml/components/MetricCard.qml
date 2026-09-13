import QtQuick

Rectangle {
    id: card

    property string title: ""
    property string value: ""
    property string detail: ""
    property string iconText: ""
    property url iconSource: ""
    property string valueFontFamily: "IBM Plex Mono"
    property color accentColor: C.accent
    // Needs the operator's attention right now (e.g. RMF offline) -- pulses
    // the border so it's noticed without animating text/value legibility.
    property bool alert: false
    readonly property real contentScale: Math.max(1.0, Math.min(1.45, width / 300))

    implicitHeight: 116
    radius: 16
    color: C.surface
    border.color: alert ? accentColor : C.border
    border.width: alert ? pulseWidth : 1

    property real pulseWidth: 1
    SequentialAnimation on pulseWidth {
        running: card.alert
        loops: Animation.Infinite
        NumberAnimation { from: 1; to: 2.5; duration: 650; easing.type: Easing.InOutQuad }
        NumberAnimation { from: 2.5; to: 1; duration: 650; easing.type: Easing.InOutQuad }
    }

    Rectangle {
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        width: 3
        radius: 2
        color: card.accentColor
        opacity: 0.9
    }

    Row {
        anchors.centerIn: parent
        spacing: 14 * card.contentScale

        Rectangle {
            id: iconBadge
            anchors.verticalCenter: parent.verticalCenter
            width: 52 * card.contentScale
            height: width
            radius: 14 * card.contentScale
            color: Qt.rgba(card.accentColor.r, card.accentColor.g,
                           card.accentColor.b, 0.14)
            border.color: Qt.rgba(card.accentColor.r, card.accentColor.g,
                                  card.accentColor.b, 0.35)

            // Same badge frame for both icon kinds, so a logo doesn't look
            // "busier" than a plain glyph just because it's an image.
            Text {
                anchors.centerIn: parent
                visible: card.iconSource.toString() === ""
                text: card.iconText
                color: card.accentColor
                font.pixelSize: 24 * card.contentScale
                font.bold: true
            }
            Image {
                anchors.centerIn: parent
                visible: card.iconSource.toString() !== ""
                source: card.iconSource
                width: parent.width * 0.62
                height: parent.height * 0.62
                fillMode: Image.PreserveAspectFit
                smooth: true
                mipmap: true
                asynchronous: true
            }
        }

        Column {
            id: textColumn
            anchors.verticalCenter: parent.verticalCenter
            width: Math.max(110, Math.min(340, card.width - iconBadge.width - 52))
            spacing: 4 * card.contentScale

            Text {
                width: parent.width
                text: card.title.toUpperCase()
                color: C.textDim
                font.pixelSize: 11 * card.contentScale
                font.bold: true
                font.letterSpacing: 1.15
                elide: Text.ElideRight
            }
            Text {
                width: parent.width
                text: card.value
                color: C.text
                font.family: card.valueFontFamily
                font.pixelSize: 28 * card.contentScale
                font.bold: true
                elide: Text.ElideRight
            }
            Text {
                width: parent.width
                text: card.detail
                color: card.accentColor
                font.pixelSize: 11 * card.contentScale
                elide: Text.ElideRight
            }
        }
    }
}
