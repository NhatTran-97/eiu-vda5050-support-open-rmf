import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Dialog "Create Task": chọn loại task + waypoint đích + số vòng → ros.dispatch()
Dialog {
    id: dlg

    property var places: []      // danh sách tên waypoint (từ nav_graph)

    modal: true
    width: 380
    padding: 0
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    background: Rectangle {
        color: C.surface; radius: 12
        border.color: C.border; border.width: 1
    }

    contentItem: ColumnLayout {
        spacing: 14

        Text {
            Layout.fillWidth: true
            Layout.topMargin: 18; Layout.leftMargin: 18; Layout.rightMargin: 18
            text: "Create Task"
            font.pixelSize: 16; font.bold: true; color: C.text
            horizontalAlignment: Text.AlignHCenter
        }

        // ── Task Category ──
        ColumnLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 18; Layout.rightMargin: 18
            spacing: 4
            Text { text: "Task Category"; font.pixelSize: 11; color: C.textDim }
            ComboBox {
                id: catCombo
                Layout.fillWidth: true
                model: ["patrol"]
                font.pixelSize: 13
                contentItem: Text {
                    text: catCombo.displayText; color: C.text; font: catCombo.font
                    leftPadding: 10; elide: Text.ElideRight
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    implicitHeight: 38; radius: 8
                    color: C.surfaceAlt; border.color: C.border; border.width: 1
                }
            }
        }

        // ── Place Name ──
        ColumnLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 18; Layout.rightMargin: 18
            spacing: 4
            Text { text: "Place Name  (waypoint đích)"; font.pixelSize: 11; color: C.textDim }
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
                    implicitHeight: 38; radius: 8
                    color: C.surfaceAlt; border.color: C.border; border.width: 1
                }
            }
        }

        // ── Loops ──
        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 18; Layout.rightMargin: 18
            spacing: 12
            Text { text: "Loops"; font.pixelSize: 11; color: C.textDim
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
                    implicitWidth: 100; implicitHeight: 38; radius: 8
                    color: C.surfaceAlt; border.color: C.border; border.width: 1
                }
            }
            Item { Layout.fillWidth: true }
        }

        // ── Buttons ──
        RowLayout {
            Layout.fillWidth: true
            Layout.margins: 18
            spacing: 10
            Item { Layout.fillWidth: true }

            Button {
                id: cancelBtn
                text: "Cancel"
                implicitHeight: 36; leftPadding: 16; rightPadding: 16
                contentItem: Text {
                    text: cancelBtn.text; color: C.text; font.pixelSize: 13
                    verticalAlignment: Text.AlignVCenter
                    horizontalAlignment: Text.AlignHCenter
                }
                background: Rectangle {
                    radius: 8; color: cancelBtn.down ? C.border : C.surfaceAlt
                    border.color: C.border; border.width: 1
                }
                onClicked: dlg.close()
            }

            Button {
                id: submitBtn
                text: "Submit"
                enabled: placeCombo.currentText !== ""
                implicitHeight: 36; leftPadding: 16; rightPadding: 16
                contentItem: Text {
                    text: submitBtn.text; color: "#ffffff"; font.pixelSize: 13; font.bold: true
                    verticalAlignment: Text.AlignVCenter
                    horizontalAlignment: Text.AlignHCenter
                }
                background: Rectangle {
                    radius: 8
                    color: !submitBtn.enabled ? C.border
                          : (submitBtn.down ? C.accentDark : C.accent)
                }
                onClicked: {
                    ros.dispatch(catCombo.currentText, placeCombo.currentText, loopsSpin.value)
                    dlg.close()
                }
            }
        }
    }
}
