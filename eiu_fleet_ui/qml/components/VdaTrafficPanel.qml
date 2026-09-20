import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Live VDA5050 message log (order / instantAction / state / connection).
// Click a row to read the original JSON.
Item {
    id: root

    property var traffic: []            // newest first, no raw payloads
    property real uiScale: 1.0
    property string typeFilter: "all"   // all | order | instantAction | state
    property var openEntry: null        // entry whose raw JSON is shown
    property string rawText: ""

    readonly property var shown: {
        if (typeFilter === "all") return traffic
        return traffic.filter(function(e) { return e.type === typeFilter })
    }

    function typeColor(e) {
        if (e.type === "order") return C.cyan
        if (e.type === "instantAction") return C.warn
        if (e.type === "connection") return e.summary === "ONLINE" ? C.success : C.err
        return C.textDim
    }

    function statusColor(status) {
        if (status === "FINISHED") return C.success
        if (status === "FAILED") return C.err
        if (status === "SENT") return C.textDim
        return C.warn
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
                    color: active ? "#143452" : "transparent"
                    border.color: active ? "#235278" : C.border
                    border.width: 1
                    Text {
                        id: chipText
                        anchors.centerIn: parent
                        text: modelData.label
                        color: active ? C.cyan : C.textDim
                        font.pixelSize: 10 * root.uiScale
                        font.bold: true
                        font.letterSpacing: 0.6
                    }
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: root.typeFilter = modelData.key
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
            model: root.shown
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

            delegate: Rectangle {
                width: ListView.view.width
                height: 30 * root.uiScale
                color: rowArea.containsMouse ? C.surfaceAlt : "transparent"

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 17
                    anchors.rightMargin: 17
                    spacing: 8

                    Text {
                        text: Qt.formatTime(new Date(modelData.ts * 1000), "HH:mm:ss")
                        color: C.textDim
                        font.family: fontMono
                        font.pixelSize: 11 * root.uiScale
                        Layout.preferredWidth: 58 * root.uiScale
                    }
                    // "out" = adapter -> robot, "in" = robot -> adapter.
                    Text {
                        text: modelData.dir === "out" ? "→" : "←"
                        color: root.typeColor(modelData)
                        font.pixelSize: 14 * root.uiScale
                        font.bold: true
                    }
                    Rectangle {
                        Layout.preferredWidth: 74 * root.uiScale
                        Layout.preferredHeight: 20 * root.uiScale
                        radius: 6
                        color: "transparent"
                        border.color: root.typeColor(modelData)
                        border.width: 1
                        Text {
                            anchors.centerIn: parent
                            text: root.typeLabel(modelData)
                            color: root.typeColor(modelData)
                            font.family: fontMono
                            font.pixelSize: 10 * root.uiScale
                            font.bold: true
                        }
                    }
                    Text {
                        text: modelData.robot
                        color: C.text
                        font.family: fontMono
                        font.pixelSize: 11 * root.uiScale
                        font.bold: true
                        elide: Text.ElideRight
                        Layout.preferredWidth: 56 * root.uiScale
                    }
                    Text {
                        text: modelData.summary
                        color: C.textDim
                        font.family: fontMono
                        font.pixelSize: 11 * root.uiScale
                        elide: Text.ElideRight
                        Layout.fillWidth: true
                    }
                    Text {
                        visible: modelData.type === "instantAction"
                        text: modelData.status
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
                    onClicked: root.showRaw(modelData)
                }
            }

            Text {
                anchors.centerIn: parent
                visible: root.shown.length === 0
                text: root.traffic.length === 0 ? "NO VDA5050 TRAFFIC YET" : "NO MATCHING MESSAGES"
                color: C.textDim
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
                    color: C.text
                    font.pixelSize: 11 * root.uiScale
                    font.bold: true
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    radius: 8
                    color: parent.hovered ? C.surfaceAlt : "transparent"
                    border.color: C.border
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
                color: root.openEntry ? root.typeColor(root.openEntry) : C.textDim
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
                    color: "white"
                    font.pixelSize: 11 * root.uiScale
                    font.bold: true
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                background: Rectangle {
                    radius: 8
                    color: parent.down ? C.accentDark : C.accent
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
                color: C.text
                font.family: fontMono
                font.pixelSize: 11 * root.uiScale
                wrapMode: TextEdit.NoWrap
                background: Rectangle { radius: 8; color: C.surfaceAlt; border.color: C.border; border.width: 1 }
            }
        }
    }
}
