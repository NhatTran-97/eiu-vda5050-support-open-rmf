import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Direct robot control: pause/resume, speed limit, re-localize.
// Talks to `control` (RosControl) -- see ros_control.py.
Dialog {
    id: dlg

    property string robotName: ""
    // Live-bound, not a snapshot -- re-evaluates on root.telemetry changes so
    // paused/resumed state updates instead of freezing at dialog-open time.
    readonly property var tele: root.telemetryFor(robotName)
    property real currentSpeedLimit: 0

    property string lastAction: ""
    property bool lastOk: true
    property string lastMessage: ""

    // "" | "pose" | "waypoint" -- which field a map click should fill. The
    // dialog hides while picking so clicks reach the map, which emits the pick back.
    property string pickTarget: ""

    function beginPick(target) {
        if (!mapLoader.item)
            return
        dlg.pickTarget = target
        mapLoader.item.pickMode = target
        dlg.close()
    }

    function applyPickedPose(x, y, yaw) {
        xField.text = x.toFixed(2)
        yField.text = y.toFixed(2)
        yawField.text = yaw.toFixed(2)
    }

    function applyPickedWaypoint(name) {
        var idx = goToCombo.find(name)
        if (idx >= 0)
            goToCombo.currentIndex = idx
    }

    Connections {
        target: mapLoader.item
        function onPosePicked(x, y, yaw) {
            dlg.applyPickedPose(x, y, yaw)
            dlg.pickTarget = ""
            dlg.open()
        }
        function onWaypointPicked(name) {
            dlg.applyPickedWaypoint(name)
            dlg.pickTarget = ""
            dlg.open()
        }
        function onPickCancelled() {
            dlg.pickTarget = ""
            dlg.open()
        }
    }

    // User-draggable position + size (0 height = "fit content"), as plain
    // properties so they persist across close()/open() (e.g. pick-on-map) --
    // the dialog reopens wherever the operator last left it.
    property real userWidth: 380
    property real userHeight: 0
    readonly property real minWidth: 320
    readonly property real minHeight: 360

    // Scales fonts/heights as the dialog is resized, clamped so text never
    // gets unreadably small or comically large.
    readonly property real uiScale: Math.max(0.82, Math.min(1.3, width / 380))
    function scaled(px) { return Math.round(px * uiScale) }

    modal: true
    width: userWidth
    height: userHeight > 0 ? userHeight : implicitHeight
    clip: true
    padding: 0
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    background: Rectangle {
        color: C.surface; radius: 16
        border.color: C.border; border.width: 1

        // Resize grip — bottom-right corner, drag to grow/shrink the dialog.
        Item {
            id: resizeGrip
            width: 22; height: 22
            anchors.right: parent.right
            anchors.bottom: parent.bottom

            Canvas {
                anchors.fill: parent
                onPaint: {
                    var ctx = getContext("2d")
                    ctx.strokeStyle = C.textDim
                    ctx.lineWidth = 1.5
                    for (var i = 1; i <= 3; i++) {
                        ctx.beginPath()
                        ctx.moveTo(width - i * 6, height - 3)
                        ctx.lineTo(width - 3, height - i * 6)
                        ctx.stroke()
                    }
                }
            }

            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.SizeFDiagCursor
                property real pressX: 0
                property real pressY: 0
                property real startW: 0
                property real startH: 0
                onPressed: (mouse) => {
                    pressX = mouse.x; pressY = mouse.y
                    startW = dlg.userWidth
                    startH = dlg.userHeight > 0 ? dlg.userHeight : dlg.height
                }
                onPositionChanged: (mouse) => {
                    dlg.userWidth = Math.max(dlg.minWidth,
                        Math.min(root.width - dlg.x - 20, startW + (mouse.x - pressX)))
                    dlg.userHeight = Math.max(dlg.minHeight,
                        Math.min(root.height - dlg.y - 20, startH + (mouse.y - pressY)))
                }
            }
        }
    }

    Connections {
        target: control
        function onCommandResult(robot, action, ok, message) {
            if (robot !== dlg.robotName)
                return
            dlg.lastAction = action
            dlg.lastOk = ok
            dlg.lastMessage = message
        }
    }

    onOpened: {
        lastAction = ""; lastMessage = ""
        speedField.text = currentSpeedLimit > 0 ? currentSpeedLimit.toFixed(2) : ""
    }

    contentItem: ColumnLayout {
        spacing: 14

        Text {
            id: titleText
            Layout.fillWidth: true
            Layout.topMargin: 18; Layout.leftMargin: 18; Layout.rightMargin: 18
            text: "ROBOT CONTROL — " + dlg.robotName
            font.pixelSize: dlg.scaled(16); font.bold: true; color: C.text
            horizontalAlignment: Text.AlignHCenter
            elide: Text.ElideRight

            // Drag the title bar to move the whole dialog around the window.
            MouseArea {
                anchors.fill: parent
                anchors.margins: -10
                cursorShape: Qt.SizeAllCursor
                property real pressX: 0
                property real pressY: 0
                onPressed: (mouse) => { pressX = mouse.x; pressY = mouse.y }
                onPositionChanged: (mouse) => {
                    dlg.x += mouse.x - pressX
                    dlg.y += mouse.y - pressY
                }
            }
        }

        Text {
            Layout.fillWidth: true
            Layout.leftMargin: 18; Layout.rightMargin: 18
            text: dlg.tele
                  ? (dlg.tele.paused ? "Paused by operator" : (dlg.tele.driving ? "Driving" : "Idle"))
                    + (dlg.tele.safety.triggered ? "  ⚠ " + dlg.tele.safety.e_stop : "")
                  : "No telemetry yet"
            font.pixelSize: dlg.scaled(11)
            color: (dlg.tele && dlg.tele.safety.triggered) ? C.err : C.textDim
            horizontalAlignment: Text.AlignHCenter
        }

        // ── Pause / Resume ──
        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 18; Layout.rightMargin: 18
            spacing: 10

            Button {
                Layout.fillWidth: true
                implicitHeight: dlg.scaled(38)
                text: (dlg.tele && dlg.tele.paused) ? "PAUSED" : "PAUSE"
                enabled: !dlg.tele || !dlg.tele.paused
                contentItem: Text { text: parent.text; color: C.text; font.pixelSize: dlg.scaled(12); font.bold: true
                                     horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
                                     elide: Text.ElideRight }
                background: Rectangle { radius: 10; color: parent.down ? C.border : C.surfaceAlt
                                         border.color: C.border; border.width: 1
                                         opacity: parent.enabled ? 1.0 : 0.5 }
                onClicked: control.pauseRobot(dlg.robotName)
            }
            Button {
                Layout.fillWidth: true
                implicitHeight: dlg.scaled(38)
                text: (dlg.tele && dlg.tele.paused) ? "RESUME" : "RESUMED"
                enabled: dlg.tele && dlg.tele.paused
                contentItem: Text { text: parent.text; color: "#ffffff"; font.pixelSize: dlg.scaled(12); font.bold: true
                                     horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
                                     elide: Text.ElideRight }
                background: Rectangle { radius: 10; color: parent.down ? C.accentDark : C.accent
                                         opacity: parent.enabled ? 1.0 : 0.5 }
                onClicked: control.resumeRobot(dlg.robotName)
            }
        }

        // ── Speed limit ──
        ColumnLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 18; Layout.rightMargin: 18
            spacing: 4
            Text { text: "SPEED LIMIT (m/s, blank = no cap)"; font.pixelSize: dlg.scaled(9); font.bold: true
                   font.letterSpacing: 1.0; color: C.textDim
                   elide: Text.ElideRight; Layout.fillWidth: true }
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                TextField {
                    id: speedField
                    Layout.fillWidth: true
                    placeholderText: "e.g. 0.30"
                    placeholderTextColor: C.placeholderText
                    validator: DoubleValidator { bottom: 0; decimals: 2; notation: DoubleValidator.StandardNotation }
                    color: C.text
                    font.pixelSize: dlg.scaled(13)
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    background: Rectangle { radius: 10; color: C.surfaceAlt; border.color: C.border; border.width: 1 }
                }
                Button {
                    text: "APPLY"
                    implicitHeight: dlg.scaled(38)
                    leftPadding: dlg.scaled(18); rightPadding: dlg.scaled(18)
                    contentItem: Text { text: parent.text; color: "#ffffff"; font.pixelSize: dlg.scaled(12); font.bold: true
                                         horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                    background: Rectangle { radius: 10; color: parent.down ? C.accentDark : C.accent }
                    onClicked: control.setSpeedLimit(dlg.robotName, speedField.text === "" ? 0.0 : parseFloat(speedField.text))
                }
            }
        }

        // ── Re-localize ──
        ColumnLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 18; Layout.rightMargin: 18
            spacing: 4
            Text { text: "RE-LOCALIZE (x, y, yaw rad — RMF frame)"; font.pixelSize: dlg.scaled(9); font.bold: true
                   font.letterSpacing: 1.0; color: C.textDim
                   elide: Text.ElideRight; Layout.fillWidth: true }
            RowLayout {
                Layout.fillWidth: true
                spacing: 6
                TextField {
                    id: xField; Layout.fillWidth: true; placeholderText: "x"
                    placeholderTextColor: C.placeholderText
                    validator: DoubleValidator { notation: DoubleValidator.StandardNotation }
                    color: C.text; font.pixelSize: dlg.scaled(13)
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    background: Rectangle { radius: 10; color: C.surfaceAlt; border.color: C.border; border.width: 1 }
                }
                TextField {
                    id: yField; Layout.fillWidth: true; placeholderText: "y"
                    placeholderTextColor: C.placeholderText
                    validator: DoubleValidator { notation: DoubleValidator.StandardNotation }
                    color: C.text; font.pixelSize: dlg.scaled(13)
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    background: Rectangle { radius: 10; color: C.surfaceAlt; border.color: C.border; border.width: 1 }
                }
                TextField {
                    id: yawField; Layout.fillWidth: true; placeholderText: "yaw"
                    placeholderTextColor: C.placeholderText
                    validator: DoubleValidator { notation: DoubleValidator.StandardNotation }
                    color: C.text; font.pixelSize: dlg.scaled(13)
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    background: Rectangle { radius: 10; color: C.surfaceAlt; border.color: C.border; border.width: 1 }
                }
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Button {
                    Layout.fillWidth: true
                    text: dlg.pickTarget === "pose" ? "CLICK MAP…" : "PICK ON MAP"
                    implicitHeight: dlg.scaled(36)
                    contentItem: Text { text: parent.text; color: C.text; font.pixelSize: dlg.scaled(12); font.bold: true
                                         horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
                                         elide: Text.ElideRight }
                    background: Rectangle { radius: 10
                                             color: dlg.pickTarget === "pose" ? C.accentDark
                                                    : (parent.down ? C.border : C.surfaceAlt)
                                             border.color: dlg.pickTarget === "pose" ? C.accent : C.border
                                             border.width: 1 }
                    onClicked: dlg.beginPick("pose")
                }
                Button {
                    Layout.fillWidth: true
                    text: "SET POSITION"
                    implicitHeight: dlg.scaled(36)
                    enabled: xField.text !== "" && yField.text !== "" && yawField.text !== ""
                    contentItem: Text { text: parent.text; color: C.text; font.pixelSize: dlg.scaled(12); font.bold: true
                                         horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
                                         elide: Text.ElideRight }
                    background: Rectangle { radius: 10; color: parent.down ? C.border : C.surfaceAlt
                                             border.color: C.border; border.width: 1
                                             opacity: parent.enabled ? 1.0 : 0.5 }
                    onClicked: {
                        control.initPosition(dlg.robotName, parseFloat(xField.text),
                                              parseFloat(yField.text), parseFloat(yawField.text))
                        if (mapLoader.item) mapLoader.item.clearPickedPose()
                    }
                }
            }
        }

        // ── Go to waypoint — drives the robot, unlike re-localize above ──
        ColumnLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 18; Layout.rightMargin: 18
            spacing: 4
            Text { text: "GO TO WAYPOINT"; font.pixelSize: dlg.scaled(9); font.bold: true
                   font.letterSpacing: 1.0; color: C.textDim
                   elide: Text.ElideRight; Layout.fillWidth: true }
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                ComboBox {
                    id: goToCombo
                    Layout.fillWidth: true
                    model: root.wpNames
                    font.pixelSize: dlg.scaled(13)
                    contentItem: Text {
                        text: goToCombo.displayText; color: C.text; font: goToCombo.font
                        leftPadding: 10; elide: Text.ElideRight
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle { implicitHeight: dlg.scaled(38); radius: 10
                                             color: C.surfaceAlt; border.color: C.border; border.width: 1 }
                }
                Button {
                    text: dlg.pickTarget === "waypoint" ? "CLICK MAP…" : "PICK ON MAP"
                    implicitHeight: dlg.scaled(38)
                    leftPadding: dlg.scaled(14); rightPadding: dlg.scaled(14)
                    contentItem: Text { text: parent.text; color: C.text; font.pixelSize: dlg.scaled(12); font.bold: true
                                         horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
                                         elide: Text.ElideRight }
                    background: Rectangle { radius: 10
                                             color: dlg.pickTarget === "waypoint" ? C.accentDark
                                                    : (parent.down ? C.border : C.surfaceAlt)
                                             border.color: dlg.pickTarget === "waypoint" ? C.accent : C.border
                                             border.width: 1 }
                    onClicked: dlg.beginPick("waypoint")
                }
                Button {
                    text: "GO"
                    implicitHeight: dlg.scaled(38)
                    leftPadding: dlg.scaled(22); rightPadding: dlg.scaled(22)
                    enabled: goToCombo.currentText !== ""
                    contentItem: Text { text: parent.text; color: "#ffffff"; font.pixelSize: dlg.scaled(12); font.bold: true
                                         horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                    background: Rectangle { radius: 10
                                             color: !parent.enabled ? C.border : (parent.down ? C.accentDark : C.accent) }
                    onClicked: {
                        ros.dispatchToRobot(cfg.taskCategories.length > 0 ? cfg.taskCategories[0] : "loop",
                                             goToCombo.currentText, 1, dlg.robotName)
                        if (mapLoader.item) mapLoader.item.clearPickedWaypoint()
                    }
                }
            }
        }

        // ── Last result ──
        Text {
            Layout.fillWidth: true
            Layout.leftMargin: 18; Layout.rightMargin: 18
            visible: dlg.lastAction !== ""
            text: dlg.lastAction + ": " + dlg.lastMessage
            color: dlg.lastOk ? C.success : C.err
            font.pixelSize: dlg.scaled(11)
            wrapMode: Text.WordWrap
        }

        Button {
            Layout.fillWidth: true
            Layout.margins: 18
            text: "CLOSE"
            implicitHeight: dlg.scaled(36)
            contentItem: Text { text: parent.text; color: C.text; font.pixelSize: dlg.scaled(13)
                                 horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
            background: Rectangle { radius: 10; color: parent.down ? C.border : C.surfaceAlt
                                     border.color: C.border; border.width: 1 }
            onClicked: {
                if (mapLoader.item) {
                    mapLoader.item.clearPickedPose()
                    mapLoader.item.clearPickedWaypoint()
                }
                dlg.close()
            }
        }
    }
}
