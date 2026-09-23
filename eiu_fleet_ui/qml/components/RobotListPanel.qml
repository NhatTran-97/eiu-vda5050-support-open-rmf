import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "Format.js" as Format

// Followed robots with their link, battery, status and current task.
Rectangle {
    id: panel

    property string selectedRobotName: ""
    property real nowTick: 0
    // Typography scale that follows the panel width.
    property real contentScale: 1.0

    signal robotSelected(string name)
    signal controlsRequested(string name)

    Layout.fillWidth: true
    Layout.fillHeight: true
    Layout.preferredHeight: 260
    radius: 16
    color: Theme.surface
    border.color: Theme.border
    border.width: 1
    clip: true

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: 52 * panel.contentScale
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 17
                anchors.rightMargin: 17
                Text {
                    text: "FLEET ROBOTS"
                    color: Theme.text
                    font.pixelSize: 13 * panel.contentScale
                    font.bold: true
                    font.letterSpacing: 0.8
                }
                TextField {
                    Layout.fillWidth: true
                    Layout.maximumWidth: 150 * panel.contentScale
                    Layout.preferredHeight: 28 * panel.contentScale
                    placeholderText: "Search robots…"
                    placeholderTextColor: Theme.placeholderText
                    color: Theme.text
                    font.pixelSize: 11 * panel.contentScale
                    verticalAlignment: Text.AlignVCenter
                    leftPadding: 8
                    onTextChanged: dashboard.robotFilter = text
                    background: Rectangle {
                        radius: 8; color: Theme.surfaceAlt
                        border.color: Theme.border; border.width: 1
                    }
                }
                Rectangle {
                    Layout.preferredWidth: 30 * panel.contentScale
                    Layout.preferredHeight: 25 * panel.contentScale
                    radius: 8
                    color: Theme.countBadgeFill
                    Text {
                        anchors.centerIn: parent
                        text: dashboard.robotList.count
                        color: Theme.cyan
                        font.family: fontMono
                        font.pixelSize: 11 * panel.contentScale
                        font.bold: true
                    }
                }
            }
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.border; opacity: 0.65 }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            ListView {
                objectName: "robotListView"
                anchors.fill: parent
                anchors.margins: 8
                model: dashboard.robotList
                clip: true
                spacing: 3
                boundsBehavior: Flickable.StopAtBounds

                delegate: Rectangle {
                    id: robotRow
                    required property var row
                    required property int index
                    width: ListView.view.width
                    // Row height follows its content; task, warnings and freshness are each conditional.
                    height: Math.max(72 * panel.contentScale,
                                     nameColumn.implicitHeight + 20 * panel.contentScale)
                    radius: 10
                    readonly property bool selected: row.name === panel.selectedRobotName
                    color: selected ? Qt.rgba(0.125, 0.89, 0.94, 0.10)
                           : (index % 2 === 0 ? Theme.surfaceAlt : "transparent")
                    border.color: Theme.cyanBright
                    border.width: selected ? 1.5 : 0

                    // Shared with the map and the telemetry panel.
                    MouseArea {
                        anchors.fill: parent
                        onClicked: panel.robotSelected(robotRow.row.name)
                    }

                    // Latest VDA5050 telemetry for this robot, summarised by the dashboard.
                    readonly property var tele: row.tele
                    readonly property bool online: row.online
                    readonly property string lastSeenText: (tele && tele.last_rx) ? Format.formatAgo(panel.nowTick, tele.last_rx) : ""

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 10
                        anchors.rightMargin: 10
                        anchors.topMargin: 10 * panel.contentScale
                        anchors.bottomMargin: 10 * panel.contentScale
                        spacing: 10

                        Rectangle {
                            Layout.preferredWidth: 52 * panel.contentScale
                            Layout.preferredHeight: 52 * panel.contentScale
                            radius: 14 * panel.contentScale
                            color: Theme.avatarFill
                            border.color: Theme.avatarBorder
                            Text {
                                anchors.centerIn: parent
                                text: robotRow.row.name.length ? robotRow.row.name.charAt(0).toUpperCase() : "R"
                                color: Theme.cyan
                                font.pixelSize: 21 * panel.contentScale
                                font.bold: true
                            }
                            // Connection indicator from the VDA5050 connection topic.
                            Rectangle {
                                width: 12 * panel.contentScale
                                height: width
                                radius: width / 2
                                anchors.right: parent.right
                                anchors.bottom: parent.bottom
                                anchors.margins: -1
                                color: robotRow.online ? Theme.success : Theme.err
                                border.width: 1.5
                                border.color: Theme.surface
                            }
                        }
                        ColumnLayout {
                            id: nameColumn
                            Layout.fillWidth: true
                            spacing: 3
                            Text { text: robotRow.row.name; color: Theme.text; font.pixelSize: 22 * panel.contentScale; font.bold: true; elide: Text.ElideRight; Layout.fillWidth: true }
                            Text {
                                text: robotRow.row.fleet + "  ·  " + robotRow.row.level
                                color: robotRow.online ? Theme.textDim : Theme.err
                                font.family: fontMono
                                font.pixelSize: 16 * panel.contentScale
                                elide: Text.ElideRight; Layout.fillWidth: true
                            }
                            FreshnessTag {
                                Layout.fillWidth: true
                                visible: !!robotRow.tele
                                online: !!robotRow.online
                                hasData: !!robotRow.tele
                                lastRx: robotRow.tele ? robotRow.tele.last_rx : 0
                                nowTick: panel.nowTick
                                fontSize: Math.max(11, 13 * panel.contentScale)
                            }
                            // Show speed, localization, and safety indicators.
                            Text {
                                visible: !!robotRow.tele
                                text: robotRow.tele
                                      ? robotRow.tele.speed.toFixed(2) + " m/s"
                                        + (robotRow.tele.stale ? "  ⚠ NO RECENT DATA" : "")
                                        + (robotRow.tele.manual ? "  ⚠ MANUAL CONTROL" : "")
                                        + (robotRow.tele.not_localized ? "  ⚠ NOT LOCALIZED" : "")
                                        + (robotRow.tele.unsafe ? "  ⚠ " + robotRow.tele.safety_label : "")
                                      : ""
                                color: robotRow.tele && robotRow.tele.unsafe ? Theme.err
                                       : (robotRow.tele && (robotRow.tele.not_localized || robotRow.tele.stale || robotRow.tele.manual)
                                          ? Theme.warn : Theme.textDim)
                                font.family: fontMono
                                font.pixelSize: Math.max(11, 14 * panel.contentScale)
                                elide: Text.ElideRight; Layout.fillWidth: true
                            }
                            Text {
                                visible: robotRow.row.task_destination !== ""
                                text: "→ " + robotRow.row.task_destination
                                color: Theme.cyan
                                font.family: fontMono
                                font.pixelSize: Math.max(11, 13 * panel.contentScale)
                                elide: Text.ElideRight; Layout.fillWidth: true
                            }
                        }
                        Text {
                            Layout.rightMargin: 14 * panel.contentScale
                            text: robotRow.row.hasBattery
                                  ? Number(robotRow.row.battery).toFixed(0) + "%"
                                    + (robotRow.tele && robotRow.tele.charging ? " ⚡" : "")
                                  : "—%"
                            color: !robotRow.row.hasBattery ? Theme.textDim
                                   : (Number(robotRow.row.battery) < uiConfig.lowBatteryPercent ? Theme.err : Theme.success)
                            // Last-known value, not live -- dim it while disconnected.
                            opacity: robotRow.online ? 1.0 : 0.5
                            font.family: fontMono
                            font.pixelSize: 20 * panel.contentScale
                            font.bold: true

                            MouseArea {
                                anchors.fill: parent
                                hoverEnabled: true
                                visible: !robotRow.online && robotRow.row.hasBattery
                                cursorShape: Qt.WhatsThisCursor
                                ToolTip.visible: containsMouse
                                ToolTip.delay: 400
                                ToolTip.text: "Last reported "
                                              + Number(robotRow.row.battery).toFixed(0) + "%"
                                              + (robotRow.lastSeenText ? " · " + robotRow.lastSeenText : "")
                            }
                        }
                        ColumnLayout {
                            id: statusBlock
                            Layout.preferredWidth: 122 * panel.contentScale
                            Layout.alignment: Qt.AlignVCenter
                            spacing: 3
                            Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 40 * panel.contentScale
                                radius: 12 * panel.contentScale
                                color: "transparent"
                                // A live RMF status is meaningless once the robot itself is unreachable.
                                border.color: robotRow.online
                                              ? Theme.statusColor(robotRow.row.status) : Theme.err
                                border.width: 1
                                Text {
                                    anchors.centerIn: parent
                                    text: robotRow.online ? robotRow.row.status : "OFFLINE"
                                    color: robotRow.online
                                           ? Theme.statusColor(robotRow.row.status) : Theme.err
                                    font.family: fontMono
                                    font.pixelSize: 15 * panel.contentScale
                                    font.bold: true
                                    elide: Text.ElideRight
                                    width: parent.width - 8
                                    horizontalAlignment: Text.AlignHCenter
                                }
                                MouseArea {
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    visible: robotRow.row.status === "PENDING SYNC"
                                    cursorShape: Qt.WhatsThisCursor
                                    ToolTip.visible: containsMouse
                                    ToolTip.delay: 400
                                    ToolTip.text: "Robot state is available through VDA5050, but "
                                                  + "has not been synchronized with RMF yet."
                                }
                            }
                            Text {
                                Layout.fillWidth: true
                                Layout.topMargin: 6 * panel.contentScale
                                visible: robotRow.row.rounds_remaining > 0
                                text: robotRow.row.rounds_current + "/" + robotRow.row.rounds_total + " rounds"
                                color: Theme.cyan
                                font.family: fontMono
                                font.pixelSize: 14 * panel.contentScale
                                font.bold: true
                                elide: Text.ElideRight
                                horizontalAlignment: Text.AlignHCenter
                            }
                        }
                        Button {
                            // Align the accent bar with the status and round count.
                            Layout.preferredWidth: statusBlock.height
                            Layout.preferredHeight: statusBlock.height
                            Layout.alignment: Qt.AlignVCenter
                            text: "⚙"
                            hoverEnabled: true
                            contentItem: Text {
                                text: parent.text; color: Theme.textDim
                                font.pixelSize: 26 * panel.contentScale
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                            background: Rectangle {
                                radius: 8
                                color: parent.down ? Theme.border
                                       : (parent.hovered ? Theme.surfaceAlt : "transparent")
                                border.color: parent.hovered ? Theme.cyan : Theme.border
                                border.width: 1
                            }
                            onClicked: panel.controlsRequested(robotRow.row.name)
                        }
                    }
                }
            }

            Text {
                // PENDING SYNC robots still count as rows -- gate on the filtered list.
                anchors.centerIn: parent
                visible: dashboard.robotList.count === 0
                         && dashboard.robotFilter === ""
                text: ros.rmfOnline ? "No robots in this fleet" : "Waiting for /fleet_states"
                color: Theme.textDim
                font.pixelSize: 13 * panel.contentScale
            }
            Text {
                anchors.centerIn: parent
                visible: dashboard.robotList.count === 0
                         && dashboard.robotFilter !== ""
                text: "No robots match “" + dashboard.robotFilter + "”"
                color: Theme.textDim
                font.pixelSize: 13 * panel.contentScale
            }
        }
    }
}
