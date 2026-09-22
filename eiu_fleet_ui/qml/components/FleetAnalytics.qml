import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Summarize fleet metrics from data already available to QML.
Rectangle {
    id: root

    property var robots: []
    property var tasks: []
    property var robotsOnline: ({})
    property var waypoints: []
    property string selectedRobotName: ""
    // True once the operator has picked a robot.
    property bool userPicked: false
    property var telemetry: ({})
    property real nowTick: 0

    // Estimate route progress without dropping the value when RMF replans.
    property string trackedTaskKey: ""
    property real initialDestinationDistance: 0
    property real taskDistanceRemaining: -1
    property real taskProgress: 0
    property var taskProgressCache: ({})


    // Available height for the analytics panel.
    property real availableHeight: 1e9
    readonly property real widthScale: Math.max(0.7, Math.min(1.35, width / 760))
    property real uiScale: widthScale
    // Smooths out the residual adjustments fitScale() still makes while settling.
    Behavior on uiScale {
        NumberAnimation { duration: 120; easing.type: Easing.OutCubic }
    }
    implicitHeight: rootColumn.implicitHeight

    // uiScale resizing rootColumn's content changes its implicitHeight, which re-triggers
    // this -- pixel rounding and non-linear content can keep that from ever settling, so
    // cap the correction chain instead of adjusting forever.
    property int _fitSettleGuard: 0

    // Scale the content to fit the panel height.
    function fitScale() {
        var need = rootColumn.implicitHeight
        if (need <= 0)
            return
        var s = Math.max(0.7, Math.min(widthScale, uiScale * availableHeight / need))
        // A wider dead-band means fewer correction attempts before this settles;
        // a 1% scale gap is a sub-pixel font-size difference, invisible either way.
        if (Math.abs(s - uiScale) <= 0.01 || _fitSettleGuard >= 4) {
            _fitSettleGuard = 0
            return
        }
        _fitSettleGuard++
        uiScale = s
    }
    onWidthScaleChanged: Qt.callLater(fitScale)
    onAvailableHeightChanged: Qt.callLater(fitScale)
    readonly property var robotNames: buildRobotNames()
    readonly property int selectedRobotIndex: robotNames.indexOf(selectedRobotName)

    readonly property var primaryRobot: selectedRobot()
    // PENDING SYNC placeholders default x/y/yaw/battery to 0 -- these gate "--" display.
    readonly property bool hasTele: primaryRobot ? !!(telemetry[primaryRobot.name]) : false
    readonly property bool hasPose: !!(primaryRobot && primaryRobot.rmfSynced)
    readonly property bool hasBatteryReading: !!(primaryRobot && primaryRobot.hasBattery)
    readonly property real battery: primaryRobot ? Number(primaryRobot.battery || 0) : 0
    readonly property real posX: primaryRobot ? Number(primaryRobot.x || 0) : 0
    readonly property real posY: primaryRobot ? Number(primaryRobot.y || 0) : 0
    readonly property real yaw: primaryRobot ? Number(primaryRobot.yaw || 0) : 0
    readonly property real speed: primaryRobot
                                   ? Number((telemetry[primaryRobot.name] || {}).speed || 0) : 0
    // Distance traveled in the current route leg.
    readonly property real distanceSinceLastNode: primaryRobot
                                   ? Number((telemetry[primaryRobot.name] || {}).distance_since_last_node || 0) : 0
    readonly property string operatingMode: primaryRobot
                                   ? String((telemetry[primaryRobot.name] || {}).operating_mode || "AUTOMATIC") : "AUTOMATIC"
    readonly property var safety: primaryRobot ? (telemetry[primaryRobot.name] || {}).safety : null
    // AGV's reported loads (VDA5050 state.loads).
    readonly property var robotLoads: primaryRobot
                                   ? (telemetry[primaryRobot.name] || {}).loads || [] : []
    readonly property bool deliveryUnderway: !!(displayTask && displayTask.category === "delivery"
                                                 && displayTaskState === "underway")
    readonly property bool eStopActive: !!(safety && safety.triggered)
    readonly property string eStopLabel: {
        if (!safety) return ""
        if (safety.e_stop && safety.e_stop !== "NONE") return safety.e_stop
        if (safety.field_violation) return "FIELD VIOLATION"
        return ""
    }
    readonly property string robotName: primaryRobot ? primaryRobot.name : "NO ROBOT"
    readonly property string robotStatus: primaryRobot ? primaryRobot.status : "OFFLINE"
    readonly property bool robotOnline: primaryRobot
                                        ? Boolean(robotsOnline[primaryRobot.name]) : false
    // Selected but disconnected -- telemetry below is frozen at its last received value.
    readonly property bool offline: !!primaryRobot && !robotOnline
    readonly property real lastRx: primaryRobot
                                    ? Number((telemetry[primaryRobot.name] || {}).last_rx || 0) : 0

    // VDA5050 order/action state, as parsed from the robot's state topic.
    readonly property string orderId: primaryRobot
                                   ? String((telemetry[primaryRobot.name] || {}).order_id || "") : ""
    readonly property var orderUpdateId: primaryRobot
                                   ? (telemetry[primaryRobot.name] || {}).order_update_id : null
    readonly property var orderNodeStates: primaryRobot
                                   ? ((telemetry[primaryRobot.name] || {}).node_states || []) : []
    readonly property var orderActionStates: primaryRobot
                                   ? ((telemetry[primaryRobot.name] || {}).action_states || []) : []
    readonly property string orderLastNodeId: primaryRobot
                                   ? String((telemetry[primaryRobot.name] || {}).last_node_id || "") : ""
    readonly property var orderLastNodeSeq: primaryRobot
                                   ? (telemetry[primaryRobot.name] || {}).last_node_sequence_id : null
    readonly property var orderDetail: primaryRobot
                                   ? ((telemetry[primaryRobot.name] || {}).order_detail || null) : null
    readonly property bool hasOrder: hasTele && orderId !== ""
    readonly property color batteryColor: !hasBatteryReading ? C.textDim
                                          : (battery < 20 ? C.err
                                             : (battery < 50 ? C.warn : C.success))
    readonly property int completedCount: countTaskState("completed")
    readonly property int underwayCount: countTaskState("underway")
    readonly property int queuedCount: countTaskState("queued")
    readonly property int cancelledCount: countTaskState("cancelled")
    readonly property int failedCount: countTaskState("failed")
    readonly property int taskTotal: countRobotTasks()
    readonly property var displayTask: findTaskForRobot()
    readonly property string displayTaskState: displayTask
                                                      ? String(displayTask.state || "queued")
                                                      : ""
    readonly property bool displayTaskTerminal: displayTaskState === "completed"
                                                || displayTaskState === "cancelled"
                                                || displayTaskState === "failed"
    readonly property color displayTaskColor: taskStateColor(displayTaskState)
    // Count cancelled and failed tasks separately.
    readonly property var taskStats: [
        { "label": "Completed", "value": completedCount, "barColor": C.success },
        { "label": "Underway",  "value": underwayCount,  "barColor": C.cyan },
        { "label": "Queued",    "value": queuedCount,    "barColor": C.warn },
        { "label": "Cancelled", "value": cancelledCount, "barColor": C.textDim },
        { "label": "Failed",    "value": failedCount,    "barColor": C.err }
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

    // Prefer an online robot; keep the current one while it is online.
    function autoPickOnline() {
        if (userPicked || robotsOnline[selectedRobotName])
            return
        var names = buildRobotNames()
        for (var i = 0; i < names.length; ++i) {
            if (robotsOnline[names[i]]) {
                selectedRobotName = names[i]
                return
            }
        }
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

        // Prefer the active task, then the robot's latest finished task.
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
        if (state === "failed")
            return C.err
        if (state === "cancelled")
            return C.textDim
        if (state === "queued")
            return C.warn
        return C.cyan   // Active task
    }

    function taskKey(task) {
        if (!task)
            return ""
        return String(task.rmf_id || task.id || "")
    }

    function destinationPoint(robot, task) {
        if (!robot || !task)
            return null

        // Prefer the named destination from the navigation graph.
        var destinationName = String(task.destination || "")
        for (var i = 0; i < waypoints.length; ++i) {
            if (String(waypoints[i].name || "") === destinationName) {
                return {
                    "x": Number(waypoints[i].x || 0),
                    "y": Number(waypoints[i].y || 0)
                }
            }
        }

        // Use destination coordinates when no waypoint name is available.
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

        // Retain progress by robot and task when the selected robot changes.
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
    onRobotsOnlineChanged: autoPickOnline()
    onRobotsChanged: {
        ensureSelectedRobot()
        autoPickOnline()
        updateTaskProgress()
    }
    onTasksChanged: updateTaskProgress()
    onWaypointsChanged: updateTaskProgress()
    onSelectedRobotNameChanged: {
        updateTaskProgress()
        batteryGauge.requestPaint()
    }
    Component.onCompleted: {
        ensureSelectedRobot()
        autoPickOnline()
    }

    radius: 12
    color: "#091827"
    border.color: C.border
    border.width: 1
    clip: true

    ColumnLayout {
        id: rootColumn
        anchors.fill: parent
        spacing: 0
        onImplicitHeightChanged: Qt.callLater(root.fitScale)

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

            // Telemetry for the selected robot.
            Rectangle {
                id: telemetryPanel
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.preferredWidth: 310
                Layout.minimumWidth: 150
                implicitHeight: telemetryColumn.implicitHeight + 24
                radius: 10
                color: C.surface
                border.color: C.border
                border.width: 1
                clip: true

                ColumnLayout {
                    id: telemetryColumn
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 13

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
                        // Place the robot selector and status badge on one row.
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8
                            ComboBox {
                                id: robotSelector
                                objectName: "robotSelector"
                                Layout.fillWidth: true
                                Layout.minimumWidth: 64
                                Layout.maximumWidth: 260 * root.uiScale
                                Layout.preferredHeight: 34 * Math.min(1.15, root.uiScale)
                                model: root.robotNames
                                currentIndex: root.selectedRobotIndex
                                enabled: count > 0
                                font.family: fontMono
                                font.pixelSize: 18 * root.uiScale
                                font.bold: true
                                onActivated: {
                                    root.userPicked = true
                                    root.selectedRobotName = currentText
                                }

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
                                    border.color: robotSelector.popup.visible ? C.cyanBright : C.border
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
                                        border.color: C.cyanBright
                                        border.width: 1
                                    }
                                }
                            }
                            Rectangle {
                                Layout.alignment: Qt.AlignVCenter
                                Layout.preferredWidth: statusText.implicitWidth + 18
                                Layout.minimumWidth: statusText.implicitWidth + 10
                                Layout.preferredHeight: 25
                                radius: 8
                                color: root.eStopActive ? C.err : "transparent"
                                border.color: root.eStopActive ? C.err
                                              : (root.offline ? C.err
                                                 : (root.primaryRobot ? C.cyan : C.border))
                                border.width: 1
                                Text {
                                    id: statusText
                                    anchors.centerIn: parent
                                    text: root.eStopActive ? "E-STOP" : (root.offline ? "OFFLINE" : root.robotStatus)
                                    color: root.eStopActive ? C.text
                                           : (root.offline ? C.err
                                              : (root.primaryRobot ? C.cyan : C.textDim))
                                    font.family: fontMono
                                    font.pixelSize: 10 * root.uiScale
                                    font.bold: true
                                }
                            }
                        }
                    }

                    FreshnessTag {
                        Layout.fillWidth: true
                        visible: !!root.primaryRobot
                        online: root.robotOnline
                        hasData: root.hasTele
                        lastRx: root.lastRx
                        nowTick: root.nowTick
                        fontSize: 10 * root.uiScale
                        bold: root.offline
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        // Last-known values, not live -- dim them while disconnected.
                        opacity: root.offline ? 0.5 : 1.0
                        // Limit column spacing to the panel width.
                        spacing: Math.max(10, Math.min(32 * root.uiScale, telemetryPanel.width * 0.09))

                        Item { Layout.fillWidth: true }

                        Item {
                            Layout.preferredWidth: 96 * Math.min(1.2, root.uiScale)
                            Layout.minimumWidth: 56
                            Layout.preferredHeight: Layout.preferredWidth
                            Layout.minimumHeight: 56

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
                                    text: root.hasBatteryReading ? root.battery.toFixed(0) + "%" : "—"
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
                            Layout.fillWidth: true
                            Layout.minimumWidth: 90
                            columns: 2
                            columnSpacing: 9
                            rowSpacing: 5

                            Text { text: "POSITION X"; color: C.textDim; font.pixelSize: 10 * root.uiScale; font.bold: true }
                            Text { Layout.fillWidth: true; elide: Text.ElideRight; text: root.hasPose ? root.posX.toFixed(2) + " m" : "—"; color: C.text; font.family: fontMono; font.pixelSize: 13 * root.uiScale; font.bold: true }
                            Text { text: "POSITION Y"; color: C.textDim; font.pixelSize: 10 * root.uiScale; font.bold: true }
                            Text { Layout.fillWidth: true; elide: Text.ElideRight; text: root.hasPose ? root.posY.toFixed(2) + " m" : "—"; color: C.text; font.family: fontMono; font.pixelSize: 13 * root.uiScale; font.bold: true }
                            Text { text: "HEADING"; color: C.textDim; font.pixelSize: 10 * root.uiScale; font.bold: true }
                            Text { Layout.fillWidth: true; elide: Text.ElideRight; text: root.hasPose ? root.headingDegrees(root.yaw).toFixed(0) + "°" : "—"; color: C.cyan; font.family: fontMono; font.pixelSize: 13 * root.uiScale; font.bold: true }
                            Text { text: "SPEED"; color: C.textDim; font.pixelSize: 10 * root.uiScale; font.bold: true }
                            Text { Layout.fillWidth: true; elide: Text.ElideRight; text: root.hasTele ? root.speed.toFixed(2) + " m/s" : "—"; color: C.text; font.family: fontMono; font.pixelSize: 13 * root.uiScale; font.bold: true }
                            Text {
                                text: "SINCE LAST NODE"
                                color: C.textDim
                                font.pixelSize: 10 * root.uiScale
                                font.bold: true
                                elide: Text.ElideRight
                                Layout.maximumWidth: 100 * root.uiScale
                                MouseArea {
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.WhatsThisCursor
                                    ToolTip.visible: containsMouse
                                    ToolTip.delay: 400
                                    ToolTip.text: "Real distance driven (from odometry) since the AGV last "
                                                  + "reached a waypoint. Resets to 0 at each new node -- this "
                                                  + "is progress on the current leg, not the total trip distance."
                                }
                            }
                            Text { Layout.fillWidth: true; elide: Text.ElideRight; text: root.hasTele ? root.distanceSinceLastNode.toFixed(2) + " m" : "—"; color: C.text; font.family: fontMono; font.pixelSize: 13 * root.uiScale; font.bold: true }
                            Text { text: "LEVEL"; color: C.textDim; font.pixelSize: 10 * root.uiScale; font.bold: true }
                            Text { Layout.fillWidth: true; elide: Text.ElideRight; text: root.primaryRobot ? root.primaryRobot.level : "—"; color: C.text; font.family: fontMono; font.pixelSize: 13 * root.uiScale; font.bold: true }
                            Text { text: "MODE"; color: C.textDim; font.pixelSize: 10 * root.uiScale; font.bold: true }
                            Text {
                                Layout.fillWidth: true; elide: Text.ElideRight
                                text: root.hasTele ? root.operatingMode : "—"
                                color: root.operatingMode === "MANUAL" ? C.warn : C.text
                                font.family: fontMono; font.pixelSize: 13 * root.uiScale; font.bold: true
                            }
                            Text { text: "SAFETY"; color: C.textDim; font.pixelSize: 10 * root.uiScale; font.bold: true }
                            Text {
                                Layout.fillWidth: true; elide: Text.ElideRight
                                text: !root.hasTele ? "—" : (root.eStopActive ? "⚠ " + root.eStopLabel : "OK")
                                color: root.eStopActive ? C.err : C.success
                                font.family: fontMono; font.pixelSize: 13 * root.uiScale; font.bold: true
                            }
                        }

                        Item { Layout.fillWidth: true }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: Math.max(84 * Math.min(1.15, root.uiScale),
                                                         taskCardColumn.implicitHeight + 14)
                        radius: 9
                        color: C.surfaceAlt
                        border.color: root.displayTask
                                      ? Qt.rgba(root.displayTaskColor.r,
                                                root.displayTaskColor.g,
                                                root.displayTaskColor.b, 0.42)
                                      : C.border
                        border.width: 1

                        ColumnLayout {
                            id: taskCardColumn
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
                                    color: root.displayTask ? root.displayTaskColor : C.textDim
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
                                        color: root.displayTaskColor

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
                                    text: root.displayTaskState !== "completed"
                                          && root.displayTask && root.displayTask.error
                                          ? root.displayTask.error
                                          : (root.displayTaskState === "completed"
                                             ? "ARRIVED"
                                             : (root.taskDistanceRemaining >= 0
                                                ? root.taskDistanceRemaining.toFixed(1)
                                                  + " m LEFT"
                                                : root.taskKey(root.displayTask)))
                                    color: root.displayTaskState !== "completed"
                                           && root.displayTask && root.displayTask.error
                                           ? C.err : root.displayTaskColor
                                    font.family: fontMono
                                    font.pixelSize: 8 * root.uiScale
                                    horizontalAlignment: Text.AlignRight
                                    elide: Text.ElideMiddle
                                }
                            }

                            Text {
                                Layout.fillWidth: true
                                visible: !!(root.displayTask && (root.displayTask.phase || root.displayTask.wait_kind))
                                text: {
                                    if (!root.displayTask) return ""
                                    var label = root.displayTask.phase
                                                ? String(root.displayTask.phase).replace(/_/g, " ")
                                                : (root.displayTask.wait_kind || "")
                                    if (!label) return ""
                                    // Workcell wait countdown.
                                    var remaining = root.displayTask.wait_seconds_remaining
                                    if (remaining !== undefined && remaining !== null)
                                        label += " · " + Math.ceil(remaining) + "s left"
                                    return label
                                }
                                color: C.textDim
                                font.pixelSize: 10 * root.uiScale
                                elide: Text.ElideRight
                            }

                            Text {
                                // AGV's reported load (VDA5050 state.loads).
                                Layout.fillWidth: true
                                visible: root.deliveryUnderway && root.robotLoads.length > 0
                                text: {
                                    if (!root.deliveryUnderway || root.robotLoads.length === 0) return ""
                                    var parts = []
                                    for (var i = 0; i < root.robotLoads.length; i++) {
                                        var l = root.robotLoads[i]
                                        var label = l.loadType || l.loadId || "load"
                                        parts.push(label + (l.weight >= 0 ? " · " + l.weight + "kg" : ""))
                                    }
                                    return "📦 " + parts.join(", ")
                                }
                                color: C.text
                                font.pixelSize: 10 * root.uiScale
                                font.bold: true
                                elide: Text.ElideRight
                            }
                        }
                    }
                }
            }

            // Right-hand column: VDA5050 order/action traffic above the task chart, half the height each.
            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.preferredWidth: 360
                spacing: 10

                VdaOrderPanel {
                    id: orderPanel
                    objectName: "vdaOrderPanel"
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.preferredHeight: Math.max(orderPanel.implicitHeight, distributionBox.implicitHeight)
                    Layout.minimumHeight: implicitHeight
                    robotName: root.robotName
                    hasTele: root.hasTele
                    hasOrder: root.hasOrder
                    offline: root.offline
                    orderId: root.orderId
                    updateId: root.orderUpdateId
                    nodeStates: root.orderNodeStates
                    actionStates: root.orderActionStates
                    orderDetail: root.orderDetail
                    lastNodeId: root.orderLastNodeId
                    lastNodeSeq: root.orderLastNodeSeq
                    uiScale: root.uiScale
                }

                // Task counts grouped by state.
                Rectangle {
                    id: distributionBox
                    objectName: "taskDistributionBox"
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.preferredHeight: Math.max(orderPanel.implicitHeight, distributionBox.implicitHeight)
                    Layout.minimumHeight: implicitHeight
                    Layout.preferredWidth: 360
                    implicitHeight: distributionColumn.implicitHeight + 24
                    radius: 10
                    color: C.surface
                    border.color: C.border
                    border.width: 1

                    ColumnLayout {
                        id: distributionColumn
                        anchors.fill: parent
                        anchors.margins: 12
                        spacing: 12

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

                        RowLayout {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            spacing: 14 * root.uiScale

                            Item {
                                Layout.preferredWidth: 84 * Math.min(1.15, root.uiScale)
                                Layout.preferredHeight: Layout.preferredWidth
                                Layout.alignment: Qt.AlignVCenter

                                Canvas {
                                    id: taskDonut
                                    anchors.fill: parent
                                    antialiasing: true
                                    Component.onCompleted: requestPaint()
                                    onPaint: {
                                        var ctx = getContext("2d")
                                        ctx.reset()
                                        var cx = width / 2
                                        var cy = height / 2
                                        var radius = Math.min(width, height) / 2 - 6
                                        var start = -Math.PI / 2

                                        ctx.lineCap = "butt"
                                        ctx.lineWidth = 10
                                        ctx.strokeStyle = C.surfaceRaised
                                        ctx.beginPath()
                                        ctx.arc(cx, cy, radius, 0, Math.PI * 2)
                                        ctx.stroke()

                                        if (root.taskTotal <= 0) return
                                        for (var i = 0; i < root.taskStats.length; i++) {
                                            var seg = root.taskStats[i]
                                            if (seg.value <= 0) continue
                                            var sweep = Math.PI * 2 * (seg.value / root.taskTotal)
                                            ctx.strokeStyle = seg.barColor
                                            ctx.beginPath()
                                            ctx.arc(cx, cy, radius, start, start + sweep)
                                            ctx.stroke()
                                            start += sweep
                                        }
                                    }
                                }
                                Column {
                                    anchors.centerIn: parent
                                    spacing: -2
                                    Text {
                                        anchors.horizontalCenter: parent.horizontalCenter
                                        text: root.taskTotal > 0
                                              ? Math.round(100 * root.completedCount / root.taskTotal) + "%"
                                              : "—"
                                        color: C.success
                                        font.family: fontMono
                                        font.pixelSize: 16 * root.uiScale
                                        font.bold: true
                                    }
                                    Text {
                                        anchors.horizontalCenter: parent.horizontalCenter
                                        text: "DONE"
                                        color: C.textDim
                                        font.pixelSize: 8 * root.uiScale
                                        font.bold: true
                                        font.letterSpacing: 0.6
                                    }
                                }
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                spacing: 4

                                Repeater {
                                    model: root.taskStats

                                    delegate: RowLayout {
                                        Layout.fillWidth: true
                                        Layout.fillHeight: true
                                        spacing: 8
                                        // Dim states with no tasks.
                                        opacity: modelData.value > 0 ? 1.0 : 0.45

                                        Text {
                                            Layout.minimumWidth: implicitWidth
                                            Layout.preferredWidth: Math.max(
                                                implicitWidth,
                                                76 * Math.min(1.15, root.uiScale))
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
                            }
                        }

                        Connections {
                            target: root
                            function onTaskStatsChanged() { taskDonut.requestPaint() }
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
}
