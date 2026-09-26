import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "Format.js" as Format

// Every active issue, most system-wide first; robot issues focus or open that robot.
Rectangle {
    id: panel

    property real nowTick: 0

    signal robotFocused(string name)
    signal controlsRequested(string name)
    signal registerRequested(var pending)

    // As tall as its items, up to five rows; it gives way when the column is short and the list scrolls.
    readonly property real contentHeight: dashboard.attention.count > 0
            ? Math.min(230, 52 + dashboard.attention.count * 50)
            : 90
    Layout.fillWidth: true
    Layout.fillHeight: true
    Layout.preferredHeight: contentHeight
    Layout.maximumHeight: contentHeight
    radius: 16
    color: Theme.surface
    border.color: dashboard.criticalCount > 0 ? Theme.errBorder
                  : (dashboard.warningCount > 0 ? Theme.warnBorder : Theme.border)
    border.width: 1
    clip: true

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: 44
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 17
                anchors.rightMargin: 17
                Text {
                    text: "NEEDS ATTENTION"
                    color: Theme.text
                    font.pixelSize: 13
                    font.bold: true
                    font.letterSpacing: 0.8
                }
                Item { Layout.fillWidth: true }
                Rectangle {
                    visible: dashboard.attention.count > 0
                    width: attnCountText.implicitWidth + 14
                    height: 20
                    radius: 10
                    color: dashboard.criticalCount > 0 ? Theme.err
                           : (dashboard.warningCount > 0 ? Theme.warn : Theme.cyan)
                    Text {
                        id: attnCountText
                        anchors.centerIn: parent
                        text: dashboard.attention.count
                        color: Theme.textOnBadge
                        font.bold: true
                        font.pixelSize: 11
                    }
                }
            }
        }

        Text {
            visible: dashboard.attention.count === 0
            Layout.fillWidth: true
            Layout.leftMargin: 17
            Layout.bottomMargin: 14
            text: "✓ No issues requiring attention"
            color: Theme.success
            font.pixelSize: 13
        }

        ListView {
            objectName: "attentionList"
            visible: dashboard.attention.count > 0
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: dashboard.attention
            ScrollIndicator.vertical: ScrollIndicator { }
            delegate: Item {
                id: attnRow
                required property var row
                width: ListView.view.width
                height: 50
                // Robot-specific items can be focused (row body) or diagnosed (chevron).
                readonly property bool clickable: row.robot !== ""

                Rectangle {
                    anchors.fill: parent
                    radius: 8
                    color: (attnRow.clickable && attnHover.hovered) ? Theme.surfaceAlt : "transparent"
                    Behavior on color { ColorAnimation { duration: 150 } }
                }
                HoverHandler { id: attnHover; enabled: attnRow.clickable }

                // Declared first so the chevron's own MouseArea below sits on top.
                MouseArea {
                    anchors.fill: parent
                    enabled: attnRow.clickable
                    cursorShape: Qt.PointingHandCursor
                    onClicked: panel.robotFocused(attnRow.row.robot)
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 17
                    anchors.rightMargin: 17
                    spacing: 10
                    Rectangle {
                        Layout.preferredWidth: 8
                        Layout.preferredHeight: 8
                        radius: 4
                        color: Theme.severityColor(attnRow.row.severity)
                        Layout.alignment: Qt.AlignVCenter
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 1
                        Text {
                            text: attnRow.row.title
                            color: Theme.text
                            font.bold: true
                            font.pixelSize: 13
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                        }
                        Text {
                            // Issues that started at a known time say how long ago, ticking locally.
                            text: attnRow.row.since > 0
                                  ? attnRow.row.detail + " · Last seen " + Format.formatAgo(panel.nowTick, attnRow.row.since)
                                  : attnRow.row.detail
                            color: Theme.textDim
                            font.pixelSize: 11
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                        }
                    }
                    // A robot found on the broker that no fleet has registered.
                    Row {
                        visible: !!attnRow.row.pending
                        Layout.alignment: Qt.AlignVCenter
                        spacing: 6
                        Button {
                            objectName: "registerRobotBtn"
                            text: "REGISTER"
                            implicitHeight: 28; leftPadding: 12; rightPadding: 12
                            contentItem: Text { text: parent.text; color: Theme.textOnAccent; font.pixelSize: 11; font.bold: true
                                                 horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                            background: Rectangle { radius: 8; color: parent.down ? Theme.accentDark : Theme.accent }
                            onClicked: panel.registerRequested(attnRow.row.pending)
                        }
                        Button {
                            objectName: "ignoreRobotBtn"
                            text: "×"
                            implicitWidth: 28; implicitHeight: 28
                            contentItem: Text { text: parent.text; color: Theme.textDim; font.pixelSize: 16
                                                 horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                            background: Rectangle { radius: 8; color: parent.hovered ? Theme.surfaceAlt : "transparent" }
                            ToolTip.visible: hovered
                            ToolTip.text: "Ignore until the dashboard restarts"
                            onClicked: registry.dismiss(attnRow.row.pending.key)
                        }
                    }
                    // Separate click target that opens the diagnostics.
                    Item {
                        visible: attnRow.clickable
                        Layout.preferredWidth: 26
                        Layout.preferredHeight: 26
                        Layout.alignment: Qt.AlignVCenter

                        Text {
                            anchors.centerIn: parent
                            text: "›"
                            color: chevronHover.hovered ? Theme.text : Theme.textDim
                            font.pixelSize: 18
                            Behavior on color { ColorAnimation { duration: 150 } }
                        }
                        HoverHandler { id: chevronHover }
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: panel.controlsRequested(attnRow.row.robot)
                        }
                    }
                }
            }
        }
    }
}
