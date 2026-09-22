import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Registers a robot the fleet adapters found on the broker; the adapter checks the form as it is filled and its verdict is shown.
Dialog {
    id: dlg

    // Entry of registry.pendingJson for the robot being registered.
    property var robot: null
    // Entries of registry.fleetsJson.
    property var fleets: []
    // Last dry-run result of the form as it is now; null while unknown.
    property var verdict: null
    property bool checking: false
    property bool registering: false
    property string checkId: ""
    property string registerId: ""

    readonly property var fleetNames: fleets.map(function (f) { return f.fleet })
    readonly property var fleet: fleetCombo.currentIndex >= 0 ? fleets[fleetCombo.currentIndex] : null
    readonly property string fleetName: fleet ? fleet.fleet : ""
    readonly property var chargers: fleet ? fleet.chargers : []
    readonly property string chargerName: chargerCombo.currentIndex >= 0 && chargerCombo.currentIndex < chargers.length
                                          ? chargers[chargerCombo.currentIndex].name : ""

    readonly property var transformFields: [rotationField, scaleField, txField, tyField]
    readonly property bool transformValid: {
        for (var i = 0; i < transformFields.length; i++) {
            var t = transformFields[i].text.trim()
            if (t !== "" && !isFinite(Number(t))) return false
        }
        return true
    }
    // The adapter reports anything that is missing, so a fleet is enough to ask.
    readonly property bool canCheck: robot !== null && fleetName !== "" && transformValid
    readonly property int freeChargers: chargers.filter(function (c) { return !c.used_by }).length

    readonly property var findings: {
        var out = []
        if (!verdict) return out
        for (var i = 0; i < verdict.errors.length; i++)
            out.push({ kind: "error", code: verdict.errors[i].code, message: verdict.errors[i].message })
        for (var j = 0; j < verdict.warnings.length; j++)
            out.push({ kind: "warning", code: verdict.warnings[j].code, message: verdict.warnings[j].message })
        return out
    }

    readonly property string facts: {
        if (!robot) return ""
        var parts = [robot.series ? robot.series + (robot.kinematic ? " (" + robot.kinematic + ")" : "")
                                  : "no factsheet, so its type is unknown"]
        if (robot.speed_max !== null && robot.speed_max !== undefined)
            parts.push("max " + Number(robot.speed_max).toFixed(2) + " m/s")
        return parts.join(" · ")
    }
    readonly property string poseText: {
        if (!robot || !robot.pose) return "No pose reported yet"
        var p = robot.pose
        return "At (" + Number(p.x).toFixed(2) + ", " + Number(p.y).toFixed(2) + ") on '" + p.map + "'"
               + (p.initialized ? "" : " — not localized")
    }
    readonly property string fleetFacts: fleet
        ? "Type " + (fleet.series || "not known yet") + " · " + Number(fleet.limits.linear_speed).toFixed(2)
          + " m/s · footprint radius " + Number(fleet.limits.footprint_radius).toFixed(2) + " m"
        : ""

    // Says why no fleet fits when the robot's type has no running fleet, or that fleet cannot see the robot.
    readonly property string typeHint: {
        if (!robot || !robot.series) return ""
        var matching = robot.matching_fleets || []
        if (matching.length === 0)
            return "No running fleet has robots of type '" + robot.series + "'. Start the fleet adapter for this type, "
                   + "on the MQTT broker this robot uses, and try again."
        var seeing = matching.filter(function (f) { return robot.reporters.indexOf(f) >= 0 })
        if (seeing.length === 0)
            return "Fleet " + matching.join(", ") + " has this type but does not see the robot: it probably uses another MQTT broker."
        return ""
    }

    function openFor(pending) {
        dlg.robot = pending
        dlg.fleets = JSON.parse(registry.fleetsJson)
        var index = dlg.fleetNames.indexOf(pending.suggested_fleet)
        fleetCombo.currentIndex = index
        confirmBox.checked = false
        for (var i = 0; i < transformFields.length; i++) transformFields[i].text = ""
        advanced.expanded = false
        dlg.verdict = null
        dlg.checking = false
        dlg.registering = false
        dlg.applySuggestions()
        dlg.open()
    }

    // Prefill the name and the charger for the chosen fleet; the operator can change both.
    function applySuggestions() {
        if (!dlg.robot || dlg.fleetName === "") {
            nameField.text = ""
            chargerCombo.currentIndex = -1
        } else {
            var s = JSON.parse(registry.suggestFor(dlg.fleetName, dlg.robot.manufacturer, dlg.robot.serial))
            nameField.text = s.name
            var index = -1
            for (var i = 0; i < dlg.chargers.length; i++)
                if (dlg.chargers[i].name === s.charger) index = i
            chargerCombo.currentIndex = index
        }
        dlg.scheduleCheck()
    }

    function scheduleCheck() {
        dlg.verdict = null
        dlg.checkId = ""
        dlg.checking = false
        checkTimer.restart()
    }

    function requestJson() {
        var request = { fleet: dlg.fleetName, name: nameField.text.trim(),
                        manufacturer: dlg.robot.manufacturer, serial: dlg.robot.serial,
                        charger: dlg.chargerName, confirm_unverified: confirmBox.checked }
        // Blank fields are left out so the adapter's own defaults apply.
        var transform = {}
        if (rotationField.text.trim() !== "") transform.rotation = Number(rotationField.text)
        if (scaleField.text.trim() !== "") transform.scale = Number(scaleField.text)
        if (txField.text.trim() !== "" || tyField.text.trim() !== "")
            transform.translation = [Number(txField.text || 0), Number(tyField.text || 0)]
        if (Object.keys(transform).length > 0) request.transform = transform
        return JSON.stringify(request)
    }

    Timer {
        id: checkTimer
        interval: 350
        onTriggered: {
            if (!dlg.canCheck) return
            dlg.checking = true
            dlg.checkId = registry.check(dlg.requestJson())
        }
    }

    Connections {
        target: registry
        function onCheckResult(text) {
            var result = JSON.parse(text)
            if (result.request_id !== dlg.checkId) return   // The form changed since it was sent.
            dlg.verdict = result
            dlg.checking = false
        }
        function onRequestResult(text) {
            var result = JSON.parse(text)
            if (result.request_id !== dlg.registerId) return
            dlg.registering = false
            if (result.ok) dlg.close()
            else dlg.verdict = result
        }
        function onChanged() {
            if (!dlg.opened) return
            // Keep the chosen fleet and charger when the registry changes under the open dialog.
            var keepFleet = dlg.fleetName
            var keepCharger = dlg.chargerName
            dlg.fleets = JSON.parse(registry.fleetsJson)
            fleetCombo.currentIndex = dlg.fleetNames.indexOf(keepFleet)
            var index = -1
            for (var i = 0; i < dlg.chargers.length; i++)
                if (dlg.chargers[i].name === keepCharger) index = i
            chargerCombo.currentIndex = index
            dlg.scheduleCheck()
        }
    }

    onClosed: checkTimer.stop()

    modal: true
    width: 480
    height: parent ? Math.min(form.implicitHeight, parent.height - 48) : form.implicitHeight
    padding: 0
    closePolicy: Popup.CloseOnEscape

    background: Rectangle {
        color: C.surface; radius: 16
        border.color: C.border; border.width: 1
    }

    contentItem: Flickable {
        id: scroller
        clip: true
        contentWidth: width
        contentHeight: form.implicitHeight
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar { }

        ColumnLayout {
            id: form
            width: scroller.width
            spacing: 14

            Text {
                Layout.fillWidth: true
                Layout.topMargin: 18; Layout.leftMargin: 18; Layout.rightMargin: 18
                text: "REGISTER NEW ROBOT"
                font.pixelSize: 16; font.bold: true; color: C.text; font.letterSpacing: 1.0
                horizontalAlignment: Text.AlignHCenter
            }

            // What the fleet adapters saw on the broker.
            Rectangle {
                Layout.fillWidth: true
                Layout.leftMargin: 18; Layout.rightMargin: 18
                implicitHeight: identityColumn.implicitHeight + 20
                radius: 10; color: C.surfaceAlt; border.color: C.border; border.width: 1
                ColumnLayout {
                    id: identityColumn
                    anchors.fill: parent; anchors.margins: 10
                    spacing: 3
                    Text {
                        Layout.fillWidth: true
                        text: dlg.robot ? dlg.robot.manufacturer + " / " + dlg.robot.serial : ""
                        color: C.text; font.pixelSize: 15; font.bold: true; elide: Text.ElideRight
                    }
                    Text { Layout.fillWidth: true; text: dlg.facts; color: C.textDim; font.pixelSize: 12; wrapMode: Text.WordWrap }
                    Text {
                        Layout.fillWidth: true
                        text: dlg.poseText
                        color: (dlg.robot && dlg.robot.pose && !dlg.robot.pose.initialized) ? C.warn : C.textDim
                        font.pixelSize: 12; wrapMode: Text.WordWrap
                    }
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                Layout.leftMargin: 18; Layout.rightMargin: 18
                spacing: 4
                Text { text: "FLEET"; font.pixelSize: 9; font.bold: true; font.letterSpacing: 1.0; color: C.textDim }
                ComboBox {
                    id: fleetCombo
                    objectName: "fleetCombo"
                    Layout.fillWidth: true
                    model: dlg.fleetNames
                    currentIndex: -1
                    font.pixelSize: 13
                    contentItem: Text {
                        text: fleetCombo.currentIndex >= 0 ? fleetCombo.currentText : "Select the fleet of this robot's type"
                        color: fleetCombo.currentIndex >= 0 ? C.text : C.textDim
                        font: fleetCombo.font; leftPadding: 10; elide: Text.ElideRight
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle { implicitHeight: 42; radius: 10; color: C.surfaceAlt; border.color: C.border; border.width: 1 }
                    onActivated: dlg.applySuggestions()
                }
                Text {
                    Layout.fillWidth: true
                    visible: dlg.fleets.length === 0
                    text: "No fleet adapter has published its robots yet."
                    color: C.warn; font.pixelSize: 11; wrapMode: Text.WordWrap
                }
                Text {
                    Layout.fillWidth: true
                    visible: dlg.typeHint !== ""
                    text: dlg.typeHint
                    color: C.warn; font.pixelSize: 11; wrapMode: Text.WordWrap
                }
                Text {
                    Layout.fillWidth: true
                    visible: dlg.fleetFacts !== ""
                    text: dlg.fleetFacts
                    color: C.textDim; font.pixelSize: 11; wrapMode: Text.WordWrap
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                Layout.leftMargin: 18; Layout.rightMargin: 18
                spacing: 4
                Text { text: "ROBOT NAME"; font.pixelSize: 9; font.bold: true; font.letterSpacing: 1.0; color: C.textDim }
                TextField {
                    id: nameField
                    objectName: "nameField"
                    Layout.fillWidth: true
                    font.pixelSize: 13
                    color: C.text
                    placeholderText: "e.g. the name RMF will show"
                    placeholderTextColor: C.placeholderText
                    background: Rectangle { implicitHeight: 42; radius: 10; color: C.surfaceAlt; border.color: C.border; border.width: 1 }
                    onTextEdited: dlg.scheduleCheck()
                }
            }

            ColumnLayout {
                Layout.fillWidth: true
                Layout.leftMargin: 18; Layout.rightMargin: 18
                spacing: 4
                Text { text: "CHARGER"; font.pixelSize: 9; font.bold: true; font.letterSpacing: 1.0; color: C.textDim }
                ComboBox {
                    id: chargerCombo
                    objectName: "chargerCombo"
                    Layout.fillWidth: true
                    model: dlg.chargers
                    textRole: "name"
                    currentIndex: -1
                    font.pixelSize: 13
                    contentItem: Text {
                        text: chargerCombo.currentIndex >= 0 ? chargerCombo.currentText : "Select a charger of the nav graph"
                        color: chargerCombo.currentIndex >= 0 ? C.text : C.textDim
                        font: chargerCombo.font; leftPadding: 10; elide: Text.ElideRight
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle { implicitHeight: 42; radius: 10; color: C.surfaceAlt; border.color: C.border; border.width: 1 }
                    delegate: ItemDelegate {
                        required property var modelData
                        required property int index
                        width: chargerCombo.width
                        highlighted: chargerCombo.highlightedIndex === index
                        text: modelData.name + (modelData.used_by
                              ? "  —  used by " + modelData.used_by + (modelData.used_by_removed ? " (removed)" : "") : "")
                    }
                    onActivated: dlg.scheduleCheck()
                }
                Text {
                    Layout.fillWidth: true
                    visible: dlg.fleet !== null && dlg.freeChargers === 0
                    text: "Every charger of this fleet's nav graph is in use."
                    color: C.warn; font.pixelSize: 11; wrapMode: Text.WordWrap
                }
                Text {
                    Layout.fillWidth: true
                    text: "Each robot of a fleet needs its own charger; the list is the chargers of the fleet's nav graph."
                    color: C.textDim; font.pixelSize: 11; wrapMode: Text.WordWrap
                }
            }

            // Only needed when the robot's map frame differs from the fleet's.
            ColumnLayout {
                id: advanced
                property bool expanded: false
                Layout.fillWidth: true
                Layout.leftMargin: 18; Layout.rightMargin: 18
                spacing: 6

                Text {
                    text: (advanced.expanded ? "▾ " : "▸ ") + "MAP FRAME (OPTIONAL)"
                    font.pixelSize: 9; font.bold: true; font.letterSpacing: 1.0; color: C.textDim
                    MouseArea { anchors.fill: parent; cursorShape: Qt.PointingHandCursor; onClicked: advanced.expanded = !advanced.expanded }
                }
                Text {
                    Layout.fillWidth: true
                    visible: advanced.expanded
                    text: "How the robot's own map maps onto the fleet's. Leave blank when they are the same."
                    color: C.textDim; font.pixelSize: 11; wrapMode: Text.WordWrap
                }
                GridLayout {
                    Layout.fillWidth: true
                    visible: advanced.expanded
                    columns: 4
                    columnSpacing: 6
                    Text { text: "ROTATION"; font.pixelSize: 9; color: C.textDim }
                    Text { text: "SCALE"; font.pixelSize: 9; color: C.textDim }
                    Text { text: "X SHIFT"; font.pixelSize: 9; color: C.textDim }
                    Text { text: "Y SHIFT"; font.pixelSize: 9; color: C.textDim }
                    TextField { id: rotationField; Layout.fillWidth: true; placeholderText: "rad"; onTextEdited: dlg.scheduleCheck()
                        color: C.text; font.pixelSize: 13; placeholderTextColor: C.placeholderText
                        validator: DoubleValidator { notation: DoubleValidator.StandardNotation }
                        background: Rectangle { implicitHeight: 38; radius: 10; color: C.surfaceAlt; border.color: C.border; border.width: 1 } }
                    TextField { id: scaleField; Layout.fillWidth: true; placeholderText: "1"; onTextEdited: dlg.scheduleCheck()
                        color: C.text; font.pixelSize: 13; placeholderTextColor: C.placeholderText
                        validator: DoubleValidator { notation: DoubleValidator.StandardNotation }
                        background: Rectangle { implicitHeight: 38; radius: 10; color: C.surfaceAlt; border.color: C.border; border.width: 1 } }
                    TextField { id: txField; Layout.fillWidth: true; placeholderText: "m"; onTextEdited: dlg.scheduleCheck()
                        color: C.text; font.pixelSize: 13; placeholderTextColor: C.placeholderText
                        validator: DoubleValidator { notation: DoubleValidator.StandardNotation }
                        background: Rectangle { implicitHeight: 38; radius: 10; color: C.surfaceAlt; border.color: C.border; border.width: 1 } }
                    TextField { id: tyField; Layout.fillWidth: true; placeholderText: "m"; onTextEdited: dlg.scheduleCheck()
                        color: C.text; font.pixelSize: 13; placeholderTextColor: C.placeholderText
                        validator: DoubleValidator { notation: DoubleValidator.StandardNotation }
                        background: Rectangle { implicitHeight: 38; radius: 10; color: C.surfaceAlt; border.color: C.border; border.width: 1 } }
                }
            }

            // The adapter's verdict for the form as it stands.
            ColumnLayout {
                Layout.fillWidth: true
                Layout.leftMargin: 18; Layout.rightMargin: 18
                spacing: 6

                Text {
                    objectName: "statusText"
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    font.pixelSize: 12; font.bold: true
                    text: {
                        if (dlg.registering) return "Registering…"
                        if (dlg.fleets.length === 0) return "Waiting for a fleet adapter"
                        if (!dlg.canCheck) return dlg.transformValid ? "Choose the fleet of this robot's type"
                                                                     : "The map frame values must be numbers"
                        if (dlg.checking || !dlg.verdict) return "Checking with the fleet adapter…"
                        if (dlg.verdict.ok) return dlg.verdict.warnings.length > 0
                                                   ? "✓ All checks passed, with " + dlg.verdict.warnings.length + " warning(s)"
                                                   : "✓ All checks passed"
                        return dlg.verdict.needs_confirmation ? "Some checks could not be made" : "✗ Cannot register yet"
                    }
                    color: (dlg.verdict && dlg.verdict.ok) ? C.success
                           : ((dlg.verdict && !dlg.verdict.ok && !dlg.verdict.needs_confirmation) ? C.err : C.textDim)
                }

                Repeater {
                    model: dlg.findings
                    delegate: RowLayout {
                        required property var modelData
                        Layout.fillWidth: true
                        spacing: 8
                        Rectangle {
                            Layout.preferredWidth: 8; Layout.preferredHeight: 8; radius: 4
                            Layout.alignment: Qt.AlignTop; Layout.topMargin: 4
                            color: modelData.kind === "error" ? C.err : C.warn
                        }
                        Text {
                            Layout.fillWidth: true
                            text: modelData.message
                            color: C.text; font.pixelSize: 12; wrapMode: Text.WordWrap
                        }
                    }
                }

                CheckBox {
                    id: confirmBox
                    objectName: "confirmBox"
                    visible: !!dlg.verdict && (dlg.verdict.needs_confirmation || checked)
                    text: "Add it anyway: I accept that the checks above could not be made"
                    font.pixelSize: 12
                    contentItem: Text { text: confirmBox.text; color: C.text; font: confirmBox.font; wrapMode: Text.WordWrap
                                         leftPadding: confirmBox.indicator.width + 8; verticalAlignment: Text.AlignVCenter }
                    onToggled: dlg.scheduleCheck()
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.margins: 18
                spacing: 10
                Item { Layout.fillWidth: true }

                Button {
                    id: cancelBtn
                    text: "CANCEL"
                    implicitHeight: 36; leftPadding: 16; rightPadding: 16
                    contentItem: Text { text: cancelBtn.text; color: C.text; font.pixelSize: 13
                                         verticalAlignment: Text.AlignVCenter; horizontalAlignment: Text.AlignHCenter }
                    background: Rectangle { radius: 10; color: cancelBtn.down ? C.border : C.surfaceAlt; border.color: C.border; border.width: 1 }
                    onClicked: dlg.close()
                }

                Button {
                    id: registerBtn
                    objectName: "registerBtn"
                    text: "REGISTER ROBOT"
                    enabled: !!dlg.verdict && dlg.verdict.ok && !dlg.checking && !dlg.registering
                    implicitHeight: 36; leftPadding: 16; rightPadding: 16
                    contentItem: Text { text: registerBtn.text; color: "#ffffff"; font.pixelSize: 13; font.bold: true
                                         verticalAlignment: Text.AlignVCenter; horizontalAlignment: Text.AlignHCenter }
                    background: Rectangle { radius: 10; color: !registerBtn.enabled ? C.border : (registerBtn.down ? C.accentDark : C.accent) }
                    onClicked: {
                        dlg.registering = true
                        dlg.registerId = registry.register(dlg.requestJson())
                    }
                }
            }
        }
    }
}
