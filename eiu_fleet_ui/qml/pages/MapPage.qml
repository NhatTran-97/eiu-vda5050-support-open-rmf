import QtQuick

// Map panel: map image + waypoints + lanes + robot marker, with zoom + pan.
Rectangle {
    id: root
    radius: 12
    color: "#081521"
    border.color: C.border
    border.width: 1
    clip: true

    property var  waypoints:   []
    property var  edges:       []
    property var  mapRobots:   []
    property string plannedDest: ""
    property url robotIconSource: ""
    property real robotMarkerSize: 34

    // Robot pose comes from /fleet_states, already expressed in the RMF frame —
    // the same frame as the nav graph and the map image. The MQTT visualization
    // topic carries the pose in the robot's own frame, which only coincides with
    // the RMF frame while the adapter's transform is identity, so it must not be
    // used for drawing.
    //
    // mapRobots can hold more than one robot once the fleet grows past one AGV —
    // every marker/path/destination below is drawn per-entry, not just index 0.

    property real minScale: 0.4
    property real maxScale: 8.0

    function requestAllPaint() {
        laneCanvas.requestPaint()
    }

    function scheduleGeometryPaint() {
        geometryPaintTimer.restart()
    }

    onWaypointsChanged: requestAllPaint()
    onEdgesChanged: laneCanvas.requestPaint()
    onMapRobotsChanged: laneCanvas.requestPaint()
    onPlannedDestChanged: laneCanvas.requestPaint()

    Timer {
        id: geometryPaintTimer
        interval: 50
        repeat: false
        onTriggered: root.requestAllPaint()
    }

    // World coordinates (m) -> source-map pixels. The overlay transform handles
    // fitting these stable coordinates to the currently rendered map image.
    function worldToScreen(wx, wy) {
        if (mapProv.pixelW <= 0 || mapProv.pixelH <= 0)
            return { x: -100, y: -100 }
        return {
            x: (wx - mapProv.originX) / mapProv.resolution,
            y: mapProv.pixelH - (wy - mapProv.originY) / mapProv.resolution
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

    // ── Zoomable/pannable content ────────────────────────────────────────────
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
            cache: true
            smooth: false
            onStatusChanged: if (status === Image.Ready) root.requestAllPaint()
            onPaintedWidthChanged: root.scheduleGeometryPaint()
            onPaintedHeightChanged: root.scheduleGeometryPaint()
        }

        Item {
            id: overlay
            x: mapImg.x + (mapImg.width - mapImg.paintedWidth) / 2
            y: mapImg.y + (mapImg.height - mapImg.paintedHeight) / 2
            width: Math.max(1, mapProv.pixelW)
            height: Math.max(1, mapProv.pixelH)
            transformOrigin: Item.TopLeft
            scale: displayScale
            property real displayScale: mapImg.paintedWidth > 0 && width > 0
                                        ? mapImg.paintedWidth / width : 1

            // ── z:1  Lanes + current RMF route ──────────────────────────────
            Canvas {
                id: laneCanvas
                anchors.fill: parent
                z: 1
                renderTarget: Canvas.Image
                renderStrategy: Canvas.Immediate
                onPaint: {
                    var ctx2d = getContext("2d")
                    ctx2d.clearRect(0, 0, width, height)
                    ctx2d.globalAlpha = 1.0
                    ctx2d.setLineDash([])
                    var uiScale = Math.max(0.001, overlay.displayScale)

                    function fillTriangle(cx, cy, dir, sz) {
                        ctx2d.save()
                        ctx2d.translate(cx, cy)
                        ctx2d.rotate(dir)
                        ctx2d.beginPath()
                        ctx2d.moveTo( sz,      0)
                        ctx2d.lineTo(-sz, -sz * 0.6)
                        ctx2d.lineTo(-sz,  sz * 0.6)
                        ctx2d.closePath()
                        ctx2d.fill()
                        ctx2d.restore()
                    }

                    // Static nav_graph lanes + direction arrows
                    for (var i = 0; i < root.edges.length; i++) {
                        var e  = root.edges[i]
                        var w1 = root.waypoints[e.from]
                        var w2 = root.waypoints[e.to]
                        if (!w1 || !w2) continue
                        var p1 = root.worldToScreen(w1.x, w1.y)
                        var p2 = root.worldToScreen(w2.x, w2.y)

                        ctx2d.strokeStyle = "#f5c400"
                        ctx2d.lineWidth   = 4 / uiScale
                        ctx2d.globalAlpha = 0.75
                        ctx2d.beginPath()
                        ctx2d.moveTo(p1.x, p1.y)
                        ctx2d.lineTo(p2.x, p2.y)
                        ctx2d.stroke()

                        var direction = Math.atan2(p2.y - p1.y, p2.x - p1.x)
                        ctx2d.fillStyle   = "rgba(41,121,255,0.95)"
                        ctx2d.globalAlpha = 1.0
                        if (e.bidir) {
                            fillTriangle(p1.x + (p2.x-p1.x)*0.33,
                                         p1.y + (p2.y-p1.y)*0.33,
                                         direction, 6 / uiScale)
                            fillTriangle(p1.x + (p2.x-p1.x)*0.67,
                                         p1.y + (p2.y-p1.y)*0.67,
                                         direction + Math.PI, 6 / uiScale)
                        } else {
                            fillTriangle((p1.x+p2.x)/2, (p1.y+p2.y)/2,
                                         direction, 6 / uiScale)
                        }
                    }

                    // Planned path: taken directly from RMF (robot.path = Location[]).
                    // Drawn per robot so a fleet with more than one AGV shows every
                    // route, not just the first robot in the array.
                    for (var ri = 0; ri < root.mapRobots.length; ri++) {
                        var rob = root.mapRobots[ri]
                        var rmfPath = rob.path || []

                        // Glow at the destination (plannedDest from dispatch, or the path's last point)
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
                            ctx2d.strokeStyle = "#00e676"; ctx2d.lineWidth = 2 / uiScale
                            ctx2d.globalAlpha = 0.20; ctx2d.beginPath(); ctx2d.arc(destPt.x, destPt.y, 28 / uiScale, 0, Math.PI*2); ctx2d.stroke()
                            ctx2d.globalAlpha = 0.35; ctx2d.beginPath(); ctx2d.arc(destPt.x, destPt.y, 18 / uiScale, 0, Math.PI*2); ctx2d.stroke()
                            ctx2d.globalAlpha = 0.55; ctx2d.beginPath(); ctx2d.arc(destPt.x, destPt.y, 10 / uiScale, 0, Math.PI*2); ctx2d.stroke()
                            ctx2d.fillStyle = "#00e676"; ctx2d.globalAlpha = 0.45
                            ctx2d.beginPath(); ctx2d.arc(destPt.x, destPt.y, 6 / uiScale, 0, Math.PI*2); ctx2d.fill()
                        }

                        // Draw the line along RMF's path (robot position -> each point in the path)
                        if (rmfPath.length > 0 && (rob.x !== 0 || rob.y !== 0)) {
                            var rp = root.worldToScreen(rob.x, rob.y)
                            ctx2d.strokeStyle = "#00e676"; ctx2d.lineWidth = 3 / uiScale
                            ctx2d.globalAlpha = 0.9; ctx2d.setLineDash([10 / uiScale, 5 / uiScale])
                            ctx2d.beginPath(); ctx2d.moveTo(rp.x, rp.y)
                            for (var m = 0; m < rmfPath.length; m++) {
                                var pp = root.worldToScreen(rmfPath[m].x, rmfPath[m].y)
                                ctx2d.lineTo(pp.x, pp.y)
                            }
                            ctx2d.stroke()
                            // Arrowhead at the final point
                            if (destPt) {
                                ctx2d.setLineDash([]); ctx2d.globalAlpha = 0.9
                                var prevPt = rmfPath.length >= 2
                                    ? root.worldToScreen(rmfPath[rmfPath.length-2].x, rmfPath[rmfPath.length-2].y)
                                    : rp
                                var ang = Math.atan2(destPt.y - prevPt.y, destPt.x - prevPt.x)
                                var al = 12 / uiScale, aa = 0.45
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
                    property color pinColor: modelData.charger ? "#F39C12"
                              : (modelData.parking ? "#2980B9" : "#27AE60")
                    x: sp.x; y: sp.y
                    z: 2
                    width: 1; height: 1
                    transformOrigin: Item.TopLeft
                    scale: 1 / Math.max(0.001, overlay.displayScale)

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

            // ── z:3  Robot markers — pose from /fleet_states (RMF frame) ───────
            // One delegate per entry in mapRobots, so every robot in the fleet
            // gets its own marker instead of only the first one.
            Repeater {
                model: root.mapRobots
                delegate: Item {
                    id: robotMarker
                    z: 3
                    property var sp: root.worldToScreen(modelData.x, modelData.y)
                    x: sp.x; y: sp.y
                    width: 1; height: 1
                    transformOrigin: Item.TopLeft
                    scale: 1 / Math.max(0.001, overlay.displayScale)

                    Image {
                        id: robotIcon
                        width: root.robotMarkerSize
                        height: root.robotMarkerSize
                        x: -width / 2
                        y: -height / 2
                        source: root.robotIconSource
                        fillMode: Image.PreserveAspectFit
                        smooth: true
                        mipmap: true
                        asynchronous: true
                        // Keep the replacement icon aligned with the old arrow.
                        rotation: -(modelData.yaw * 180 / Math.PI) + 90
                    }
                    Text {
                        x: root.robotMarkerSize / 2 + 5; y: -7
                        text: modelData.name || cfg.primaryRobot
                        font.pixelSize: 11; font.bold: true
                        color: "#2979ff"
                        style: Text.Outline; styleColor: C.bg
                    }
                }
            }
        }
    }

    // ── Interaction: scroll = zoom, drag = pan, double-click = reset ────────
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

    // ── Zoom +/-/reset buttons ────────────────────────────────────────────────
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
                color: btnMa.containsMouse ? C.surfaceRaised : C.surface
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
        text: "Loading map…"
        color: C.textDim; font.pixelSize: 13
    }
}
