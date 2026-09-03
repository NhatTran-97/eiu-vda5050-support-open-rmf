import QtQuick

Rectangle {
    id: card

    property string title: ""
    property string value: ""
    property string detail: ""
    property string iconText: ""
    property string valueFontFamily: "IBM Plex Mono"
    property color accentColor: C.accent
    readonly property real contentScale: Math.max(1.0, Math.min(1.45, width / 300))

    implicitHeight: 116
    radius: 16
    color: C.surface
    border.color: C.border
    border.width: 1

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
            width: 52 * card.contentScale
            height: width
            radius: 14 * card.contentScale
            color: Qt.rgba(card.accentColor.r, card.accentColor.g,
                           card.accentColor.b, 0.14)
            border.color: Qt.rgba(card.accentColor.r, card.accentColor.g,
                                  card.accentColor.b, 0.35)

            Text {
                anchors.centerIn: parent
                text: card.iconText
                color: card.accentColor
                font.pixelSize: 24 * card.contentScale
                font.bold: true
            }
        }

        Column {
            anchors.verticalCenter: parent.verticalCenter
            width: Math.max(110, Math.min(340,
                       card.width - (52 * card.contentScale) - 52))
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
