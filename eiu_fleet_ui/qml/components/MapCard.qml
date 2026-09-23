import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../pages"

// The live map above the selected robot's analytics.
Rectangle {
    id: card

    property var waypoints: []
    property var lanes: []
    property var blockedEdgeIndices: []
    property string selectedRobotName: ""
    property real nowTick: 0
    readonly property alias mapView: mapView
    readonly property alias analytics: fleetAnalytics

    signal robotPicked(string name)
    // The analytics panel picked a robot itself (its selector, or the first online one).
    signal analyticsRobotChanged(string name)

    radius: 16
    color: Theme.surface
    border.color: Theme.border
    border.width: 1
    clip: true

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: 54

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 18
                anchors.rightMargin: 18
                spacing: 12

                Rectangle {
                    Layout.preferredWidth: 4
                    Layout.preferredHeight: 22
                    radius: 2
                    color: Theme.accent
                }
                Text {
                    text: "LIVE NAVIGATION MAP"
                    color: Theme.text
                    font.pixelSize: 13
                    font.bold: true
                    font.letterSpacing: 0.8
                }
                Item { Layout.fillWidth: true }
                Row {
                    spacing: 10
                    Repeater {
                        model: [
                            { label: "Graph",        color: Theme.mapLane },
                            { label: "Active route", color: Theme.mapRoute },
                            { label: "Robot",        color: Theme.mapRobot },
                            { label: "Charger",      color: Theme.mapCharger },
                            { label: "Blocked",      color: Theme.mapBlocked }
                        ]
                        delegate: Row {
                            spacing: 5
                            Rectangle { width: 7; height: 7; radius: 4; color: modelData.color; anchors.verticalCenter: parent.verticalCenter }
                            Text { text: modelData.label; color: Theme.textDim; font.pixelSize: 10 }
                        }
                    }
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: Theme.border
            opacity: 0.7
        }

        SplitView {
            id: mapAnalyticsSplit
            Layout.fillWidth: true
            Layout.fillHeight: true
            orientation: Qt.Vertical

            // Size the map region to the source image aspect ratio.
            readonly property real naturalMapHeight: {
                if (mapProv.pixelW <= 0 || mapProv.pixelH <= 0)
                    return 0
                var frameInsets = 36  // Map image margins
                var contentWidth = Math.max(1, width - frameInsets)
                return frameInsets + contentWidth
                       * mapProv.pixelH / mapProv.pixelW
            }

            // The map keeps its natural height up to a share of the column; analytics takes
            // the rest and scrolls when its content needs more.
            readonly property real handleHeight: 12
            readonly property real mapShare: 0.6
            readonly property real minimumPaneHeight: 160
            readonly property real mapHeight: Math.max(
                minimumPaneHeight, Math.min(naturalMapHeight, height * mapShare))

            handle: Rectangle {
                implicitHeight: 12
                color: SplitHandle.pressed
                       ? Qt.rgba(0.12, 0.47, 1.0, 0.10)
                       : "transparent"

                HoverHandler { cursorShape: Qt.SplitVCursor }

                Rectangle {
                    anchors.centerIn: parent
                    width: 64
                    height: SplitHandle.pressed ? 4 : 2
                    radius: 2
                    color: SplitHandle.hovered || SplitHandle.pressed
                           ? Theme.accent : Theme.border

                    Behavior on height {
                        NumberAnimation { duration: 100 }
                    }
                    Behavior on color {
                        ColorAnimation { duration: 120 }
                    }
                }
            }

            Item {
                objectName: "mapArea"
                SplitView.preferredHeight: mapAnalyticsSplit.mapHeight
                SplitView.minimumHeight: mapAnalyticsSplit.minimumPaneHeight

                MapPage {
                    id: mapView
                    objectName: "mapView"
                    anchors.fill: parent
                    anchors.margins: 10
                    waypoints: card.waypoints
                    edges: card.lanes
                    blockedEdgeIndices: card.blockedEdgeIndices
                    selectedRobotName: card.selectedRobotName
                    robotIconSource: robotIconUrl
                    robotIconUrls: cfg.robotIconUrls
                    onRobotPicked: (name) => card.robotPicked(name)
                }
            }

            Item {
                objectName: "analyticsArea"
                SplitView.fillHeight: true
                SplitView.minimumHeight: mapAnalyticsSplit.minimumPaneHeight

                FleetAnalytics {
                    id: fleetAnalytics
                    anchors.fill: parent
                    anchors.leftMargin: 10
                    anchors.rightMargin: 10
                    anchors.bottomMargin: 10
                    robots: dashboard.robotRows
                    robotNames: dashboard.robotNames
                    tasks: dashboard.taskRows
                    robotsOnline: dashboard.online
                    waypoints: card.waypoints
                    telemetry: dashboard.telemetry
                    nowTick: card.nowTick
                }
                // Mirror FleetAnalytics's own combo-box/auto-select choice upward.
                Connections {
                    target: fleetAnalytics
                    function onSelectedRobotNameChanged() {
                        card.analyticsRobotChanged(fleetAnalytics.selectedRobotName)
                    }
                }
            }
        }
    }
}
