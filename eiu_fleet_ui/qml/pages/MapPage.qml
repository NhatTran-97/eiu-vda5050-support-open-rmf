import QtQuick

// Panel bản đồ: ảnh map + waypoints + lanes + robot marker, có zoom + pan.
Rectangle {
    id: root
    radius: 10
    color: C.surface
    border.color: C.border
    border.width: 1
    clip: true

    property var  waypoints:   []
    property var  edges:       []
    property var  mapRobots:   []
    property string plannedDest: ""

    // MQTT fallback (khi wired sau)
    property real robotX:  0
    property real robotY:  0
    property bool robotOk: false

    property real minScale: 0.4
    property real maxScale: 8.0

    Component.onCompleted: {
        root.waypoints  = JSON.parse(mapProv.wpJson)
        root.edges      = JSON.parse(mapProv.lanesJson)
        root.mapRobots  = JSON.parse(ros.robotsJson)
    }

    Connections {
        target: mqtt
        function onVizChanged() {
            root.robotX  = mqtt.posX
            root.robotY  = mqtt.posY
            root.robotOk = true
        }
    }

    Connections {
        target: ros
        function onRobotsChanged() {
            root.mapRobots = JSON.parse(ros.robotsJson)
            laneCanvas.requestPaint()
        }
        function onPathChanged() {
            root.plannedDest = ros.plannedDest
            laneCanvas.requestPaint()
        }
    }

    // toạ độ thế giới (m) → pixel trên ảnh đã vẽ (chưa tính scale của mapContent)
    function worldToScreen(wx, wy) {
        if (mapProv.pixelW <= 0 || mapProv.pixelH <= 0) return { x: -100, y: -100 }
        var px = (wx - mapProv.originX) / mapProv.resolution
        var py = mapProv.pixelH - (wy - mapProv.originY) / mapProv.resolution
        var offX = (mapImg.width  - mapImg.paintedWidth)  / 2
        var offY = (mapImg.height - mapImg.paintedHeight) / 2
        return {
            x: offX + px * mapImg.paintedWidth  / mapProv.pixelW,
            y: offY + py * mapImg.paintedHeight / mapProv.pixelH
        }
    }

    function zoomAt(px, py, factor) {
        var ns = Math.max(minScale, Math.min(maxScale, mapContent.scale * factor))
        var f  = ns / mapContent.scale
        mapContent.x = px - (px - mapContent.x) * f
        mapContent.y = py - (py - mapContent.y) * f
        mapContent.scale = ns
    }
    function resetView() {
        mapContent.scale = 1
        mapContent.x = 0
        mapContent.y = 0
    }

    // ── Nội dung có thể zoom/pan ─────────────────────────────────────────────
    Item {
        id: mapContent
        width:  root.width
        height: root.height
        transformOrigin: Item.TopLeft
        scale: 1

        Image {
            id: mapImg
            anchors.fill: parent
            anchors.margins: 8
            source: mapProv.imagePath
            fillMode: Image.PreserveAspectFit
            asynchronous: false
            cache: false
            smooth: false
            onStatusChanged:      if (status === Image.Ready) laneCanvas.requestPaint()
            onPaintedWidthChanged:  laneCanvas.requestPaint()
            onPaintedHeightChanged: laneCanvas.requestPaint()
        }

        Item {
            id: overlay
            anchors.fill: mapImg

            // ── z:1  Canvas: lanes + planned path ────────────────────────────
            Canvas {
                id: laneCanvas
                anchors.fill: parent
                z: 1
                onPaint: {
                    var ctx2d = getContext("2d")
                    ctx2d.clearRect(0, 0, width, height)

                    // Nav_graph lanes
                    ctx2d.strokeStyle = "#f5c400"
                    ctx2d.lineWidth   = 4
                    ctx2d.globalAlpha = 0.75
                    ctx2d.setLineDash([])
                    for (var i = 0; i < root.edges.length; i++) {
                        var e  = root.edges[i]
                        if (e.from >= e.to) continue        // bỏ chiều ngược (undirected)
                        var w1 = root.waypoints[e.from]
                        var w2 = root.waypoints[e.to]
                        if (!w1 || !w2) continue
                        var p1 = root.worldToScreen(w1.x, w1.y)
                        var p2 = root.worldToScreen(w2.x, w2.y)
                        ctx2d.beginPath()
                        ctx2d.moveTo(p1.x, p1.y)
                        ctx2d.lineTo(p2.x, p2.y)
                        ctx2d.stroke()
                    }

                    // Planned path: lấy trực tiếp từ RMF (robot.path = Location[])
                    if (root.mapRobots.length > 0) {
                        var rob = root.mapRobots[0]
                        var rmfPath = rob.path || []

                        // Glow tại đích (plannedDest từ dispatch, hoặc điểm cuối path)
                        var destPt = null
                        if (root.plannedDest !== "") {
                            for (var j = 0; j < root.waypoints.length; j++) {
                                if (root.waypoints[j].name === root.plannedDest) {
                                    destPt = root.worldToScreen(root.waypoints[j].x, root.waypoints[j].y)
                                    break
                                }
                            }
                        }
                        if (!destPt && rmfPath.length > 0) {
                            var last = rmfPath[rmfPath.length - 1]
                            destPt = root.worldToScreen(last.x, last.y)
                        }
                        if (destPt) {
                            ctx2d.setLineDash([])
                            ctx2d.strokeStyle = "#00e676"; ctx2d.lineWidth = 2
                            ctx2d.globalAlpha = 0.20; ctx2d.beginPath(); ctx2d.arc(destPt.x, destPt.y, 28, 0, Math.PI*2); ctx2d.stroke()
                            ctx2d.globalAlpha = 0.35; ctx2d.beginPath(); ctx2d.arc(destPt.x, destPt.y, 18, 0, Math.PI*2); ctx2d.stroke()
                            ctx2d.globalAlpha = 0.55; ctx2d.beginPath(); ctx2d.arc(destPt.x, destPt.y, 10, 0, Math.PI*2); ctx2d.stroke()
                            ctx2d.fillStyle = "#00e676"; ctx2d.globalAlpha = 0.45
                            ctx2d.beginPath(); ctx2d.arc(destPt.x, destPt.y, 6, 0, Math.PI*2); ctx2d.fill()
                        }

                        // Vẽ đường theo path RMF (vị trí robot → từng điểm trong path)
                        if (rmfPath.length > 0 && (rob.x !== 0 || rob.y !== 0)) {
                            var rp = root.worldToScreen(rob.x, rob.y)
                            ctx2d.strokeStyle = "#00e676"; ctx2d.lineWidth = 3
                            ctx2d.globalAlpha = 0.9; ctx2d.setLineDash([10, 5])
                            ctx2d.beginPath(); ctx2d.moveTo(rp.x, rp.y)
                            for (var m = 0; m < rmfPath.length; m++) {
                                var pp = root.worldToScreen(rmfPath[m].x, rmfPath[m].y)
                                ctx2d.lineTo(pp.x, pp.y)
                            }
                            ctx2d.stroke()
                            // Mũi tên tại điểm cuối
                            if (destPt) {
                                ctx2d.setLineDash([]); ctx2d.globalAlpha = 0.9
                                var prevPt = rmfPath.length >= 2
                                    ? root.worldToScreen(rmfPath[rmfPath.length-2].x, rmfPath[rmfPath.length-2].y)
                                    : rp
                                var ang = Math.atan2(destPt.y - prevPt.y, destPt.x - prevPt.x)
                                var al = 12, aa = 0.45
                                ctx2d.beginPath()
                                ctx2d.moveTo(destPt.x, destPt.y)
                                ctx2d.lineTo(destPt.x - al*Math.cos(ang-aa), destPt.y - al*Math.sin(ang-aa))
                                ctx2d.moveTo(destPt.x, destPt.y)
                                ctx2d.lineTo(destPt.x - al*Math.cos(ang+aa), destPt.y - al*Math.sin(ang+aa))
                                ctx2d.stroke()
                            }
                        }
                    }
                }
            }

            // ── z:2  Waypoint pins ───────────────────────────────────────────
            Repeater {
                model: root.waypoints
                delegate: Item {
                    property var   sp: root.worldToScreen(modelData.x, modelData.y)
                    property color pinColor: modelData.charger ? C.warn
                              : (modelData.parking ? C.blue : C.accent)
                    x: sp.x; y: sp.y
                    z: 2

                    Rectangle {
                        x: -11; y: -11
                        width: 22; height: 22; radius: 11
                        color: parent.pinColor
                        border.color: C.bg; border.width: 1.5
                    }
                    Rectangle {
                        x: -4; y: -4
                        width: 8; height: 8; radius: 4
                        color: "#ffffff"
                    }
                    Text {
                        x: 14; y: -7
                        text: modelData.name
                        font.pixelSize: 14; font.bold: true
                        color: parent.pinColor
                        style: Text.Outline; styleColor: C.bg
                    }
                }
            }

            // ── z:3  Robot marker — MQTT realtime (vizChanged ~10 Hz) ──────────
            Item {
                id: mqttMarker
                visible: root.robotOk
                z: 3
                property var sp: root.worldToScreen(root.robotX, root.robotY)
                x: sp.x; y: sp.y

                Canvas {
                    id: arrowCanvas
                    width: 36; height: 36
                    x: -18; y: -18
                    // rotation binding auto-updates when mqtt.theta changes — no requestPaint needed
                    rotation: -(mqtt.theta * 180 / Math.PI) + 90
                    Component.onCompleted: requestPaint()
                    onPaint: {
                        var c = getContext("2d")
                        c.clearRect(0, 0, width, height)
                        c.fillStyle   = "#cc2979ff"
                        c.beginPath(); c.arc(18, 18, 14, 0, Math.PI * 2); c.fill()
                        c.strokeStyle = "#ffffff"; c.lineWidth = 2
                        c.beginPath(); c.arc(18, 18, 14, 0, Math.PI * 2); c.stroke()
                        c.fillStyle = "#ffffff"
                        c.beginPath()
                        c.moveTo(18, 5)
                        c.lineTo(26, 22)
                        c.lineTo(18, 18)
                        c.lineTo(10, 22)
                        c.closePath(); c.fill()
                    }
                }
                Text {
                    x: 20; y: -7
                    text: root.mapRobots.length > 0 ? root.mapRobots[0].name : "TB3"
                    font.pixelSize: 11; font.bold: true
                    color: "#2979ff"
                    style: Text.Outline; styleColor: C.bg
                }
            }
        }
    }

    // ── Tương tác: cuộn = zoom, kéo = pan, double-click = reset ─────────────
    MouseArea {
        anchors.fill: parent
        drag.target: mapContent
        drag.threshold: 0
        acceptedButtons: Qt.LeftButton
        onWheel: (wheel) => {
            var factor = wheel.angleDelta.y > 0 ? 1.15 : (1.0 / 1.15)
            root.zoomAt(wheel.x, wheel.y, factor)
        }
        onDoubleClicked: root.resetView()
    }

    // ── Nút zoom +/−/reset ──────────────────────────────────────────────────
    Column {
        anchors.right: parent.right
        anchors.top:   parent.top
        anchors.margins: 10
        spacing: 6
        z: 10

        Repeater {
            model: [ { t: "+", a: "in" }, { t: "−", a: "out" }, { t: "⌂", a: "reset" } ]
            delegate: Rectangle {
                width: 30; height: 30; radius: 6
                color: btnMa.containsMouse ? C.surfaceAlt : C.surface
                border.color: C.border; border.width: 1
                opacity: 0.95
                Text {
                    anchors.centerIn: parent
                    text: modelData.t
                    color: C.text; font.pixelSize: 16; font.bold: true
                }
                MouseArea {
                    id: btnMa
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        if (modelData.a === "in")       root.zoomAt(root.width / 2, root.height / 2, 1.25)
                        else if (modelData.a === "out") root.zoomAt(root.width / 2, root.height / 2, 1.0 / 1.25)
                        else                            root.resetView()
                    }
                }
            }
        }
    }

    Text {
        anchors.centerIn: parent
        visible: mapImg.status !== Image.Ready
        text: "Đang tải bản đồ…"
        color: C.textDim; font.pixelSize: 13
    }
}
