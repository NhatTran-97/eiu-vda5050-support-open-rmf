import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Modal prompt of the graph editor: waypoint name, lane direction or save-as filename, chosen by `mode`.
Dialog {
    id: dlg

    property string mode: "vertex"   // "vertex" | "lane" | "save"
    property string fromName: ""     // lane mode: endpoint labels
    property string toName: ""
    property string defaultPath: ""  // save mode: prefilled path
    property string prefillName: ""  // vertex mode: existing values when renaming
    property bool   prefillCharger: false

    signal vertexConfirmed(string name, bool isCharger)
    signal laneConfirmed(bool bidirectional)
    signal saveConfirmed(string path)

    modal: true
    width: 340
    padding: 0
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    onOpened: {
        if (mode === "vertex") {
            nameField.text = prefillName; chargerBox.checked = prefillCharger
            nameField.forceActiveFocus()
        } else if (mode === "lane") {
            bidirBox.checked = true
        } else if (mode === "save") {
            pathField.text = defaultPath; pathField.forceActiveFocus()
        }
    }

    background: Rectangle {
        color: Theme.surface; radius: 16
        border.color: Theme.border; border.width: 1
    }

    contentItem: ColumnLayout {
        spacing: 14

        Text {
            Layout.fillWidth: true
            Layout.topMargin: 18; Layout.leftMargin: 18; Layout.rightMargin: 18
            text: dlg.mode === "vertex" ? "NEW WAYPOINT"
                  : dlg.mode === "lane" ? "NEW LANE"
                  : "SAVE NAV GRAPH AS"
            font.pixelSize: 15; font.bold: true; color: Theme.text
            font.letterSpacing: 1.0
            horizontalAlignment: Text.AlignHCenter
        }

        // Vertex mode
        ColumnLayout {
            visible: dlg.mode === "vertex"
            Layout.fillWidth: true
            Layout.leftMargin: 18; Layout.rightMargin: 18
            spacing: 4
            Text { text: "NAME (optional)"; font.pixelSize: 9; font.bold: true; font.letterSpacing: 1.0; color: Theme.textDim }
            TextField {
                id: nameField
                Layout.fillWidth: true
                placeholderText: "e.g. wp1, charger_1"
                font.pixelSize: 13; color: Theme.text
                background: Rectangle {
                    implicitHeight: 40; radius: 10
                    color: Theme.surfaceAlt; border.color: Theme.border; border.width: 1
                }
                Keys.onReturnPressed: vertexBtn.clicked()
            }
            CheckBox {
                id: chargerBox
                text: "Charger waypoint"
                Layout.topMargin: 4
                contentItem: Text {
                    text: chargerBox.text; color: Theme.text; font.pixelSize: 12
                    leftPadding: chargerBox.indicator.width + 6
                    verticalAlignment: Text.AlignVCenter
                }
            }
        }

        // Lane mode
        ColumnLayout {
            visible: dlg.mode === "lane"
            Layout.fillWidth: true
            Layout.leftMargin: 18; Layout.rightMargin: 18
            spacing: 8
            Text {
                Layout.fillWidth: true
                text: dlg.fromName + "  →  " + dlg.toName
                font.pixelSize: 13; color: Theme.text
                horizontalAlignment: Text.AlignHCenter
            }
            CheckBox {
                id: bidirBox
                text: "Bidirectional (both directions)"
                contentItem: Text {
                    text: bidirBox.text; color: Theme.text; font.pixelSize: 12
                    leftPadding: bidirBox.indicator.width + 6
                    verticalAlignment: Text.AlignVCenter
                }
            }
        }

        // Save mode
        ColumnLayout {
            visible: dlg.mode === "save"
            Layout.fillWidth: true
            Layout.leftMargin: 18; Layout.rightMargin: 18
            spacing: 4
            Text { text: "FILE PATH"; font.pixelSize: 9; font.bold: true; font.letterSpacing: 1.0; color: Theme.textDim }
            TextField {
                id: pathField
                Layout.fillWidth: true
                font.pixelSize: 12; color: Theme.text
                background: Rectangle {
                    implicitHeight: 40; radius: 10
                    color: Theme.surfaceAlt; border.color: Theme.border; border.width: 1
                }
                Keys.onReturnPressed: saveBtn.clicked()
            }
            Text {
                Layout.fillWidth: true
                text: "Won't overwrite the loaded graph unless you keep the same path."
                font.pixelSize: 10; color: Theme.textDim
                wrapMode: Text.WordWrap
            }
        }

        // Buttons
        RowLayout {
            Layout.fillWidth: true
            Layout.margins: 18
            spacing: 10

            Button {
                text: "CANCEL"
                implicitHeight: 36; leftPadding: 16; rightPadding: 16
                Layout.fillWidth: true
                contentItem: Text {
                    text: parent.text; color: Theme.textDim; font.pixelSize: 12; font.bold: true
                    horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle { radius: 10; color: Theme.surfaceAlt; border.color: Theme.border; border.width: 1 }
                onClicked: dlg.close()
            }

            Button {
                id: vertexBtn
                visible: dlg.mode === "vertex"
                text: "ADD"
                implicitHeight: 36; leftPadding: 16; rightPadding: 16
                Layout.fillWidth: true
                contentItem: Text {
                    text: parent.text; color: Theme.textOnAccent; font.pixelSize: 12; font.bold: true
                    horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle { radius: 10; color: vertexBtn.down ? Theme.accentDark : Theme.accent }
                onClicked: {
                    dlg.vertexConfirmed(nameField.text, chargerBox.checked)
                    dlg.close()
                }
            }

            Button {
                id: laneBtn
                visible: dlg.mode === "lane"
                text: "ADD LANE"
                implicitHeight: 36; leftPadding: 16; rightPadding: 16
                Layout.fillWidth: true
                contentItem: Text {
                    text: parent.text; color: Theme.textOnAccent; font.pixelSize: 12; font.bold: true
                    horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle { radius: 10; color: laneBtn.down ? Theme.accentDark : Theme.accent }
                onClicked: {
                    dlg.laneConfirmed(bidirBox.checked)
                    dlg.close()
                }
            }

            Button {
                id: saveBtn
                visible: dlg.mode === "save"
                text: "SAVE"
                enabled: pathField.text.trim() !== ""
                implicitHeight: 36; leftPadding: 16; rightPadding: 16
                Layout.fillWidth: true
                contentItem: Text {
                    text: parent.text; color: Theme.textOnAccent; font.pixelSize: 12; font.bold: true
                    horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle { radius: 10; color: !saveBtn.enabled ? Theme.border : (saveBtn.down ? Theme.accentDark : Theme.accent) }
                onClicked: {
                    dlg.saveConfirmed(pathField.text.trim())
                    dlg.close()
                }
            }
        }
    }
}
