import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtCore

// "Create Task" dialog: pick a task category + destination waypoint + loop count -> ros.dispatch()
Dialog {
    id: dlg

    property var places: []      // list of waypoint names (from nav_graph)
    property string errorMessage: ""

    Connections {
        target: ros
        function onDispatchResult(ok, message) {
            if (ok) { dlg.errorMessage = ""; dlg.close() }
            else    { dlg.errorMessage = message }
        }
    }

    // Remembers the last loop count across UI restarts -- everything else in
    // this dialog (category/place) is meant to be picked fresh each time.
    Settings {
        category: "newTaskDialog"
        property alias lastLoops: loopsSpin.value
    }

    modal: true
    width: 420
    padding: 0
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    background: Rectangle {
        color: C.surface; radius: 16
        border.color: C.border; border.width: 1
    }

    contentItem: ColumnLayout {
        spacing: 15

        Text {
            Layout.fillWidth: true
            Layout.topMargin: 18; Layout.leftMargin: 18; Layout.rightMargin: 18
            text: "CREATE NEW MISSION"
            font.pixelSize: 16; font.bold: true; color: C.text
            font.letterSpacing: 1.0
            horizontalAlignment: Text.AlignHCenter
        }

        Text {
            Layout.fillWidth: true
            Layout.leftMargin: 18; Layout.rightMargin: 18
            text: "Dispatch a patrol task to the Open-RMF fleet"
            font.pixelSize: 10; color: C.textDim
            horizontalAlignment: Text.AlignHCenter
        }

        // ── Task Category ──
        ColumnLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 18; Layout.rightMargin: 18
            spacing: 4
            Text { text: "TASK CATEGORY"; font.pixelSize: 9; font.bold: true; font.letterSpacing: 1.0; color: C.textDim }
            ComboBox {
                id: catCombo
                Layout.fillWidth: true
                model: cfg.taskCategories
                font.pixelSize: 13
                contentItem: Text {
                    text: catCombo.displayText; color: C.text; font: catCombo.font
                    leftPadding: 10; elide: Text.ElideRight
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    implicitHeight: 42; radius: 10
                    color: C.surfaceAlt; border.color: C.border; border.width: 1
                }
            }
        }

        // ── Place Name ──
        ColumnLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 18; Layout.rightMargin: 18
            spacing: 4
            Text { text: "DESTINATION WAYPOINT"; font.pixelSize: 9; font.bold: true; font.letterSpacing: 1.0; color: C.textDim }
            ComboBox {
                id: placeCombo
                Layout.fillWidth: true
                model: dlg.places
                font.pixelSize: 13
                contentItem: Text {
                    text: placeCombo.displayText; color: C.text; font: placeCombo.font
                    leftPadding: 10; elide: Text.ElideRight
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    implicitHeight: 42; radius: 10
                    color: C.surfaceAlt; border.color: C.border; border.width: 1
                }
            }
        }

        // ── Loops ──
        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 18; Layout.rightMargin: 18
            spacing: 12
            Text { text: "PATROL LOOPS"; font.pixelSize: 9; font.bold: true; font.letterSpacing: 1.0; color: C.textDim
                   Layout.alignment: Qt.AlignVCenter }
            SpinBox {
                id: loopsSpin
                from: 1; to: 50; value: 1
                editable: true
                font.pixelSize: 13
                contentItem: TextInput {
                    text: loopsSpin.textFromValue(loopsSpin.value, loopsSpin.locale)
                    color: C.text
                    horizontalAlignment: Qt.AlignHCenter
                    verticalAlignment: Qt.AlignVCenter
                    readOnly: !loopsSpin.editable
                    validator: loopsSpin.validator
                    inputMethodHints: Qt.ImhFormattedNumbersOnly
                }
                background: Rectangle {
                    implicitWidth: 100; implicitHeight: 42; radius: 10
                    color: C.surfaceAlt; border.color: C.border; border.width: 1
                }
            }
            Item { Layout.fillWidth: true }
        }

        Text {
            Layout.fillWidth: true
            Layout.leftMargin: 18; Layout.rightMargin: 18
            visible: dlg.errorMessage !== ""
            text: dlg.errorMessage
            color: C.err
            font.pixelSize: 11
            wrapMode: Text.WordWrap
        }

        // ── Buttons ──
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
                    text: cancelBtn.text; color: C.text; font.pixelSize: 13
                    verticalAlignment: Text.AlignVCenter
                    horizontalAlignment: Text.AlignHCenter
                }
                background: Rectangle {
                    radius: 10; color: cancelBtn.down ? C.border : C.surfaceAlt
                    border.color: C.border; border.width: 1
                }
                onClicked: dlg.close()
            }

            Button {
                id: submitBtn
                text: "DISPATCH TASK"
                enabled: placeCombo.currentText !== ""
                implicitHeight: 36; leftPadding: 16; rightPadding: 16
                contentItem: Text {
                    text: submitBtn.text; color: "#ffffff"; font.pixelSize: 13; font.bold: true
                    verticalAlignment: Text.AlignVCenter
                    horizontalAlignment: Text.AlignHCenter
                }
                background: Rectangle {
                    radius: 10
                    color: !submitBtn.enabled ? C.border
                          : (submitBtn.down ? C.accentDark : C.accent)
                }
                onClicked: {
                    dlg.errorMessage = ""
                    ros.dispatch(catCombo.currentText, placeCombo.currentText, loopsSpin.value)
                }
            }
        }
    }
}
