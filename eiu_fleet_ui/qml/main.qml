import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import "components"

ApplicationWindow {
    id: root
    visible: true
    width: 1440
    height: 900
    minimumWidth: 1120
    minimumHeight: 700
    title: "EIU Fleet Control Center"
    color: C.bg

    property var waypoints: []
    property var wpNames: []
    property var lanes: []
    // Map nav_graph lane indices to deduplicated lanes for drawing.
    property var laneIndexMap: []
    // Deduplicated lanes currently closed by RMF.
    readonly property var blockedEdgeIndices: {
        var raw = []
        try { raw = JSON.parse(ros.closedLaneIndicesJson) } catch (e) { raw = [] }
        var out = []
        for (var i = 0; i < raw.length; i++) {
            var edge = root.laneIndexMap[raw[i]]
            if (edge !== undefined && out.indexOf(edge) < 0)
                out.push(edge)
        }
        return out
    }
    property var robots: []
    property var tasks: []
    // Waypoint names that are any robot's current underway-task destination.
    readonly property var activeDestinations: {
        var set = {}
        for (var i = 0; i < tasks.length; i++) {
            var t = tasks[i]
            if (t.state === "underway" && t.destination)
                set[t.destination] = true
        }
        return set
    }
    // VDA5050 connection state by robot name.
    property var robotsOnline: ({})
    // VDA5050 telemetry by robot name.
    property var telemetry: ({})
    property var traffic: []
    // Applied speed limit by robot name; zero means no limit.
    property var speedLimits: ({})
    readonly property string monoFontFamily: fontMono

    // Shared by Fleet Robots, the map, and the telemetry panel -- pick one anywhere, all follow.
    property string selectedRobotName: ""
    function selectRobot(name) {
        root.selectedRobotName = name
        if (fleetAnalytics) {
            fleetAnalytics.userPicked = true
            fleetAnalytics.selectedRobotName = name
        }
    }

    function countTasksByState(state) {
        var count = 0
        for (var i = 0; i < tasks.length; ++i) {
            if (tasks[i].state === state) count++
        }
        return count
    }
    readonly property int runningTaskCount: countTasksByState("underway")
    readonly property int queuedTaskCount: countTasksByState("queued")
    readonly property int completedTaskCount: countTasksByState("completed")

    // Calculate average battery from robot measurements.
    readonly property real averageBattery: {
        if (displayRobots.length === 0)
            return 0
        var total = 0
        for (var i = 0; i < displayRobots.length; ++i)
            total += Number(displayRobots[i].battery || 0)
        return total / displayRobots.length
    }
    readonly property real kpiHeight: Math.max(116, Math.min(142, width / 15))

    // Ticks "last seen" text forward between backend telemetry updates.
    property real nowTick: Date.now()
    Timer { interval: 1000; running: true; repeat: true; onTriggered: root.nowTick = Date.now() }

    function formatAgo(epochSec) {
        if (!epochSec) return ""
        var diff = Math.max(0, root.nowTick / 1000 - epochSec)
        if (diff < 60) return Math.floor(diff) + "s ago"
        if (diff < 3600) return Math.floor(diff / 60) + "m " + Math.floor(diff % 60) + "s ago"
        if (diff < 86400) return Math.floor(diff / 3600) + "h " + Math.floor((diff % 3600) / 60) + "m ago"
        return Math.floor(diff / 86400) + "d ago"
    }

    function reloadRobots() { root.robots = JSON.parse(ros.robotsJson) }
    function reloadTasks()  { root.tasks = JSON.parse(ros.tasksJson) }
    function reloadRobotsOnline() { root.robotsOnline = JSON.parse(mqtt.robotsOnlineJson) }
    function reloadTelemetry() { root.telemetry = JSON.parse(mqtt.telemetryJson) }
    function reloadTraffic() { root.traffic = JSON.parse(mqtt.trafficJson) }
    function telemetryFor(name) { return root.telemetry[name] || null }
    function reloadSpeedLimits() { root.speedLimits = JSON.parse(control.speedLimitsJson) }

    // Include configured robots not yet present in /fleet_states.
    readonly property var displayRobots: {
        var known = JSON.parse(cfg.robotNamesJson)
        var fleetOf = JSON.parse(cfg.robotFleetsJson)
        var byName = {}
        for (var i = 0; i < root.robots.length; i++)
            byName[root.robots[i].name] = root.robots[i]

        var out = []
        for (var j = 0; j < known.length; j++) {
            var name = known[j]
            var tele = root.telemetryFor(name)
            if (byName[name]) {
                var merged = byName[name]
                // Prefer VDA5050 battery readings over RMF planning values.
                if (tele && tele.battery_soc != null) {
                    merged = Object.assign({}, merged, { battery: tele.battery_soc * 100 })
                }
                // Tracked by RMF: position/battery are real, not placeholders.
                merged = Object.assign({}, merged, { rmfSynced: true, hasBattery: true })
                out.push(merged)
                continue
            }
            var pendingHasBattery = !!(tele && tele.battery_soc != null)
            // Use the robot's own fleet name.
            var ownFleet = fleetOf[name] || cfg.fleetName
            out.push({
                key: ownFleet + "/" + name,
                name: name,
                fleet: ownFleet,
                model: "",
                status: "PENDING SYNC",
                battery: pendingHasBattery ? tele.battery_soc * 100 : 0,
                level: (tele && tele.map_id) ? tele.map_id : "—",
                task: "",
                finish: "",
                updated: "",
                x: 0, y: 0, yaw: 0,
                path: [],
                // Never appeared in /fleet_states: x/y/yaw are placeholders, not real pose.
                rmfSynced: false,
                hasBattery: pendingHasBattery
            })
        }
        return out
    }

    function statusColor(status) {
        if (status === "CHARGING")
            return C.success
        if (status === "MOVING" || status === "DOCKING" ||
                status === "GOING_HOME" || status === "WORKING")
            return C.cyan
        if (status === "EMERGENCY" || status === "ERROR")
            return C.err
        if (status === "PAUSED" || status === "WAITING" || status === "PENDING SYNC")
            return C.warn
        return C.textDim
    }

    // Count robots with a direct VDA5050 connection.
    function countRobotsOnline() {
        var n = 0
        for (var name in root.robotsOnline) {
            if (root.robotsOnline[name]) n++
        }
        return n
    }

    // Feeds the alert badge, System Health card, and Needs Attention panel.
    // `robot` (empty for fleet-wide items) lets the panel jump to that robot's controls.
    readonly property var attentionItems: {
        var items = []
        if (!ros.rmfOnline)
            items.push({ severity: "critical", robot: "", title: "RMF connection lost",
                         detail: "Fleet traffic coordination unavailable" })
        if (!mqtt.connected)
            items.push({ severity: "critical", robot: "", title: "MQTT broker disconnected",
                         detail: "No VDA5050 telemetry from any robot" })
        if (ros.activeConflicts > 0)
            items.push({ severity: "critical", robot: "", title: ros.activeConflicts + " traffic conflict(s)",
                         detail: "RMF is negotiating a route conflict" })
        if (ros.blockedLanes > 0)
            items.push({ severity: "warning", robot: "", title: ros.blockedLanes + " lane(s) blocked",
                         detail: "A no-go zone is closing part of the map" })
        for (var i = 0; i < root.displayRobots.length; i++) {
            var r = root.displayRobots[i]
            var t = root.telemetryFor(r.name)
            if (!root.robotsOnline[r.name])
                // One robot down degrades the fleet; losing the last one is critical.
                items.push({ severity: root.countRobotsOnline() === 0 ? "critical" : "warning",
                             robot: r.name, title: r.name + " VDA5050 offline",
                             detail: (t && t.last_rx) ? "No VDA5050 state received · Last seen " + root.formatAgo(t.last_rx)
                                     : "No VDA5050 state ever received" })
            if (t && t.safety && t.safety.triggered)
                items.push({ severity: "critical", robot: r.name, title: r.name + " emergency stop",
                             detail: (t.safety.e_stop && t.safety.e_stop !== "NONE")
                                     ? t.safety.e_stop : "Field violation" })
            if (t && t.fatal_error)
                items.push({ severity: "critical", robot: r.name, title: r.name + " fatal error", detail: t.fatal_error })
            if (Number(r.battery) > 0 && Number(r.battery) < 20)
                items.push({ severity: "warning", robot: r.name, title: r.name + " battery low",
                             detail: Number(r.battery).toFixed(0) + "% remaining" })
            if (t && t.position_initialized === false)
                items.push({ severity: "warning", robot: r.name, title: r.name + " not localized",
                             detail: "No usable pose for route planning" })
            if (t && t.stale === true)
                items.push({ severity: "warning", robot: r.name, title: r.name + " telemetry stale",
                             detail: "No recent VDA5050 state update" })
            if (root.robotsOnline[r.name] && t && t.off_graph)
                items.push({ severity: "warning", robot: r.name, title: r.name + " off the navigation graph",
                             detail: Number(t.off_graph_m).toFixed(1) + " m from the nearest lane · RMF may not be able to plan its route" })
            if (root.robotsOnline[r.name] && t && t.paused)
                items.push({ severity: "warning", robot: r.name, title: r.name + " paused",
                             detail: "RMF will not assign tasks until it is resumed" })
        }
        var failedTasks = 0
        for (var j = 0; j < root.tasks.length; j++) {
            if (root.tasks[j].state === "failed") failedTasks++
        }
        if (failedTasks > 0)
            items.push({ severity: "warning", robot: "", title: failedTasks + " task(s) failed",
                         detail: "See Recent Tasks for details" })
        return items
    }

    function firstAttentionOfSeverity(sev) {
        for (var i = 0; i < attentionItems.length; i++)
            if (attentionItems[i].severity === sev) return attentionItems[i]
        return null
    }

    readonly property int criticalAlertCount: {
        var n = 0
        for (var i = 0; i < attentionItems.length; i++)
            if (attentionItems[i].severity === "critical") n++
        return n
    }
    readonly property int warningAlertCount: {
        var n = 0
        for (var i = 0; i < attentionItems.length; i++)
            if (attentionItems[i].severity === "warning") n++
        return n
    }

    // Healthy -> Degraded -> Critical -> Offline.
    readonly property string systemHealthLevel: {
        if (!ros.rmfOnline && !mqtt.connected) return "OFFLINE"
        if (root.criticalAlertCount > 0) return "CRITICAL"
        if (root.warningAlertCount > 0) return "DEGRADED"
        return "HEALTHY"
    }
    readonly property string systemHealthDetail: {
        if (systemHealthLevel === "OFFLINE") return "No connection to fleet"
        if (systemHealthLevel === "CRITICAL") {
            var crit = root.attentionItems.filter(function(i) { return i.severity === "critical" })
            if (crit.length === 0) return "Critical issue detected"
            // A single item's own title is clearer; several get a count -- see Needs Attention for detail.
            return crit.length === 1 ? crit[0].title : crit.length + " critical issues — see Needs Attention"
        }
        if (systemHealthLevel === "DEGRADED") {
            var warn = root.attentionItems.filter(function(i) { return i.severity === "warning" })
            if (warn.length === 0) return "Degraded"
            return warn.length === 1 ? warn[0].title : warn.length + " warnings — see Needs Attention"
        }
        return "All services nominal"
    }
    readonly property color systemHealthColor: systemHealthLevel === "HEALTHY" ? C.success
            : (systemHealthLevel === "DEGRADED" ? C.warn : C.err)

    // Robot status breakdown for the Fleet KPI card.
    readonly property string fleetStatusSummary: {
        var navigating = 0, idle = 0, charging = 0, error = 0, offline = 0
        for (var i = 0; i < displayRobots.length; i++) {
            var rr = displayRobots[i]
            // A robot with no VDA5050 connection is offline, whatever RMF last reported.
            if (!root.robotsOnline[rr.name]) { offline++; continue }
            var s = rr.status
            if (s === "MOVING" || s === "DOCKING" || s === "GOING_HOME" || s === "WORKING") navigating++
            else if (s === "CHARGING") charging++
            else if (s === "EMERGENCY" || s === "ERROR") error++
            else idle++
        }
        var parts = []
        if (error > 0) parts.push(error + " error")
        if (navigating > 0) parts.push(navigating + " navigating")
        if (charging > 0) parts.push(charging + " charging")
        if (idle > 0) parts.push(idle + " idle")
        if (offline > 0) parts.push(offline + " offline")
        return parts.length > 0 ? parts.join(" · ") : "No robots"
    }

    function taskColor(state) {
        if (state === "completed" || state.indexOf("complet") >= 0)
            return C.success
        if (state === "failed" || state.indexOf("fail") >= 0)
            return C.err
        if (state === "cancelled" || state.indexOf("cancel") >= 0 || state.indexOf("stale") >= 0)
            return C.textDim
        if (state === "queued" || state.indexOf("queue") >= 0)
            return C.warn
        return C.cyan
    }

    Component.onCompleted: {
        var wps = JSON.parse(mapProv.wpJson)
        root.waypoints = wps
        root.lanes = JSON.parse(mapProv.lanesJson)
        root.laneIndexMap = JSON.parse(mapProv.laneIndexMapJson)
        var names = []
        for (var i = 0; i < wps.length; ++i) {
            if (wps[i].name && wps[i].name.length > 0)
                names.push(wps[i].name)
        }
        root.wpNames = names
        reloadRobots()
        reloadTasks()
        reloadRobotsOnline()
        reloadTelemetry()
        reloadTraffic()
        reloadSpeedLimits()
        // Children complete first, so FleetAnalytics has already auto-selected a robot.
        if (fleetAnalytics.selectedRobotName)
            root.selectedRobotName = fleetAnalytics.selectedRobotName
    }

    Connections {
        target: ros
        function onRobotsChanged() { root.reloadRobots() }
        function onTasksChanged()  { root.reloadTasks() }
    }

    Connections {
        target: mqtt
        function onOnlineChanged() { root.reloadRobotsOnline() }
        function onTelemetryChanged() { root.reloadTelemetry() }
        function onTrafficChanged() { root.reloadTraffic() }
    }

    Connections {
        target: control
        function onSpeedLimitsChanged() { root.reloadSpeedLimits() }
    }

    RobotControlDialog {
        id: controlDialog
    }

    NewTaskDialog {
        id: taskDialog
        places: root.wpNames
        robotsOnline: root.robotsOnline
        anchors.centerIn: parent
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        // Navigation sidebar.
        Rectangle {
            objectName: "navRail"
            Layout.preferredWidth: 218
            Layout.fillHeight: true
            color: "#081726"

            Rectangle {
                anchors.right: parent.right
                width: 1
                height: parent.height
                color: C.border
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
                            color: C.accent

                            Text {
                                anchors.centerIn: parent
                                text: "E"
                                color: "white"
                                font.pixelSize: 21
                                font.bold: true
                            }
                        }

                        ColumnLayout {
                            spacing: 0
                            Text {
                                text: "EIU FLEET"
                                color: C.text
                                font.pixelSize: 15
                                font.bold: true
                                font.letterSpacing: 0.8
                            }
                            Text {
                                text: "CONTROL OS"
                                color: C.accent
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
                    color: C.textDim
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
                            color: modelData.active ? "#3573C4" : "transparent"

                            Rectangle {
                                visible: modelData.active
                                anchors.left: parent.left
                                anchors.verticalCenter: parent.verticalCenter
                                width: 3
                                height: 22
                                radius: 2
                                color: "#9AC4FF"
                            }

                            Row {
                                anchors.left: parent.left
                                anchors.leftMargin: 15
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 13

                                Text {
                                    width: 22
                                    text: modelData.glyph
                                    color: modelData.active ? "white" : C.textDim
                                    font.pixelSize: 18
                                    horizontalAlignment: Text.AlignHCenter
                                }
                                Text {
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: modelData.label
                                    // Text color for navigation labels.
                                    color: modelData.active ? "white" : "#8BA6BD"
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
                    color: C.textDim
                    opacity: 0.6
                    font.family: root.monoFontFamily
                    font.pixelSize: 11
                }
            }
        }

        // Dashboard content.
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 82
                color: "#091827"

                Rectangle {
                    anchors.bottom: parent.bottom
                    width: parent.width
                    height: 1
                    color: C.border
                    opacity: 0.55
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 24
                    anchors.rightMargin: 24
                    spacing: 12

                    Image {
                        Layout.preferredWidth: 92
                        Layout.preferredHeight: 32
                        source: eiuLogoUrl
                        fillMode: Image.PreserveAspectFit
                        smooth: true
                        mipmap: true
                        asynchronous: true
                    }

                    ColumnLayout {
                        spacing: 2
                        Text {
                            text: "Fleet Command Center"
                            color: C.text
                            font.pixelSize: 22
                            font.bold: true
                        }
                        Text {
                            text: "Live operations overview"
                            color: C.textDim
                            font.pixelSize: 12
                        }
                    }

                    Item { Layout.fillWidth: true }

                    Rectangle {
                        Layout.preferredWidth: 118
                        Layout.preferredHeight: 34
                        radius: 17
                        color: C.surface
                        border.color: ros.rmfOnline ? "#286A60" : "#673044"
                        border.width: 1
                        Row {
                            anchors.centerIn: parent
                            spacing: 7
                            Rectangle {
                                width: 7
                                height: 7
                                radius: 4
                                color: ros.rmfOnline ? C.success : C.err
                                anchors.verticalCenter: parent.verticalCenter
                            }
                            Text {
                                text: ros.rmfOnline ? "RMF ONLINE" : "RMF OFFLINE"
                                color: C.text
                                font.family: root.monoFontFamily
                                font.pixelSize: 10
                                font.bold: true
                                anchors.verticalCenter: parent.verticalCenter
                            }
                        }
                    }

                    Rectangle {
                        Layout.preferredWidth: 128
                        Layout.preferredHeight: 34
                        radius: 17
                        color: C.surface
                        border.color: mqtt.connected ? "#286A60" : "#673044"
                        border.width: 1
                        Row {
                            anchors.centerIn: parent
                            spacing: 7
                            Rectangle {
                                width: 7
                                height: 7
                                radius: 4
                                color: mqtt.connected ? C.success : C.err
                                anchors.verticalCenter: parent.verticalCenter
                            }
                            Text {
                                text: mqtt.connected ? "MQTT ONLINE" : "MQTT OFFLINE"
                                color: C.text
                                font.family: root.monoFontFamily
                                font.pixelSize: 10
                                font.bold: true
                                anchors.verticalCenter: parent.verticalCenter
                            }
                        }
                    }

                    Rectangle {
                        Layout.preferredWidth: alertsText.implicitWidth + 30
                        Layout.preferredHeight: 34
                        radius: 17
                        color: C.surface
                        border.color: root.criticalAlertCount > 0 ? "#673044"
                                      : (root.warningAlertCount > 0 ? "#5A4A26" : "#286A60")
                        border.width: 1
                        Row {
                            anchors.centerIn: parent
                            spacing: 7
                            Rectangle {
                                width: 7
                                height: 7
                                radius: 4
                                color: root.criticalAlertCount > 0 ? C.err
                                       : (root.warningAlertCount > 0 ? C.warn : C.success)
                                anchors.verticalCenter: parent.verticalCenter
                            }
                            Text {
                                id: alertsText
                                text: root.criticalAlertCount > 0
                                      ? root.criticalAlertCount + " CRITICAL"
                                      : (root.warningAlertCount > 0
                                         ? root.warningAlertCount + " WARNING"
                                         : "ALL CLEAR")
                                color: C.text
                                font.family: root.monoFontFamily
                                font.pixelSize: 10
                                font.bold: true
                                anchors.verticalCenter: parent.verticalCenter
                            }
                        }
                    }

                    // Show WebSocket status when a connection URL is configured.
                    Rectangle {
                        visible: cfg.websocketEnabled
                        Layout.preferredWidth: 128
                        Layout.preferredHeight: 34
                        radius: 17
                        color: C.surface
                        border.color: wsTasks.connected ? "#286A60" : "#673044"
                        border.width: 1
                        Row {
                            anchors.centerIn: parent
                            spacing: 7
                            Rectangle {
                                width: 7
                                height: 7
                                radius: 4
                                color: wsTasks.connected ? C.success : C.err
                                anchors.verticalCenter: parent.verticalCenter
                            }
                            Text {
                                text: wsTasks.connected ? "TASK EVENTS ON" : "TASK EVENTS OFF"
                                color: C.text
                                font.family: root.monoFontFamily
                                font.pixelSize: 10
                                font.bold: true
                                anchors.verticalCenter: parent.verticalCenter
                            }
                        }
                    }

                    Button {
                        id: newTaskButton
                        text: "+  NEW TASK"
                        Layout.preferredHeight: 38
                        leftPadding: 17
                        rightPadding: 17
                        contentItem: Text {
                            text: newTaskButton.text
                            color: "white"
                            font.pixelSize: 11
                            font.bold: true
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                        background: Rectangle {
                            radius: 10
                            color: newTaskButton.down ? C.accentDark : C.accent
                            border.color: "#5A9BFF"
                            border.width: 1
                        }
                        onClicked: taskDialog.open()
                    }
                }
            }

            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 20
                    spacing: 16

                    RowLayout {
                        objectName: "kpiRow"
                        Layout.fillWidth: true
                        Layout.preferredHeight: root.kpiHeight
                        Layout.minimumHeight: root.kpiHeight
                        Layout.maximumHeight: root.kpiHeight
                        spacing: 14

                        MetricCard {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            Layout.minimumWidth: 210
                            title: "System health"
                            value: root.systemHealthLevel
                            valueFontFamily: root.monoFontFamily
                            detail: root.systemHealthDetail
                            iconSource: statusActiveIconUrl
                            accentColor: root.systemHealthColor
                            alert: root.systemHealthLevel !== "HEALTHY"
                        }
                        MetricCard {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            Layout.minimumWidth: 210
                            title: "Robots"
                            value: root.displayRobots.length > 0
                                   ? root.countRobotsOnline() + "/" + root.displayRobots.length + " ONLINE"
                                   : "0 ROBOTS"
                            valueFontFamily: root.monoFontFamily
                            detail: root.fleetStatusSummary
                            iconSource: fleetRobotIconUrl
                            accentColor: C.cyan
                        }
                        MetricCard {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            Layout.minimumWidth: 210
                            title: "Traffic status"
                            value: !ros.rmfOnline ? "NO DATA"
                                   : (ros.activeConflicts > 0 ? "CONFLICT"
                                      : (ros.blockedLanes > 0 ? "CONGESTED" : "NORMAL"))
                            valueFontFamily: root.monoFontFamily
                            detail: !ros.rmfOnline ? "RMF connection required"
                                    : (ros.activeConflicts + " conflicts · " + ros.blockedLanes + " blocked lanes")
                            iconText: "⇄"
                            accentColor: !ros.rmfOnline ? C.textDim
                                         : (ros.activeConflicts > 0 ? C.err
                                            : (ros.blockedLanes > 0 ? C.warn : C.success))
                            alert: ros.rmfOnline && ros.activeConflicts > 0
                        }
                        MetricCard {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            Layout.minimumWidth: 210
                            title: "Tasks"
                            value: root.runningTaskCount + " ACTIVE"
                            valueFontFamily: root.monoFontFamily
                            detail: root.queuedTaskCount + " queued · " + root.completedTaskCount + " completed"
                            iconText: "✓"
                            accentColor: C.accent
                        }
                    }

                    SplitView {
                        id: dashboardSplit
                        objectName: "dashboardSplit"
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        orientation: Qt.Horizontal

                        handle: Rectangle {
                            implicitWidth: 16
                            color: SplitHandle.pressed
                                   ? Qt.rgba(0.12, 0.47, 1.0, 0.12)
                                   : "transparent"

                            HoverHandler { cursorShape: Qt.SplitHCursor }

                            Rectangle {
                                anchors.centerIn: parent
                                width: SplitHandle.pressed ? 4 : 2
                                height: 64
                                radius: 2
                                color: SplitHandle.hovered || SplitHandle.pressed
                                       ? C.accent : C.border

                                Behavior on width {
                                    NumberAnimation { duration: 100 }
                                }
                                Behavior on color {
                                    ColorAnimation { duration: 120 }
                                }
                            }
                        }

                        // Main map card.
                        Rectangle {
                            id: mapPanel
                            objectName: "mapPanel"
                            SplitView.fillWidth: true
                            SplitView.minimumWidth: 500
                            radius: 16
                            color: C.surface
                            border.color: C.border
                            border.width: 1
                            clip: true

                            ColumnLayout {
                                anchors.fill: parent
                                spacing: 0

                                Item {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 54

                                    RowLayout {
                                        anchors.fill: parent
                                        anchors.leftMargin: 18
                                        anchors.rightMargin: 18
                                        spacing: 12

                                        Rectangle {
                                            Layout.preferredWidth: 4
                                            Layout.preferredHeight: 22
                                            radius: 2
                                            color: C.accent
                                        }
                                        Text {
                                            text: "LIVE NAVIGATION MAP"
                                            color: C.text
                                            font.pixelSize: 13
                                            font.bold: true
                                            font.letterSpacing: 0.8
                                        }
                                        Item { Layout.fillWidth: true }
                                        Row {
                                            spacing: 10
                                            Repeater {
                                                model: [
                                                    { label: "Graph",        color: "#f5c400" },
                                                    { label: "Active route", color: "#00e676" },
                                                    { label: "Robot",        color: "#2979ff" },
                                                    { label: "Charger",      color: "#F39C12" },
                                                    { label: "Blocked",      color: "#F05265" }
                                                ]
                                                delegate: Row {
                                                    spacing: 5
                                                    Rectangle { width: 7; height: 7; radius: 4; color: modelData.color; anchors.verticalCenter: parent.verticalCenter }
                                                    Text { text: modelData.label; color: C.textDim; font.pixelSize: 10 }
                                                }
                                            }
                                        }
                                    }
                                }

                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 1
                                    color: C.border
                                    opacity: 0.7
                                }

                                SplitView {
                                    id: mapAnalyticsSplit
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true
                                    orientation: Qt.Vertical

                                    // Size the map region to the source image aspect ratio.
                                    readonly property real naturalMapHeight: {
                                        if (mapProv.pixelW <= 0 || mapProv.pixelH <= 0)
                                            return 0
                                        var frameInsets = 36  // Map image margins
                                        var contentWidth = Math.max(1, width - frameInsets)
                                        return frameInsets + contentWidth
                                               * mapProv.pixelH / mapProv.pixelW
                                    }

                                    // Cap map height to leave room for analytics.
                                    readonly property real handleHeight: 12
                                    readonly property real mapReserve: Math.min(
                                        naturalMapHeight, Math.max(160, height * 0.45))
                                    readonly property real analyticsNeed: fleetAnalytics.implicitHeight + 10

                                    handle: Rectangle {
                                        implicitHeight: 12
                                        color: SplitHandle.pressed
                                               ? Qt.rgba(0.12, 0.47, 1.0, 0.10)
                                               : "transparent"

                                        HoverHandler { cursorShape: Qt.SplitVCursor }

                                        Rectangle {
                                            anchors.centerIn: parent
                                            width: 64
                                            height: SplitHandle.pressed ? 4 : 2
                                            radius: 2
                                            color: SplitHandle.hovered || SplitHandle.pressed
                                                   ? C.accent : C.border

                                            Behavior on height {
                                                NumberAnimation { duration: 100 }
                                            }
                                            Behavior on color {
                                                ColorAnimation { duration: 120 }
                                            }
                                        }
                                    }

                                    Item {
                                        objectName: "mapArea"
                                        SplitView.preferredHeight: Math.min(
                                            mapAnalyticsSplit.naturalMapHeight,
                                            mapAnalyticsSplit.height - mapAnalyticsSplit.analyticsNeed
                                                - mapAnalyticsSplit.handleHeight)
                                        SplitView.minimumHeight: 160

                                        Loader {
                                            id: mapLoader
                                            anchors.fill: parent
                                            anchors.margins: 10
                                            source: Qt.resolvedUrl("pages/MapPage.qml")
                                        }
                                    }

                                    Item {
                                        objectName: "analyticsArea"
                                        SplitView.fillHeight: true
                                        SplitView.minimumHeight: mapAnalyticsSplit.analyticsNeed

                                        FleetAnalytics {
                                            id: fleetAnalytics
                                            availableHeight: mapAnalyticsSplit.height - mapAnalyticsSplit.mapReserve
                                                             - mapAnalyticsSplit.handleHeight - 10
                                            anchors.fill: parent
                                            anchors.leftMargin: 10
                                            anchors.rightMargin: 10
                                            anchors.bottomMargin: 10
                                            robots: root.displayRobots
                                            tasks: root.tasks
                                            robotsOnline: root.robotsOnline
                                            waypoints: root.waypoints
                                            telemetry: root.telemetry
                                            nowTick: root.nowTick
                                        }
                                        // Mirror FleetAnalytics's own combo-box/auto-select choice upward.
                                        Connections {
                                            target: fleetAnalytics
                                            function onSelectedRobotNameChanged() {
                                                root.selectedRobotName = fleetAnalytics.selectedRobotName
                                            }
                                        }
                                    }
                                }
                            }

                            Binding {
                                target: mapLoader.item
                                property: "waypoints"
                                value: root.waypoints
                                when: mapLoader.status === Loader.Ready
                            }
                            Binding {
                                target: mapLoader.item
                                property: "edges"
                                value: root.lanes
                                when: mapLoader.status === Loader.Ready
                            }
                            Binding {
                                target: mapLoader.item
                                property: "blockedEdgeIndices"
                                value: root.blockedEdgeIndices
                                when: mapLoader.status === Loader.Ready
                            }
                            Binding {
                                target: mapLoader.item
                                property: "mapRobots"
                                value: root.robots
                                when: mapLoader.status === Loader.Ready
                            }
                            Binding {
                                target: mapLoader.item
                                property: "telemetry"
                                value: root.telemetry
                                when: mapLoader.status === Loader.Ready
                            }
                            Binding {
                                target: mapLoader.item
                                property: "plannedDest"
                                value: ros.plannedDest
                                when: mapLoader.status === Loader.Ready
                            }
                            Binding {
                                target: mapLoader.item
                                property: "activeDestinations"
                                value: root.activeDestinations
                                when: mapLoader.status === Loader.Ready
                            }
                            Binding {
                                target: mapLoader.item
                                property: "robotsOnline"
                                value: root.robotsOnline
                                when: mapLoader.status === Loader.Ready
                            }
                            Binding {
                                target: mapLoader.item
                                property: "selectedRobotName"
                                value: root.selectedRobotName
                                when: mapLoader.status === Loader.Ready
                            }
                            Connections {
                                target: mapLoader.item
                                function onRobotPicked(name) { root.selectRobot(name) }
                            }
                            Binding {
                                target: mapLoader.item
                                property: "robotIconSource"
                                value: robotIconUrl
                                when: mapLoader.status === Loader.Ready
                            }
                            Binding {
                                target: mapLoader.item
                                property: "robotIconUrls"
                                value: robotIconUrls
                                when: mapLoader.status === Loader.Ready
                            }
                        }

                        // Live robot list.
                        ColumnLayout {
                            id: fleetPanel
                            objectName: "fleetPanel"
                            // Map : robot panel split is 60 : 40 by default.
                            readonly property real panelRatio: 0.4
                            readonly property bool smallFleet: root.displayRobots.length <= 3
                            SplitView.preferredWidth: dashboardSplit.width * panelRatio
                            SplitView.minimumWidth: smallFleet ? 420 : 640
                            SplitView.maximumWidth: dashboardSplit.width * 0.5
                            spacing: 16

                            // Scale typography with the robot panel width.
                            readonly property real contentScale: Math.max(
                                0.7, Math.min(1.25, width / 720))

                            property string robotSearchText: ""
                            readonly property var filteredDisplayRobots: {
                                if (robotSearchText === "") return root.displayRobots
                                var q = robotSearchText.toLowerCase()
                                return root.displayRobots.filter(function(r) {
                                    return String(r.name || "").toLowerCase().indexOf(q) >= 0
                                })
                            }

                            Rectangle {
                                objectName: "needsAttentionPanel"
                                Layout.fillWidth: true
                                Layout.preferredHeight: root.attentionItems.length > 0
                                        ? Math.min(230, 52 + root.attentionItems.length * 50)
                                        : 90
                                radius: 16
                                color: C.surface
                                border.color: root.criticalAlertCount > 0 ? "#673044"
                                              : (root.warningAlertCount > 0 ? "#5A4A26" : C.border)
                                border.width: 1
                                clip: true

                                ColumnLayout {
                                    anchors.fill: parent
                                    spacing: 0

                                    Item {
                                        Layout.fillWidth: true
                                        Layout.preferredHeight: 44
                                        RowLayout {
                                            anchors.fill: parent
                                            anchors.leftMargin: 17
                                            anchors.rightMargin: 17
                                            Text {
                                                text: "NEEDS ATTENTION"
                                                color: C.text
                                                font.pixelSize: 13
                                                font.bold: true
                                                font.letterSpacing: 0.8
                                            }
                                            Item { Layout.fillWidth: true }
                                            Rectangle {
                                                visible: root.attentionItems.length > 0
                                                width: attnCountText.implicitWidth + 14
                                                height: 20
                                                radius: 10
                                                color: root.criticalAlertCount > 0 ? C.err : C.warn
                                                Text {
                                                    id: attnCountText
                                                    anchors.centerIn: parent
                                                    text: root.attentionItems.length
                                                    color: "#0B1420"
                                                    font.bold: true
                                                    font.pixelSize: 11
                                                }
                                            }
                                        }
                                    }

                                    Text {
                                        visible: root.attentionItems.length === 0
                                        Layout.fillWidth: true
                                        Layout.leftMargin: 17
                                        Layout.bottomMargin: 14
                                        text: "✓ No issues requiring attention"
                                        color: C.success
                                        font.pixelSize: 13
                                    }

                                    ListView {
                                        visible: root.attentionItems.length > 0
                                        Layout.fillWidth: true
                                        Layout.fillHeight: true
                                        clip: true
                                        model: root.attentionItems
                                        ScrollIndicator.vertical: ScrollIndicator { }
                                        delegate: Item {
                                            id: attnRow
                                            width: ListView.view.width
                                            height: 50
                                            // Robot-specific items can be focused (row body) or diagnosed (chevron).
                                            readonly property bool clickable: modelData.robot !== ""

                                            Rectangle {
                                                anchors.fill: parent
                                                radius: 8
                                                color: (attnRow.clickable && attnHover.hovered) ? C.surfaceAlt : "transparent"
                                                Behavior on color { ColorAnimation { duration: 150 } }
                                            }
                                            HoverHandler { id: attnHover; enabled: attnRow.clickable }

                                            // Declared first so the chevron's own MouseArea below sits on top.
                                            MouseArea {
                                                anchors.fill: parent
                                                enabled: attnRow.clickable
                                                cursorShape: Qt.PointingHandCursor
                                                onClicked: root.selectRobot(modelData.robot)
                                            }

                                            RowLayout {
                                                anchors.fill: parent
                                                anchors.leftMargin: 17
                                                anchors.rightMargin: 17
                                                spacing: 10
                                                Rectangle {
                                                    Layout.preferredWidth: 8
                                                    Layout.preferredHeight: 8
                                                    radius: 4
                                                    color: modelData.severity === "critical" ? C.err : C.warn
                                                    Layout.alignment: Qt.AlignVCenter
                                                }
                                                ColumnLayout {
                                                    Layout.fillWidth: true
                                                    spacing: 1
                                                    Text {
                                                        text: modelData.title
                                                        color: C.text
                                                        font.bold: true
                                                        font.pixelSize: 13
                                                        elide: Text.ElideRight
                                                        Layout.fillWidth: true
                                                    }
                                                    Text {
                                                        text: modelData.detail
                                                        color: C.textDim
                                                        font.pixelSize: 11
                                                        elide: Text.ElideRight
                                                        Layout.fillWidth: true
                                                    }
                                                }
                                                // Separate hit target: opens diagnostics instead of just focusing.
                                                Item {
                                                    visible: attnRow.clickable
                                                    Layout.preferredWidth: 26
                                                    Layout.preferredHeight: 26
                                                    Layout.alignment: Qt.AlignVCenter

                                                    Text {
                                                        anchors.centerIn: parent
                                                        text: "›"
                                                        color: chevronHover.hovered ? C.text : C.textDim
                                                        font.pixelSize: 18
                                                        Behavior on color { ColorAnimation { duration: 150 } }
                                                    }
                                                    HoverHandler { id: chevronHover }
                                                    MouseArea {
                                                        anchors.fill: parent
                                                        cursorShape: Qt.PointingHandCursor
                                                        onClicked: {
                                                            controlDialog.robotName = modelData.robot
                                                            controlDialog.currentSpeedLimit = root.speedLimits[modelData.robot] || 0
                                                            controlDialog.open()
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }

                            Rectangle {
                                objectName: "activeRobotsPanel"
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                Layout.preferredHeight: 260
                                radius: 16
                                color: C.surface
                                border.color: C.border
                                border.width: 1
                                clip: true

                                ColumnLayout {
                                    anchors.fill: parent
                                    spacing: 0

                                    Item {
                                        Layout.fillWidth: true
                                        Layout.preferredHeight: 52 * fleetPanel.contentScale
                                        RowLayout {
                                            anchors.fill: parent
                                            anchors.leftMargin: 17
                                            anchors.rightMargin: 17
                                            Text {
                                                text: "FLEET ROBOTS"
                                                color: C.text
                                                font.pixelSize: 13 * fleetPanel.contentScale
                                                font.bold: true
                                                font.letterSpacing: 0.8
                                            }
                                            TextField {
                                                Layout.fillWidth: true
                                                Layout.maximumWidth: 150 * fleetPanel.contentScale
                                                Layout.preferredHeight: 28 * fleetPanel.contentScale
                                                placeholderText: "Search robots…"
                                                placeholderTextColor: C.placeholderText
                                                color: C.text
                                                font.pixelSize: 11 * fleetPanel.contentScale
                                                verticalAlignment: Text.AlignVCenter
                                                leftPadding: 8
                                                onTextChanged: fleetPanel.robotSearchText = text
                                                background: Rectangle {
                                                    radius: 8; color: C.surfaceAlt
                                                    border.color: C.border; border.width: 1
                                                }
                                            }
                                            Rectangle {
                                                Layout.preferredWidth: 30 * fleetPanel.contentScale
                                                Layout.preferredHeight: 25 * fleetPanel.contentScale
                                                radius: 8
                                                color: "#17365A"
                                                Text {
                                                    anchors.centerIn: parent
                                                    text: fleetPanel.filteredDisplayRobots.length
                                                    color: C.cyan
                                                    font.family: root.monoFontFamily
                                                    font.pixelSize: 11 * fleetPanel.contentScale
                                                    font.bold: true
                                                }
                                            }
                                        }
                                    }

                                    Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: C.border; opacity: 0.65 }

                                    Item {
                                        Layout.fillWidth: true
                                        Layout.fillHeight: true

                                        ListView {
                                            anchors.fill: parent
                                            anchors.margins: 8
                                            model: fleetPanel.filteredDisplayRobots
                                            clip: true
                                            spacing: 3
                                            boundsBehavior: Flickable.StopAtBounds

                                            delegate: Rectangle {
                                                id: robotRow
                                                width: ListView.view.width
                                                // How many lines a row needs varies (task/warnings/freshness
                                                // are each conditional) -- size to content, not a fixed guess.
                                                height: Math.max(72 * fleetPanel.contentScale,
                                                                 nameColumn.implicitHeight + 20 * fleetPanel.contentScale)
                                                radius: 10
                                                readonly property bool selected: modelData.name === root.selectedRobotName
                                                color: selected ? Qt.rgba(0.125, 0.89, 0.94, 0.10)
                                                       : (index % 2 === 0 ? C.surfaceAlt : "transparent")
                                                border.color: C.cyanBright
                                                border.width: selected ? 1.5 : 0

                                                // Shared with the map and the telemetry panel.
                                                MouseArea {
                                                    anchors.fill: parent
                                                    onClicked: root.selectRobot(modelData.name)
                                                }

                                                // Latest VDA5050 telemetry for this robot.
                                                readonly property var tele: root.telemetryFor(modelData.name)
                                                readonly property bool teleUnsafe: tele && (tele.safety.triggered || tele.fatal_error !== "")
                                                // Prioritize critical errors and emergency stop indicators.
                                                readonly property string safetyLabel: {
                                                    if (!tele) return ""
                                                    if (tele.fatal_error) return tele.fatal_error
                                                    if (tele.safety.e_stop && tele.safety.e_stop !== "NONE") return tele.safety.e_stop
                                                    if (tele.safety.field_violation) return "FIELD VIOLATION"
                                                    return ""
                                                }
                                                // Warn when the adapter has no pose usable for route planning.
                                                readonly property bool notLocalized: tele && tele.position_initialized === false
                                                readonly property bool stale: tele && tele.stale === true
                                                readonly property string lastSeenText: {
                                                    var _ = root.nowTick
                                                    return (tele && tele.last_rx) ? root.formatAgo(tele.last_rx) : ""
                                                }
                                                // Robot is under manual control through twist_mux.
                                                readonly property bool manualMode: tele && tele.operating_mode === "MANUAL"

                                                // The robot's active multi-round patrol task.
                                                readonly property var currentTask: {
                                                    if (!modelData.task) return null
                                                    for (var i = 0; i < root.tasks.length; i++) {
                                                        if (root.tasks[i].rmf_id === modelData.task)
                                                            return root.tasks[i]
                                                    }
                                                    return null
                                                }
                                                readonly property int roundsTotal:
                                                    (currentTask && currentTask.rounds > 1) ? currentTask.rounds : 0
                                                readonly property int roundsRemaining:
                                                    (currentTask && currentTask.rounds > 1)
                                                    ? (currentTask.rounds_remaining || 0) : 0
                                                // Current patrol round, counted from one.
                                                readonly property int roundsCurrent:
                                                    roundsTotal > 0
                                                    ? Math.min(roundsTotal, Math.max(1, roundsTotal - roundsRemaining + 1))
                                                    : 0

                                                RowLayout {
                                                    anchors.fill: parent
                                                    anchors.leftMargin: 10
                                                    anchors.rightMargin: 10
                                                    anchors.topMargin: 10 * fleetPanel.contentScale
                                                    anchors.bottomMargin: 10 * fleetPanel.contentScale
                                                    spacing: 10

                                                    Rectangle {
                                                        Layout.preferredWidth: 52 * fleetPanel.contentScale
                                                        Layout.preferredHeight: 52 * fleetPanel.contentScale
                                                        radius: 14 * fleetPanel.contentScale
                                                        color: "#153B65"
                                                        border.color: "#285B8C"
                                                        Text {
                                                            anchors.centerIn: parent
                                                            text: modelData.name && modelData.name.length ? modelData.name.charAt(0).toUpperCase() : "R"
                                                            color: C.cyan
                                                            font.pixelSize: 21 * fleetPanel.contentScale
                                                            font.bold: true
                                                        }
                                                        // Connection indicator from the VDA5050 connection topic.
                                                        Rectangle {
                                                            width: 12 * fleetPanel.contentScale
                                                            height: width
                                                            radius: width / 2
                                                            anchors.right: parent.right
                                                            anchors.bottom: parent.bottom
                                                            anchors.margins: -1
                                                            color: root.robotsOnline[modelData.name] ? C.success : C.err
                                                            border.width: 1.5
                                                            border.color: C.surface
                                                        }
                                                    }
                                                    ColumnLayout {
                                                        id: nameColumn
                                                        Layout.fillWidth: true
                                                        spacing: 3
                                                        Text { text: modelData.name; color: C.text; font.pixelSize: 22 * fleetPanel.contentScale; font.bold: true; elide: Text.ElideRight; Layout.fillWidth: true }
                                                        Text {
                                                            text: modelData.fleet + "  ·  " + modelData.level
                                                            color: root.robotsOnline[modelData.name] ? C.textDim : C.err
                                                            font.family: root.monoFontFamily
                                                            font.pixelSize: 16 * fleetPanel.contentScale
                                                            elide: Text.ElideRight; Layout.fillWidth: true
                                                        }
                                                        FreshnessTag {
                                                            Layout.fillWidth: true
                                                            visible: !!robotRow.tele
                                                            online: !!root.robotsOnline[modelData.name]
                                                            hasData: !!robotRow.tele
                                                            lastRx: robotRow.tele ? Number(robotRow.tele.last_rx || 0) : 0
                                                            nowTick: root.nowTick
                                                            fontSize: Math.max(11, 13 * fleetPanel.contentScale)
                                                        }
                                                        // Show speed, localization, and safety indicators.
                                                        Text {
                                                            visible: !!robotRow.tele
                                                            text: robotRow.tele
                                                                  ? Number(robotRow.tele.speed).toFixed(2) + " m/s"
                                                                    + (robotRow.stale ? "  ⚠ NO RECENT DATA" : "")
                                                                    + (robotRow.manualMode ? "  ⚠ MANUAL CONTROL" : "")
                                                                    + (robotRow.notLocalized ? "  ⚠ NOT LOCALIZED" : "")
                                                                    + (robotRow.teleUnsafe ? "  ⚠ " + robotRow.safetyLabel : "")
                                                                  : ""
                                                            color: robotRow.teleUnsafe ? C.err
                                                                   : ((robotRow.notLocalized || robotRow.stale || robotRow.manualMode) ? C.warn : C.textDim)
                                                            font.family: root.monoFontFamily
                                                            font.pixelSize: Math.max(11, 14 * fleetPanel.contentScale)
                                                            elide: Text.ElideRight; Layout.fillWidth: true
                                                        }
                                                        Text {
                                                            visible: !!robotRow.currentTask
                                                            text: "→ " + (robotRow.currentTask ? robotRow.currentTask.destination : "")
                                                            color: C.cyan
                                                            font.family: root.monoFontFamily
                                                            font.pixelSize: Math.max(11, 13 * fleetPanel.contentScale)
                                                            elide: Text.ElideRight; Layout.fillWidth: true
                                                        }
                                                    }
                                                    Text {
                                                        Layout.rightMargin: 14 * fleetPanel.contentScale
                                                        text: modelData.hasBattery
                                                              ? Number(modelData.battery).toFixed(0) + "%"
                                                                + (robotRow.tele && robotRow.tele.charging ? " ⚡" : "")
                                                              : "—%"
                                                        color: !modelData.hasBattery ? C.textDim
                                                               : (Number(modelData.battery) < 20 ? C.err : C.success)
                                                        // Last-known value, not live -- dim it while disconnected.
                                                        opacity: root.robotsOnline[modelData.name] ? 1.0 : 0.5
                                                        font.family: root.monoFontFamily
                                                        font.pixelSize: 20 * fleetPanel.contentScale
                                                        font.bold: true

                                                        MouseArea {
                                                            anchors.fill: parent
                                                            hoverEnabled: true
                                                            visible: !root.robotsOnline[modelData.name] && modelData.hasBattery
                                                            cursorShape: Qt.WhatsThisCursor
                                                            ToolTip.visible: containsMouse
                                                            ToolTip.delay: 400
                                                            ToolTip.text: "Last reported "
                                                                          + Number(modelData.battery).toFixed(0) + "%"
                                                                          + (robotRow.lastSeenText ? " · " + robotRow.lastSeenText : "")
                                                        }
                                                    }
                                                    ColumnLayout {
                                                        id: statusBlock
                                                        Layout.preferredWidth: 122 * fleetPanel.contentScale
                                                        Layout.alignment: Qt.AlignVCenter
                                                        spacing: 3
                                                        Rectangle {
                                                            Layout.fillWidth: true
                                                            Layout.preferredHeight: 40 * fleetPanel.contentScale
                                                            radius: 12 * fleetPanel.contentScale
                                                            color: "transparent"
                                                            // A live RMF status is meaningless once the robot itself is unreachable.
                                                            border.color: root.robotsOnline[modelData.name]
                                                                          ? root.statusColor(modelData.status) : C.err
                                                            border.width: 1
                                                            Text {
                                                                anchors.centerIn: parent
                                                                text: root.robotsOnline[modelData.name] ? modelData.status : "OFFLINE"
                                                                color: root.robotsOnline[modelData.name]
                                                                       ? root.statusColor(modelData.status) : C.err
                                                                font.family: root.monoFontFamily
                                                                font.pixelSize: 15 * fleetPanel.contentScale
                                                                font.bold: true
                                                                elide: Text.ElideRight
                                                                width: parent.width - 8
                                                                horizontalAlignment: Text.AlignHCenter
                                                            }
                                                            MouseArea {
                                                                anchors.fill: parent
                                                                hoverEnabled: true
                                                                visible: modelData.status === "PENDING SYNC"
                                                                cursorShape: Qt.WhatsThisCursor
                                                                ToolTip.visible: containsMouse
                                                                ToolTip.delay: 400
                                                                ToolTip.text: "Robot state is available through VDA5050, but "
                                                                              + "has not been synchronized with RMF yet."
                                                            }
                                                        }
                                                        Text {
                                                            Layout.fillWidth: true
                                                            Layout.topMargin: 6 * fleetPanel.contentScale
                                                            visible: robotRow.roundsRemaining > 0
                                                            text: robotRow.roundsCurrent + "/" + robotRow.roundsTotal + " rounds"
                                                            color: C.cyan
                                                            font.family: root.monoFontFamily
                                                            font.pixelSize: 14 * fleetPanel.contentScale
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
                                                            text: parent.text; color: C.textDim
                                                            font.pixelSize: 26 * fleetPanel.contentScale
                                                            horizontalAlignment: Text.AlignHCenter
                                                            verticalAlignment: Text.AlignVCenter
                                                        }
                                                        background: Rectangle {
                                                            radius: 8
                                                            color: parent.down ? C.border
                                                                   : (parent.hovered ? C.surfaceAlt : "transparent")
                                                            border.color: parent.hovered ? C.cyan : C.border
                                                            border.width: 1
                                                        }
                                                        onClicked: {
                                                            controlDialog.robotName = modelData.name
                                                            controlDialog.currentSpeedLimit = root.speedLimits[modelData.name] || 0
                                                            controlDialog.open()
                                                        }
                                                    }
                                                }
                                            }
                                        }

                                        Text {
                                            // PENDING SYNC robots still count as rows -- gate on the filtered list.
                                            anchors.centerIn: parent
                                            visible: fleetPanel.filteredDisplayRobots.length === 0
                                                     && fleetPanel.robotSearchText === ""
                                            text: ros.rmfOnline ? "No robots in this fleet" : "Waiting for /fleet_states"
                                            color: C.textDim
                                            font.pixelSize: 13 * fleetPanel.contentScale
                                        }
                                        Text {
                                            anchors.centerIn: parent
                                            visible: fleetPanel.filteredDisplayRobots.length === 0
                                                     && fleetPanel.robotSearchText !== ""
                                            text: "No robots match “" + fleetPanel.robotSearchText + "”"
                                            color: C.textDim
                                            font.pixelSize: 13 * fleetPanel.contentScale
                                        }
                                    }
                                }
                            }

                            Rectangle {
                                id: recentTasksPanel
                                objectName: "recentTasksPanel"
                                Layout.fillWidth: true
                                // Don't stretch an empty table into a tall blank panel.
                                Layout.fillHeight: root.tasks.length > 0 || panelTab === "traffic"
                                Layout.preferredHeight: 280
                                radius: 16
                                color: C.surface
                                border.color: C.border
                                border.width: 1
                                clip: true

                                // Size columns to the task table width.
                                readonly property real tableScale: Math.max(
                                    1.0, Math.min(1.40, width / 680))
                                readonly property real dateColumnWidth: 78 * tableScale
                                readonly property real pickupColumnWidth: 42 * tableScale
                                readonly property real robotColumnWidth: 48 * tableScale
                                readonly property real timeColumnWidth: 74 * tableScale
                                readonly property real stateColumnWidth: 78 * tableScale
                                readonly property real actionColumnWidth: 26
                                readonly property real identityColumnGap: 8 * tableScale

                                property string panelTab: "tasks"   // tasks | traffic
                                property string taskSearchText: ""
                                property string taskStateFilter: "All"

                                readonly property var filteredTasks: {
                                    var list = root.tasks
                                    if (taskStateFilter !== "All") {
                                        var wanted = taskStateFilter.toLowerCase()
                                        list = list.filter(function(t) {
                                            return String(t.state || "").toLowerCase() === wanted
                                        })
                                    }
                                    if (taskSearchText !== "") {
                                        var q = taskSearchText.toLowerCase()
                                        list = list.filter(function(t) {
                                            return String(t.robot || "").toLowerCase().indexOf(q) >= 0
                                                || String(t.destination || "").toLowerCase().indexOf(q) >= 0
                                                || String(t.requester || "").toLowerCase().indexOf(q) >= 0
                                        })
                                    }
                                    // Show active tasks first, newest first within each group.
                                    var underway = []
                                    var rest = []
                                    for (var k = 0; k < list.length; k++) {
                                        if (list[k].state === "underway") underway.push(list[k])
                                        else rest.push(list[k])
                                    }
                                    return underway.concat(rest)
                                }

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
                                                color: C.accent
                                            }
                                            Repeater {
                                                model: [{ key: "tasks", label: "RECENT TASKS" },
                                                        { key: "traffic", label: "VDA5050 TRAFFIC" }]
                                                delegate: Text {
                                                    readonly property bool active: recentTasksPanel.panelTab === modelData.key
                                                    text: modelData.label
                                                    color: active ? C.text : C.textDim
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
                                                        color: C.accent
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
                                                placeholderTextColor: C.placeholderText
                                                color: C.text
                                                font.pixelSize: 11 * recentTasksPanel.tableScale
                                                verticalAlignment: Text.AlignVCenter
                                                leftPadding: 8
                                                onTextChanged: recentTasksPanel.taskSearchText = text
                                                background: Rectangle {
                                                    radius: 8; color: C.surfaceAlt
                                                    border.color: C.border; border.width: 1
                                                }
                                            }
                                            ComboBox {
                                                id: taskStateCombo
                                                visible: recentTasksPanel.panelTab === "tasks"
                                                Layout.preferredWidth: 108 * recentTasksPanel.tableScale
                                                Layout.preferredHeight: 28 * recentTasksPanel.tableScale
                                                model: ["All", "Queued", "Underway", "Completed", "Cancelled", "Failed"]
                                                font.pixelSize: 11 * recentTasksPanel.tableScale
                                                onActivated: recentTasksPanel.taskStateFilter = currentText
                                                contentItem: Text {
                                                    text: taskStateCombo.displayText; color: C.text; font: taskStateCombo.font
                                                    leftPadding: 8; elide: Text.ElideRight
                                                    verticalAlignment: Text.AlignVCenter
                                                }
                                                background: Rectangle {
                                                    radius: 8; color: C.surfaceAlt
                                                    border.color: C.border; border.width: 1
                                                }
                                            }
                                            Rectangle {
                                                // Hide the count on a narrow card.
                                                visible: recentTasksPanel.panelTab === "traffic" || recentTasksPanel.width >= 560
                                                Layout.preferredWidth: recordsText.implicitWidth + 18
                                                Layout.preferredHeight: 24
                                                radius: 8
                                                color: "#143452"
                                                border.color: "#235278"
                                                border.width: 1

                                                Text {
                                                    id: recordsText
                                                    anchors.centerIn: parent
                                                    text: recentTasksPanel.panelTab === "traffic"
                                                          ? root.traffic.length + " messages"
                                                          : (recentTasksPanel.taskSearchText === "" && recentTasksPanel.taskStateFilter === "All")
                                                            ? root.tasks.length + " records"
                                                            : recentTasksPanel.filteredTasks.length + " / " + root.tasks.length
                                                    color: C.cyan
                                                    font.family: root.monoFontFamily
                                                    font.pixelSize: Math.max(11, 10 * recentTasksPanel.tableScale)
                                                    font.bold: true
                                                }
                                            }
                                        }
                                    }

                                    Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: C.border; opacity: 0.65 }

                                    // Use the same column widths for headers and rows.
                                    Rectangle {
                                        visible: recentTasksPanel.panelTab === "tasks"
                                        Layout.fillWidth: true
                                        Layout.preferredHeight: 34 * recentTasksPanel.tableScale
                                        color: "#0A1A2B"
                                        RowLayout {
                                            anchors.fill: parent
                                            anchors.leftMargin: 19
                                            anchors.rightMargin: 16
                                            spacing: 4
                                            Text { text: "DATE";      Layout.preferredWidth: recentTasksPanel.dateColumnWidth;      Layout.minimumWidth: Layout.preferredWidth; Layout.rightMargin: recentTasksPanel.identityColumnGap; color: C.textDim; font.pixelSize: 13 * recentTasksPanel.tableScale; font.bold: true; font.letterSpacing: 0.6; horizontalAlignment: Text.AlignHCenter }
                                            Text { text: "PICKUP";    Layout.preferredWidth: recentTasksPanel.pickupColumnWidth;    Layout.minimumWidth: Layout.preferredWidth; color: C.textDim; font.pixelSize: 13 * recentTasksPanel.tableScale; font.bold: true; font.letterSpacing: 0.6; horizontalAlignment: Text.AlignHCenter }
                                            Text { text: "DEST.";     Layout.fillWidth: true; color: C.textDim; font.pixelSize: 13 * recentTasksPanel.tableScale; font.bold: true; font.letterSpacing: 0.6; horizontalAlignment: Text.AlignHCenter }
                                            Text { text: "ROBOT";     Layout.preferredWidth: recentTasksPanel.robotColumnWidth;     Layout.minimumWidth: Layout.preferredWidth; color: C.textDim; font.pixelSize: 13 * recentTasksPanel.tableScale; font.bold: true; font.letterSpacing: 0.6; horizontalAlignment: Text.AlignHCenter }
                                            Text { text: "TIME";      Layout.preferredWidth: recentTasksPanel.timeColumnWidth;      Layout.minimumWidth: Layout.preferredWidth; color: C.textDim; font.pixelSize: 13 * recentTasksPanel.tableScale; font.bold: true; font.letterSpacing: 0.6; horizontalAlignment: Text.AlignHCenter }
                                            Text { text: "STATE";     Layout.preferredWidth: recentTasksPanel.stateColumnWidth;     Layout.minimumWidth: Layout.preferredWidth; color: C.textDim; font.pixelSize: 13 * recentTasksPanel.tableScale; font.bold: true; font.letterSpacing: 0.6; horizontalAlignment: Text.AlignHCenter }
                                            Item { Layout.preferredWidth: recentTasksPanel.actionColumnWidth; Layout.minimumWidth: Layout.preferredWidth }
                                        }
                                    }
                                    Rectangle { visible: recentTasksPanel.panelTab === "tasks"; Layout.fillWidth: true; Layout.preferredHeight: 1; color: C.border; opacity: 0.4 }

                                    VdaTrafficPanel {
                                        objectName: "vdaTraffic"
                                        visible: recentTasksPanel.panelTab === "traffic"
                                        Layout.fillWidth: true
                                        Layout.fillHeight: true
                                        traffic: root.traffic
                                        uiScale: recentTasksPanel.tableScale
                                    }

                                    Item {
                                        visible: recentTasksPanel.panelTab === "tasks"
                                        Layout.fillWidth: true
                                        Layout.fillHeight: true

                                        ListView {
                                            anchors.fill: parent
                                            anchors.margins: 8
                                            model: recentTasksPanel.filteredTasks
                                            clip: true
                                            spacing: 2
                                            boundsBehavior: Flickable.StopAtBounds

                                            delegate: Rectangle {
                                                width: ListView.view.width
                                                height: 48 * recentTasksPanel.tableScale
                                                radius: 8
                                                color: taskRowHover.hovered
                                                       ? C.surfaceRaised
                                                       : (index % 2 === 0 ? C.surfaceAlt : "transparent")

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
                                                        text: modelData.date
                                                        Layout.preferredWidth: recentTasksPanel.dateColumnWidth
                                                        Layout.minimumWidth: Layout.preferredWidth
                                                        Layout.rightMargin: recentTasksPanel.identityColumnGap
                                                        color: C.text
                                                        font.family: root.monoFontFamily
                                                        font.pixelSize: 12 * recentTasksPanel.tableScale
                                                        font.bold: true
                                                        horizontalAlignment: Text.AlignHCenter
                                                    }
                                                    Text { text: modelData.pickup; Layout.preferredWidth: recentTasksPanel.pickupColumnWidth; Layout.minimumWidth: Layout.preferredWidth; color: C.textDim; font.family: root.monoFontFamily; font.pixelSize: 12 * recentTasksPanel.tableScale; font.bold: true; elide: Text.ElideRight; horizontalAlignment: Text.AlignHCenter }
                                                    Text { text: modelData.destination; Layout.fillWidth: true; color: C.text; font.bold: true; font.pixelSize: 14 * recentTasksPanel.tableScale; elide: Text.ElideRight; horizontalAlignment: Text.AlignHCenter }
                                                    Text { text: modelData.robot; Layout.preferredWidth: recentTasksPanel.robotColumnWidth; Layout.minimumWidth: Layout.preferredWidth; color: C.text; font.family: root.monoFontFamily; font.pixelSize: 12 * recentTasksPanel.tableScale; font.bold: true; elide: Text.ElideRight; horizontalAlignment: Text.AlignHCenter }
                                                    Column {
                                                        Layout.preferredWidth: recentTasksPanel.timeColumnWidth
                                                        Layout.minimumWidth: Layout.preferredWidth
                                                        spacing: 1
                                                        Text {
                                                            width: parent.width
                                                            text: modelData.start
                                                            color: "#B7CCE0"
                                                            font.family: root.monoFontFamily
                                                            font.pixelSize: 11 * recentTasksPanel.tableScale
                                                            elide: Text.ElideRight
                                                            horizontalAlignment: Text.AlignHCenter
                                                        }
                                                        Text {
                                                            width: parent.width
                                                            text: modelData.end !== "—" ? "→ " + modelData.end : "—"
                                                            color: C.textDim
                                                            font.family: root.monoFontFamily
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
                                                        property color badgeColor: root.taskColor(modelData.state)
                                                        color: Qt.rgba(badgeColor.r, badgeColor.g, badgeColor.b, 0.12)
                                                        border.color: Qt.rgba(badgeColor.r, badgeColor.g, badgeColor.b, 0.45)
                                                        border.width: 1

                                                        Text {
                                                            anchors.centerIn: parent
                                                            width: parent.width - 8
                                                            text: modelData.state.toUpperCase()
                                                            color: parent.badgeColor
                                                            font.family: root.monoFontFamily
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
                                                            visible: modelData.state === "queued" || modelData.state === "underway"
                                                            padding: 0
                                                            contentItem: Text {
                                                                text: "×"
                                                                color: C.err
                                                                font.pixelSize: 14
                                                                font.bold: true
                                                                horizontalAlignment: Text.AlignHCenter
                                                                verticalAlignment: Text.AlignVCenter
                                                            }
                                                            background: Rectangle {
                                                                radius: 7
                                                                color: cancelTaskButton.hovered ? "#3B1B2A" : "transparent"
                                                                border.color: C.err
                                                                border.width: 1
                                                            }
                                                            onClicked: {
                                                                if (modelData.rmf_id && modelData.rmf_id.length > 0)
                                                                    ros.cancel_task(modelData.rmf_id)
                                                            }
                                                        }
                                                    }
                                                }
                                            }
                                        }

                                        Column {
                                            anchors.centerIn: parent
                                            visible: recentTasksPanel.filteredTasks.length === 0
                                            spacing: 10
                                            Text {
                                                anchors.horizontalCenter: parent.horizontalCenter
                                                text: root.tasks.length === 0 ? "NO ACTIVE MISSIONS" : "NO MATCHING TASKS"
                                                color: C.textDim; font.pixelSize: 12; font.bold: true
                                            }
                                            Text {
                                                visible: root.tasks.length > 0
                                                anchors.horizontalCenter: parent.horizontalCenter
                                                text: "Try clearing the search or filter"
                                                color: C.textDim; opacity: 0.65; font.pixelSize: 10
                                            }
                                            Button {
                                                visible: root.tasks.length === 0
                                                anchors.horizontalCenter: parent.horizontalCenter
                                                text: "+  CREATE NEW TASK"
                                                implicitHeight: 34
                                                leftPadding: 15
                                                rightPadding: 15
                                                contentItem: Text {
                                                    text: parent.text
                                                    color: "white"
                                                    font.pixelSize: 11
                                                    font.bold: true
                                                    horizontalAlignment: Text.AlignHCenter
                                                    verticalAlignment: Text.AlignVCenter
                                                }
                                                background: Rectangle {
                                                    radius: 9
                                                    color: parent.down ? C.accentDark : C.accent
                                                }
                                                onClicked: taskDialog.open()
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
