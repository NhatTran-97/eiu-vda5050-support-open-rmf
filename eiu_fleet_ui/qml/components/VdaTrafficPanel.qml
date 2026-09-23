import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Live VDA5050 message log (order / instantAction / state / connection).
// Click a row to read the original JSON.
Item {
    id: root

    property var messages: null         // keyed model of log entries matching typeFilter, newest first
    property real uiScale: 1.0
    property string typeFilter: "all"   // all | order | instantAction | state
    property var openEntry: null        // entry whose raw JSON is shown
    property string rawText: ""

    signal filterPicked(string kind)

    function typeColor(e) {
        if (e.type === "order") return Theme.cyan
        if (e.type === "instantAction") return Theme.warn
        if (e.type === "connection") return e.summary === "ONLINE" ? Theme.success : Theme.err
        return Theme.textDim
    }

    function statusColor(status) {
        if (status === "FINISHED") return Theme.success
        if (status === "FAILED") return Theme.err
        if (status === "SENT") return Theme.textDim
        return Theme.warn
    }

    function typeLabel(e) {
        return e.type === "instantAction" ? "ACTION" : e.type.toUpperCase()
    }

    function showRaw(entry) {
        root.rawText = mqtt.rawFor(entry.id) || "(payload no longer buffered)"
        root.openEntry = entry
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // Type filter chips.
        RowLayout {
            visible: root.openEntry === null
            Layout.fillWidth: true
            Layout.leftMargin: 17
            Layout.rightMargin: 17
            Layout.topMargin: 6
            Layout.bottomMargin: 4
            spacing: 6
            Repeater {
                model: [{ key: "all", label: "ALL" }, { key: "order", label: "ORDER" },
                        { key: "instantAction", label: "ACTION" }, { key: "state", label: "STATE" }]
                delegate: Rectangle {
                    readonly property bool active: root.typeFilter === modelData.key
                    Layout.preferredWidth: chipText.implicitWidth + 18
                    Layout.preferredHeight: 22 * root.uiScale
                    radius: 8
                    color: active ? Theme.badgeFill : "transparent"
                    border.color: active ? Theme.badgeBorder : Theme.border
                    border.width: 1
                    Text {
                        id: chipText
                        anchors.centerIn: parent
                        text: modelData.label
                        color: active ? Theme.cyan : Theme.textDim
                        font.pixelSize: 10 * root.uiScale
                        font.bold: true
                        font.letterSpacing: 0.6
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.filterPicked(modelData.key)
                    }
                }
            }
            Item { Layout.fillWidth: true }
        }

        // Message list.
        ListView {
            id: list
            visible: root.openEntry === null
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: root.messages
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

            delegate: Rectangle {
                id: entryRow
                required property var row
                width: ListView.view.width
                height: 30 * root.uiScale
                color: rowArea.containsMouse ? Theme.surfaceAlt : "transparent"

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 17
                    anchors.rightMargin: 17
                    spacing: 8

                    Text {
                        text: Qt.formatTime(new Date(entryRow.row.ts * 1000), "HH:mm:ss")
                        color: Theme.textDim
                        font.family: fontMono
                        font.pixelSize: 11 * root.uiScale
                        Layout.preferredWidth: 58 * root.uiScale
                    }
                    // "out" = adapter -> robot, "in" = robot -> adapter.
                    Text {
                        text: entryRow.row.dir === "out" ? "→" : "←"
                        color: root.typeColor(entryRow.row)
                        font.pixelSize: 14 * root.uiScale
                        font.bold: true
                    }
                    Rectangle {
                        Layout.preferredWidth: 74 * root.uiScale
                        Layout.preferredHeight: 20 * root.uiScale
                        radius: 6
                        color: "transparent"
                        border.color: root.typeColor(entryRow.row)
                        border.width: 1
                        Text {
                            anchors.centerIn: parent
                            text: root.typeLabel(entryRow.row)
                            color: root.typeColor(entryRow.row)
                            font.family: fontMono
                            font.pixelSize: 10 * root.uiScale
                            font.bold: true
                        }
                    }
                    Text {
                        text: entryRow.row.robot
                        color: Theme.text
                        font.family: fontMono
                        font.pixelSize: 11 * root.uiScale
                        font.bold: true
                        elide: Text.ElideRight
                        Layout.preferredWidth: 56 * root.uiScale
                    }
                    Text {
                        text: entryRow.row.summary
                        color: Theme.textDim
                        font.family: fontMono
                        font.pixelSize: 11 * root.uiScale
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }
                    Text {
                        visible: entryRow.row.type === "instantAction"
                        text: entryRow.row.status
                        color: root.statusColor(text)
                        font.family: fontMono
                        font.pixelSize: 10 * root.uiScale
                        font.bold: true
                    }
                }

                MouseArea {
                    id: rowArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: root.showRaw(entryRow.row)
                }
            }

            Text {
                anchors.centerIn: parent
                visible: !root.messages || root.messages.count === 0
                text: root.typeFilter === "all" ? "NO VDA5050 TRAFFIC YET" : "NO MATCHING MESSAGES"
                color: Theme.textDim
                font.pixelSize: 12
                font.bold: true
            }
        }

        // Raw JSON of the clicked message.
        RowLayout {
            visible: root.openEntry !== null
            Layout.fillWidth: true
            Layout.leftMargin: 17
            Layout.rightMargin: 17
            Layout.topMargin: 8
            Layout.bottomMargin: 6
            spacing: 10

            Button {
                text: "← Back"
                implicitHeight: 24 * root.uiScale
                leftPadding: 10
                rightPadding: 10
                contentItem: Text {
                    text: parent.text
                    color: Theme.text
                    font.pixelSize: 11 * root.uiScale
                    font.bold: true
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    radius: 8
                    color: parent.hovered ? Theme.surfaceAlt : "transparent"
                    border.color: Theme.border
                    border.width: 1
                }
                onClicked: root.openEntry = null
            }
            Text {
                Layout.fillWidth: true
                text: root.openEntry
                      ? (root.typeLabel(root.openEntry) + "  ·  " + root.openEntry.robot + "  ·  "
                         + Qt.formatTime(new Date(root.openEntry.ts * 1000), "HH:mm:ss"))
                      : ""
                color: root.openEntry ? root.typeColor(root.openEntry) : Theme.textDim
                font.family: fontMono
                font.pixelSize: 11 * root.uiScale
                font.bold: true
                elide: Text.ElideRight
            }
            Button {
                id: copyButton
                property bool copied: false
                text: copied ? "Copied" : "Copy"
                implicitHeight: 24 * root.uiScale
                leftPadding: 12
                rightPadding: 12
                contentItem: Text {
                    text: parent.text
                    color: Theme.textOnAccent
                    font.pixelSize: 11 * root.uiScale
                    font.bold: true
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    radius: 8
                    color: parent.down ? Theme.accentDark : Theme.accent
                }
                onClicked: {
                    rawView.selectAll()
                    rawView.copy()
                    rawView.deselect()
                    copied = true
                    copyReset.restart()
                }
                Timer { id: copyReset; interval: 1200; onTriggered: copyButton.copied = false }
            }
        }

        ScrollView {
            visible: root.openEntry !== null
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.leftMargin: 17
            Layout.rightMargin: 17
            Layout.bottomMargin: 10
            clip: true

            TextArea {
                id: rawView
                readOnly: true
                selectByMouse: true
                text: root.rawText
                color: Theme.text
                font.family: fontMono
                font.pixelSize: 11 * root.uiScale
                wrapMode: TextEdit.NoWrap
                background: Rectangle { radius: 8; color: Theme.surfaceAlt; border.color: Theme.border; border.width: 1 }
            }
        }
    }
}
