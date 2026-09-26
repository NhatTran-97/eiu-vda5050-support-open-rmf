import QtQuick
import QtQuick.Layouts
import QtQuick.Controls
import "components"
import "pages"

ApplicationWindow {
    id: root
    visibility: Window.Maximized
    width: 1440
    height: 900
    minimumWidth: 1120
    minimumHeight: 700
    title: "EIU Fleet Control Center"
    color: Theme.bg

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
    // Robots the fleet adapters found on the broker that no fleet has registered.
    property var pendingRobots: []
    // Values the dashboard refreshes, read once here and shared by every binding.
    readonly property var robotsOnline: dashboard.online
    readonly property var telemetry: dashboard.telemetry
    readonly property var speedLimits: dashboard.speedLimits

    // Selected robot, shared by Fleet Robots, the map and the telemetry panel.
    property string selectedRobotName: ""
    function selectRobot(name) {
        root.selectedRobotName = name
        mapCard.analytics.userPicked = true
        mapCard.analytics.selectedRobotName = name
    }

    function openControls(name) {
        controlDialog.robotName = name
        controlDialog.currentSpeedLimit = root.speedLimits[name] || 0
        controlDialog.open()
    }

    readonly property real kpiHeight: Math.max(116, Math.min(142, width / 15))

    // Ticks "last seen" text forward between backend telemetry updates.
    property real nowTick: Date.now()
    Timer { interval: 1000; running: true; repeat: true; onTriggered: root.nowTick = Date.now() }

    function telemetryFor(name) { return root.telemetry[name] || null }
    function reloadPendingRobots() { root.pendingRobots = JSON.parse(registry.pendingJson) }

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
        reloadPendingRobots()
        // Children complete first, so FleetAnalytics has already auto-selected a robot.
        if (mapCard.analytics.selectedRobotName)
            root.selectedRobotName = mapCard.analytics.selectedRobotName
    }

    // Report a cancel that RMF refused or did not answer.
    Connections {
        target: ros
        function onDispatchResult(id, kind, ok, message) {
            if (kind !== "cancel") return
            robotToast.pending = null
            robotToast.show(ok ? "Task cancelled." : "Cancel not done: " + message, ok ? "ok" : "error", "", ok ? 5000 : 12000)
        }
    }

    Connections {
        target: registry
        function onChanged() { root.reloadPendingRobots() }
        function onNewRobotsDetected(keysJson) {
            var keys = JSON.parse(keysJson)
            var fresh = root.pendingRobots.filter(function (p) { return keys.indexOf(p.key) >= 0 })
            if (fresh.length === 1) {
                robotToast.pending = fresh[0]
                var removedAs = fresh[0].removed_as
                robotToast.show(removedAs
                                ? "Removed robot " + removedAs.name + " is online again. Register it to restore it in " + removedAs.fleet + "."
                                : "New robot detected: " + fresh[0].manufacturer + "/" + fresh[0].serial
                                  + (fresh[0].series ? " (" + fresh[0].series + ")" : "")
                                  + ". It is not registered in any fleet.", "info", "REGISTER", 20000)
            } else if (fresh.length > 1) {
                robotToast.pending = null
                robotToast.show(fresh.length + " new robots detected. See Needs Attention to register them.",
                                "info", "", 12000)
            }
        }
        function onRequestResult(text) {
            var result = JSON.parse(text)
            // The dialog shows its own failures.
            if (!result.ok && registerDialog.opened) return
            robotToast.pending = null
            if (result.ok && result.action === "remove")
                robotToast.show("Robot " + result.name + " removed. RMF keeps it decommissioned until the fleet adapter restarts.",
                                "warn", "", 12000)
            else if (result.ok)
                robotToast.show("Robot " + result.name + " registered in " + result.fleet
                                + (result.persisted ? "." : ", but it could not be saved for the next start."),
                                result.persisted ? "ok" : "warn", "", 9000)
            else
                robotToast.show((result.action === "remove" ? "Could not remove " : "Could not register ") + result.name + ": "
                                + (result.errors.length > 0 ? result.errors[0].message : "no reason given"),
                                "error", "", 12000)
        }
    }

    RobotControlDialog {
        id: controlDialog
        mapPage: mapCard.mapView
    }

    NewTaskDialog {
        id: taskDialog
        places: root.wpNames
        onlineRobots: dashboard.onlineRobotNames
        anchors.centerIn: parent
    }

    RegisterRobotDialog {
        id: registerDialog
        anchors.centerIn: parent
    }

    SystemMetricsDialog {
        id: systemDialog
        objectName: "systemDialog"
        // Covers the dashboard, leaving the navigation rail.
        railWidth: navRail.width
    }

    Toast {
        id: robotToast
        objectName: "robotToast"
        property var pending: null
        z: 100
        // Bottom edge: the top is the title and the RMF/MQTT status chips.
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 24
        anchors.horizontalCenter: parent.horizontalCenter
        onActionTriggered: if (pending) registerDialog.openFor(pending)
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        NavRail {
            id: navRail
            objectName: "navRail"
            Layout.preferredWidth: 218
            Layout.fillHeight: true
            onSystemRequested: systemDialog.open()
        }

        // Dashboard content.
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            TopBar {
                Layout.fillWidth: true
                Layout.preferredHeight: 82
                onSystemRequested: systemDialog.open()
                onNewTaskRequested: taskDialog.open()
            }

            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 20
                    spacing: 16

                    KpiRow {
                        objectName: "kpiRow"
                        Layout.fillWidth: true
                        Layout.preferredHeight: root.kpiHeight
                        Layout.minimumHeight: root.kpiHeight
                        Layout.maximumHeight: root.kpiHeight
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
                                       ? Theme.accent : Theme.border

                                Behavior on width {
                                    NumberAnimation { duration: 100 }
                                }
                                Behavior on color {
                                    ColorAnimation { duration: 120 }
                                }
                            }
                        }

                        MapCard {
                            id: mapCard
                            objectName: "mapPanel"
                            SplitView.fillWidth: true
                            SplitView.minimumWidth: 500
                            waypoints: root.waypoints
                            lanes: root.lanes
                            blockedEdgeIndices: root.blockedEdgeIndices
                            selectedRobotName: root.selectedRobotName
                            nowTick: root.nowTick
                            onRobotPicked: (name) => root.selectRobot(name)
                            onAnalyticsRobotChanged: (name) => root.selectedRobotName = name
                        }

                        // What needs attention, the robots, and the tasks; each panel keeps a usable
                        // minimum height and the lists inside scroll.
                        ColumnLayout {
                            id: fleetPanel
                            objectName: "fleetPanel"
                            // Map : robot panel split is 72 : 28 by default.
                            readonly property real panelRatio: 0.28
                            readonly property bool smallFleet: dashboard.robotCount <= 3
                            SplitView.preferredWidth: dashboardSplit.width * panelRatio
                            SplitView.minimumWidth: smallFleet ? 420 : 640
                            SplitView.maximumWidth: dashboardSplit.width * 0.38
                            spacing: 16

                            // Scale typography with the robot panel width.
                            readonly property real contentScale: Math.max(
                                0.7, Math.min(1.25, width / 720))

                            NeedsAttentionPanel {
                                objectName: "needsAttentionPanel"
                                Layout.minimumHeight: 90
                                nowTick: root.nowTick
                                onRobotFocused: (name) => root.selectRobot(name)
                                onControlsRequested: (name) => root.openControls(name)
                                onRegisterRequested: (pending) => registerDialog.openFor(pending)
                            }

                            RobotListPanel {
                                objectName: "activeRobotsPanel"
                                Layout.minimumHeight: 150
                                selectedRobotName: root.selectedRobotName
                                nowTick: root.nowTick
                                contentScale: fleetPanel.contentScale
                                onRobotSelected: (name) => root.selectRobot(name)
                                onControlsRequested: (name) => root.openControls(name)
                            }

                            TasksPanel {
                                objectName: "recentTasksPanel"
                                Layout.fillWidth: true
                                Layout.minimumHeight: 160
                                onNewTaskRequested: taskDialog.open()
                            }
                        }
                    }
                }
            }
        }
    }
}
