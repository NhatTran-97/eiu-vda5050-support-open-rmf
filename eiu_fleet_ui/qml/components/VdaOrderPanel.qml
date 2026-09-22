import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// VDA5050 order layer under the RMF task: order and update id, route progress, running action and picked node details.
Rectangle {
    id: root

    property string robotName: ""
    property bool hasTele: false
    property bool hasOrder: false
    property bool offline: false
    property string orderId: ""
    property var updateId: null
    property var nodeStates: []         // nodes still to traverse (the robot drops passed ones)
    property var actionStates: []
    property string lastNodeId: ""
    property var lastNodeSeq: null
    property var orderDetail: null      // trimmed order message: node poses, actions, edges
    property real uiScale: 1.0
    property var pickedSeq: null        // node the operator clicked; null = follow the current node

    onOrderIdChanged: pickedSeq = null

    radius: 10
    color: C.surface
    border.color: C.border
    border.width: 1
    opacity: offline ? 0.6 : 1.0
    implicitHeight: column.implicitHeight + 20

    // done = passed (sequenceId <= lastNodeSequenceId), current = first node to traverse, pending = released ahead,
    // horizon = not yet released. Ids can repeat in an order, so nodes are told apart by sequenceId.
    readonly property var route: {
        var remaining = {}
        var firstSeq = null
        for (var i = 0; i < nodeStates.length; i++) {
            var ns = nodeStates[i]
            remaining[ns.sequenceId] = ns
            if (firstSeq === null || ns.sequenceId < firstSeq) firstSeq = ns.sequenceId
        }

        var haveOrder = orderDetail !== null && orderDetail.orderId === orderId
        var src = []
        if (haveOrder) {
            src = orderDetail.nodes
        } else {
            // No order message seen (UI started mid-order): rebuild from state alone.
            if (lastNodeId !== "")
                src.push({ nodeId: lastNodeId, sequenceId: lastNodeSeq, passed: true, actions: [] })
            for (var j = 0; j < nodeStates.length; j++) {
                var n = nodeStates[j]
                var pos = n.nodePosition || {}
                src.push({ nodeId: n.nodeId, sequenceId: n.sequenceId, x: pos.x, y: pos.y,
                           theta: pos.theta, actions: [] })
            }
        }

        var out = []
        for (var k = 0; k < src.length; k++) {
            var node = src[k]
            var r = remaining[node.sequenceId]
            var st
            if (r !== undefined)
                st = node.sequenceId === firstSeq ? "current" : (r.released ? "pending" : "horizon")
            else if (node.passed || (lastNodeSeq !== null && node.sequenceId <= lastNodeSeq))
                st = "done"
            else
                st = "pending"     // in the order but state hasn't caught up yet

            var edge = null
            if (haveOrder) {
                for (var e = 0; e < orderDetail.edges.length; e++)
                    if (orderDetail.edges[e].sequenceId === node.sequenceId + 1) edge = orderDetail.edges[e]
            }
            out.push({ nodeId: node.nodeId, seq: node.sequenceId, state: st, x: node.x, y: node.y,
                       theta: node.theta, actions: node.actions || [], edge: edge })
        }
        return out
    }

    readonly property int doneCount: {
        var n = 0
        for (var i = 0; i < route.length; i++)
            if (route[i].state === "done") n++
        return n
    }

    // Index of the node whose details are shown: the picked one, else the current one.
    readonly property int shownIndex: {
        var fallback = -1
        for (var i = 0; i < route.length; i++) {
            if (pickedSeq !== null && route[i].seq === pickedSeq) return i
            if (route[i].state === "current") fallback = i
        }
        return fallback
    }
    readonly property var shownNode: shownIndex >= 0 ? route[shownIndex] : null

    readonly property string shortOrderId: orderId.length > 20
                                           ? orderId.substring(0, 8) + "…" + orderId.slice(-4) : orderId

    function num(v, unit) { return (v === null || v === undefined) ? "—" : Number(v).toFixed(2) + unit }

    function statusOf(actionId) {
        for (var i = 0; i < actionStates.length; i++)
            if (actionStates[i].actionId === actionId) return actionStates[i].actionStatus
        return "WAITING"
    }

    // blockingType comes from the order message; NONE/unknown gets no tag.
    function blockTag(a) {
        if (a.blockingType === "HARD") return " [BLOCKING]"
        if (a.blockingType === "SOFT") return " [SOFT]"
        return ""
    }

    readonly property var runningActions: {
        var out = []
        for (var i = 0; i < actionStates.length; i++) {
            var a = actionStates[i]
            if (a && a.actionStatus === "RUNNING") out.push(a)
        }
        return out
    }

    ColumnLayout {
        id: column
        anchors.fill: parent
        anchors.margins: 10
        spacing: 12 * root.uiScale

        RowLayout {
            Layout.fillWidth: true
            spacing: 10
            Text {
                text: "VDA5050 ORDER"
                color: C.textDim
                font.pixelSize: 11 * root.uiScale
                font.bold: true
                font.letterSpacing: 1.0
            }
            Text {
                visible: root.hasOrder && root.route.length > 0
                text: root.doneCount + "/" + root.route.length + " nodes"
                color: C.textDim
                font.family: fontMono
                font.pixelSize: 11 * root.uiScale
            }
            // Show the running action inline.
            Text {
                visible: root.hasOrder
                text: root.runningActions.length > 0
                      ? root.runningActions.map(function(a) { return (a.actionType || a.actionId) + root.blockTag(a) + " · RUNNING" }).join("  ·  ")
                      : "no action running"
                color: root.runningActions.length > 0 ? C.warn : C.textDim
                font.family: fontMono
                font.pixelSize: 11 * root.uiScale
                elide: Text.ElideRight
                Layout.fillWidth: true
                Layout.minimumWidth: 0
            }
            Text {
                visible: root.hasOrder
                // Elide the action text before the order id.
                Layout.minimumWidth: implicitWidth
                horizontalAlignment: Text.AlignRight
                text: "Order " + root.shortOrderId + "  ·  Update " + root.updateId
                color: C.cyan
                font.family: fontMono
                font.pixelSize: 11 * root.uiScale
                font.bold: true

                MouseArea {
                    anchors.fill: parent
                    hoverEnabled: true
                    ToolTip.visible: containsMouse
                    ToolTip.delay: 300
                    ToolTip.text: "Order " + root.orderId + "  ·  Update " + root.updateId
                }
            }
        }

        Text {
            visible: !root.hasOrder
            Layout.fillWidth: true
            text: !root.hasTele ? "No telemetry received" : "No active order"
            color: C.textDim
            font.pixelSize: 12 * root.uiScale
        }

        // Route: done -> current -> pending -> horizon (unreleased); wraps onto more rows when it is long.
        Flow {
            visible: root.hasOrder && root.route.length > 0
            Layout.fillWidth: true
            spacing: 4
            Repeater {
                model: root.route
                delegate: Row {
                    readonly property string tileState: modelData.state
                    spacing: 4
                    Rectangle {
                        width: 74 * root.uiScale
                        height: 44 * root.uiScale
                        radius: 8
                        anchors.verticalCenter: parent.verticalCenter
                        opacity: tileState === "horizon" ? 0.55 : 1.0
                        readonly property bool picked: root.shownIndex === index
                        color: tileState === "current" ? Qt.rgba(0.09, 0.78, 0.89, 0.12)
                               : (picked ? Qt.rgba(1, 1, 1, 0.07) : "transparent")
                        border.width: tileState === "current" || picked ? 2 : 1
                        border.color: tileState === "done" ? C.success
                                      : (tileState === "current" ? C.cyan
                                         : (picked ? C.textDim : C.border))
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.pickedSeq = modelData.seq
                        }
                        Column {
                            anchors.centerIn: parent
                            spacing: 1
                            Text {
                                anchors.horizontalCenter: parent.horizontalCenter
                                text: modelData.nodeId || ("#" + index)
                                color: tileState === "done" ? C.success
                                       : (tileState === "current" ? C.cyan : C.textDim)
                                font.family: fontMono
                                font.pixelSize: 11 * root.uiScale
                                font.bold: true
                                elide: Text.ElideRight
                                width: 68 * root.uiScale
                                horizontalAlignment: Text.AlignHCenter
                            }
                            Text {
                                anchors.horizontalCenter: parent.horizontalCenter
                                text: tileState === "done" ? "passed" : (tileState === "current" ? "next" : tileState)
                                color: C.textDim
                                font.pixelSize: 9 * root.uiScale
                            }
                        }
                    }
                    Text {
                        visible: index < root.route.length - 1
                        anchors.verticalCenter: parent.verticalCenter
                        text: "→"
                        color: C.textDim
                        font.pixelSize: 13 * root.uiScale
                    }
                }
            }
        }

        // Node details: pose and actions of the shown node, and the edge that leaves it.
        Rectangle { visible: detailBox.visible; Layout.fillWidth: true; Layout.preferredHeight: 1; color: C.border }
        Column {
            id: detailBox
            visible: root.hasOrder && root.shownNode !== null
            Layout.fillWidth: true
            spacing: 5
            Text {
                width: parent.width
                text: root.shownNode
                      ? ("NODE " + root.shownNode.nodeId + "  ·  x " + root.num(root.shownNode.x, " m")
                         + "  y " + root.num(root.shownNode.y, " m") + "  θ " + root.num(root.shownNode.theta, " rad"))
                      : ""
                color: C.text
                font.family: fontMono
                font.pixelSize: 11 * root.uiScale
                font.bold: true
                elide: Text.ElideRight
            }
            Repeater {
                model: root.shownNode ? root.shownNode.actions : []
                delegate: Text {
                    width: detailBox.width
                    readonly property string status: root.statusOf(modelData.actionId)
                    text: "⚙ " + modelData.actionType + root.blockTag(modelData) + " · " + status
                    color: status === "RUNNING" ? C.warn : (status === "FINISHED" ? C.success
                           : (status === "FAILED" ? C.err : C.textDim))
                    font.family: fontMono
                    font.pixelSize: 11 * root.uiScale
                    elide: Text.ElideRight
                }
            }
            Text {
                width: parent.width
                visible: text !== ""
                text: {
                    var n = root.shownNode
                    if (!n) return ""
                    if (n.edge) {
                        var speed = n.edge.maxSpeed
                        return "→ " + n.edge.endNodeId + (speed !== null && speed !== undefined ? "  ·  max " + root.num(speed, " m/s") : "")
                    }
                    return root.shownIndex === root.route.length - 1 ? "last node of the order" : ""
                }
                color: C.textDim
                font.family: fontMono
                font.pixelSize: 11 * root.uiScale
                elide: Text.ElideRight
            }
        }

        // Keep the content at the top when the panel is taller than it needs.
        Item { Layout.fillHeight: true }
    }
}
