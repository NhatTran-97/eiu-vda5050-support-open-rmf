pragma Singleton
import QtQuick

// The dashboard's palette and the colors it gives to states.
QtObject {
    // Surfaces.
    readonly property color bg: "#071727"
    readonly property color surface: "#0D2135"
    readonly property color surfaceAlt: "#12283F"
    readonly property color surfaceRaised: "#173049"
    readonly property color border: "#14324A"
    readonly property color railBg: "#081726"
    readonly property color headerBg: "#091827"
    readonly property color mapBg: "#081521"
    readonly property color tableHeaderBg: "#0A1A2B"

    // Text.
    readonly property color text: "#F1F6FA"
    readonly property color textDim: "#93B7D2"
    readonly property color textSoft: "#B7CCE0"
    readonly property color placeholderText: "#A9C4DA"
    readonly property color navText: "#8BA6BD"
    readonly property color textOnAccent: "#FFFFFF"
    readonly property color textOnBadge: "#0B1420"

    // Accents.
    readonly property color accent: "#2F80ED"
    readonly property color accentDark: "#1F5FBD"
    readonly property color accentBorder: "#5A9BFF"
    readonly property color blue: "#2F80ED"
    readonly property color cyan: "#18C8E3"
    readonly property color cyanBright: "#20E3F0"
    readonly property color navActive: "#3573C4"
    readonly property color navIndicator: "#9AC4FF"

    // States.
    readonly property color success: "#2DDC8C"
    readonly property color warn: "#F3B33D"
    readonly property color err: "#F04F64"
    readonly property color successBorder: "#286A60"
    readonly property color warnBorder: "#5A4A26"
    readonly property color errBorder: "#673044"
    readonly property color errHover: "#3B1B2A"

    // Badges and robot avatars.
    readonly property color badgeFill: "#143452"
    readonly property color badgeBorder: "#235278"
    readonly property color countBadgeFill: "#17365A"
    readonly property color avatarFill: "#153B65"
    readonly property color avatarBorder: "#285B8C"

    // Map.
    readonly property color mapLane: "#F5C400"
    readonly property string mapLaneArrow: "rgba(41,121,255,0.95)"
    readonly property color mapRoute: "#00E676"
    readonly property color mapRobot: "#2979FF"
    readonly property color mapRobotLabel: "#6FB2FF"
    readonly property color mapWaypoint: "#27AE60"
    readonly property color mapParking: "#2980B9"
    readonly property color mapCharger: "#F39C12"
    readonly property color mapBlocked: "#F05265"
    readonly property color mapPicked: "#FF3DAE"
    readonly property color mapEditor: "#00D9FF"
    readonly property color mapEditorStroke: "#04202B"
    readonly property color mapHighlight: "#FFFFFF"
    readonly property color mapLabel: "#EAF4FF"
    readonly property color mapWait: "#F3AE3D"

    // Robot status as RMF reports it.
    function statusColor(status) {
        if (status === "CHARGING")
            return success
        if (status === "MOVING" || status === "DOCKING" || status === "GOING_HOME" || status === "WORKING")
            return cyan
        if (status === "EMERGENCY" || status === "ERROR")
            return err
        if (status === "PAUSED" || status === "WAITING" || status === "PENDING SYNC" || status === "NO RMF DATA")
            return warn
        return textDim
    }

    // Task state as the task table shows it.
    function taskColor(state) {
        if (state === "cancel failed")
            return warn
        if (state.indexOf("complet") >= 0)
            return success
        if (state.indexOf("fail") >= 0)
            return err
        if (state.indexOf("cancel") >= 0 || state.indexOf("stale") >= 0)
            return textDim
        if (state.indexOf("queue") >= 0)
            return warn
        return cyan
    }

    // System health level: HEALTHY, DEGRADED, CRITICAL or OFFLINE.
    function healthColor(level) {
        return level === "HEALTHY" ? success : (level === "DEGRADED" ? warn : err)
    }

    // Needs Attention severity.
    function severityColor(severity) {
        return severity === "critical" ? err : (severity === "info" ? cyan : warn)
    }
}
