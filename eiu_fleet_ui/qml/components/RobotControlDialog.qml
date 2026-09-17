import QtQuick
import QtQuick.Controls
import QtQuick.Layouts


Popup {
    id: dlg

    property string robotName: ""
    // Keep the status bound to live robot telemetry.
    readonly property var tele: root.telemetryFor(robotName)
    readonly property bool online: !!root.robotsOnline[robotName]
    // VDA5050 offline means every command below would silently go nowhere.
    readonly property bool controlsEnabled: online
    property real currentSpeedLimit: 0

    property string lastAction: ""
    property bool lastOk: true
    property string lastMessage: ""

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

    function closeDrawer() {
        if (mapLoader.item) {
            mapLoader.item.clearPickedPose()
            mapLoader.item.clearPickedWaypoint()
        }
        dlg.close()
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

    parent: Overlay.overlay
    width: 380
    height: parent ? parent.height : 600
    x: parent ? parent.width - width : 0
    y: 0
    clip: true
    padding: 0
    modal: false
    focus: true
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    background: Rectangle {
        color: C.surface
        Rectangle { anchors.left: parent.left; width: 1; height: parent.height; color: C.border }
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

    Connections {
        target: ros
        function onDispatchResult(ok, message) {
            dlg.lastAction = "go_to"
            dlg.lastOk = ok
            dlg.lastMessage = message
        }
    }

    onOpened: {
        lastAction = ""; lastMessage = ""
        speedField.text = currentSpeedLimit > 0 ? currentSpeedLimit.toFixed(2) : ""
    }

    // Re-localizing overrides the robot's believed pose -- confirm before sending it.
    Popup {
        id: confirmPopup
        modal: true
        focus: true
        parent: Overlay.overlay
        x: (dlg.parent.width - width) / 2
        y: (dlg.parent.height - height) / 2
        width: 300
        padding: 18
        property var onConfirm: null

        background: Rectangle { color: C.surfaceRaised; radius: 14; border.color: C.border; border.width: 1 }

        contentItem: ColumnLayout {
            spacing: 14
            Text {
                text: "Re-localize " + dlg.robotName + " to (" + xField.text + ", " + yField.text + ", " + yawField.text + ")?"
                color: C.text; font.pixelSize: 14; font.bold: true
                wrapMode: Text.WordWrap; Layout.fillWidth: true
            }
            Text {
                text: "This overrides its current believed pose immediately."
                color: C.textDim; font.pixelSize: 12
                wrapMode: Text.WordWrap; Layout.fillWidth: true
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 10
                Button {
                    Layout.fillWidth: true
                    text: "CANCEL"
                    implicitHeight: 38
                    contentItem: Text { text: parent.text; color: C.textDim; font.pixelSize: 13; font.bold: true
                                         horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                    background: Rectangle { radius: 10; color: C.surfaceAlt; border.color: C.border; border.width: 1 }
                    onClicked: confirmPopup.close()
                }
                Button {
                    Layout.fillWidth: true
                    text: "CONFIRM"
                    implicitHeight: 38
                    contentItem: Text { text: parent.text; color: "#ffffff"; font.pixelSize: 13; font.bold: true
                                         horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                    background: Rectangle { radius: 10; color: parent.down ? C.accentDark : C.err }
                    onClicked: { if (confirmPopup.onConfirm) confirmPopup.onConfirm(); confirmPopup.close() }
                }
            }
        }
    }

    contentItem: ColumnLayout {
        spacing: 22

        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: 16; Layout.leftMargin: 18; Layout.rightMargin: 10
            Text {
                Layout.fillWidth: true
                text: "ROBOT CONTROL"
                font.pixelSize: 13; font.bold: true; font.letterSpacing: 0.8; color: C.textDim
            }
            Button {
                Layout.preferredWidth: 30; Layout.preferredHeight: 30
                text: "×"
                contentItem: Text { text: parent.text; color: C.textDim; font.pixelSize: 20
                                     horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                background: Rectangle { radius: 8; color: parent.down ? C.border : (parent.hovered ? C.surfaceAlt : "transparent") }
                onClicked: dlg.closeDrawer()
            }
        }

        // Status first -- controls below would silently no-op if unreachable.
        ColumnLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 18; Layout.rightMargin: 18
            spacing: 8

            RowLayout {
                Layout.fillWidth: true
                spacing: 10
                Text {
                    Layout.fillWidth: true
                    text: dlg.robotName
                    font.pixelSize: 23; font.bold: true; color: C.text
                    elide: Text.ElideRight
                }
                Rectangle {
                    Layout.preferredWidth: statusPill.implicitWidth + 18
                    Layout.preferredHeight: 24
                    radius: 12
                    color: "transparent"
                    border.color: dlg.online ? C.success : C.err
                    border.width: 1
                    Text {
                        id: statusPill
                        anchors.centerIn: parent
                        text: dlg.online ? "ONLINE" : "OFFLINE"
                        color: dlg.online ? C.success : C.err
                        font.pixelSize: 12; font.bold: true
                    }
                }
            }

            FreshnessTag {
                Layout.fillWidth: true
                online: dlg.online
                hasData: !!dlg.tele
                lastRx: dlg.tele ? Number(dlg.tele.last_rx || 0) : 0
                nowTick: root.nowTick
                fontSize: 12
            }

            Text {
                Layout.fillWidth: true
                visible: dlg.online
                text: dlg.tele
                      ? (dlg.tele.paused ? "Paused by operator" : (dlg.tele.driving ? "Driving" : "Idle"))
                        + (dlg.tele.safety.triggered ? "  ⚠ " + dlg.tele.safety.e_stop : "")
                        + (dlg.tele.battery_soc != null ? "  ·  " + (dlg.tele.battery_soc * 100).toFixed(0) + "%" : "")
                      : ""
                font.pixelSize: 12
                color: (dlg.tele && dlg.tele.safety.triggered) ? C.err : C.textDim
            }

            Text {
                Layout.fillWidth: true
                visible: !dlg.controlsEnabled
                text: "⚠ Controls unavailable while robot is offline"
                color: C.warn
                font.pixelSize: 12
                wrapMode: Text.WordWrap
            }
        }

        Rectangle { Layout.fillWidth: true; Layout.leftMargin: 18; Layout.rightMargin: 18
                    Layout.preferredHeight: 1; color: C.border; opacity: 0.6 }

        // Pause or resume the robot.
        ColumnLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 18; Layout.rightMargin: 18
            spacing: 6
            enabled: dlg.controlsEnabled
            opacity: dlg.controlsEnabled ? 1.0 : 0.65

            Text { text: "MOTION CONTROL"; font.pixelSize: 12; font.bold: true
                   font.letterSpacing: 0.8; color: C.textDim
                   elide: Text.ElideRight; Layout.fillWidth: true }

            RowLayout {
                Layout.fillWidth: true
                spacing: 10

                Button {
                    Layout.fillWidth: true
                    implicitHeight: 40
                    text: (dlg.tele && dlg.tele.paused) ? "PAUSED" : "PAUSE"
                    enabled: !dlg.tele || !dlg.tele.paused
                    contentItem: Text { text: parent.text; color: C.text; font.pixelSize: 14; font.bold: true
                                         horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
                                         elide: Text.ElideRight }
                    background: Rectangle { radius: 10; color: parent.down ? C.border : C.surfaceAlt
                                             border.color: C.border; border.width: 1
                                             opacity: parent.enabled ? 1.0 : 0.65 }
                    onClicked: control.pauseRobot(dlg.robotName)
                }
                Button {
                    Layout.fillWidth: true
                    implicitHeight: 40
                    text: (dlg.tele && dlg.tele.paused) ? "RESUME" : "RESUMED"
                    enabled: dlg.tele && dlg.tele.paused
                    contentItem: Text { text: parent.text; color: "#ffffff"; font.pixelSize: 14; font.bold: true
                                         horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
                                         elide: Text.ElideRight }
                    background: Rectangle { radius: 10; color: parent.down ? C.accentDark : C.accent
                                             opacity: parent.enabled ? 1.0 : 0.65 }
                    onClicked: control.resumeRobot(dlg.robotName)
                }
            }
        }

        // Set a speed limit.
        ColumnLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 18; Layout.rightMargin: 18
            spacing: 6
            enabled: dlg.controlsEnabled
            opacity: dlg.controlsEnabled ? 1.0 : 0.65

            Text { text: "SPEED LIMIT"; font.pixelSize: 12; font.bold: true
                   font.letterSpacing: 0.8; color: C.textDim
                   elide: Text.ElideRight; Layout.fillWidth: true }
            Text { text: "Maximum speed (m/s, blank = no cap)"; font.pixelSize: 11; color: C.textDim
                   elide: Text.ElideRight; Layout.fillWidth: true }
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                TextField {
                    id: speedField
                    Layout.fillWidth: true
                    Layout.preferredHeight: 40
                    placeholderText: "e.g. 0.30"
                    placeholderTextColor: C.placeholderText
                    validator: DoubleValidator { bottom: 0; decimals: 2; notation: DoubleValidator.StandardNotation }
                    color: C.text
                    font.pixelSize: 14
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    background: Rectangle { radius: 10; color: C.surfaceAlt; border.color: C.border; border.width: 1 }
                }
                Button {
                    text: "APPLY"
                    implicitHeight: 40
                    leftPadding: 18; rightPadding: 18
                    contentItem: Text { text: parent.text; color: "#ffffff"; font.pixelSize: 14; font.bold: true
                                         horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                    background: Rectangle { radius: 10; color: parent.down ? C.accentDark : C.accent }
                    onClicked: control.setSpeedLimit(dlg.robotName, speedField.text === "" ? 0.0 : parseFloat(speedField.text))
                }
            }
        }

        // Set the robot position.
        ColumnLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 18; Layout.rightMargin: 18
            spacing: 6
            enabled: dlg.controlsEnabled
            opacity: dlg.controlsEnabled ? 1.0 : 0.65

            Text { text: "LOCALIZATION"; font.pixelSize: 12; font.bold: true
                   font.letterSpacing: 0.8; color: C.textDim
                   elide: Text.ElideRight; Layout.fillWidth: true }
            Text { text: "Set the robot's believed pose (RMF frame)"; font.pixelSize: 11; color: C.textDim
                   elide: Text.ElideRight; Layout.fillWidth: true }
            RowLayout {
                Layout.fillWidth: true
                spacing: 6
                Text { Layout.fillWidth: true; text: "X"; font.pixelSize: 10; color: C.textDim; horizontalAlignment: Text.AlignHCenter }
                Text { Layout.fillWidth: true; text: "Y"; font.pixelSize: 10; color: C.textDim; horizontalAlignment: Text.AlignHCenter }
                Text { Layout.fillWidth: true; text: "YAW"; font.pixelSize: 10; color: C.textDim; horizontalAlignment: Text.AlignHCenter }
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 6
                TextField {
                    id: xField; Layout.fillWidth: true; Layout.preferredHeight: 40; placeholderText: "x"
                    placeholderTextColor: C.placeholderText
                    validator: DoubleValidator { notation: DoubleValidator.StandardNotation }
                    color: C.text; font.pixelSize: 14
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    background: Rectangle { radius: 10; color: C.surfaceAlt; border.color: C.border; border.width: 1 }
                }
                TextField {
                    id: yField; Layout.fillWidth: true; Layout.preferredHeight: 40; placeholderText: "y"
                    placeholderTextColor: C.placeholderText
                    validator: DoubleValidator { notation: DoubleValidator.StandardNotation }
                    color: C.text; font.pixelSize: 14
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    background: Rectangle { radius: 10; color: C.surfaceAlt; border.color: C.border; border.width: 1 }
                }
                TextField {
                    id: yawField; Layout.fillWidth: true; Layout.preferredHeight: 40; placeholderText: "yaw"
                    placeholderTextColor: C.placeholderText
                    validator: DoubleValidator { notation: DoubleValidator.StandardNotation }
                    color: C.text; font.pixelSize: 14
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
                    implicitHeight: 38
                    contentItem: Text { text: parent.text; color: C.text; font.pixelSize: 13; font.bold: true
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
                    implicitHeight: 38
                    enabled: xField.text !== "" && yField.text !== "" && yawField.text !== ""
                    contentItem: Text { text: parent.text; color: C.text; font.pixelSize: 13; font.bold: true
                                         horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
                                         elide: Text.ElideRight }
                    background: Rectangle { radius: 10; color: parent.down ? C.border : C.surfaceAlt
                                             border.color: C.border; border.width: 1
                                             opacity: parent.enabled ? 1.0 : 0.65 }
                    onClicked: {
                        confirmPopup.onConfirm = function() {
                            control.initPosition(dlg.robotName, parseFloat(xField.text),
                                                  parseFloat(yField.text), parseFloat(yawField.text))
                            if (mapLoader.item) mapLoader.item.clearPickedPose()
                        }
                        confirmPopup.open()
                    }
                }
            }
        }

        // Send the robot to a waypoint.
        ColumnLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 18; Layout.rightMargin: 18
            spacing: 6
            enabled: dlg.controlsEnabled
            opacity: dlg.controlsEnabled ? 1.0 : 0.65

            Text { text: "NAVIGATION"; font.pixelSize: 12; font.bold: true
                   font.letterSpacing: 0.8; color: C.textDim
                   elide: Text.ElideRight; Layout.fillWidth: true }
            Text { text: "Destination"; font.pixelSize: 11; color: C.textDim
                   elide: Text.ElideRight; Layout.fillWidth: true }
            ComboBox {
                id: goToCombo
                Layout.fillWidth: true
                model: root.wpNames
                font.pixelSize: 14
                contentItem: Text {
                    text: goToCombo.displayText; color: C.text; font: goToCombo.font
                    leftPadding: 10; elide: Text.ElideRight
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle { implicitHeight: 40; radius: 10
                                         color: C.surfaceAlt; border.color: C.border; border.width: 1 }
            }
            Button {
                Layout.fillWidth: true
                text: dlg.pickTarget === "waypoint" ? "CLICK MAP…" : "PICK ON MAP"
                implicitHeight: 38
                contentItem: Text { text: parent.text; color: C.text; font.pixelSize: 13; font.bold: true
                                     horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
                                     elide: Text.ElideRight }
                background: Rectangle { radius: 10
                                         color: dlg.pickTarget === "waypoint" ? C.accentDark
                                                : (parent.down ? C.border : C.surfaceAlt)
                                         border.color: dlg.pickTarget === "waypoint" ? C.accent : C.border
                                         border.width: 1 }
                onClicked: dlg.beginPick("waypoint")
            }
            // Names the destination on the button so a stale combo value is obvious.
            Button {
                Layout.fillWidth: true
                text: goToCombo.currentText !== "" ? "GO → " + goToCombo.currentText : "GO"
                implicitHeight: 42
                enabled: goToCombo.currentText !== ""
                contentItem: Text { text: parent.text; color: "#ffffff"; font.pixelSize: 14; font.bold: true
                                     horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
                                     elide: Text.ElideRight }
                background: Rectangle { radius: 10
                                         color: !parent.enabled ? C.border : (parent.down ? C.accentDark : C.accent) }
                onClicked: {
                    ros.dispatchToRobot(cfg.taskCategories.length > 0 ? cfg.taskCategories[0] : "loop",
                                         goToCombo.currentText, 1, dlg.robotName)
                    if (mapLoader.item) mapLoader.item.clearPickedWaypoint()
                }
            }
        }

        // Show the most recent command result.
        Text {
            Layout.fillWidth: true
            Layout.leftMargin: 18; Layout.rightMargin: 18
            Layout.bottomMargin: 18
            visible: dlg.lastAction !== ""
            text: dlg.lastAction + ": " + dlg.lastMessage
            color: dlg.lastOk ? C.success : C.err
            font.pixelSize: 12
            wrapMode: Text.WordWrap
        }
    }
}
