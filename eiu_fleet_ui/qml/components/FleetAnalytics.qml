import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Presentation-only analytics built from the robot/task arrays already exposed
// to QML. No extra ROS topics or backend state are introduced here.
Rectangle {
    id: root

    property var robots: []
    property var tasks: []
    property var robotsOnline: ({})
    property var waypoints: []
    property string selectedRobotName: ""

    // Approximate route completion for the task associated with the displayed
    // robot. Progress is kept monotonic while RMF replans the remaining path.
    property string trackedTaskKey: ""
    property real initialDestinationDistance: 0
    property real taskDistanceRemaining: -1
    property real taskProgress: 0
    property var taskProgressCache: ({})

    // Visual thresholds are component inputs so the dashboard can tune them in
    // one place without changing the task/progress calculation.
    property real progressWarningThreshold: 0.30
    property real progressSuccessThreshold: 0.70

    readonly property real uiScale: Math.max(1.0, Math.min(1.35, width / 760))
    readonly property var robotNames: buildRobotNames()
    readonly property int selectedRobotIndex: robotNames.indexOf(selectedRobotName)

    readonly property var primaryRobot: selectedRobot()
    readonly property real battery: primaryRobot ? Number(primaryRobot.battery || 0) : 0
    readonly property real posX: primaryRobot ? Number(primaryRobot.x || 0) : 0
    readonly property real posY: primaryRobot ? Number(primaryRobot.y || 0) : 0
    readonly property real yaw: primaryRobot ? Number(primaryRobot.yaw || 0) : 0
    readonly property string robotName: primaryRobot ? primaryRobot.name : "NO ROBOT"
    readonly property string robotStatus: primaryRobot ? primaryRobot.status : "OFFLINE"
    readonly property bool robotOnline: primaryRobot
                                        ? Boolean(robotsOnline[primaryRobot.name]) : false
    readonly property color batteryColor: battery < 20 ? C.err
                                          : (battery < 50 ? C.warn : C.success)
    readonly property int completedCount: countTaskState("completed")
    readonly property int underwayCount: countTaskState("underway")
    readonly property int queuedCount: countTaskState("queued")
    readonly property int stoppedCount: countStoppedTasks()
    readonly property int taskTotal: countRobotTasks()
    readonly property var displayTask: findTaskForRobot()
    readonly property string displayTaskState: displayTask
                                                      ? String(displayTask.state || "queued")
                                                      : ""
    readonly property bool displayTaskTerminal: displayTaskState === "completed"
                                                || displayTaskState === "cancelled"
                                                || displayTaskState === "failed"
    readonly property color displayTaskColor: taskStateColor(displayTaskState)
    readonly property color displayProgressColor: taskProgressColor(
                                                       displayTaskState,
                                                       taskProgress)
    readonly property var taskStats: [
        { "label": "Completed", "value": completedCount, "barColor": C.success },
        { "label": "Underway",  "value": underwayCount,  "barColor": C.cyan },
        { "label": "Queued",    "value": queuedCount,    "barColor": C.warn },
        { "label": "Stopped",   "value": stoppedCount,   "barColor": C.err }
    ]

    function buildRobotNames() {
        var names = []
        for (var i = 0; i < robots.length; ++i) {
            var name = String(robots[i].name || "")
            if (name.length > 0)
                names.push(name)
        }
        return names
    }

    function ensureSelectedRobot() {
        var names = buildRobotNames()
        if (names.length === 0) {
            selectedRobotName = ""
            return
        }
        if (names.indexOf(selectedRobotName) < 0)
            selectedRobotName = names[0]
    }

    function selectedRobot() {
        if (robots.length === 0)
            return null
        for (var i = 0; i < robots.length; ++i) {
            if (String(robots[i].name || "") === selectedRobotName)
                return robots[i]
        }
        return robots[0]
    }

    function taskBelongsToSelectedRobot(task) {
        var robot = selectedRobot()
        if (!robot || !task)
            return false
        if (String(task.robot || "") === String(robot.name || ""))
            return true

        var activeId = String(robot.task || "")
        return activeId.length > 0
                && (String(task.rmf_id || "") === activeId
                    || String(task.id || "") === activeId)
    }

    function countRobotTasks() {
        var count = 0
        for (var i = 0; i < tasks.length; ++i) {
            if (taskBelongsToSelectedRobot(tasks[i]))
                count++
        }
        return count
    }

    function countTaskState(state) {
        var count = 0
        for (var i = 0; i < tasks.length; ++i) {
            if (taskBelongsToSelectedRobot(tasks[i]) && tasks[i].state === state)
                count++
        }
        return count
    }

    function countStoppedTasks() {
        var count = 0
        for (var i = 0; i < tasks.length; ++i) {
            if (taskBelongsToSelectedRobot(tasks[i])
                    && (tasks[i].state === "failed"
                        || tasks[i].state === "cancelled"))
                count++
        }
        return count
    }

    function findTaskForRobot() {
        var robot = selectedRobot()
        if (!robot)
            return null

        var activeId = String(robot.task || "")
        if (activeId.length > 0) {
            for (var i = 0; i < tasks.length; ++i) {
                if (String(tasks[i].rmf_id || "") === activeId
                        || String(tasks[i].id || "") === activeId)
                    return tasks[i]
            }
        }

        // The task table is newest-first. Prefer a task still assigned to this
        // robot, then retain its latest terminal result after RMF clears task_id.
        for (var j = 0; j < tasks.length; ++j) {
            if (tasks[j].robot === robot.name
                    && (tasks[j].state === "queued" || tasks[j].state === "underway"))
                return tasks[j]
        }
        for (var k = 0; k < tasks.length; ++k) {
            if (tasks[k].robot === robot.name)
                return tasks[k]
        }
        return null
    }

    function taskStateColor(state) {
        if (state === "completed")
            return C.success
        if (state === "failed" || state === "cancelled")
            return C.err
        if (state === "queued")
            return C.warn
        return C.cyan
    }

    function taskProgressColor(state, progress) {
        // Terminal/error colors always win over percentage colors.
        if (state === "failed" || state === "cancelled")
            return C.err
        if (state === "completed")
            return C.success
        if (state === "queued")
            return C.warn

        if (progress < progressWarningThreshold)
            return C.err
        if (progress < progressSuccessThreshold)
            return C.warn
        return C.success
    }

    function taskKey(task) {
        if (!task)
            return ""
        return String(task.rmf_id || task.id || "")
    }

    function destinationPoint(robot, task) {
        if (!robot || !task)
            return null

        // Prefer the named destination from nav_graph. It stays fixed even if
        // RMF continuously replans or republishes robot.path.
        var destinationName = String(task.destination || "")
        for (var i = 0; i < waypoints.length; ++i) {
            if (String(waypoints[i].name || "") === destinationName) {
                return {
                    "x": Number(waypoints[i].x || 0),
                    "y": Number(waypoints[i].y || 0)
                }
            }
        }

        // Standalone fallback when no nav_graph waypoint was provided.
        var route = robot.path || []
        if (route.length > 0) {
            var last = route[route.length - 1]
            return { "x": Number(last.x || 0), "y": Number(last.y || 0) }
        }
        return null
    }

    function distanceToDestination(robot, task) {
        var destination = destinationPoint(robot, task)
        if (!destination)
            return -1
        var dx = destination.x - Number(robot.x || 0)
        var dy = destination.y - Number(robot.y || 0)
        return Math.sqrt(dx * dx + dy * dy)
    }

    function updateTaskProgress() {
        var robot = selectedRobot()
        var task = findTaskForRobot()
        if (!robot || !task) {
            trackedTaskKey = ""
            initialDestinationDistance = 0
            taskDistanceRemaining = -1
            taskProgress = 0
            return
        }

        // Keep one progress snapshot per robot/task. Switching the selector
        // away and back must not restart an underway task at zero.
        var key = String(robot.name || "") + "::" + taskKey(task)
        if (key !== trackedTaskKey) {
            trackedTaskKey = key
            var cached = taskProgressCache[key]
            initialDestinationDistance = cached ? Number(cached.initial || 0) : 0
            taskDistanceRemaining = cached && cached.remaining !== undefined
                                    ? Number(cached.remaining) : -1
            taskProgress = cached ? Number(cached.progress || 0) : 0
        }

        var state = String(task.state || "queued")
        if (state === "completed") {
            taskProgress = 1
            taskDistanceRemaining = 0
            taskProgressCache[key] = {
                "initial": initialDestinationDistance,
                "remaining": 0,
                "progress": taskProgress
            }
            return
        }
        if (state === "queued") {
            taskProgress = 0
            taskDistanceRemaining = distanceToDestination(robot, task)
            taskProgressCache[key] = {
                "initial": 0,
                "remaining": taskDistanceRemaining,
                "progress": 0
            }
            return
        }
        if (state === "failed" || state === "cancelled")
            return

        var remaining = distanceToDestination(robot, task)
        if (remaining < 0)
            return
        taskDistanceRemaining = remaining
        if (initialDestinationDistance <= 0)
            initialDestinationDistance = remaining

        var measured = 1 - remaining / Math.max(0.001, initialDestinationDistance)
        taskProgress = Math.max(taskProgress, Math.min(0.98, measured))
        taskProgressCache[key] = {
            "initial": initialDestinationDistance,
            "remaining": taskDistanceRemaining,
            "progress": taskProgress
        }
    }

    function headingDegrees(radians) {
        var degrees = radians * 180 / Math.PI
        return (degrees % 360 + 360) % 360
    }

    onBatteryChanged: batteryGauge.requestPaint()
    onRobotsChanged: {
        ensureSelectedRobot()
        updateTaskProgress()
    }
    onTasksChanged: updateTaskProgress()
    onWaypointsChanged: updateTaskProgress()
    onSelectedRobotNameChanged: {
        updateTaskProgress()
        batteryGauge.requestPaint()
    }
    Component.onCompleted: ensureSelectedRobot()

    radius: 12
    color: "#091827"
    border.color: C.border
    border.width: 1
    clip: true

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: 46

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 15
                anchors.rightMargin: 15
                spacing: 9

                Rectangle {
                    Layout.preferredWidth: 4
                    Layout.preferredHeight: 20
                    radius: 2
                    color: C.cyan
                }
                Text {
                    text: "FLEET ANALYTICS"
                    color: C.text
                    font.pixelSize: 14 * root.uiScale
                    font.bold: true
                    font.letterSpacing: 0.8
                }
                Item { Layout.fillWidth: true }
                Rectangle {
                    Layout.preferredWidth: liveRow.implicitWidth + 18
                    Layout.preferredHeight: 24
                    radius: 12
                    color: Qt.rgba(0.13, 0.84, 0.63, 0.08)
                    border.color: robotOnline ? C.success : C.border
                    border.width: 1

                    Row {
                        id: liveRow
                        anchors.centerIn: parent
                        spacing: 6
                        Rectangle {
                            width: 7; height: 7; radius: 3.5
                            color: robotOnline ? C.success : C.textDim
                            anchors.verticalCenter: parent.verticalCenter
                        }
                        Text {
                            text: robotOnline ? "LIVE DATA" : "WAITING DATA"
                            color: robotOnline ? C.success : C.textDim
                            font.family: fontMono
                            font.pixelSize: 10 * root.uiScale
                            font.bold: true
                        }
                    }
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: C.border
            opacity: 0.65
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.margins: 10
            spacing: 10

            // ── Primary robot telemetry ──────────────────────────────────────
            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.preferredWidth: 310
                radius: 10
                color: C.surface
                border.color: C.border
                border.width: 1

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 8

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 4
                        Text {
                            text: "ROBOT TELEMETRY"
                            color: C.textDim
                            font.pixelSize: 11 * root.uiScale
                            font.bold: true
                            font.letterSpacing: 1.0
                        }
                        // Selector and status badge share this row so the badge
                        // sits level with the combo box, not the label above it.
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8
                            ComboBox {
                                id: robotSelector
                                objectName: "robotSelector"
                                Layout.fillWidth: true
                                Layout.maximumWidth: 260 * root.uiScale
                                Layout.preferredHeight: 34 * Math.min(1.15, root.uiScale)
                                model: root.robotNames
                                currentIndex: root.selectedRobotIndex
                                enabled: count > 0
                                font.family: fontMono
                                font.pixelSize: 18 * root.uiScale
                                font.bold: true
                                onActivated: root.selectedRobotName = currentText

                                contentItem: Text {
                                    leftPadding: 11
                                    rightPadding: 31
                                    text: root.robotName
                                    color: C.text
                                    font: robotSelector.font
                                    verticalAlignment: Text.AlignVCenter
                                    elide: Text.ElideRight
                                }

                                indicator: Text {
                                    x: robotSelector.width - width - 11
                                    y: (robotSelector.height - height) / 2 - 1
                                    text: "▾"
                                    color: robotSelector.count > 1 ? C.cyan : C.textDim
                                    font.pixelSize: 15 * root.uiScale
                                    font.bold: true
                                }

                                background: Rectangle {
                                    radius: 8
                                    color: robotSelector.hovered || robotSelector.popup.visible
                                           ? C.surfaceRaised : C.surfaceAlt
                                    border.color: robotSelector.popup.visible ? C.cyan : C.border
                                    border.width: 1
                                }

                                delegate: ItemDelegate {
                                    required property var modelData
                                    required property int index
                                    width: robotSelector.popup.availableWidth
                                    height: 38 * Math.min(1.15, root.uiScale)
                                    highlighted: robotSelector.highlightedIndex === index

                                    contentItem: RowLayout {
                                        spacing: 8
                                        Rectangle {
                                            Layout.preferredWidth: 8
                                            Layout.preferredHeight: 8
                                            radius: 4
                                            color: Boolean(root.robotsOnline[modelData])
                                                   ? C.success : C.textDim
                                        }
                                        Text {
                                            Layout.fillWidth: true
                                            text: modelData
                                            color: C.text
                                            font.family: fontMono
                                            font.pixelSize: 13 * root.uiScale
                                            font.bold: true
                                            elide: Text.ElideRight
                                            verticalAlignment: Text.AlignVCenter
                                        }
                                        Text {
                                            visible: modelData === root.selectedRobotName
                                            text: "✓"
                                            color: C.cyan
                                            font.pixelSize: 13 * root.uiScale
                                            font.bold: true
                                        }
                                    }

                                    background: Rectangle {
                                        radius: 6
                                        color: highlighted ? C.surfaceRaised : "transparent"
                                    }
                                }

                                popup: Popup {
                                    y: robotSelector.height + 5
                                    width: robotSelector.width
                                    implicitHeight: Math.min(contentItem.implicitHeight + 8,
                                                             230 * root.uiScale)
                                    padding: 4

                                    contentItem: ListView {
                                        clip: true
                                        implicitHeight: contentHeight
                                        model: robotSelector.popup.visible
                                               ? robotSelector.delegateModel : null
                                        currentIndex: robotSelector.highlightedIndex
                                        ScrollIndicator.vertical: ScrollIndicator { }
                                    }

                                    background: Rectangle {
                                        radius: 9
                                        color: C.surfaceAlt
                                        border.color: C.cyan
                                        border.width: 1
                                    }
                                }
                            }
                            Rectangle {
                                Layout.alignment: Qt.AlignVCenter
                                Layout.preferredWidth: statusText.implicitWidth + 18
                                Layout.preferredHeight: 25
                                radius: 8
                                color: "transparent"
                                border.color: root.primaryRobot
                                              ? C.cyan : C.border
                                border.width: 1
                                Text {
                                    id: statusText
                                    anchors.centerIn: parent
                                    text: root.robotStatus
                                    color: root.primaryRobot ? C.cyan : C.textDim
                                    font.family: fontMono
                                    font.pixelSize: 10 * root.uiScale
                                    font.bold: true
                                }
                            }
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        spacing: 12

                        Item { Layout.fillWidth: true }

                        Item {
                            Layout.preferredWidth: 96 * Math.min(1.2, root.uiScale)
                            Layout.preferredHeight: Layout.preferredWidth

                            Canvas {
                                id: batteryGauge
                                anchors.fill: parent
                                antialiasing: true
                                Component.onCompleted: requestPaint()
                                onPaint: {
                                    var ctx = getContext("2d")
                                    ctx.reset()
                                    var cx = width / 2
                                    var cy = height / 2
                                    var radius = Math.min(width, height) / 2 - 9
                                    var start = -Math.PI / 2

                                    ctx.lineCap = "round"
                                    ctx.lineWidth = 9
                                    ctx.strokeStyle = C.surfaceRaised
                                    ctx.beginPath()
                                    ctx.arc(cx, cy, radius, 0, Math.PI * 2)
                                    ctx.stroke()

                                    ctx.strokeStyle = root.batteryColor
                                    ctx.beginPath()
                                    ctx.arc(cx, cy, radius, start,
                                            start + Math.PI * 2 * Math.max(
                                                0, Math.min(1, root.battery / 100)))
                                    ctx.stroke()
                                }
                            }
                            Column {
                                anchors.centerIn: parent
                                spacing: -2
                                Text {
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    text: root.primaryRobot ? root.battery.toFixed(0) + "%" : "—"
                                    color: root.batteryColor
                                    font.family: fontMono
                                    font.pixelSize: 21 * root.uiScale
                                    font.bold: true
                                }
                                Text {
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    text: "BATTERY"
                                    color: C.textDim
                                    font.pixelSize: 9 * root.uiScale
                                    font.bold: true
                                    font.letterSpacing: 0.8
                                }
                            }
                        }

                        GridLayout {
                            columns: 2
                            columnSpacing: 9
                            rowSpacing: 5

                            Text { text: "POSITION X"; color: C.textDim; font.pixelSize: 10 * root.uiScale; font.bold: true }
                            Text { text: root.primaryRobot ? root.posX.toFixed(2) + " m" : "—"; color: C.text; font.family: fontMono; font.pixelSize: 13 * root.uiScale; font.bold: true }
                            Text { text: "POSITION Y"; color: C.textDim; font.pixelSize: 10 * root.uiScale; font.bold: true }
                            Text { text: root.primaryRobot ? root.posY.toFixed(2) + " m" : "—"; color: C.text; font.family: fontMono; font.pixelSize: 13 * root.uiScale; font.bold: true }
                            Text { text: "HEADING"; color: C.textDim; font.pixelSize: 10 * root.uiScale; font.bold: true }
                            Text { text: root.primaryRobot ? root.headingDegrees(root.yaw).toFixed(0) + "°" : "—"; color: C.cyan; font.family: fontMono; font.pixelSize: 13 * root.uiScale; font.bold: true }
                            Text { text: "LEVEL"; color: C.textDim; font.pixelSize: 10 * root.uiScale; font.bold: true }
                            Text { text: root.primaryRobot ? root.primaryRobot.level : "—"; color: C.text; font.family: fontMono; font.pixelSize: 13 * root.uiScale; font.bold: true }
                        }

                        Item { Layout.fillWidth: true }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 84 * Math.min(1.15, root.uiScale)
                        radius: 9
                        color: C.surfaceAlt
                        border.color: root.displayTask
                                      ? Qt.rgba(root.displayTaskColor.r,
                                                root.displayTaskColor.g,
                                                root.displayTaskColor.b, 0.42)
                                      : C.border
                        border.width: 1

                        ColumnLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 10
                            anchors.rightMargin: 10
                            anchors.topMargin: 7
                            anchors.bottomMargin: 7
                            spacing: 3

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 7

                                Text {
                                    text: root.displayTaskTerminal ? "LAST TASK" : "CURRENT TASK"
                                    color: C.textDim
                                    font.pixelSize: 10 * root.uiScale
                                    font.bold: true
                                    font.letterSpacing: 0.8
                                }
                                Item { Layout.fillWidth: true }
                                Rectangle {
                                    visible: root.displayTask !== null
                                    Layout.preferredWidth: taskStateText.implicitWidth + 16
                                    Layout.preferredHeight: 21 * Math.min(1.15, root.uiScale)
                                    radius: height / 2
                                    color: Qt.rgba(root.displayTaskColor.r,
                                                   root.displayTaskColor.g,
                                                   root.displayTaskColor.b, 0.10)
                                    border.color: root.displayTaskColor
                                    border.width: 1

                                    Text {
                                        id: taskStateText
                                        anchors.centerIn: parent
                                        text: root.displayTaskState.toUpperCase()
                                        color: root.displayTaskColor
                                        font.family: fontMono
                                        font.pixelSize: 9 * root.uiScale
                                        font.bold: true
                                    }
                                }
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 7
                                Text {
                                    Layout.fillWidth: true
                                    text: root.displayTask
                                          ? String(root.displayTask.destination
                                                   || root.taskKey(root.displayTask))
                                          : "No task assigned"
                                    color: root.displayTask ? C.text : C.textDim
                                    font.pixelSize: 14 * root.uiScale
                                    font.bold: true
                                    elide: Text.ElideRight
                                }
                                Text {
                                    text: !root.displayTask ? "—"
                                          : (root.displayTaskState === "queued" ? "WAIT"
                                             : Math.round(root.taskProgress * 100) + "%")
                                    color: root.displayTask ? root.displayProgressColor : C.textDim
                                    font.family: fontMono
                                    font.pixelSize: 13 * root.uiScale
                                    font.bold: true
                                }
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 7

                                Rectangle {
                                    id: taskProgressTrack
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 8
                                    radius: height / 2
                                    color: C.surfaceRaised

                                    Rectangle {
                                        width: {
                                            if (!root.displayTask)
                                                return 0
                                            var measured = parent.width * root.taskProgress
                                            if (root.displayTaskState === "queued"
                                                    || root.displayTaskState === "underway"
                                                    || root.displayTaskState === "cancelled"
                                                    || root.displayTaskState === "failed")
                                                return Math.max(7, measured)
                                            return measured
                                        }
                                        height: parent.height
                                        radius: parent.radius
                                        color: root.displayProgressColor

                                        Behavior on width {
                                            NumberAnimation {
                                                duration: 300
                                                easing.type: Easing.OutCubic
                                            }
                                        }
                                        Behavior on color {
                                            ColorAnimation { duration: 180 }
                                        }
                                    }
                                }

                                Text {
                                    Layout.preferredWidth: 78 * Math.min(1.15, root.uiScale)
                                    text: root.displayTask && root.displayTask.error
                                          ? root.displayTask.error
                                          : (root.displayTaskState === "completed"
                                             ? "ARRIVED"
                                             : (root.taskDistanceRemaining >= 0
                                                ? root.taskDistanceRemaining.toFixed(1)
                                                  + " m LEFT"
                                                : root.taskKey(root.displayTask)))
                                    color: root.displayTask && root.displayTask.error
                                           ? C.err : root.displayProgressColor
                                    font.family: fontMono
                                    font.pixelSize: 8 * root.uiScale
                                    horizontalAlignment: Text.AlignRight
                                    elide: Text.ElideMiddle
                                }
                            }
                        }
                    }
                }
            }

            // ── Task distribution chart ─────────────────────────────────────
            Rectangle {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.preferredWidth: 360
                radius: 10
                color: C.surface
                border.color: C.border
                border.width: 1

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 7

                    RowLayout {
                        Layout.fillWidth: true
                        ColumnLayout {
                            spacing: 1
                            Text {
                                text: "TASK DISTRIBUTION"
                                color: C.textDim
                                font.pixelSize: 11 * root.uiScale
                                font.bold: true
                                font.letterSpacing: 1.0
                            }
                            Text {
                                text: root.robotName
                                color: C.cyan
                                font.family: fontMono
                                font.pixelSize: 10 * root.uiScale
                                font.bold: true
                                elide: Text.ElideRight
                                Layout.maximumWidth: 180 * root.uiScale
                            }
                        }
                        Item { Layout.fillWidth: true }
                        Text {
                            text: root.taskTotal + " TOTAL"
                            color: C.cyan
                            font.family: fontMono
                            font.pixelSize: 11 * root.uiScale
                            font.bold: true
                        }
                    }

                    Repeater {
                        model: root.taskStats

                        delegate: RowLayout {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            spacing: 8

                            Text {
                                Layout.minimumWidth: implicitWidth
                                Layout.preferredWidth: Math.max(
                                    implicitWidth,
                                    82 * Math.min(1.15, root.uiScale))
                                text: modelData.label
                                color: C.text
                                font.pixelSize: 11 * root.uiScale
                                font.bold: true
                            }
                            Rectangle {
                                id: chartTrack
                                Layout.fillWidth: true
                                Layout.preferredHeight: 9
                                radius: 4.5
                                color: C.surfaceRaised

                                Rectangle {
                                    width: modelData.value > 0
                                           ? Math.max(5, parent.width * modelData.value
                                                      / Math.max(1, root.taskTotal))
                                           : 0
                                    height: parent.height
                                    radius: parent.radius
                                    color: modelData.barColor

                                    Behavior on width {
                                        NumberAnimation { duration: 280; easing.type: Easing.OutCubic }
                                    }
                                }
                            }
                            Text {
                                Layout.preferredWidth: 24
                                text: modelData.value
                                color: modelData.barColor
                                font.family: fontMono
                                font.pixelSize: 13 * root.uiScale
                                font.bold: true
                                horizontalAlignment: Text.AlignRight
                            }
                        }
                    }

                    Text {
                        Layout.fillWidth: true
                        visible: root.taskTotal === 0
                        text: root.primaryRobot
                              ? "No task history for " + root.robotName
                              : "Waiting for robot telemetry"
                        color: C.textDim
                        font.pixelSize: 10 * root.uiScale
                        horizontalAlignment: Text.AlignHCenter
                    }
                }
            }
        }
    }
}
