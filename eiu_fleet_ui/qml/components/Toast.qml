import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// A short message that fades away by itself, with an optional action button.
Rectangle {
    id: toast

    // info | ok | warn | error
    property string tone: "info"
    property string message: ""
    property string actionText: ""
    property int lifetimeMs: 9000
    signal actionTriggered()

    readonly property color toneColor: tone === "ok" ? Theme.success
                                       : (tone === "warn" ? Theme.warn : (tone === "error" ? Theme.err : Theme.cyan))

    function show(text, kind, action, lifetime) {
        toast.message = text
        toast.tone = kind || "info"
        toast.actionText = action || ""
        toast.lifetimeMs = lifetime || 9000
        toast.opacity = 1
        hideTimer.restart()
    }

    function hide() {
        hideTimer.stop()
        toast.opacity = 0
    }

    visible: opacity > 0
    opacity: 0
    implicitWidth: Math.min(560, row.implicitWidth + 32)
    implicitHeight: row.implicitHeight + 20
    radius: 12
    color: Theme.surfaceRaised
    border.color: toast.toneColor
    border.width: 1

    Behavior on opacity { NumberAnimation { duration: 200 } }
    Timer { id: hideTimer; interval: toast.lifetimeMs; onTriggered: toast.hide() }

    RowLayout {
        id: row
        anchors.centerIn: parent
        width: parent.width - 32
        spacing: 12

        Rectangle {
            Layout.preferredWidth: 8; Layout.preferredHeight: 8; radius: 4
            Layout.alignment: Qt.AlignVCenter
            color: toast.toneColor
        }
        Text {
            Layout.fillWidth: true
            text: toast.message
            color: Theme.text
            font.pixelSize: 13
            wrapMode: Text.WordWrap
        }
        Button {
            visible: toast.actionText !== ""
            text: toast.actionText
            implicitHeight: 30; leftPadding: 14; rightPadding: 14
            contentItem: Text { text: parent.text; color: Theme.textOnAccent; font.pixelSize: 12; font.bold: true
                                 horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
            background: Rectangle { radius: 8; color: parent.down ? Theme.accentDark : Theme.accent }
            onClicked: { toast.actionTriggered(); toast.hide() }
        }
        Button {
            Layout.preferredWidth: 26; Layout.preferredHeight: 26
            text: "×"
            contentItem: Text { text: parent.text; color: Theme.textDim; font.pixelSize: 18
                                 horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
            background: Rectangle { radius: 6; color: parent.hovered ? Theme.surfaceAlt : "transparent" }
            onClicked: toast.hide()
        }
    }
}
