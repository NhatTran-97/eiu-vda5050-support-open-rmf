import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Workspace navigation; System opens the fleet adapters' health view.
Rectangle {
    id: rail

    signal systemRequested()

    color: Theme.railBg

    Rectangle {
        anchors.right: parent.right
        width: 1
        height: parent.height
        color: Theme.border
        opacity: 0.65
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: 82

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 22
                anchors.rightMargin: 16
                spacing: 11

                Rectangle {
                    Layout.preferredWidth: 42
                    Layout.preferredHeight: 42
                    radius: 12
                    color: Theme.accent

                    Text {
                        anchors.centerIn: parent
                        text: "E"
                        color: Theme.textOnAccent
                        font.pixelSize: 21
                        font.bold: true
                    }
                }

                ColumnLayout {
                    spacing: 0
                    Text {
                        text: "EIU FLEET"
                        color: Theme.text
                        font.pixelSize: 15
                        font.bold: true
                        font.letterSpacing: 0.8
                    }
                    Text {
                        text: "CONTROL OS"
                        color: Theme.accent
                        font.pixelSize: 9
                        font.bold: true
                        font.letterSpacing: 1.7
                    }
                }
            }
        }

        Text {
            Layout.leftMargin: 22
            Layout.topMargin: 14
            Layout.bottomMargin: 10
            text: "WORKSPACE"
            color: Theme.textDim
            opacity: 0.65
            font.pixelSize: 9
            font.bold: true
            font.letterSpacing: 1.8
        }

        Repeater {
            model: [
                { "label": "Dashboard", "glyph": "▦", "active": true },
                { "label": "Navigation", "glyph": "⌖", "active": false },
                { "label": "Robots", "glyph": "◎", "active": false },
                { "label": "Tasks", "glyph": "✓", "active": false },
                { "label": "System", "glyph": "⚙", "active": false }
            ]

            delegate: Item {
                Layout.fillWidth: true
                Layout.preferredHeight: 48

                Rectangle {
                    anchors.fill: parent
                    anchors.leftMargin: 12
                    anchors.rightMargin: 12
                    anchors.topMargin: 3
                    anchors.bottomMargin: 3
                    radius: 10
                    // Accent for the selected navigation item.
                    color: modelData.active ? Theme.navActive : "transparent"

                    MouseArea {
                        anchors.fill: parent
                        enabled: modelData.label === "System"
                        cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                        onClicked: rail.systemRequested()
                    }

                    Rectangle {
                        visible: modelData.active
                        anchors.left: parent.left
                        anchors.verticalCenter: parent.verticalCenter
                        width: 3
                        height: 22
                        radius: 2
                        color: Theme.navIndicator
                    }

                    Row {
                        anchors.left: parent.left
                        anchors.leftMargin: 15
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 13

                        Text {
                            width: 22
                            text: modelData.glyph
                            color: modelData.active ? Theme.textOnAccent : Theme.textDim
                            font.pixelSize: 18
                            horizontalAlignment: Text.AlignHCenter
                        }
                        Text {
                            anchors.verticalCenter: parent.verticalCenter
                            text: modelData.label
                            // Text color for navigation labels.
                            color: modelData.active ? Theme.textOnAccent : Theme.navText
                            font.pixelSize: 14
                            font.bold: modelData.active
                        }
                    }
                }
            }
        }

        Item { Layout.fillHeight: true }

        // System health is already shown in the KPI card and Needs Attention.
        Text {
            Layout.leftMargin: 22
            Layout.bottomMargin: 16
            text: "Fleet UI  ·  v0.2.0"
            color: Theme.textDim
            opacity: 0.6
            font.family: fontMono
            font.pixelSize: 11
        }
    }
}
