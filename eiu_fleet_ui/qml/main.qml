import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import "components"

ApplicationWindow {
    id: root
    visible: true
    width: 1280
    height: 800
    minimumWidth: 1000
    minimumHeight: 640
    title: "EIU Fleet Management UI"
    color: C.bg

    property var waypoints: []
    property var wpNames:   []
    property var robots:    []
    property var tasks:     []

    function reloadRobots() { root.robots = JSON.parse(ros.robotsJson) }
    function reloadTasks()  { root.tasks  = JSON.parse(ros.tasksJson) }

    // Màu theo trạng thái robot (cột Status)
    function statusColor(s) {
        if (s === "MOVING" || s === "DOCKING" || s === "GOING_HOME" || s === "WORKING") return C.blue
        if (s === "CHARGING")                                        return C.accent
        if (s === "EMERGENCY" || s === "ERROR")                      return C.err
        if (s === "PAUSED" || s === "WAITING")                       return C.warn
        return C.textDim
    }

    // Màu theo trạng thái task (cột State)
    function taskColor(s) {
        if (s === "completed")                         return C.accent
        if (s === "failed" || s === "cancelled")       return C.err
        if (s === "queued")                            return C.warn
        if (s === "underway")                          return C.blue
        // fallback cho các state chứa keyword
        if (s.indexOf("complet") >= 0)                return C.accent
        if (s.indexOf("fail") >= 0 || s.indexOf("cancel") >= 0) return C.err
        if (s.indexOf("queue") >= 0 || s.indexOf("stale") >= 0) return C.warn
        return C.blue
    }

    Component.onCompleted: {
        var wps = JSON.parse(mapProv.wpJson)
        root.waypoints = wps
        var names = []
        for (var i = 0; i < wps.length; ++i)
            if (wps[i].name && wps[i].name.length > 0) names.push(wps[i].name)
        root.wpNames = names
        reloadRobots()
        reloadTasks()
    }

    Connections {
        target: ros
        function onRobotsChanged() { root.reloadRobots() }
        function onTasksChanged()  { root.reloadTasks() }
    }

    NewTaskDialog {
        id: taskDialog
        places: root.wpNames
        anchors.centerIn: parent
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ── Top bar ──────────────────────────────────────────────────────────
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 56
            color: C.surface

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 16
                anchors.rightMargin: 16
                spacing: 12

                Image {
                    source: "../icons/eiu.png"
                    Layout.preferredHeight: 22
                    Layout.preferredWidth:  77
                    fillMode: Image.PreserveAspectFit
                    smooth: true
                }
                Text {
                    text: "Fleet Management"
                    font.pixelSize: 15; font.bold: true; color: C.text
                }

                Item { Layout.fillWidth: true }

                Row {
                    spacing: 6
                    Rectangle {
                        width: 8; height: 8; radius: 4
                        color: ros.rmfOnline ? C.accent : C.err
                        anchors.verticalCenter: parent.verticalCenter
                    }
                    Text {
                        text: ros.rmfOnline ? "RMF online" : "RMF offline"
                        font.pixelSize: 11; color: C.textDim
                        anchors.verticalCenter: parent.verticalCenter
                    }
                }
                Row {
                    spacing: 6
                    Rectangle {
                        width: 8; height: 8; radius: 4
                        color: mqtt.connected ? C.accent : C.err
                        anchors.verticalCenter: parent.verticalCenter
                    }
                    Text {
                        text: mqtt.connected ? "Broker online" : "Broker offline"
                        font.pixelSize: 11; color: C.textDim
                        anchors.verticalCenter: parent.verticalCenter
                    }
                }

                Button {
                    id: newTaskBtn
                    text: "+ New Task"
                    Layout.leftMargin: 8
                    implicitHeight: 36
                    leftPadding: 14; rightPadding: 14
                    contentItem: Text {
                        text: newTaskBtn.text; color: "#ffffff"
                        font.pixelSize: 13; font.bold: true
                        verticalAlignment: Text.AlignVCenter
                        horizontalAlignment: Text.AlignHCenter
                    }
                    background: Rectangle {
                        radius: 8; color: newTaskBtn.down ? C.accentDark : C.accent
                    }
                    onClicked: taskDialog.open()
                }
            }
        }

        // ── Nội dung: 2 card (robot+task) bên trái = 4, map bên phải = 6 ──────
        Item {
            id: contentArea
            Layout.fillWidth: true
            Layout.fillHeight: true

            // ════════════ LEFT (4 phần) ════════════
            ColumnLayout {
                id: leftPanel
                anchors.left:   parent.left
                anchors.top:    parent.top
                anchors.bottom: parent.bottom
                anchors.margins: 12
                width: (parent.width - 36) * 0.5
                spacing: 10

                // ── Card ROBOT ──
                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.preferredHeight: 100
                    radius: 10; color: C.surface
                    border.color: C.border; border.width: 1
                    clip: true

                    Column {
                        anchors.fill: parent

                        // thanh tiêu đề màu accent
                        Rectangle {
                            width: parent.width; height: 34; color: C.accent
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                leftPadding: 12
                                text: "Active Robots  (" + root.robots.length + ")"
                                font.pixelSize: 13; font.bold: true; color: "#ffffff"
                            }
                        }
                        // header cột
                        Rectangle {
                            width: parent.width; height: 28; color: C.surfaceAlt
                            Row {
                                width: parent.width
                                anchors.verticalCenter: parent.verticalCenter
                                Text { width: parent.width*0.16; leftPadding: 10; elide: Text.ElideRight; font.pixelSize: 11; font.bold: true; color: C.textDim; text: "Name" }
                                Text { width: parent.width*0.14; leftPadding: 6;  elide: Text.ElideRight; font.pixelSize: 11; font.bold: true; color: C.textDim; text: "Fleet" }
                                Text { width: parent.width*0.17; leftPadding: 6;  elide: Text.ElideRight; font.pixelSize: 11; font.bold: true; color: C.textDim; text: "Finish" }
                                Text { width: parent.width*0.10; leftPadding: 6;  elide: Text.ElideRight; font.pixelSize: 11; font.bold: true; color: C.textDim; text: "Level" }
                                Text { width: parent.width*0.13; leftPadding: 6;  elide: Text.ElideRight; font.pixelSize: 11; font.bold: true; color: C.textDim; text: "Battery" }
                                Text { width: parent.width*0.16; leftPadding: 6;  elide: Text.ElideRight; font.pixelSize: 11; font.bold: true; color: C.textDim; text: "Updated" }
                                Text { width: parent.width*0.14; leftPadding: 6;  elide: Text.ElideRight; font.pixelSize: 11; font.bold: true; color: C.textDim; text: "Status" }
                            }
                        }
                        // rows
                        ListView {
                            width: parent.width; height: parent.height - 34 - 28
                            clip: true; model: root.robots
                            boundsBehavior: Flickable.StopAtBounds
                            delegate: Rectangle {
                                width: ListView.view.width; height: 32
                                color: index % 2 === 1 ? C.surfaceAlt : "transparent"
                                Row {
                                    width: parent.width
                                    anchors.verticalCenter: parent.verticalCenter
                                    Text { width: parent.width*0.16; leftPadding: 10; elide: Text.ElideRight; font.pixelSize: 11; font.bold: true; color: C.text;    text: modelData.name }
                                    Text { width: parent.width*0.14; leftPadding: 6;  elide: Text.ElideRight; font.pixelSize: 11; color: C.textDim; text: modelData.fleet }
                                    Text { width: parent.width*0.17; leftPadding: 6;  elide: Text.ElideRight; font.pixelSize: 11; color: C.textDim; text: (modelData.finish && modelData.finish.length) ? modelData.finish : "—" }
                                    Text { width: parent.width*0.10; leftPadding: 6;  elide: Text.ElideRight; font.pixelSize: 11; color: C.text;    text: modelData.level }
                                    Text { width: parent.width*0.13; leftPadding: 6;  elide: Text.ElideRight; font.pixelSize: 11; color: C.text;    text: modelData.battery.toFixed(0) + "%" }
                                    Text { width: parent.width*0.16; leftPadding: 6;  elide: Text.ElideRight; font.pixelSize: 11; color: C.textDim; text: modelData.updated }
                                    Text { width: parent.width*0.14; leftPadding: 6;  elide: Text.ElideRight; font.pixelSize: 11; font.bold: true; color: root.statusColor(modelData.status); text: modelData.status }
                                }
                            }
                        }
                    }
                    Text {
                        anchors.centerIn: parent; visible: root.robots.length === 0
                        width: parent.width - 24
                        horizontalAlignment: Text.AlignHCenter; wrapMode: Text.WordWrap
                        text: ros.rmfOnline ? "Fleet chưa có robot" : "Đang chờ RMF (/fleet_states)…"
                        font.pixelSize: 12; color: C.textDim
                    }
                }

                // ── Card TASK ──
                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.preferredHeight: 100
                    radius: 10; color: C.surface
                    border.color: C.border; border.width: 1
                    clip: true

                    Column {
                        anchors.fill: parent

                        Rectangle {
                            width: parent.width; height: 34; color: C.accent
                            Text {
                                anchors.verticalCenter: parent.verticalCenter
                                leftPadding: 12
                                text: "Tasks  (" + root.tasks.length + ")"
                                font.pixelSize: 13; font.bold: true; color: "#ffffff"
                            }
                        }
                        Rectangle {
                            width: parent.width; height: 28; color: C.surfaceAlt
                            Row {
                                width: parent.width
                                anchors.verticalCenter: parent.verticalCenter
                                Text { width: parent.width*0.12; leftPadding: 10; elide: Text.ElideRight; font.pixelSize: 11; font.bold: true; color: C.textDim; text: "Date" }
                                Text { width: parent.width*0.11; leftPadding: 6;  elide: Text.ElideRight; font.pixelSize: 11; font.bold: true; color: C.textDim; text: "Req." }
                                Text { width: parent.width*0.10; leftPadding: 6;  elide: Text.ElideRight; font.pixelSize: 11; font.bold: true; color: C.textDim; text: "Pickup" }
                                Text { width: parent.width*0.14; leftPadding: 6;  elide: Text.ElideRight; font.pixelSize: 11; font.bold: true; color: C.textDim; text: "Dest." }
                                Text { width: parent.width*0.13; leftPadding: 6;  elide: Text.ElideRight; font.pixelSize: 11; font.bold: true; color: C.textDim; text: "Robot" }
                                Text { width: parent.width*0.11; leftPadding: 6;  elide: Text.ElideRight; font.pixelSize: 11; font.bold: true; color: C.textDim; text: "Start" }
                                Text { width: parent.width*0.10; leftPadding: 6;  elide: Text.ElideRight; font.pixelSize: 11; font.bold: true; color: C.textDim; text: "End" }
                                Text { width: parent.width*0.12; leftPadding: 6;  elide: Text.ElideRight; font.pixelSize: 11; font.bold: true; color: C.textDim; text: "State" }
                                Text { width: parent.width*0.07; leftPadding: 6;  elide: Text.ElideRight; font.pixelSize: 11; font.bold: true; color: C.textDim; text: "" }
                            }
                        }
                        ListView {
                            width: parent.width; height: parent.height - 34 - 28
                            clip: true; model: root.tasks
                            boundsBehavior: Flickable.StopAtBounds
                            delegate: Rectangle {
                                width: ListView.view.width; height: 32
                                color: index % 2 === 1 ? C.surfaceAlt : "transparent"
                                Row {
                                    width: parent.width
                                    anchors.verticalCenter: parent.verticalCenter
                                    Text { width: parent.width*0.12; leftPadding: 10; elide: Text.ElideRight; font.pixelSize: 11; color: C.text;    text: modelData.date }
                                    Text { width: parent.width*0.11; leftPadding: 6;  elide: Text.ElideRight; font.pixelSize: 11; color: C.textDim; text: modelData.requester }
                                    Text { width: parent.width*0.10; leftPadding: 6;  elide: Text.ElideRight; font.pixelSize: 11; color: C.textDim; text: modelData.pickup }
                                    Text { width: parent.width*0.14; leftPadding: 6;  elide: Text.ElideRight; font.pixelSize: 11; color: C.text;    text: modelData.destination }
                                    Text { width: parent.width*0.13; leftPadding: 6;  elide: Text.ElideRight; font.pixelSize: 11; color: C.text;    text: modelData.robot }
                                    Text { width: parent.width*0.11; leftPadding: 6;  elide: Text.ElideRight; font.pixelSize: 11; color: C.textDim; text: modelData.start }
                                    Text { width: parent.width*0.10; leftPadding: 6;  elide: Text.ElideRight; font.pixelSize: 11; color: C.textDim; text: modelData.end }
                                    Text { width: parent.width*0.12; leftPadding: 6;  elide: Text.ElideRight; font.pixelSize: 11; font.bold: true; color: root.taskColor(modelData.state); text: modelData.state }
                                    Item {
                                        width: parent.width*0.07; height: 32
                                        visible: modelData.state === "queued" || modelData.state === "underway"
                                        Rectangle {
                                            anchors.centerIn: parent
                                            width: 22; height: 22; radius: 4
                                            color: cancelMa.containsMouse ? "#c0392b" : "transparent"
                                            border.color: cancelMa.containsMouse ? "#c0392b" : C.err
                                            border.width: 1
                                            Text {
                                                anchors.centerIn: parent
                                                text: "✕"
                                                font.pixelSize: 11; font.bold: true
                                                color: C.err
                                            }
                                            MouseArea {
                                                id: cancelMa
                                                anchors.fill: parent
                                                hoverEnabled: true
                                                cursorShape: Qt.PointingHandCursor
                                                onClicked: {
                                                    if (modelData.rmf_id && modelData.rmf_id.length > 0)
                                                        ros.cancel_task(modelData.rmf_id)
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                    Text {
                        anchors.centerIn: parent; visible: root.tasks.length === 0
                        width: parent.width - 24
                        horizontalAlignment: Text.AlignHCenter; wrapMode: Text.WordWrap
                        text: "Chưa có task — bấm  + New Task  để dispatch"
                        font.pixelSize: 12; color: C.textDim
                    }
                }
            }

            // ════════════ RIGHT: MAP (6 phần) ════════════
            Rectangle {
                id: mapHeader
                anchors.left:  leftPanel.right
                anchors.leftMargin: 12
                anchors.right: parent.right
                anchors.rightMargin: 12
                anchors.top:   parent.top
                anchors.topMargin: 12
                height: 34
                radius: 8
                color: C.accent
                Text {
                    anchors.verticalCenter: parent.verticalCenter
                    leftPadding: 12
                    text: "Map"
                    font.pixelSize: 13; font.bold: true; color: "#ffffff"
                }
            }
            Loader {
                anchors.left:  leftPanel.right
                anchors.leftMargin: 12
                anchors.right: parent.right
                anchors.rightMargin: 12
                anchors.top:   mapHeader.bottom
                anchors.topMargin: 8
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 12
                source: Qt.resolvedUrl("pages/MapPage.qml")
            }
        }
    }
}
