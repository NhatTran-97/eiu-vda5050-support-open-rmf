.pragma library

// Text for times the dashboard shows; epoch seconds for "ago", epoch milliseconds for task times.

function formatAgo(nowMs, epochSec) {
    if (!epochSec)
        return ""
    var diff = Math.max(0, nowMs / 1000 - epochSec)
    if (diff < 60) return Math.floor(diff) + "s ago"
    if (diff < 3600) return Math.floor(diff / 60) + "m " + Math.floor(diff % 60) + "s ago"
    if (diff < 86400) return Math.floor(diff / 3600) + "h " + Math.floor((diff % 3600) / 60) + "m ago"
    return Math.floor(diff / 86400) + "d ago"
}

function formatAbsolute(epochSec) {
    return epochSec ? Qt.formatDateTime(new Date(epochSec * 1000), "d MMM yyyy, HH:mm:ss") : ""
}

function formatDay(ms) {
    return ms ? Qt.formatDate(new Date(ms), "dd MMM yyyy") : "—"
}

function formatClock(ms) {
    return ms ? Qt.formatTime(new Date(ms), "hh:mm:ss AP") : "—"
}
