import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// System health, robots, traffic and tasks at a glance.
RowLayout {
    spacing: 14

    MetricCard {
        Layout.fillWidth: true
        Layout.fillHeight: true
        Layout.minimumWidth: 210
        title: "System health"
        value: dashboard.healthLevel
        valueFontFamily: fontMono
        detail: dashboard.healthDetail
        iconSource: statusActiveIconUrl
        accentColor: Theme.healthColor(dashboard.healthLevel)
        alert: dashboard.healthLevel !== "HEALTHY"
    }
    MetricCard {
        Layout.fillWidth: true
        Layout.fillHeight: true
        Layout.minimumWidth: 210
        title: "Robots"
        value: dashboard.robotCount > 0
               ? dashboard.onlineCount + "/" + dashboard.robotCount + " ONLINE"
               : "0 ROBOTS"
        valueFontFamily: fontMono
        detail: dashboard.fleetSummary
        iconSource: fleetRobotIconUrl
        accentColor: Theme.cyan
    }
    MetricCard {
        Layout.fillWidth: true
        Layout.fillHeight: true
        Layout.minimumWidth: 210
        title: "Traffic status"
        value: !ros.rmfOnline ? "NO DATA"
               : (ros.activeConflicts > 0 ? "CONFLICT"
                  : (ros.blockedLanes > 0 ? "CONGESTED" : "NORMAL"))
        valueFontFamily: fontMono
        detail: !ros.rmfOnline ? "RMF connection required"
                : (ros.activeConflicts + " conflicts · " + ros.blockedLanes + " blocked lanes")
        iconText: "⇄"
        accentColor: !ros.rmfOnline ? Theme.textDim
                     : (ros.activeConflicts > 0 ? Theme.err
                        : (ros.blockedLanes > 0 ? Theme.warn : Theme.success))
        alert: ros.rmfOnline && ros.activeConflicts > 0
    }
    MetricCard {
        Layout.fillWidth: true
        Layout.fillHeight: true
        Layout.minimumWidth: 210
        title: "Tasks"
        value: dashboard.runningTasks + " ACTIVE"
        valueFontFamily: fontMono
        detail: dashboard.queuedTasks + " queued · " + dashboard.completedTasks + " completed"
        iconText: "✓"
        accentColor: Theme.accent
    }
}
