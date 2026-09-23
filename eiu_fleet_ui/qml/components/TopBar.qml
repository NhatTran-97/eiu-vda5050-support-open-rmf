import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Title, connection chips, the alert count and New Task.
Rectangle {
    id: bar

    signal systemRequested()
    signal newTaskRequested()

    color: Theme.headerBg

    Rectangle {
        anchors.bottom: parent.bottom
        width: parent.width
        height: 1
        color: Theme.border
        opacity: 0.55
    }

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 24
        anchors.rightMargin: 24
        spacing: 12

        Image {
            // The logo gives way to the chips on a narrow window.
            visible: bar.width >= 1000
            Layout.preferredWidth: 92
            Layout.preferredHeight: 32
            source: eiuLogoUrl
            fillMode: Image.PreserveAspectFit
            smooth: true
            mipmap: true
            asynchronous: true
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.minimumWidth: 120
            spacing: 2
            Text {
                Layout.fillWidth: true
                text: "Fleet Command Center"
                color: Theme.text
                font.pixelSize: 22
                font.bold: true
                elide: Text.ElideRight
            }
            Text {
                Layout.fillWidth: true
                text: "Live operations overview"
                color: Theme.textDim
                font.pixelSize: 12
                elide: Text.ElideRight
            }
        }

        StatusChip {
            label: ros.rmfOnline ? "RMF ONLINE" : "RMF OFFLINE"
            level: ros.rmfOnline ? "ok" : "err"
        }

        StatusChip {
            label: mqtt.connected ? "MQTT ONLINE" : "MQTT OFFLINE"
            level: mqtt.connected ? "ok" : "err"
        }

        // Whether the fleet adapters are there: found by their metrics topic, before any report arrives.
        StatusChip {
            label: adapterMetrics.adaptersLevel === "wait" ? "ADAPTERS …"
                   : "ADAPTERS " + adapterMetrics.adaptersFound + "/" + adapterMetrics.adaptersTotal
            level: adapterMetrics.adaptersLevel === "wait" ? "idle" : adapterMetrics.adaptersLevel
            clickable: true
            onClicked: bar.systemRequested()
        }

        StatusChip {
            label: dashboard.criticalCount > 0 ? dashboard.criticalCount + " CRITICAL"
                   : (dashboard.warningCount > 0 ? dashboard.warningCount + " WARNING" : "ALL CLEAR")
            level: dashboard.criticalCount > 0 ? "err" : (dashboard.warningCount > 0 ? "warn" : "ok")
        }

        // Shown when the adapter is configured to send task events over the websocket.
        StatusChip {
            visible: cfg.websocketEnabled
            label: wsTasks.connected ? "TASK EVENTS ON" : "TASK EVENTS OFF"
            level: wsTasks.connected ? "ok" : "err"
        }

        Button {
            id: newTaskButton
            text: "+  NEW TASK"
            Layout.preferredHeight: 38
            leftPadding: 17
            rightPadding: 17
            contentItem: Text {
                text: newTaskButton.text
                color: Theme.textOnAccent
                font.pixelSize: 11
                font.bold: true
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
            }
            background: Rectangle {
                radius: 10
                color: newTaskButton.down ? Theme.accentDark : Theme.accent
                border.color: Theme.accentBorder
                border.width: 1
            }
            onClicked: bar.newTaskRequested()
        }
    }
}
