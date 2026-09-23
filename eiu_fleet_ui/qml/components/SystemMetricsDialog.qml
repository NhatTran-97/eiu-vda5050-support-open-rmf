import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtCore


Dialog {
    id: dlg

    // Entries of adapterMetrics.metricsJson.
    property var adapters: []
    // Width on the left of the window that stays uncovered.
    property real railWidth: 0
    // Zoom chosen by the user: 1 is the size that fits the window; 0.6 to 1.8.
    property real zoom: 1.0
    // Scale of text, spacing and charts: follows the window's width (1 up to 1.7), times the zoom.
    readonly property real ui: Math.max(1.0, Math.min(1.7, width / 1100)) * zoom

    function zoomBy(step) {
        zoom = Math.max(0.6, Math.min(1.8, Math.round((zoom + step) * 10) / 10))
    }
    Settings {
        category: "SystemView"
        property alias zoom: dlg.zoom
    }

    function reload() {
        try { adapters = JSON.parse(adapterMetrics.metricsJson).adapters }
        catch (e) { adapters = [] }
    }
    function lastNumber(list) {
        for (var i = list.length - 1; i >= 0; i--)
            if (list[i] !== null && list[i] !== undefined) return list[i]
        return null
    }
    function statusText(status) {
        return { ok: "✓ HEALTHY", warning: "▲ WARNING", critical: "● CRITICAL", silent: "◌ NO REPORTS", waiting: "◌ LOOKING…", found: "✓ CONNECTED", absent: "✕ NOT FOUND" }[status] || status
    }
    function statusColor(status) {
        return status === "ok" || status === "found" ? Theme.success : (status === "critical" ? Theme.err : (status === "waiting" ? Theme.textDim : Theme.warn))
    }
    function agoText(s) {
        if (s === null || s === undefined) return "never reported"
        return s < 90 ? Math.round(s) + " s ago" : Math.round(s / 60) + " min ago"
    }

    Connections {
        target: adapterMetrics
        function onChanged() { if (dlg.opened) dlg.reload() }
    }
    onAboutToShow: reload()

    modal: true
    dim: false
    padding: 0
    x: railWidth
    y: 0
    width: parent ? parent.width - railWidth : 1180
    height: parent ? parent.height : 860
    closePolicy: Popup.CloseOnEscape

    background: Rectangle { color: Theme.bg }

    component Stat: Rectangle {
        property string label: ""
        property string value: ""
        property string detail: ""
        property color valueColor: Theme.text
        property real ui: 1.0
        Layout.fillWidth: true
        implicitHeight: 92 * ui
        radius: 10 * ui
        color: Theme.surfaceAlt
        border.color: Theme.border
        Column {
            anchors.verticalCenter: parent.verticalCenter
            anchors.left: parent.left; anchors.right: parent.right
            anchors.leftMargin: 14 * ui; anchors.rightMargin: 14 * ui
            spacing: 4 * ui
            Text { width: parent.width; text: label.toUpperCase(); color: Theme.textDim; font.pixelSize: 12 * ui; font.bold: true; font.letterSpacing: 1.0; elide: Text.ElideRight }
            Text { width: parent.width; text: value; color: valueColor; font.family: "IBM Plex Mono"; font.pixelSize: 26 * ui; font.bold: true; elide: Text.ElideRight }
            Text { width: parent.width; text: detail; color: Theme.textDim; font.pixelSize: 13 * ui; elide: Text.ElideRight }
        }
    }

    contentItem: ColumnLayout {
        spacing: 0

        Shortcut { enabled: dlg.opened; sequences: ["Ctrl++", "Ctrl+="]; onActivated: dlg.zoomBy(0.1) }
        Shortcut { enabled: dlg.opened; sequences: ["Ctrl+-"]; onActivated: dlg.zoomBy(-0.1) }
        Shortcut { enabled: dlg.opened; sequences: ["Ctrl+0"]; onActivated: dlg.zoom = 1.0 }

        RowLayout {
            Layout.fillWidth: true
            Layout.margins: 22 * dlg.ui
            Layout.bottomMargin: 12 * dlg.ui
            ColumnLayout {
                spacing: 3 * dlg.ui
                Text { text: "SYSTEM · FLEET ADAPTERS"; color: Theme.text; font.pixelSize: 22 * dlg.ui; font.bold: true; font.letterSpacing: 1.0 }
                Text { text: "Health of each adapter's VDA5050 message path, from its own metrics report"; color: Theme.textDim; font.pixelSize: 14 * dlg.ui }
            }
            Item { Layout.fillWidth: true }
            // Zoom out, the current zoom (a click resets it), zoom in.
            Row {
                spacing: 6 * dlg.ui
                Layout.rightMargin: 14 * dlg.ui
                Button {
                    text: "−"
                    enabled: dlg.zoom > 0.61
                    onClicked: dlg.zoomBy(-0.1)
                    contentItem: Text { text: parent.text; color: parent.enabled ? Theme.text : Theme.textDim; font.pixelSize: 20 * dlg.ui; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                    background: Rectangle { implicitWidth: 42 * dlg.ui; implicitHeight: 40 * dlg.ui; radius: 10; color: parent.hovered ? Theme.surfaceRaised : Theme.surfaceAlt; border.color: Theme.border }
                }
                Button {
                    text: Math.round(dlg.zoom * 100) + "%"
                    onClicked: dlg.zoom = 1.0
                    contentItem: Text { text: parent.text; color: Theme.text; font.family: "IBM Plex Mono"; font.pixelSize: 14 * dlg.ui; font.bold: true; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                    background: Rectangle { implicitWidth: 64 * dlg.ui; implicitHeight: 40 * dlg.ui; radius: 10; color: parent.hovered ? Theme.surfaceRaised : Theme.surfaceAlt; border.color: Theme.border }
                }
                Button {
                    text: "+"
                    enabled: dlg.zoom < 1.79
                    onClicked: dlg.zoomBy(0.1)
                    contentItem: Text { text: parent.text; color: parent.enabled ? Theme.text : Theme.textDim; font.pixelSize: 20 * dlg.ui; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                    background: Rectangle { implicitWidth: 42 * dlg.ui; implicitHeight: 40 * dlg.ui; radius: 10; color: parent.hovered ? Theme.surfaceRaised : Theme.surfaceAlt; border.color: Theme.border }
                }
            }
            Button {
                text: "Close"
                onClicked: dlg.close()
                contentItem: Text { text: parent.text; color: Theme.text; font.pixelSize: 15 * dlg.ui; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                background: Rectangle { implicitWidth: 92 * dlg.ui; implicitHeight: 40 * dlg.ui; radius: 10; color: parent.hovered ? Theme.surfaceRaised : Theme.surfaceAlt; border.color: Theme.border }
            }
        }

        Flickable {
            id: scroller
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            contentWidth: width
            contentHeight: cards.implicitHeight + 22 * dlg.ui
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { }
            // Ctrl + wheel zooms; the plain wheel still scrolls.
            WheelHandler {
                acceptedModifiers: Qt.ControlModifier
                onWheel: function (event) { dlg.zoomBy(event.angleDelta.y > 0 ? 0.1 : -0.1) }
            }

            ColumnLayout {
                id: cards
                width: scroller.width - 44 * dlg.ui
                x: 22 * dlg.ui
                spacing: 16 * dlg.ui

                Text {
                    visible: dlg.adapters.length === 0
                    Layout.fillWidth: true
                    Layout.topMargin: 30 * dlg.ui
                    text: "No fleet adapter is known yet."
                    color: Theme.textDim
                    horizontalAlignment: Text.AlignHCenter
                    font.pixelSize: 16 * dlg.ui
                }

                Repeater {
                    // Only the count drives the delegates, so a report does not rebuild them or make the charts flicker.
                    model: dlg.adapters.length

                    delegate: Rectangle {
                        id: card
                        readonly property var a: dlg.adapters[index]
                        readonly property var series: a.series
                        readonly property bool reporting: a.reported_ago_s !== null
                        Layout.fillWidth: true
                        implicitHeight: cardColumn.implicitHeight + 36 * dlg.ui
                        radius: 16
                        color: Theme.surface
                        border.color: dlg.statusColor(a.status)
                        border.width: a.status === "ok" || a.status === "waiting" || a.status === "found" ? 1 : 2

                        ColumnLayout {
                            id: cardColumn
                            anchors.fill: parent
                            anchors.margins: 18 * dlg.ui
                            spacing: 14 * dlg.ui

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 14 * dlg.ui
                                Text { text: card.a.fleet; color: Theme.text; font.pixelSize: 22 * dlg.ui; font.bold: true }
                                Text { text: card.a.node; color: Theme.textDim; font.family: "IBM Plex Mono"; font.pixelSize: 15 * dlg.ui }
                                Rectangle {
                                    implicitWidth: pillText.implicitWidth + 24 * dlg.ui; implicitHeight: 28 * dlg.ui; radius: height / 2
                                    color: Qt.rgba(1, 1, 1, 0.06)
                                    border.color: dlg.statusColor(card.a.status)
                                    Text { id: pillText; anchors.centerIn: parent; text: dlg.statusText(card.a.status); color: Theme.text; font.pixelSize: 13 * dlg.ui; font.bold: true; font.letterSpacing: 0.6 }
                                }
                                Item { Layout.fillWidth: true }
                                Text {
                                    text: card.reporting
                                          ? "Last report " + dlg.agoText(card.a.reported_ago_s) + " · every " + Math.round(card.a.interval_s) + " s"
                                          : (card.a.status === "found" ? "Adapter found · first report within its metrics_period_s"
                                             : "Nothing publishes /" + card.a.node + "/metrics")
                                    color: Theme.textDim; font.pixelSize: 14 * dlg.ui
                                }
                            }

                            GridLayout {
                                Layout.fillWidth: true
                                columns: cardColumn.width >= 6 * 172 * dlg.ui ? 6 : (cardColumn.width >= 3 * 172 * dlg.ui ? 3 : 2)
                                columnSpacing: 12 * dlg.ui; rowSpacing: 12 * dlg.ui
                                Stat {
                                    ui: dlg.ui
                                    label: "Robots online"
                                    value: card.reporting ? card.a.robots.online + " / " + card.a.robots.registered : "—"
                                    detail: card.reporting && card.a.robots.online < card.a.robots.registered ? "some robots are silent" : "all reporting"
                                    valueColor: card.reporting && card.a.robots.online < card.a.robots.registered ? Theme.warn : Theme.text
                                }
                                Stat {
                                    ui: dlg.ui
                                    label: "Oldest state"
                                    value: card.reporting ? Number(card.a.robots.state_age_max_s).toFixed(1) + " s" : "—"
                                    detail: card.reporting && card.a.robots.oldest_state_robot !== ""
                                            ? card.a.robots.oldest_state_robot + " · offline after " + card.a.robots.state_timeout_s + " s" : ""
                                }
                                Stat {
                                    ui: dlg.ui
                                    label: "Messages / s"
                                    value: card.reporting && dlg.lastNumber(card.series.msg_per_s) !== null ? Number(dlg.lastNumber(card.series.msg_per_s)).toFixed(1) : "—"
                                    detail: card.reporting ? "other fleets: " + card.a.totals.unregistered + " in all" : ""
                                }
                                Stat {
                                    ui: dlg.ui
                                    label: "Dropped"
                                    value: card.reporting ? String(card.a.totals.dropped) : "—"
                                    detail: card.reporting ? "+" + card.a.delta.dropped + " in the last report" : ""
                                    valueColor: card.reporting && card.a.delta.dropped > 0 ? Theme.warn : Theme.text
                                }
                                Stat {
                                    ui: dlg.ui
                                    label: "Update loop"
                                    value: card.reporting ? Number(card.a.latency_us.loop_pass.p99 / 1000).toFixed(1) + " ms" : "—"
                                    detail: card.reporting ? "p99 · " + card.a.delta.overruns + " overrun(s) lately" : ""
                                    valueColor: card.reporting && card.a.delta.overruns > 0 ? Theme.warn : Theme.text
                                }
                                Stat {
                                    ui: dlg.ui
                                    label: "MQTT link"
                                    value: !card.reporting ? "—" : (card.a.mqtt.connected ? "Connected" : "Lost")
                                    detail: card.reporting ? card.a.mqtt.connections_lost + " connection(s) lost since start" : ""
                                    valueColor: card.reporting && !card.a.mqtt.connected ? Theme.err : Theme.text
                                }
                            }

                            GridLayout {
                                Layout.fillWidth: true
                                columns: cardColumn.width >= 4 * 430 * dlg.ui ? 4 : (cardColumn.width >= 2 * 430 * dlg.ui ? 2 : 1)
                                columnSpacing: 12 * dlg.ui; rowSpacing: 12 * dlg.ui
                                MetricChart {
                                    Layout.fillWidth: true
                                    s: dlg.ui
                                    title: "Messages per second"; unit: "/s"; decimals: 1
                                    ages: card.series.age_s; values: card.series.msg_per_s
                                }
                                MetricChart {
                                    Layout.fillWidth: true
                                    s: dlg.ui
                                    title: "State handling, p99"; unit: "µs"; decimals: 0
                                    ages: card.series.age_s; values: card.series.handle_p99_us
                                }
                                MetricChart {
                                    Layout.fillWidth: true
                                    s: dlg.ui
                                    title: "Oldest state age"; unit: "s"; decimals: 1
                                    ages: card.series.age_s; values: card.series.state_age_max_s
                                    limit: card.a.robots.state_timeout_s
                                    limitLabel: "offline after " + card.a.robots.state_timeout_s + " s"
                                }
                                MetricChart {
                                    Layout.fillWidth: true
                                    s: dlg.ui
                                    title: "Update loop pass, p99"; unit: "ms"; decimals: 2
                                    ages: card.series.age_s; values: card.series.loop_p99_ms
                                    limit: card.a.period_ms
                                    limitLabel: "update period " + Math.round(card.a.period_ms) + " ms"
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
