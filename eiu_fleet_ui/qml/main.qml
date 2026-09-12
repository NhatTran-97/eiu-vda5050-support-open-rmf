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
    property var robots: []
    property var tasks: []
    // {robot_name: bool} — direct VDA5050 connectivity, distinct from rmfOnline
    // (which only says the /fleet_states pipe is alive, not any one robot).
    property var robotsOnline: ({})
    // {robot_name: RobotState dict} — raw VDA5050 telemetry (speed, safety,
    // errors, ...), keyed by whichever robots are actually reporting.
    property var telemetry: ({})
    // {robot_name: float} — operator speed cap currently applied, 0 = none.
    property var speedLimits: ({})
    readonly property string monoFontFamily: fontMono

    readonly property int activeTaskCount: {
        var count = 0
        for (var i = 0; i < tasks.length; ++i) {
            if (tasks[i].state === "queued" || tasks[i].state === "underway")
                count++
        }
        return count
    }

    readonly property real averageBattery: {
        if (robots.length === 0)
            return 0
        var total = 0
        for (var i = 0; i < robots.length; ++i)
            total += Number(robots[i].battery || 0)
        return total / robots.length
    }
    readonly property real kpiHeight: Math.max(116, Math.min(142, width / 15))
    function reloadRobots() { root.robots = JSON.parse(ros.robotsJson) }
    function reloadTasks()  { root.tasks = JSON.parse(ros.tasksJson) }
    function reloadRobotsOnline() { root.robotsOnline = JSON.parse(mqtt.robotsOnlineJson) }
    function reloadTelemetry() { root.telemetry = JSON.parse(mqtt.telemetryJson) }
    function telemetryFor(name) { return root.telemetry[name] || null }
    function reloadSpeedLimits() { root.speedLimits = JSON.parse(control.speedLimitsJson) }

    // root.robots (from /fleet_states) omits any robot RMF hasn't merged onto
    // the nav graph yet -- but Robot Control (pause/speed/re-localize) talks
    // to the fleet adapter directly over ROS and works before that merge, so
    // gating the whole row on /fleet_states would make re-localizing a
    // never-merged robot unreachable from this UI. Every configured robot
    // gets a row; one missing from /fleet_states gets a placeholder instead.
    readonly property var displayRobots: {
        var known = JSON.parse(cfg.robotNamesJson)
        var byName = {}
        for (var i = 0; i < root.robots.length; i++)
            byName[root.robots[i].name] = root.robots[i]

        var out = []
        for (var j = 0; j < known.length; j++) {
            var name = known[j]
            if (byName[name]) {
                out.push(byName[name])
                continue
            }
            var tele = root.telemetryFor(name)
            out.push({
                key: cfg.fleetName + "/" + name,
                name: name,
                fleet: cfg.fleetName,
                model: "",
                status: "NOT MERGED",
                battery: (tele && tele.battery_soc != null) ? tele.battery_soc * 100 : 0,
                level: (tele && tele.map_id) ? tele.map_id : "—",
                task: "",
                finish: "",
                updated: "",
                x: 0, y: 0, yaw: 0,
                path: []
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
        if (status === "PAUSED" || status === "WAITING" || status === "NOT MERGED")
            return C.warn
        return C.textDim
    }

    function taskColor(state) {
        if (state === "completed" || state.indexOf("complet") >= 0)
            return C.success
        if (state === "failed" || state === "cancelled" ||
                state.indexOf("fail") >= 0 || state.indexOf("cancel") >= 0)
            return C.err
        if (state === "queued" || state.indexOf("queue") >= 0 || state.indexOf("stale") >= 0)
            return C.warn
        return C.cyan
    }

    Component.onCompleted: {
        var wps = JSON.parse(mapProv.wpJson)
        root.waypoints = wps
        root.lanes = JSON.parse(mapProv.lanesJson)
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
        reloadSpeedLimits()
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
        anchors.centerIn: parent
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        // ── Navigation rail ──────────────────────────────────────────────────
        Rectangle {
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
                            color: modelData.active ? C.accent : "transparent"

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
                                    color: modelData.active ? "white" : C.textDim
                                    font.pixelSize: 13
                                    font.bold: modelData.active
                                }
                            }
                        }
                    }
                }

                Item { Layout.fillHeight: true }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 98
                    Layout.leftMargin: 16
                    Layout.rightMargin: 16
                    Layout.bottomMargin: 18
                    radius: 13
                    color: C.surface
                    border.color: C.border
                    border.width: 1

                    Column {
                        anchors.fill: parent
                        anchors.margins: 14
                        spacing: 8

                        Text {
                            text: "SYSTEM HEALTH"
                            color: C.textDim
                            font.pixelSize: 9
                            font.bold: true
                            font.letterSpacing: 1.2
                        }
                        Row {
                            spacing: 8
                            Rectangle {
                                width: 8
                                height: 8
                                radius: 4
                                color: ros.rmfOnline ? C.success : C.err
                                anchors.verticalCenter: parent.verticalCenter
                            }
                            Text {
                                text: ros.rmfOnline ? "All services nominal" : "RMF connection offline"
                                color: ros.rmfOnline ? C.success : C.err
                                font.pixelSize: 11
                            }
                        }
                        Text {
                            text: "Fleet UI  ·  v0.2.0"
                            color: C.textDim
                            opacity: 0.7
                            font.family: root.monoFontFamily
                            font.pixelSize: 10
                        }
                    }
                }
            }
        }

        // ── Dashboard ────────────────────────────────────────────────────────
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

                    // Only shown when vda5050.ui_websocket_uri is configured --
                    // otherwise "WS OFFLINE" would just be noise for a feature
                    // nobody enabled.
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
                        Layout.fillWidth: true
                        Layout.preferredHeight: root.kpiHeight
                        Layout.minimumHeight: root.kpiHeight
                        Layout.maximumHeight: root.kpiHeight
                        spacing: 14

                        MetricCard {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            Layout.minimumWidth: 210
                            title: "System status"
                            value: ros.rmfOnline ? "ACTIVE" : "OFFLINE"
                            valueFontFamily: root.monoFontFamily
                            detail: ros.rmfOnline ? "Open-RMF is responding" : "Waiting for fleet states"
                            iconText: "●"
                            accentColor: ros.rmfOnline ? C.success : C.err
                        }
                        MetricCard {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            Layout.minimumWidth: 210
                            title: "Fleet"
                            value: root.robots.length + (root.robots.length === 1 ? " robot" : " robots")
                            valueFontFamily: root.monoFontFamily
                            detail: root.robots.length > 0 ? "Reporting live telemetry" : "No robots discovered"
                            iconText: "R"
                            accentColor: C.cyan
                        }
                        MetricCard {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            Layout.minimumWidth: 210
                            title: "Average battery"
                            value: root.robots.length > 0 ? root.averageBattery.toFixed(0) + "%" : "—"
                            valueFontFamily: root.monoFontFamily
                            detail: root.robots.length > 0 ? "Across the active fleet" : "Waiting for telemetry"
                            iconText: "ϟ"
                            accentColor: root.averageBattery > 20 ? C.success : C.warn
                        }
                        MetricCard {
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            Layout.minimumWidth: 210
                            title: "Active tasks"
                            value: root.activeTaskCount.toString()
                            valueFontFamily: root.monoFontFamily
                            detail: root.tasks.length + " total task records"
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

                        // ── Main map card ────────────────────────────────────
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
                                            spacing: 12
                                            Row {
                                                spacing: 5
                                                Rectangle { width: 7; height: 7; radius: 4; color: C.accent; anchors.verticalCenter: parent.verticalCenter }
                                                Text { text: "Lanes"; color: C.textDim; font.pixelSize: 10 }
                                            }
                                            Row {
                                                spacing: 5
                                                Rectangle { width: 7; height: 7; radius: 4; color: C.success; anchors.verticalCenter: parent.verticalCenter }
                                                Text { text: "Route"; color: C.textDim; font.pixelSize: 10 }
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

                                    // Fit the map region to the occupancy image's real
                                    // aspect ratio. The remaining height belongs to the
                                    // analytics region, so no fixed 50/50-style ratio is
                                    // needed when the window or SplitView width changes.
                                    readonly property real naturalMapHeight: {
                                        if (mapProv.pixelW <= 0 || mapProv.pixelH <= 0)
                                            return 0
                                        var frameInsets = 36  // Loader + MapPage image margins
                                        var contentWidth = Math.max(1, width - frameInsets)
                                        return frameInsets + contentWidth
                                               * mapProv.pixelH / mapProv.pixelW
                                    }

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
                                        SplitView.preferredHeight: mapAnalyticsSplit.naturalMapHeight
                                        SplitView.minimumHeight: 160

                                        Loader {
                                            id: mapLoader
                                            anchors.fill: parent
                                            anchors.margins: 10
                                            source: Qt.resolvedUrl("pages/MapPage.qml")
                                        }
                                    }

                                    Item {
                                        SplitView.fillHeight: true
                                        SplitView.minimumHeight: 205

                                        FleetAnalytics {
                                            anchors.fill: parent
                                            anchors.leftMargin: 10
                                            anchors.rightMargin: 10
                                            anchors.bottomMargin: 10
                                            robots: root.displayRobots
                                            tasks: root.tasks
                                            robotsOnline: root.robotsOnline
                                            waypoints: root.waypoints
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
                                property: "mapRobots"
                                value: root.robots
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
                                property: "robotIconSource"
                                value: robotIconUrl
                                when: mapLoader.status === Loader.Ready
                            }
                        }

                        // ── Live fleet panels ────────────────────────────────
                        ColumnLayout {
                            id: fleetPanel
                            objectName: "fleetPanel"
                            SplitView.preferredWidth: 720
                            SplitView.minimumWidth: 640
                            SplitView.maximumWidth: 900
                            spacing: 16

                            // Typography follows the width of this panel so it remains
                            // comfortably readable on large control-room displays.
                            readonly property real contentScale: Math.max(
                                1.0, Math.min(1.25, width / 720))

                            Rectangle {
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
                                                text: "ACTIVE ROBOTS"
                                                color: C.text
                                                font.pixelSize: 13 * fleetPanel.contentScale
                                                font.bold: true
                                                font.letterSpacing: 0.8
                                            }
                                            Item { Layout.fillWidth: true }
                                            Rectangle {
                                                Layout.preferredWidth: 30 * fleetPanel.contentScale
                                                Layout.preferredHeight: 25 * fleetPanel.contentScale
                                                radius: 8
                                                color: "#17365A"
                                                Text {
                                                    anchors.centerIn: parent
                                                    text: root.robots.length
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
                                            model: root.displayRobots
                                            clip: true
                                            spacing: 3
                                            boundsBehavior: Flickable.StopAtBounds

                                            delegate: Rectangle {
                                                id: robotRow
                                                width: ListView.view.width
                                                height: 110 * fleetPanel.contentScale
                                                radius: 10
                                                color: index % 2 === 0 ? C.surfaceAlt : "transparent"

                                                // VDA5050 state.* telemetry for this robot, if any has arrived yet.
                                                readonly property var tele: root.telemetryFor(modelData.name)
                                                readonly property bool teleUnsafe: tele && (tele.safety.triggered || tele.fatal_error !== "")
                                                // No usable pose on the fleet adapter side: it can't path-plan a new
                                                // dispatch or a finishing_request return until re-localized.
                                                readonly property bool notLocalized: tele && tele.position_initialized === false

                                                // Multi-round loop task currently assigned to this robot, if any.
                                                readonly property var currentTask: {
                                                    if (!modelData.task) return null
                                                    for (var i = 0; i < root.tasks.length; i++) {
                                                        if (root.tasks[i].rmf_id === modelData.task)
                                                            return root.tasks[i]
                                                    }
                                                    return null
                                                }
                                                readonly property int roundsRemaining:
                                                    (currentTask && currentTask.rounds > 1)
                                                    ? (currentTask.rounds_remaining || 0) : 0

                                                RowLayout {
                                                    anchors.fill: parent
                                                    anchors.leftMargin: 10
                                                    anchors.rightMargin: 10
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
                                                        // VDA5050 connectivity dot, straight from the robot's own
                                                        // `connection` topic — not inferred from /fleet_states age.
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
                                                        Layout.fillWidth: true
                                                        spacing: 3
                                                        Text { text: modelData.name; color: C.text; font.pixelSize: 22 * fleetPanel.contentScale; font.bold: true; elide: Text.ElideRight; Layout.fillWidth: true }
                                                        Text {
                                                            text: modelData.fleet + "  ·  " + modelData.level
                                                                  + (root.robotsOnline[modelData.name] ? "" : "  ·  VDA5050 offline")
                                                            color: root.robotsOnline[modelData.name] ? C.textDim : C.err
                                                            font.family: root.monoFontFamily
                                                            font.pixelSize: 16 * fleetPanel.contentScale
                                                            elide: Text.ElideRight; Layout.fillWidth: true
                                                        }
                                                        // Live VDA5050 telemetry: speed, a not-localized warning
                                                        // (blocks path planning until SET POSITION is used), and a
                                                        // safety/error flag when eStop, a field violation or a
                                                        // FATAL error is active.
                                                        Text {
                                                            visible: !!robotRow.tele
                                                            text: robotRow.tele
                                                                  ? Number(robotRow.tele.speed).toFixed(2) + " m/s"
                                                                    + (robotRow.notLocalized ? "  ⚠ NOT LOCALIZED" : "")
                                                                    + (robotRow.teleUnsafe
                                                                       ? "  ⚠ " + (robotRow.tele.fatal_error || robotRow.tele.safety.e_stop)
                                                                       : "")
                                                                  : ""
                                                            color: robotRow.teleUnsafe ? C.err
                                                                   : (robotRow.notLocalized ? C.warn : C.textDim)
                                                            font.family: root.monoFontFamily
                                                            font.pixelSize: 14 * fleetPanel.contentScale
                                                            elide: Text.ElideRight; Layout.fillWidth: true
                                                        }
                                                    }
                                                    Text {
                                                        text: Number(modelData.battery).toFixed(0) + "%"
                                                              + (robotRow.tele && robotRow.tele.charging ? " ⚡" : "")
                                                        color: Number(modelData.battery) < 20 ? C.err : C.success
                                                        font.family: root.monoFontFamily
                                                        font.pixelSize: 20 * fleetPanel.contentScale
                                                        font.bold: true
                                                    }
                                                    ColumnLayout {
                                                        Layout.preferredWidth: 122 * fleetPanel.contentScale
                                                        spacing: 3
                                                        Rectangle {
                                                            Layout.preferredWidth: 122 * fleetPanel.contentScale
                                                            Layout.preferredHeight: 40 * fleetPanel.contentScale
                                                            radius: 12 * fleetPanel.contentScale
                                                            color: "transparent"
                                                            border.color: root.statusColor(modelData.status)
                                                            border.width: 1
                                                            Text {
                                                                anchors.centerIn: parent
                                                                text: modelData.status
                                                                color: root.statusColor(modelData.status)
                                                                font.family: root.monoFontFamily
                                                                font.pixelSize: 15 * fleetPanel.contentScale
                                                                font.bold: true
                                                                elide: Text.ElideRight
                                                                width: parent.width - 8
                                                                horizontalAlignment: Text.AlignHCenter
                                                            }
                                                        }
                                                        Text {
                                                            Layout.fillWidth: true
                                                            visible: robotRow.roundsRemaining > 0
                                                            text: robotRow.roundsRemaining + (robotRow.roundsRemaining === 1 ? " round left" : " rounds left")
                                                            color: C.textDim
                                                            font.family: root.monoFontFamily
                                                            font.pixelSize: 12 * fleetPanel.contentScale
                                                            horizontalAlignment: Text.AlignHCenter
                                                        }
                                                    }
                                                    Button {
                                                        Layout.preferredWidth: 36 * fleetPanel.contentScale
                                                        Layout.preferredHeight: 36 * fleetPanel.contentScale
                                                        text: "⚙"
                                                        contentItem: Text {
                                                            text: parent.text; color: C.textDim
                                                            font.pixelSize: 18 * fleetPanel.contentScale
                                                            horizontalAlignment: Text.AlignHCenter
                                                            verticalAlignment: Text.AlignVCenter
                                                        }
                                                        background: Rectangle {
                                                            radius: 8; color: parent.down ? C.border : "transparent"
                                                            border.color: C.border; border.width: 1
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
                                            anchors.centerIn: parent
                                            visible: root.robots.length === 0
                                            text: ros.rmfOnline ? "No robots in this fleet" : "Waiting for /fleet_states"
                                            color: C.textDim
                                            font.pixelSize: 13 * fleetPanel.contentScale
                                        }
                                    }
                                }
                            }

                            Rectangle {
                                id: recentTasksPanel
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                Layout.preferredHeight: 300
                                radius: 16
                                color: C.surface
                                border.color: C.border
                                border.width: 1
                                clip: true

                                // Scale from the space actually available to the table,
                                // not from the whole window. This keeps every column
                                // readable after the SplitView handle is dragged.
                                readonly property real tableScale: Math.max(
                                    1.0, Math.min(1.40, width / 680))
                                readonly property real dateColumnWidth: 78 * tableScale
                                readonly property real requesterColumnWidth: 86 * tableScale
                                readonly property real pickupColumnWidth: 42 * tableScale
                                readonly property real robotColumnWidth: 48 * tableScale
                                readonly property real timeColumnWidth: 74 * tableScale
                                readonly property real stateColumnWidth: 78 * tableScale
                                readonly property real actionColumnWidth: 26
                                readonly property real identityColumnGap: 8 * tableScale

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
                                            Text {
                                                text: "RECENT TASKS"
                                                color: C.text
                                                font.pixelSize: 13 * recentTasksPanel.tableScale
                                                font.bold: true
                                                font.letterSpacing: 0.8
                                            }
                                            Item { Layout.fillWidth: true }
                                            Rectangle {
                                                Layout.preferredWidth: recordsText.implicitWidth + 18
                                                Layout.preferredHeight: 24
                                                radius: 8
                                                color: "#143452"
                                                border.color: "#235278"
                                                border.width: 1

                                                Text {
                                                    id: recordsText
                                                    anchors.centerIn: parent
                                                    text: root.tasks.length + " records"
                                                    color: C.cyan
                                                    font.family: root.monoFontFamily
                                                    font.pixelSize: 10 * recentTasksPanel.tableScale
                                                    font.bold: true
                                                }
                                            }
                                        }
                                    }

                                    Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: C.border; opacity: 0.65 }

                                    // Column headers share the exact same widths as the rows.
                                    Rectangle {
                                        Layout.fillWidth: true
                                        Layout.preferredHeight: 34 * recentTasksPanel.tableScale
                                        color: "#0A1A2B"
                                        RowLayout {
                                            anchors.fill: parent
                                            anchors.leftMargin: 19
                                            anchors.rightMargin: 16
                                            spacing: 4
                                            Text { text: "DATE";      Layout.preferredWidth: recentTasksPanel.dateColumnWidth;      Layout.minimumWidth: Layout.preferredWidth; Layout.rightMargin: recentTasksPanel.identityColumnGap; color: C.textDim; font.pixelSize: 11 * recentTasksPanel.tableScale; font.bold: true; font.letterSpacing: 0.6; horizontalAlignment: Text.AlignHCenter }
                                            Text { text: "REQUESTER"; Layout.preferredWidth: recentTasksPanel.requesterColumnWidth; Layout.minimumWidth: Layout.preferredWidth; Layout.rightMargin: recentTasksPanel.identityColumnGap; color: C.textDim; font.pixelSize: 11 * recentTasksPanel.tableScale; font.bold: true; font.letterSpacing: 0.6; horizontalAlignment: Text.AlignHCenter }
                                            Text { text: "PICKUP";    Layout.preferredWidth: recentTasksPanel.pickupColumnWidth;    Layout.minimumWidth: Layout.preferredWidth; color: C.textDim; font.pixelSize: 11 * recentTasksPanel.tableScale; font.bold: true; font.letterSpacing: 0.6; horizontalAlignment: Text.AlignHCenter }
                                            Text { text: "DEST.";     Layout.fillWidth: true; color: C.textDim; font.pixelSize: 11 * recentTasksPanel.tableScale; font.bold: true; font.letterSpacing: 0.6; horizontalAlignment: Text.AlignHCenter }
                                            Text { text: "ROBOT";     Layout.preferredWidth: recentTasksPanel.robotColumnWidth;     Layout.minimumWidth: Layout.preferredWidth; color: C.textDim; font.pixelSize: 11 * recentTasksPanel.tableScale; font.bold: true; font.letterSpacing: 0.6; horizontalAlignment: Text.AlignHCenter }
                                            Text { text: "START";     Layout.preferredWidth: recentTasksPanel.timeColumnWidth;      Layout.minimumWidth: Layout.preferredWidth; color: C.textDim; font.pixelSize: 11 * recentTasksPanel.tableScale; font.bold: true; font.letterSpacing: 0.6; horizontalAlignment: Text.AlignHCenter }
                                            Text { text: "END";       Layout.preferredWidth: recentTasksPanel.timeColumnWidth;      Layout.minimumWidth: Layout.preferredWidth; color: C.textDim; font.pixelSize: 11 * recentTasksPanel.tableScale; font.bold: true; font.letterSpacing: 0.6; horizontalAlignment: Text.AlignHCenter }
                                            Text { text: "STATE";     Layout.preferredWidth: recentTasksPanel.stateColumnWidth;     Layout.minimumWidth: Layout.preferredWidth; color: C.textDim; font.pixelSize: 11 * recentTasksPanel.tableScale; font.bold: true; font.letterSpacing: 0.6; horizontalAlignment: Text.AlignHCenter }
                                            Item { Layout.preferredWidth: recentTasksPanel.actionColumnWidth; Layout.minimumWidth: Layout.preferredWidth }
                                        }
                                    }
                                    Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: C.border; opacity: 0.4 }

                                    Item {
                                        Layout.fillWidth: true
                                        Layout.fillHeight: true

                                        ListView {
                                            anchors.fill: parent
                                            anchors.margins: 8
                                            model: root.tasks
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
                                                    Text {
                                                        text: modelData.requester
                                                        Layout.preferredWidth: recentTasksPanel.requesterColumnWidth
                                                        Layout.minimumWidth: Layout.preferredWidth
                                                        Layout.rightMargin: recentTasksPanel.identityColumnGap
                                                        color: C.cyan
                                                        font.family: root.monoFontFamily
                                                        font.pixelSize: 12 * recentTasksPanel.tableScale
                                                        font.bold: true
                                                        horizontalAlignment: Text.AlignHCenter
                                                    }
                                                    Text { text: modelData.pickup; Layout.preferredWidth: recentTasksPanel.pickupColumnWidth; Layout.minimumWidth: Layout.preferredWidth; color: C.textDim; font.family: root.monoFontFamily; font.pixelSize: 12 * recentTasksPanel.tableScale; font.bold: true; elide: Text.ElideRight; horizontalAlignment: Text.AlignHCenter }
                                                    Text { text: modelData.destination; Layout.fillWidth: true; color: C.text; font.bold: true; font.pixelSize: 14 * recentTasksPanel.tableScale; elide: Text.ElideRight; horizontalAlignment: Text.AlignHCenter }
                                                    Text { text: modelData.robot; Layout.preferredWidth: recentTasksPanel.robotColumnWidth; Layout.minimumWidth: Layout.preferredWidth; color: C.text; font.family: root.monoFontFamily; font.pixelSize: 12 * recentTasksPanel.tableScale; font.bold: true; elide: Text.ElideRight; horizontalAlignment: Text.AlignHCenter }
                                                    Text { text: modelData.start; Layout.preferredWidth: recentTasksPanel.timeColumnWidth; Layout.minimumWidth: Layout.preferredWidth; color: "#B7CCE0"; font.family: root.monoFontFamily; font.pixelSize: 11 * recentTasksPanel.tableScale; elide: Text.ElideRight; horizontalAlignment: Text.AlignHCenter }
                                                    Text { text: modelData.end;   Layout.preferredWidth: recentTasksPanel.timeColumnWidth; Layout.minimumWidth: Layout.preferredWidth; color: "#B7CCE0"; font.family: root.monoFontFamily; font.pixelSize: 11 * recentTasksPanel.tableScale; elide: Text.ElideRight; horizontalAlignment: Text.AlignHCenter }
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
                                                            font.pixelSize: 10 * recentTasksPanel.tableScale
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
                                            visible: root.tasks.length === 0
                                            spacing: 5
                                            Text { anchors.horizontalCenter: parent.horizontalCenter; text: "NO ACTIVE MISSIONS"; color: C.textDim; font.pixelSize: 12; font.bold: true }
                                            Text { anchors.horizontalCenter: parent.horizontalCenter; text: "Create a task to get started"; color: C.textDim; opacity: 0.65; font.pixelSize: 10 }
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
