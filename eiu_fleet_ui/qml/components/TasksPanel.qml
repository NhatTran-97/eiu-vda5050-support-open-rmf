import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "Format.js" as Format

// Recent tasks with search, state filter and cancel; the VDA5050 message log on its own tab.
Rectangle {
    id: recentTasksPanel

    signal newTaskRequested()

    // Grows with the column once there is something to list; an empty table stays short.
    Layout.fillHeight: true
    Layout.preferredHeight: 280
    Layout.maximumHeight: dashboard.taskTotal > 0 || panelTab === "traffic" ? Number.POSITIVE_INFINITY : 280
    radius: 16
    color: Theme.surface
    border.color: Theme.border
    border.width: 1
    clip: true

    // Size columns to the task table width.
    readonly property real tableScale: Math.max(
        1.0, Math.min(1.40, width / 680))
    readonly property real dateColumnWidth: 78 * tableScale
    readonly property real pickupColumnWidth: 56 * tableScale
    readonly property real robotColumnWidth: 48 * tableScale
    readonly property real timeColumnWidth: 74 * tableScale
    readonly property real stateColumnWidth: 78 * tableScale
    readonly property real actionColumnWidth: 26
    readonly property real identityColumnGap: 8 * tableScale

    property string panelTab: "tasks"   // tasks | traffic
    // A narrow panel drops the pickup and date columns and shortens the tab names.
    readonly property bool showPickup: width >= 560
    readonly property bool showDate: width >= 480
    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: 52
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 17
                anchors.rightMargin: 17
                spacing: 9

                Rectangle {
                    Layout.preferredWidth: 4
                    Layout.preferredHeight: 20
                    radius: 2
                    color: Theme.accent
                }
                Repeater {
                    model: [{ key: "tasks", label: "RECENT TASKS", short: "TASKS" },
                            { key: "traffic", label: "VDA5050 TRAFFIC", short: "VDA5050" }]
                    delegate: Text {
                        readonly property bool active: recentTasksPanel.panelTab === modelData.key
                        text: recentTasksPanel.width >= 560 ? modelData.label : modelData.short
                        color: active ? Theme.text : Theme.textDim
                        font.pixelSize: 12 * recentTasksPanel.tableScale
                        font.bold: true
                        font.letterSpacing: 0.8
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: recentTasksPanel.panelTab = modelData.key
                        }
                        Rectangle {
                            visible: parent.active
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.top: parent.bottom
                            anchors.topMargin: 3
                            height: 2
                            radius: 1
                            color: Theme.accent
                        }
                    }
                }
                // Keeps the tab-specific controls on the right.
                Item { visible: recentTasksPanel.panelTab === "traffic"; Layout.fillWidth: true }
                TextField {
                    id: taskSearchField
                    visible: recentTasksPanel.panelTab === "tasks"
                    Layout.fillWidth: true
                    Layout.minimumWidth: 70
                    Layout.maximumWidth: 180 * recentTasksPanel.tableScale
                    Layout.preferredHeight: 28 * recentTasksPanel.tableScale
                    placeholderText: "Search robot / dest…"
                    placeholderTextColor: Theme.placeholderText
                    color: Theme.text
                    font.pixelSize: 11 * recentTasksPanel.tableScale
                    verticalAlignment: Text.AlignVCenter
                    leftPadding: 8
                    onTextChanged: dashboard.taskSearch = text
                    background: Rectangle {
                        radius: 8; color: Theme.surfaceAlt
                        border.color: Theme.border; border.width: 1
                    }
                }
                ComboBox {
                    id: taskStateCombo
                    visible: recentTasksPanel.panelTab === "tasks"
                    Layout.preferredWidth: 108 * recentTasksPanel.tableScale
                    Layout.preferredHeight: 28 * recentTasksPanel.tableScale
                    model: ["All", "Queued", "Underway", "Completed", "Cancelled", "Failed"]
                    font.pixelSize: 11 * recentTasksPanel.tableScale
                    onActivated: dashboard.taskStateFilter = currentText
                    contentItem: Text {
                        text: taskStateCombo.displayText; color: Theme.text; font: taskStateCombo.font
                        leftPadding: 8; elide: Text.ElideRight
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        radius: 8; color: Theme.surfaceAlt
                        border.color: Theme.border; border.width: 1
                    }
                }
                Rectangle {
                    // Hide the count on a narrow card.
                    visible: recentTasksPanel.panelTab === "traffic" || recentTasksPanel.width >= 560
                    Layout.preferredWidth: recordsText.implicitWidth + 18
                    Layout.preferredHeight: 24
                    radius: 8
                    color: Theme.badgeFill
                    border.color: Theme.badgeBorder
                    border.width: 1

                    Text {
                        id: recordsText
                        anchors.centerIn: parent
                        text: recentTasksPanel.panelTab === "traffic"
                              ? dashboard.traffic.count + " messages"
                              : (dashboard.taskSearch === "" && dashboard.taskStateFilter === "All")
                                ? dashboard.taskTotal + " records"
                                : dashboard.tasks.count + " / " + dashboard.taskTotal
                        color: Theme.cyan
                        font.family: fontMono
                        font.pixelSize: Math.max(11, 10 * recentTasksPanel.tableScale)
                        font.bold: true
                    }
                }
            }
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.border; opacity: 0.65 }

        // Use the same column widths for headers and rows.
        Rectangle {
            visible: recentTasksPanel.panelTab === "tasks"
            Layout.fillWidth: true
            Layout.preferredHeight: 34 * recentTasksPanel.tableScale
            color: Theme.tableHeaderBg
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 19
                anchors.rightMargin: 16
                spacing: 4
                Text { text: "DATE";      visible: recentTasksPanel.showDate; Layout.preferredWidth: recentTasksPanel.dateColumnWidth;      Layout.minimumWidth: Layout.preferredWidth; Layout.rightMargin: recentTasksPanel.identityColumnGap; color: Theme.textDim; font.pixelSize: 13 * recentTasksPanel.tableScale; font.bold: true; font.letterSpacing: 0.6; horizontalAlignment: Text.AlignHCenter }
                Text { text: "PICKUP";    visible: recentTasksPanel.showPickup; Layout.preferredWidth: recentTasksPanel.pickupColumnWidth;    Layout.minimumWidth: Layout.preferredWidth; color: Theme.textDim; font.pixelSize: 13 * recentTasksPanel.tableScale; font.bold: true; font.letterSpacing: 0.6; horizontalAlignment: Text.AlignHCenter }
                Text { text: "DEST.";     Layout.fillWidth: true; color: Theme.textDim; font.pixelSize: 13 * recentTasksPanel.tableScale; font.bold: true; font.letterSpacing: 0.6; horizontalAlignment: Text.AlignHCenter }
                Text { text: "ROBOT";     Layout.preferredWidth: recentTasksPanel.robotColumnWidth;     Layout.minimumWidth: Layout.preferredWidth; color: Theme.textDim; font.pixelSize: 13 * recentTasksPanel.tableScale; font.bold: true; font.letterSpacing: 0.6; horizontalAlignment: Text.AlignHCenter }
                Text { text: "TIME";      Layout.preferredWidth: recentTasksPanel.timeColumnWidth;      Layout.minimumWidth: Layout.preferredWidth; color: Theme.textDim; font.pixelSize: 13 * recentTasksPanel.tableScale; font.bold: true; font.letterSpacing: 0.6; horizontalAlignment: Text.AlignHCenter }
                Text { text: "STATE";     Layout.preferredWidth: recentTasksPanel.stateColumnWidth;     Layout.minimumWidth: Layout.preferredWidth; color: Theme.textDim; font.pixelSize: 13 * recentTasksPanel.tableScale; font.bold: true; font.letterSpacing: 0.6; horizontalAlignment: Text.AlignHCenter }
                Item { Layout.preferredWidth: recentTasksPanel.actionColumnWidth; Layout.minimumWidth: Layout.preferredWidth }
            }
        }
        Rectangle { visible: recentTasksPanel.panelTab === "tasks"; Layout.fillWidth: true; Layout.preferredHeight: 1; color: Theme.border; opacity: 0.4 }

        VdaTrafficPanel {
            objectName: "vdaTraffic"
            visible: recentTasksPanel.panelTab === "traffic"
            Layout.fillWidth: true
            Layout.fillHeight: true
            messages: dashboard.traffic
            typeFilter: dashboard.trafficFilter
            onFilterPicked: (kind) => dashboard.trafficFilter = kind
            uiScale: recentTasksPanel.tableScale
        }

        Item {
            visible: recentTasksPanel.panelTab === "tasks"
            Layout.fillWidth: true
            Layout.fillHeight: true

            ListView {
                objectName: "taskListView"
                anchors.fill: parent
                anchors.margins: 8
                model: dashboard.tasks
                clip: true
                spacing: 2
                boundsBehavior: Flickable.StopAtBounds

                delegate: Rectangle {
                    id: taskRow
                    required property var row
                    required property int index
                    width: ListView.view.width
                    height: 48 * recentTasksPanel.tableScale
                    radius: 8
                    color: taskRowHover.hovered
                           ? Theme.surfaceRaised
                           : (index % 2 === 0 ? Theme.surfaceAlt : "transparent")

                    Behavior on color {
                        ColorAnimation { duration: 110 }
                    }

                    HoverHandler { id: taskRowHover }

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 11
                        anchors.rightMargin: 8
                        spacing: 4

                        Text {
                            visible: recentTasksPanel.showDate
                            text: Format.formatDay(taskRow.row.created_ms)
                            Layout.preferredWidth: recentTasksPanel.dateColumnWidth
                            Layout.minimumWidth: Layout.preferredWidth
                            Layout.rightMargin: recentTasksPanel.identityColumnGap
                            color: Theme.text
                            font.family: fontMono
                            font.pixelSize: 12 * recentTasksPanel.tableScale
                            font.bold: true
                            horizontalAlignment: Text.AlignHCenter
                        }
                        Text { text: taskRow.row.pickup; visible: recentTasksPanel.showPickup; Layout.preferredWidth: recentTasksPanel.pickupColumnWidth; Layout.minimumWidth: Layout.preferredWidth; color: Theme.textDim; font.family: fontMono; font.pixelSize: 12 * recentTasksPanel.tableScale; font.bold: true; elide: Text.ElideRight; horizontalAlignment: Text.AlignHCenter }
                        Text { text: taskRow.row.destination; Layout.fillWidth: true; color: Theme.text; font.bold: true; font.pixelSize: 14 * recentTasksPanel.tableScale; elide: Text.ElideRight; horizontalAlignment: Text.AlignHCenter }
                        Text { text: taskRow.row.robot; Layout.preferredWidth: recentTasksPanel.robotColumnWidth; Layout.minimumWidth: Layout.preferredWidth; color: Theme.text; font.family: fontMono; font.pixelSize: 12 * recentTasksPanel.tableScale; font.bold: true; elide: Text.ElideRight; horizontalAlignment: Text.AlignHCenter }
                        Column {
                            Layout.preferredWidth: recentTasksPanel.timeColumnWidth
                            Layout.minimumWidth: Layout.preferredWidth
                            spacing: 1
                            Text {
                                width: parent.width
                                text: Format.formatClock(taskRow.row.created_ms)
                                color: Theme.textSoft
                                font.family: fontMono
                                font.pixelSize: 11 * recentTasksPanel.tableScale
                                elide: Text.ElideRight
                                horizontalAlignment: Text.AlignHCenter
                            }
                            Text {
                                width: parent.width
                                // An end RMF only expects is marked with "~".
                                text: taskRow.row.end_ms
                                      ? "→ " + (taskRow.row.end_estimated ? "~" : "") + Format.formatClock(taskRow.row.end_ms)
                                      : "—"
                                color: Theme.textDim
                                font.family: fontMono
                                font.pixelSize: 10 * recentTasksPanel.tableScale
                                elide: Text.ElideRight
                                horizontalAlignment: Text.AlignHCenter
                            }
                        }
                        Rectangle {
                            Layout.preferredWidth: recentTasksPanel.stateColumnWidth
                            Layout.minimumWidth: Layout.preferredWidth
                            Layout.preferredHeight: 22 * recentTasksPanel.tableScale
                            radius: height / 2
                            property color badgeColor: Theme.taskColor(taskRow.row.display_state)
                            color: Qt.rgba(badgeColor.r, badgeColor.g, badgeColor.b, 0.12)
                            border.color: Qt.rgba(badgeColor.r, badgeColor.g, badgeColor.b, 0.45)
                            border.width: 1

                            Text {
                                anchors.centerIn: parent
                                width: parent.width - 8
                                text: taskRow.row.display_state.toUpperCase()
                                color: parent.badgeColor
                                font.family: fontMono
                                font.pixelSize: Math.max(11, 10 * recentTasksPanel.tableScale)
                                font.bold: true
                                horizontalAlignment: Text.AlignHCenter
                                elide: Text.ElideRight
                            }
                        }
                        Item {
                            Layout.preferredWidth: recentTasksPanel.actionColumnWidth
                            Layout.minimumWidth: Layout.preferredWidth
                            Layout.preferredHeight: 26

                            Button {
                                id: cancelTaskButton
                                anchors.fill: parent
                                visible: taskRow.row.cancellable
                                padding: 0
                                contentItem: Text {
                                    text: "×"
                                    color: Theme.err
                                    font.pixelSize: 14
                                    font.bold: true
                                    horizontalAlignment: Text.AlignHCenter
                                    verticalAlignment: Text.AlignVCenter
                                }
                                background: Rectangle {
                                    radius: 7
                                    color: cancelTaskButton.hovered ? Theme.errHover : "transparent"
                                    border.color: Theme.err
                                    border.width: 1
                                }
                                onClicked: {
                                    if (taskRow.row.rmf_id && taskRow.row.rmf_id.length > 0)
                                        ros.cancel_task(taskRow.row.rmf_id)
                                }
                            }
                        }
                    }
                }
            }

            Column {
                anchors.centerIn: parent
                visible: dashboard.tasks.count === 0
                spacing: 10
                Text {
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: dashboard.taskTotal === 0 ? "NO ACTIVE MISSIONS" : "NO MATCHING TASKS"
                    color: Theme.textDim; font.pixelSize: 12; font.bold: true
                }
                Text {
                    visible: dashboard.taskTotal > 0
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: "Try clearing the search or filter"
                    color: Theme.textDim; opacity: 0.65; font.pixelSize: 10
                }
                Button {
                    visible: dashboard.taskTotal === 0
                    anchors.horizontalCenter: parent.horizontalCenter
                    text: "+  CREATE NEW TASK"
                    implicitHeight: 34
                    leftPadding: 15
                    rightPadding: 15
                    contentItem: Text {
                        text: parent.text
                        color: Theme.textOnAccent
                        font.pixelSize: 11
                        font.bold: true
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        radius: 9
                        color: parent.down ? Theme.accentDark : Theme.accent
                    }
                    onClicked: recentTasksPanel.newTaskRequested()
                }
            }
        }
    }
}
