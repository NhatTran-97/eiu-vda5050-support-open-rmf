import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtCore

// Create a patrol or delivery task.
Dialog {
    id: dlg

    property var places: []      // Waypoint names from nav_graph
    // Only online robots can take a task.
    property var onlineRobots: []
    // The robot the operator picked; kept while the list of online robots changes.
    property string chosenRobot: ""
    property string errorMessage: ""
    // Id of the request this dialog sent; results of other requests are ignored.
    property string requestId: ""
    // Dispensers and ingestors RMF reports; the handler fields offer them.
    property var workcells: JSON.parse(ros.workcellsJson)
    // What the form still lacks before it can be sent; empty when complete.
    readonly property string missingFields: {
        var missing = []
        if (catCombo.currentText === "delivery") {
            if (pickupCombo.currentText === "") missing.push("pickup waypoint")
            if (pickupHandlerField.editText.trim() === "") missing.push("dispenser")
            if (dropoffCombo.currentText === "") missing.push("dropoff waypoint")
            if (dropoffHandlerField.editText.trim() === "") missing.push("ingestor")
            if (skuField.text.trim() === "") missing.push("payload SKU")
        } else if (placeCombo.currentText === "") {
            missing.push("waypoint")
        }
        return missing.join(", ")
    }

    onAboutToShow: {
        dlg.chosenRobot = ""
        robotCombo.currentIndex = 0
        dlg.requestId = ""
        dlg.errorMessage = ""
    }

    // A new list of online robots resets the combo box; put the operator's choice back, or say it went away.
    onOnlineRobotsChanged: Qt.callLater(function() {
        if (dlg.chosenRobot === "") return
        var index = robotCombo.find(dlg.chosenRobot)
        if (index > 0) {
            robotCombo.currentIndex = index
        } else {
            robotCombo.currentIndex = 0
            if (dlg.opened)
                dlg.errorMessage = dlg.chosenRobot + " went offline; choose a robot again"
            dlg.chosenRobot = ""
        }
    })

    Connections {
        target: ros
        function onDispatchResult(id, kind, ok, message) {
            if (id !== dlg.requestId) return
            if (ok) { dlg.errorMessage = ""; dlg.close() }
            else    { dlg.errorMessage = message }
        }
        function onWorkcellsChanged() { dlg.workcells = JSON.parse(ros.workcellsJson) }
    }

    // Remember the selected patrol loop count and the delivery payload across app restarts.
    Settings {
        category: "newTaskDialog"
        property alias lastLoops: loopsSpin.value
        property alias lastSku: skuField.text
        property alias lastQuantity: quantitySpin.value
    }

    modal: true
    width: 420
    padding: 0
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    background: Rectangle {
        color: Theme.surface; radius: 16
        border.color: Theme.border; border.width: 1
    }

    contentItem: ColumnLayout {
        spacing: 15

        Text {
            id: titleText
            Layout.fillWidth: true
            Layout.topMargin: 18; Layout.leftMargin: 18; Layout.rightMargin: 18
            text: "CREATE NEW MISSION"
            font.pixelSize: 16; font.bold: true; color: Theme.text
            font.letterSpacing: 1.0
            horizontalAlignment: Text.AlignHCenter

            // Drag to move the dialog.
            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.SizeAllCursor
                property point pressPos: Qt.point(0, 0)
                onPressed: (mouse) => {
                    dlg.anchors.centerIn = undefined
                    pressPos = Qt.point(mouse.x, mouse.y)
                }
                onPositionChanged: (mouse) => {
                    dlg.x += mouse.x - pressPos.x
                    dlg.y += mouse.y - pressPos.y
                }
            }
        }

        Text {
            Layout.fillWidth: true
            Layout.leftMargin: 18; Layout.rightMargin: 18
            text: "Dispatch a task to the Open-RMF fleet"
            font.pixelSize: 10; color: Theme.textDim
            horizontalAlignment: Text.AlignHCenter
        }

        // Choose the task category.
        ColumnLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 18; Layout.rightMargin: 18
            spacing: 4
            Text { text: "TASK CATEGORY"; font.pixelSize: 9; font.bold: true; font.letterSpacing: 1.0; color: Theme.textDim }
            ComboBox {
                id: catCombo
                Layout.fillWidth: true
                model: cfg.taskCategories
                font.pixelSize: 13
                contentItem: Text {
                    text: catCombo.displayText; color: Theme.text; font: catCombo.font
                    leftPadding: 10; elide: Text.ElideRight
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    implicitHeight: 42; radius: 10
                    color: Theme.surfaceAlt; border.color: Theme.border; border.width: 1
                }
            }
        }

        // Choose a patrol waypoint.
        ColumnLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 18; Layout.rightMargin: 18
            spacing: 4
            visible: catCombo.currentText !== "delivery"
            Text { text: "DESTINATION WAYPOINT"; font.pixelSize: 9; font.bold: true; font.letterSpacing: 1.0; color: Theme.textDim }
            ComboBox {
                id: placeCombo
                Layout.fillWidth: true
                model: dlg.places
                font.pixelSize: 13
                contentItem: Text {
                    text: placeCombo.displayText; color: Theme.text; font: placeCombo.font
                    leftPadding: 10; elide: Text.ElideRight
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    implicitHeight: 42; radius: 10
                    color: Theme.surfaceAlt; border.color: Theme.border; border.width: 1
                }
            }
        }

        // Choose which robot performs the task, or let RMF decide.
        ColumnLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 18; Layout.rightMargin: 18
            spacing: 4
            Text { text: "ASSIGN TO ROBOT"; font.pixelSize: 9; font.bold: true; font.letterSpacing: 1.0; color: Theme.textDim }
            ComboBox {
                id: robotCombo
                objectName: "robotCombo"
                Layout.fillWidth: true
                model: ["Auto (RMF chooses)"].concat(dlg.onlineRobots)
                font.pixelSize: 13
                onActivated: (index) => dlg.chosenRobot = index > 0 ? currentText : ""
                contentItem: Text {
                    text: robotCombo.displayText; color: robotCombo.currentIndex > 0 ? Theme.text : Theme.textDim
                    font: robotCombo.font
                    leftPadding: 10; elide: Text.ElideRight
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    implicitHeight: 42; radius: 10
                    color: Theme.surfaceAlt; border.color: Theme.border; border.width: 1
                }
            }
        }

        // Choose the patrol loop count.
        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 18; Layout.rightMargin: 18
            spacing: 12
            visible: catCombo.currentText !== "delivery"
            Text { text: "PATROL LOOPS"; font.pixelSize: 9; font.bold: true; font.letterSpacing: 1.0; color: Theme.textDim
                   Layout.alignment: Qt.AlignVCenter }
            SpinBox {
                id: loopsSpin
                from: 1; to: 50; value: 1
                editable: true
                font.pixelSize: 13
                contentItem: TextInput {
                    text: loopsSpin.textFromValue(loopsSpin.value, loopsSpin.locale)
                    color: Theme.text
                    horizontalAlignment: Qt.AlignHCenter
                    verticalAlignment: Qt.AlignVCenter
                    readOnly: !loopsSpin.editable
                    validator: loopsSpin.validator
                    inputMethodHints: Qt.ImhFormattedNumbersOnly
                }
                background: Rectangle {
                    implicitWidth: 100; implicitHeight: 42; radius: 10
                    color: Theme.surfaceAlt; border.color: Theme.border; border.width: 1
                }
            }
            Item { Layout.fillWidth: true }
        }

        // Choose delivery places and handlers.
        ColumnLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 18; Layout.rightMargin: 18
            spacing: 10
            visible: catCombo.currentText === "delivery"

            ColumnLayout {
                Layout.fillWidth: true; spacing: 4
                Text { text: "PICKUP WAYPOINT"; font.pixelSize: 9; font.bold: true; font.letterSpacing: 1.0; color: Theme.textDim }
                ComboBox {
                    id: pickupCombo
                    Layout.fillWidth: true
                    model: dlg.places
                    font.pixelSize: 13
                    contentItem: Text {
                        text: pickupCombo.displayText; color: Theme.text; font: pickupCombo.font
                        leftPadding: 10; elide: Text.ElideRight
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        implicitHeight: 42; radius: 10
                        color: Theme.surfaceAlt; border.color: Theme.border; border.width: 1
                    }
                }
            }
            ColumnLayout {
                Layout.fillWidth: true; spacing: 4
                Text { text: "DISPENSER (PICKUP HANDLER)"; font.pixelSize: 9; font.bold: true; font.letterSpacing: 1.0; color: Theme.textDim }
                ComboBox {
                    id: pickupHandlerField
                    Layout.fillWidth: true
                    editable: true
                    model: dlg.workcells.dispensers
                    font.pixelSize: 13
                    background: Rectangle {
                        implicitHeight: 42; radius: 10
                        color: Theme.surfaceAlt; border.color: Theme.border; border.width: 1
                    }
                }
            }
            ColumnLayout {
                Layout.fillWidth: true; spacing: 4
                Text { text: "DROPOFF WAYPOINT"; font.pixelSize: 9; font.bold: true; font.letterSpacing: 1.0; color: Theme.textDim }
                ComboBox {
                    id: dropoffCombo
                    Layout.fillWidth: true
                    model: dlg.places
                    font.pixelSize: 13
                    contentItem: Text {
                        text: dropoffCombo.displayText; color: Theme.text; font: dropoffCombo.font
                        leftPadding: 10; elide: Text.ElideRight
                        verticalAlignment: Text.AlignVCenter
                    }
                    background: Rectangle {
                        implicitHeight: 42; radius: 10
                        color: Theme.surfaceAlt; border.color: Theme.border; border.width: 1
                    }
                }
            }
            ColumnLayout {
                Layout.fillWidth: true; spacing: 4
                Text { text: "INGESTOR (DROPOFF HANDLER)"; font.pixelSize: 9; font.bold: true; font.letterSpacing: 1.0; color: Theme.textDim }
                ComboBox {
                    id: dropoffHandlerField
                    Layout.fillWidth: true
                    editable: true
                    model: dlg.workcells.ingestors
                    font.pixelSize: 13
                    background: Rectangle {
                        implicitHeight: 42; radius: 10
                        color: Theme.surfaceAlt; border.color: Theme.border; border.width: 1
                    }
                }
            }
            RowLayout {
                Layout.fillWidth: true; spacing: 12
                ColumnLayout {
                    Layout.fillWidth: true; spacing: 4
                    Text { text: "PAYLOAD (SKU)"; font.pixelSize: 9; font.bold: true; font.letterSpacing: 1.0; color: Theme.textDim }
                    TextField {
                        id: skuField
                        Layout.fillWidth: true
                        font.pixelSize: 13
                        color: Theme.text
                        placeholderText: "as the workcells expect it"
                        placeholderTextColor: Theme.placeholderText
                        background: Rectangle {
                            implicitHeight: 42; radius: 10
                            color: Theme.surfaceAlt; border.color: Theme.border; border.width: 1
                        }
                    }
                }
                ColumnLayout {
                    spacing: 4
                    Text { text: "QUANTITY"; font.pixelSize: 9; font.bold: true; font.letterSpacing: 1.0; color: Theme.textDim }
                    SpinBox {
                        id: quantitySpin
                        from: 1; to: 999; value: 1
                        editable: true
                        font.pixelSize: 13
                        contentItem: TextInput {
                            text: quantitySpin.textFromValue(quantitySpin.value, quantitySpin.locale)
                            color: Theme.text
                            horizontalAlignment: Qt.AlignHCenter
                            verticalAlignment: Qt.AlignVCenter
                            readOnly: !quantitySpin.editable
                            validator: quantitySpin.validator
                            inputMethodHints: Qt.ImhFormattedNumbersOnly
                        }
                        background: Rectangle {
                            implicitWidth: 100; implicitHeight: 42; radius: 10
                            color: Theme.surfaceAlt; border.color: Theme.border; border.width: 1
                        }
                    }
                }
            }
        }

        Text {
            Layout.fillWidth: true
            Layout.leftMargin: 18; Layout.rightMargin: 18
            visible: dlg.errorMessage !== ""
            text: dlg.errorMessage
            color: Theme.err
            font.pixelSize: 11
            wrapMode: Text.WordWrap
        }

        Text {
            Layout.fillWidth: true
            Layout.leftMargin: 18; Layout.rightMargin: 18
            visible: dlg.missingFields !== ""
            text: "Still needed: " + dlg.missingFields
            color: Theme.textDim
            font.pixelSize: 11
            wrapMode: Text.WordWrap
        }

        // Submit or close the dialog.
        RowLayout {
            Layout.fillWidth: true
            Layout.margins: 18
            spacing: 10
            Item { Layout.fillWidth: true }

            Button {
                id: cancelBtn
                text: "CANCEL"
                implicitHeight: 36; leftPadding: 16; rightPadding: 16
                contentItem: Text {
                    text: cancelBtn.text; color: Theme.text; font.pixelSize: 13
                    verticalAlignment: Text.AlignVCenter
                    horizontalAlignment: Text.AlignHCenter
                }
                background: Rectangle {
                    radius: 10; color: cancelBtn.down ? Theme.border : Theme.surfaceAlt
                    border.color: Theme.border; border.width: 1
                }
                onClicked: dlg.close()
            }

            Button {
                id: submitBtn
                objectName: "submitBtn"
                text: "DISPATCH TASK"
                enabled: dlg.missingFields === ""
                implicitHeight: 36; leftPadding: 16; rightPadding: 16
                contentItem: Text {
                    text: submitBtn.text; color: Theme.textOnAccent; font.pixelSize: 13; font.bold: true
                    verticalAlignment: Text.AlignVCenter
                    horizontalAlignment: Text.AlignHCenter
                }
                background: Rectangle {
                    radius: 10
                    color: !submitBtn.enabled ? Theme.border
                          : (submitBtn.down ? Theme.accentDark : Theme.accent)
                }
                onClicked: {
                    dlg.errorMessage = ""
                    if (catCombo.currentText === "delivery") {
                        dlg.requestId = robotCombo.currentIndex > 0
                            ? ros.dispatchDeliveryToRobot(pickupCombo.currentText, pickupHandlerField.editText.trim(),
                                                          dropoffCombo.currentText, dropoffHandlerField.editText.trim(),
                                                          skuField.text.trim(), quantitySpin.value, robotCombo.currentText)
                            : ros.dispatchDelivery(pickupCombo.currentText, pickupHandlerField.editText.trim(),
                                                   dropoffCombo.currentText, dropoffHandlerField.editText.trim(),
                                                   skuField.text.trim(), quantitySpin.value)
                    } else if (robotCombo.currentIndex > 0) {
                        dlg.requestId = ros.dispatchToRobot(catCombo.currentText, placeCombo.currentText, loopsSpin.value,
                                                            robotCombo.currentText)
                    } else {
                        dlg.requestId = ros.dispatch(catCombo.currentText, placeCombo.currentText, loopsSpin.value)
                    }
                }
            }
        }
    }
}
