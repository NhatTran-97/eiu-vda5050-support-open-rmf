import QtQuick
import "../components"

// Show the map, waypoints, lanes, robots, and zoom controls.
Rectangle {
    id: root
    radius: 12
    color: "#081521"
    border.color: C.border
    border.width: 1
    clip: true

    property var  waypoints:   []
    property var  edges:       []
    property var  blockedEdgeIndices: []
    property var  mapRobots:   []
    property var  telemetry:   ({})
    property string plannedDest: ""
    property url robotIconSource: ""
    property var robotIconUrls: ({})   // robot name -> icon url; falls back to robotIconSource
    property real robotMarkerSize: 46

    function iconForRobot(name) {
        return (robotIconUrls && robotIconUrls[name]) ? robotIconUrls[name] : robotIconSource
    }

    // Scale labels with the map panel size.
    readonly property real labelScale: Math.max(0.75, Math.min(1.35, width / 900))

    FontMetrics {
        id: labelMetrics
        font.family: fontSans
        font.bold: true
        font.pixelSize: 15 * root.labelScale
    }

    // Place each waypoint label where it overlaps least.
    readonly property var labelOffsets: {
        var placed = []
        var offsets = new Array(waypoints.length)
        var order = waypoints.map(function(w, i) { return i })
                             .sort(function(a, b) { return waypoints[a].x - waypoints[b].x })
        for (var k = 0; k < order.length; k++) {
            var i = order[k]
            var w = waypoints[i]
            var grow = (plannedDest !== "" && w.name === plannedDest) ? 17 / 15 : 1
            var lw = labelMetrics.advanceWidth(w.name || "") * grow
            var lh = labelMetrics.height * grow
            var p = worldToScreen(w.x, w.y)
            var px = p.x * overlay.displayScale
            var py = p.y * overlay.displayScale
            var gap = 16 * labelScale
            var candidates = [
                { x: -lw / 2,    y: -30 * labelScale },
                { x: -lw / 2,    y: 14 * labelScale },
                { x: gap,        y: -lh / 2 },
                { x: -lw - gap,  y: -lh / 2 }
            ]
            var best = candidates[0], bestCost = Infinity
            for (var c = 0; c < candidates.length && bestCost > 0; c++) {
                var r = { l: px + candidates[c].x - 4, t: py + candidates[c].y,
                          r: px + candidates[c].x + lw + 4, b: py + candidates[c].y + lh }
                var cost = placed.reduce(function(sum, o) { return sum + overlapArea(o, r) }, 0)
                if (cost < bestCost) {
                    best = candidates[c]
                    bestCost = cost
                    best.rect = r
                }
            }
            offsets[i] = best
            placed.push(best.rect)
        }
        return offsets
    }

    function overlapArea(a, b) {
        return Math.max(0, Math.min(a.r, b.r) - Math.max(a.l, b.l))
             * Math.max(0, Math.min(a.b, b.b) - Math.max(a.t, b.t))
    }

    // Pick a pose or waypoint directly on the map.
    property string pickMode: ""
    property var    pickPoint: null   // Drag pose in world coordinates
    // Keep the selected pose visible after the drag ends.
    property var    pickedPose: null       // Confirmed pose
    property string pickedWaypoint: ""
    signal posePicked(real x, real y, real yaw)
    signal waypointPicked(string name)
    signal pickCancelled()

    function clearPickedPose() { pickedPose = null }
    function clearPickedWaypoint() { pickedWaypoint = "" }

    // Find the nearest waypoint within the pick radius.
    function nearestWaypoint(wx, wy) {
        var best = ""
        var bestDist = 0.6
        for (var i = 0; i < waypoints.length; i++) {
            var w = waypoints[i]
            var d = Math.hypot(w.x - wx, w.y - wy)
            if (d < bestDist) { bestDist = d; best = w.name }
        }
        return best
    }

    // ── nav_graph.yaml editor (see graph_editor.py) ─────────────────────────
    readonly property bool graphEditMode: {
        try { return graphEd.active } catch (e) { return false }
    }
    readonly property var graphVertices: {
        try { return JSON.parse(graphEd.verticesJson) } catch (e) { return [] }
    }
    readonly property var graphLanes: {
        try { return JSON.parse(graphEd.lanesJson) } catch (e) { return [] }
    }
    onGraphVerticesChanged: laneCanvas.requestPaint()
    onGraphLanesChanged: laneCanvas.requestPaint()

    property int _laneFromIndex: -1        // first endpoint picked for a new lane
    property var _pendingVertexPos: null   // world {x,y} awaiting the name dialog
    property int selectedGraphVertex: -1
    property var selectedGraphLane: null   // {from, to}

    function nearestGraphVertex(wx, wy) {
        var best = -1, bestDist = 0.6
        for (var i = 0; i < root.graphVertices.length; i++) {
            var v = root.graphVertices[i]
            var d = Math.hypot(v.x - wx, v.y - wy)
            if (d < bestDist) { bestDist = d; best = v.index }
        }
        return best
    }

    // Nearest graph lane whose segment passes within range of this point.
    function nearestGraphLane(wx, wy) {
        var best = null, bestDist = 0.35
        for (var i = 0; i < root.graphLanes.length; i++) {
            var l = root.graphLanes[i]
            var v1 = root.graphVertices[l.from], v2 = root.graphVertices[l.to]
            if (!v1 || !v2) continue
            var d = pointToSegmentDist(wx, wy, v1.x, v1.y, v2.x, v2.y)
            if (d < bestDist) { bestDist = d; best = l }
        }
        return best
    }

    function pointToSegmentDist(px, py, ax, ay, bx, by) {
        var dx = bx - ax, dy = by - ay
        var len2 = dx * dx + dy * dy
        var t = len2 > 0 ? Math.max(0, Math.min(1, ((px - ax) * dx + (py - ay) * dy) / len2)) : 0
        var cx = ax + t * dx, cy = ay + t * dy
        return Math.hypot(px - cx, py - cy)
    }

    function cancelLanePick() { root._laneFromIndex = -1 }

    // Find an RMF waypoint by name.
    function waypointByName(name) {
        for (var i = 0; i < waypoints.length; i++) {
            if (waypoints[i].name === name) return waypoints[i]
        }
        return null
    }

    property real minScale: 0.4
    property real maxScale: 8.0

    // No-go zones: each closes the lanes it overlaps via RMF's lane_closure_requests.
    property var zoneRect: null   // {x1,y1,x2,y2} in world coordinates, while dragging
    property var closedZones: []  // [{id, x1,y1,x2,y2, laneIndices}]
    property int selectedZoneId: -1
    property int _nextZoneId: 1

    readonly property var liveClosedRaw: {
        try { return JSON.parse(ros.closedLaneIndicesJson) } catch (e) { return [] }
    }

    // Match zone markers to RMF's current raw lane closures.
    function syncZonesFromLive() {
        var covered = {}
        for (var i = 0; i < root.closedZones.length; i++) {
            var li = root.closedZones[i].laneIndices
            for (var j = 0; j < li.length; j++) covered[li[j]] = true
        }
        var kept = root.closedZones.filter(function(z) {
            return z.laneIndices.some(function(r) { return root.liveClosedRaw.indexOf(r) >= 0 })
        })
        var added = []
        for (var k = 0; k < root.edges.length; k++) {
            var e = root.edges[k]
            var closedHere = e.raw.filter(function(r) { return root.liveClosedRaw.indexOf(r) >= 0 })
            if (closedHere.length === 0 || e.raw.some(function(r) { return covered[r] })) continue
            var w1 = root.waypoints[e.from], w2 = root.waypoints[e.to]
            if (!w1 || !w2) continue
            var mx = (w1.x + w2.x) / 2, my = (w1.y + w2.y) / 2
            var hw = Math.max(Math.abs(w2.x - w1.x) / 2, 0.25)
            var hh = Math.max(Math.abs(w2.y - w1.y) / 2, 0.25)
            added.push({ id: root._nextZoneId++, x1: mx - hw, y1: my - hh, x2: mx + hw, y2: my + hh,
                        laneIndices: closedHere })
        }
        if (kept.length !== root.closedZones.length || added.length > 0)
            root.closedZones = kept.concat(added)
    }
    onLiveClosedRawChanged: syncZonesFromLive()

    function segmentsIntersect(ax1, ay1, ax2, ay2, bx1, by1, bx2, by2) {
        function cross(ox, oy, ax, ay, bx, by) {
            return (ax - ox) * (by - oy) - (ay - oy) * (bx - ox)
        }
        var d1 = cross(bx1, by1, bx2, by2, ax1, ay1)
        var d2 = cross(bx1, by1, bx2, by2, ax2, ay2)
        var d3 = cross(ax1, ay1, ax2, ay2, bx1, by1)
        var d4 = cross(ax1, ay1, ax2, ay2, bx2, by2)
        return ((d1 > 0 && d2 < 0) || (d1 < 0 && d2 > 0))
            && ((d3 > 0 && d4 < 0) || (d3 < 0 && d4 > 0))
    }

    // Include lane endpoints and boundary crossings when testing a zone.
    function segmentIntersectsRect(x1, y1, x2, y2, rx1, ry1, rx2, ry2) {
        var minX = Math.min(rx1, rx2), maxX = Math.max(rx1, rx2)
        var minY = Math.min(ry1, ry2), maxY = Math.max(ry1, ry2)
        if ((x1 >= minX && x1 <= maxX && y1 >= minY && y1 <= maxY)
                || (x2 >= minX && x2 <= maxX && y2 >= minY && y2 <= maxY))
            return true
        return segmentsIntersect(x1, y1, x2, y2, minX, minY, maxX, minY)
            || segmentsIntersect(x1, y1, x2, y2, maxX, minY, maxX, maxY)
            || segmentsIntersect(x1, y1, x2, y2, maxX, maxY, minX, maxY)
            || segmentsIntersect(x1, y1, x2, y2, minX, maxY, minX, minY)
    }

    // Close every raw lane index intersecting the drawn zone.
    function finishZoneDraw() {
        if (!root.zoneRect) return
        var r = root.zoneRect
        var laneIndices = []
        for (var i = 0; i < root.edges.length; i++) {
            var e = root.edges[i]
            var w1 = root.waypoints[e.from], w2 = root.waypoints[e.to]
            if (w1 && w2 && root.segmentIntersectsRect(w1.x, w1.y, w2.x, w2.y, r.x1, r.y1, r.x2, r.y2))
                laneIndices = laneIndices.concat(e.raw)
        }
        if (laneIndices.length === 0) return
        root.closedZones = root.closedZones.concat([{
            id: root._nextZoneId++, x1: r.x1, y1: r.y1, x2: r.x2, y2: r.y2,
            laneIndices: laneIndices
        }])
        ros.closeLanes(JSON.stringify(laneIndices))
    }

    // Reopen a zone's lanes and remove it.
    function deleteZone(id) {
        var kept = [], removed = null
        for (var i = 0; i < root.closedZones.length; i++) {
            if (root.closedZones[i].id === id) removed = root.closedZones[i]
            else kept.push(root.closedZones[i])
        }
        if (!removed) return
        ros.openLanes(JSON.stringify(removed.laneIndices))
        root.closedZones = kept
        if (root.selectedZoneId === id) root.selectedZoneId = -1
    }

    // Topmost zone containing this world point, or null.
    function zoneAt(wx, wy) {
        for (var i = root.closedZones.length - 1; i >= 0; i--) {
            var z = root.closedZones[i]
            var minX = Math.min(z.x1, z.x2), maxX = Math.max(z.x1, z.x2)
            var minY = Math.min(z.y1, z.y2), maxY = Math.max(z.y1, z.y2)
            if (wx >= minX && wx <= maxX && wy >= minY && wy <= maxY) return z
        }
        return null
    }

    function requestAllPaint() {
        laneCanvas.requestPaint()
    }

    function scheduleGeometryPaint() {
        geometryPaintTimer.restart()
    }

    // Repaint routes only when their released-node state changes.
    readonly property string releasedSignature: {
        var names = Object.keys(telemetry).sort()
        var sig = ""
        for (var i = 0; i < names.length; i++) {
            var nodeStates = (telemetry[names[i]] && telemetry[names[i]].node_states) || []
            sig += names[i] + ":"
            for (var j = 0; j < nodeStates.length; j++)
                sig += nodeStates[j].nodeId + (nodeStates[j].released ? "1" : "0")
            sig += "|"
        }
        return sig
    }

    onWaypointsChanged: requestAllPaint()
    onEdgesChanged: laneCanvas.requestPaint()
    onBlockedEdgeIndicesChanged: laneCanvas.requestPaint()
    onMapRobotsChanged: laneCanvas.requestPaint()
    onReleasedSignatureChanged: laneCanvas.requestPaint()
    onPlannedDestChanged: laneCanvas.requestPaint()
    onPickPointChanged: laneCanvas.requestPaint()
    onPickedPoseChanged: laneCanvas.requestPaint()
    onPickedWaypointChanged: laneCanvas.requestPaint()
    onZoneRectChanged: laneCanvas.requestPaint()
    onClosedZonesChanged: laneCanvas.requestPaint()
    onSelectedZoneIdChanged: laneCanvas.requestPaint()

    Timer {
        id: geometryPaintTimer
        interval: 50
        repeat: false
        onTriggered: root.requestAllPaint()
    }

    // Convert world coordinates to source-map pixels.
    function worldToScreen(wx, wy) {
        if (mapProv.pixelW <= 0 || mapProv.pixelH <= 0)
            return { x: -100, y: -100 }
        return {
            x: (wx - mapProv.originX) / mapProv.resolution,
            y: mapProv.pixelH - (wy - mapProv.originY) / mapProv.resolution
        }
    }

    // Convert overlay coordinates back to world coordinates.
    function screenToWorld(ox, oy) {
        return {
            x: ox * mapProv.resolution + mapProv.originX,
            y: (mapProv.pixelH - oy) * mapProv.resolution + mapProv.originY
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

    // Map content with zoom and pan.
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
            // Dim the map image so routes and robots stay clear.
            opacity: 0.5
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

            // Draw lanes and the current RMF routes.
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

                    function drawZoneRect(x1, y1, x2, y2, selected) {
                        var p1 = root.worldToScreen(x1, y1)
                        var p2 = root.worldToScreen(x2, y2)
                        var rx = Math.min(p1.x, p2.x), ry = Math.min(p1.y, p2.y)
                        var rw = Math.abs(p2.x - p1.x), rh = Math.abs(p2.y - p1.y)
                        ctx2d.setLineDash(selected ? [] : [8 / uiScale, 5 / uiScale])
                        ctx2d.fillStyle = "#F05265"; ctx2d.globalAlpha = selected ? 0.28 : 0.16
                        ctx2d.fillRect(rx, ry, rw, rh)
                        ctx2d.strokeStyle = "#F05265"; ctx2d.globalAlpha = selected ? 1.0 : 0.7
                        ctx2d.lineWidth = (selected ? 3 : 2) / uiScale
                        ctx2d.strokeRect(rx, ry, rw, rh)
                    }

                    // No-go zones: confirmed ones first, then the one still being dragged.
                    for (var zi = 0; zi < root.closedZones.length; zi++) {
                        var z = root.closedZones[zi]
                        drawZoneRect(z.x1, z.y1, z.x2, z.y2, z.id === root.selectedZoneId)
                    }
                    if (root.zoneRect)
                        drawZoneRect(root.zoneRect.x1, root.zoneRect.y1,
                                    root.zoneRect.x2, root.zoneRect.y2, false)
                    ctx2d.setLineDash([]); ctx2d.globalAlpha = 1.0

                    // Draw navigation lanes with direction arrows.
                    for (var i = 0; i < root.edges.length; i++) {
                        var e  = root.edges[i]
                        var w1 = root.waypoints[e.from]
                        var w2 = root.waypoints[e.to]
                        if (!w1 || !w2) continue
                        var p1 = root.worldToScreen(w1.x, w1.y)
                        var p2 = root.worldToScreen(w2.x, w2.y)
                        // Blocked indices refer to deduplicated edges.
                        var blocked = root.blockedEdgeIndices.indexOf(i) >= 0

                        ctx2d.strokeStyle = blocked ? "#F05265" : "#f5c400"
                        ctx2d.lineWidth   = (blocked ? 6 : 4) / uiScale
                        ctx2d.globalAlpha = blocked ? 0.9 : 0.75
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

                    // Draw the graph editor's working copy (cyan, on top of the live graph).
                    if (root.graphEditMode) {
                        ctx2d.setLineDash([])
                        for (var gi = 0; gi < root.graphLanes.length; gi++) {
                            var gl = root.graphLanes[gi]
                            var gv1 = root.graphVertices[gl.from], gv2 = root.graphVertices[gl.to]
                            if (!gv1 || !gv2) continue
                            var gp1 = root.worldToScreen(gv1.x, gv1.y)
                            var gp2 = root.worldToScreen(gv2.x, gv2.y)
                            var isSelLane = root.selectedGraphLane
                                && ((root.selectedGraphLane.from === gl.from && root.selectedGraphLane.to === gl.to)
                                    || (root.selectedGraphLane.from === gl.to && root.selectedGraphLane.to === gl.from))
                            ctx2d.strokeStyle = isSelLane ? "#ffffff" : "#00D9FF"
                            ctx2d.lineWidth = (isSelLane ? 5 : 3) / uiScale
                            ctx2d.globalAlpha = 0.9
                            ctx2d.beginPath()
                            ctx2d.moveTo(gp1.x, gp1.y)
                            ctx2d.lineTo(gp2.x, gp2.y)
                            ctx2d.stroke()
                            var gdir = Math.atan2(gp2.y - gp1.y, gp2.x - gp1.x)
                            ctx2d.fillStyle = "#00D9FF"
                            if (gl.bidir) {
                                fillTriangle(gp1.x + (gp2.x-gp1.x)*0.33, gp1.y + (gp2.y-gp1.y)*0.33, gdir, 6 / uiScale)
                                fillTriangle(gp1.x + (gp2.x-gp1.x)*0.67, gp1.y + (gp2.y-gp1.y)*0.67, gdir + Math.PI, 6 / uiScale)
                            } else {
                                fillTriangle((gp1.x+gp2.x)/2, (gp1.y+gp2.y)/2, gdir, 6 / uiScale)
                            }
                        }

                        // Lane being connected: highlight the first endpoint.
                        if (root._laneFromIndex >= 0 && root.graphVertices[root._laneFromIndex]) {
                            var fv = root.graphVertices[root._laneFromIndex]
                            var fp = root.worldToScreen(fv.x, fv.y)
                            ctx2d.strokeStyle = "#00D9FF"; ctx2d.globalAlpha = 0.9; ctx2d.lineWidth = 2 / uiScale
                            ctx2d.beginPath(); ctx2d.arc(fp.x, fp.y, 16 / uiScale, 0, Math.PI*2); ctx2d.stroke()
                        }

                        for (var vi2 = 0; vi2 < root.graphVertices.length; vi2++) {
                            var gv = root.graphVertices[vi2]
                            var vp = root.worldToScreen(gv.x, gv.y)
                            var selected = gv.index === root.selectedGraphVertex
                            ctx2d.globalAlpha = 1.0
                            ctx2d.fillStyle = gv.is_charger ? "#F39C12" : "#00D9FF"
                            ctx2d.beginPath(); ctx2d.arc(vp.x, vp.y, 8 / uiScale, 0, Math.PI*2); ctx2d.fill()
                            ctx2d.strokeStyle = selected ? "#ffffff" : "#04202b"
                            ctx2d.lineWidth = (selected ? 3 : 1.5) / uiScale
                            ctx2d.stroke()
                            ctx2d.fillStyle = "#ffffff"
                            ctx2d.font = (12 / uiScale) + "px sans-serif"
                            ctx2d.textAlign = "center"
                            ctx2d.fillText(gv.name || ("#" + gv.index), vp.x, vp.y - 14 / uiScale)
                        }
                    }

                    // Draw a planned path for each robot.
                    for (var ri = 0; ri < root.mapRobots.length; ri++) {
                        var rob = root.mapRobots[ri]
                        var rmfPath = rob.path || []

                        // Mark the destination of an active path.
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

                        // Draw released path segments more strongly than planned segments.
                        if (rmfPath.length > 0 && (rob.x !== 0 || rob.y !== 0)) {
                            var rp = root.worldToScreen(rob.x, rob.y)
                            var tele = root.telemetry[rob.name]
                            var nodeStates = (tele && tele.node_states) || []
                            ctx2d.strokeStyle = "#00e676"

                            if (nodeStates.length > 0) {
                                var prevScreen = rp
                                for (var ns = 0; ns < nodeStates.length; ns++) {
                                    var wp = root.waypointByName(nodeStates[ns].nodeId)
                                    if (!wp) continue
                                    var segPt = root.worldToScreen(wp.x, wp.y)
                                    if (nodeStates[ns].released) {
                                        ctx2d.lineWidth = 3 / uiScale; ctx2d.globalAlpha = 0.9
                                        ctx2d.setLineDash([10 / uiScale, 5 / uiScale])
                                    } else {
                                        ctx2d.lineWidth = 2 / uiScale; ctx2d.globalAlpha = 0.4
                                        ctx2d.setLineDash([3 / uiScale, 6 / uiScale])
                                    }
                                    ctx2d.beginPath()
                                    ctx2d.moveTo(prevScreen.x, prevScreen.y)
                                    ctx2d.lineTo(segPt.x, segPt.y)
                                    ctx2d.stroke()
                                    prevScreen = segPt
                                }
                            } else {
                                ctx2d.lineWidth = 3 / uiScale
                                ctx2d.globalAlpha = 0.9; ctx2d.setLineDash([10 / uiScale, 5 / uiScale])
                                ctx2d.beginPath(); ctx2d.moveTo(rp.x, rp.y)
                                for (var m = 0; m < rmfPath.length; m++) {
                                    var pp = root.worldToScreen(rmfPath[m].x, rmfPath[m].y)
                                    ctx2d.lineTo(pp.x, pp.y)
                                }
                                ctx2d.stroke()
                            }
                            // Arrowhead at the end of the path.
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

                    // Preview the pose and heading while dragging.
                    if (root.pickPoint) {
                        var pk = root.worldToScreen(root.pickPoint.x, root.pickPoint.y)
                        var screenYaw = -root.pickPoint.yaw
                        ctx2d.setLineDash([])
                        ctx2d.fillStyle = "#ffffff"; ctx2d.globalAlpha = 0.9
                        ctx2d.beginPath(); ctx2d.arc(pk.x, pk.y, 6 / uiScale, 0, Math.PI*2); ctx2d.fill()
                        ctx2d.strokeStyle = "#ffffff"; ctx2d.lineWidth = 2 / uiScale
                        var arm = 22 / uiScale
                        var tipX = pk.x + arm * Math.cos(screenYaw)
                        var tipY = pk.y + arm * Math.sin(screenYaw)
                        ctx2d.beginPath()
                        ctx2d.moveTo(pk.x, pk.y)
                        ctx2d.lineTo(tipX, tipY)
                        ctx2d.stroke()
                        fillTriangle(tipX, tipY, screenYaw, 7 / uiScale)
                    }

                    // Mark the confirmed pose for robot repositioning.
                    if (root.pickedPose) {
                        var cp = root.worldToScreen(root.pickedPose.x, root.pickedPose.y)
                        var cpYaw = -root.pickedPose.yaw
                        ctx2d.setLineDash([])
                        ctx2d.strokeStyle = "#FF3DAE"; ctx2d.globalAlpha = 0.5
                        ctx2d.beginPath(); ctx2d.arc(cp.x, cp.y, 14 / uiScale, 0, Math.PI*2); ctx2d.stroke()
                        ctx2d.fillStyle = "#FF3DAE"; ctx2d.globalAlpha = 1.0
                        ctx2d.beginPath(); ctx2d.arc(cp.x, cp.y, 6 / uiScale, 0, Math.PI*2); ctx2d.fill()
                        ctx2d.strokeStyle = "#FF3DAE"; ctx2d.lineWidth = 2.5 / uiScale
                        var cArm = 26 / uiScale
                        var cTipX = cp.x + cArm * Math.cos(cpYaw)
                        var cTipY = cp.y + cArm * Math.sin(cpYaw)
                        ctx2d.beginPath()
                        ctx2d.moveTo(cp.x, cp.y)
                        ctx2d.lineTo(cTipX, cTipY)
                        ctx2d.stroke()
                        fillTriangle(cTipX, cTipY, cpYaw, 8 / uiScale)
                    }
                }
            }

            // Draw waypoint pins.
            Repeater {
                model: root.waypoints
                delegate: Item {
                    property var   sp: root.worldToScreen(modelData.x, modelData.y)
                    property color pinColor: modelData.charger ? "#F39C12"
                              : (modelData.parking ? "#2980B9" : "#27AE60")
                    property bool  picked: modelData.name === root.pickedWaypoint
                    property bool  hasTarget: root.plannedDest !== ""
                    property bool  isTaskTarget: hasTarget && modelData.name === root.plannedDest
                    // Dim unrelated waypoints when a destination is active.
                    property bool  emphasized: !hasTarget || isTaskTarget
                    x: sp.x; y: sp.y
                    z: 2
                    width: 1; height: 1
                    transformOrigin: Item.TopLeft
                    scale: 1 / Math.max(0.001, overlay.displayScale)

                    // Highlight the waypoint selected on the map.
                    Rectangle {
                        visible: parent.picked
                        x: -18; y: -18
                        width: 36; height: 36; radius: 18
                        color: "transparent"
                        border.color: "#FF3DAE"; border.width: 3
                    }
                    Rectangle {
                        x: -11; y: -11
                        width: 22; height: 22; radius: 11
                        color: parent.pinColor
                        border.color: parent.picked ? "#FF3DAE" : C.bg
                        border.width: parent.picked ? 2.5 : 1.5
                    }
                    Rectangle {
                        x: -4; y: -4
                        width: 8; height: 8; radius: 4
                        color: "#ffffff"
                    }
                    Text {
                        // Label only chargers/target/picked when zoomed out -- full labels clutter a large graph.
                        visible: mapContent.scale >= 1.3 || parent.picked || parent.isTaskTarget
                                 || modelData.charger || modelData.parking
                        // Move labels that overlap their default position.
                        x: root.labelOffsets[index] ? root.labelOffsets[index].x : -width / 2
                        y: root.labelOffsets[index] ? root.labelOffsets[index].y : -30 * root.labelScale
                        horizontalAlignment: Text.AlignHCenter
                        text: modelData.name
                        font.pixelSize: (parent.isTaskTarget ? 17 : 15) * root.labelScale
                        font.bold: true
                        color: parent.emphasized ? "#EAF4FF" : C.textDim
                        opacity: parent.emphasized ? 1.0 : 0.65
                        style: Text.Outline; styleColor: C.bg
                    }
                }
            }

            // Draw one marker per robot in /fleet_states.
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

                    // Distinguish robots from waypoint pins.
                    Rectangle {
                        id: pulseRing
                        x: -width / 2; y: -height / 2
                        width: root.robotMarkerSize * 1.7
                        height: width
                        radius: width / 2
                        color: "transparent"
                        border.color: "#2979ff"
                        border.width: 2
                        opacity: 0.7
                        scale: 0.7

                        SequentialAnimation on scale {
                            loops: Animation.Infinite
                            NumberAnimation { from: 0.7; to: 1.25; duration: 1200; easing.type: Easing.OutCubic }
                            PauseAnimation { duration: 200 }
                        }
                        SequentialAnimation on opacity {
                            loops: Animation.Infinite
                            NumberAnimation { from: 0.7; to: 0.0; duration: 1200; easing.type: Easing.OutCubic }
                            PauseAnimation { duration: 200 }
                        }
                    }

                    Image {
                        id: robotIcon
                        width: root.robotMarkerSize
                        height: root.robotMarkerSize
                        x: -width / 2
                        y: -height / 2
                        source: root.iconForRobot(modelData.name)
                        fillMode: Image.PreserveAspectFit
                        smooth: true
                        mipmap: true
                        asynchronous: true
                        // Center the robot icon on its marker.
                        rotation: -(modelData.yaw * 180 / Math.PI) + 90
                    }
                    Rectangle {
                        visible: modelData.status === "WAITING"
                        anchors.horizontalCenter: parent.horizontalCenter
                        y: -root.robotMarkerSize / 2 - 22 * root.labelScale
                        width: waitText.implicitWidth + 12
                        height: 18 * root.labelScale
                        radius: height / 2
                        color: "#5A4A26"
                        border.color: "#F3AE3D"; border.width: 1
                        Text {
                            id: waitText
                            anchors.centerIn: parent
                            text: "WAIT"
                            color: "#F3AE3D"
                            font.pixelSize: 10 * root.labelScale
                            font.bold: true
                        }
                    }
                    Text {
                        anchors.horizontalCenter: parent.horizontalCenter
                        y: root.robotMarkerSize / 2 + 6
                        horizontalAlignment: Text.AlignHCenter
                        text: modelData.name || cfg.primaryRobot
                        font.pixelSize: 15 * root.labelScale; font.bold: true
                        color: "#6FB2FF"
                        style: Text.Outline; styleColor: C.bg
                    }
                }
            }
        }
    }

    // Scroll to zoom and drag to pan; in pick mode, dragging sets the heading.
    MouseArea {
        id: interactionArea
        anchors.fill: parent
        drag.target: root.pickMode === "" ? mapContent : null
        drag.threshold: 0
        acceptedButtons: Qt.LeftButton
        cursorShape: root.pickMode !== "" ? Qt.CrossCursor : Qt.ArrowCursor
        onWheel: (wheel) => {
            var factor = wheel.angleDelta.y > 0 ? 1.15 : (1.0 / 1.15)
            root.zoomAt(wheel.x, wheel.y, factor)
        }
        onDoubleClicked: if (root.pickMode === "") root.resetView()
        onClicked: (mouse) => {
            var op = overlay.mapFromItem(interactionArea, mouse.x, mouse.y)
            var wp = root.screenToWorld(op.x, op.y)

            if (root.pickMode === "graph_vertex") {
                root._pendingVertexPos = wp
                graphVertexDialog.editIndex = -1
                graphVertexDialog.open()
                return
            }
            if (root.pickMode === "graph_lane") {
                var vi = root.nearestGraphVertex(wp.x, wp.y)
                if (vi < 0) return
                if (root._laneFromIndex < 0) {
                    root._laneFromIndex = vi
                } else if (vi !== root._laneFromIndex) {
                    graphLaneDialog.fromIndex = root._laneFromIndex
                    graphLaneDialog.toIndex = vi
                    graphLaneDialog.fromName = root.graphVertices[root._laneFromIndex].name || ("#" + root._laneFromIndex)
                    graphLaneDialog.toName = root.graphVertices[vi].name || ("#" + vi)
                    graphLaneDialog.open()
                    root._laneFromIndex = -1
                }
                return
            }

            if (root.pickMode !== "") return

            if (root.graphEditMode) {
                var gv = root.nearestGraphVertex(wp.x, wp.y)
                if (gv >= 0) {
                    root.selectedGraphVertex = gv
                    root.selectedGraphLane = null
                    return
                }
                var gl = root.nearestGraphLane(wp.x, wp.y)
                if (gl) {
                    root.selectedGraphLane = gl
                    root.selectedGraphVertex = -1
                    return
                }
                root.selectedGraphVertex = -1
                root.selectedGraphLane = null
            }

            var hit = root.zoneAt(wp.x, wp.y)
            root.selectedZoneId = hit ? hit.id : -1
        }

        property var pickStart: null
        onPressed: (mouse) => {
            if (root.pickMode === "" || root.pickMode === "graph_vertex" || root.pickMode === "graph_lane") return
            var op = overlay.mapFromItem(interactionArea, mouse.x, mouse.y)
            pickStart = root.screenToWorld(op.x, op.y)
            if (root.pickMode === "zone")
                root.zoneRect = { x1: pickStart.x, y1: pickStart.y, x2: pickStart.x, y2: pickStart.y }
            else
                root.pickPoint = { x: pickStart.x, y: pickStart.y, yaw: 0 }
        }
        onPositionChanged: (mouse) => {
            if (root.pickMode === "" || !pickStart) return
            var op = overlay.mapFromItem(interactionArea, mouse.x, mouse.y)
            var cur = root.screenToWorld(op.x, op.y)
            if (root.pickMode === "zone") {
                root.zoneRect = { x1: pickStart.x, y1: pickStart.y, x2: cur.x, y2: cur.y }
                return
            }
            var yaw = Math.hypot(cur.x - pickStart.x, cur.y - pickStart.y) > 0.05
                      ? Math.atan2(cur.y - pickStart.y, cur.x - pickStart.x)
                      : root.pickPoint.yaw
            root.pickPoint = { x: pickStart.x, y: pickStart.y, yaw: yaw }
        }
        onReleased: (mouse) => {
            if (root.pickMode === "" || !pickStart) return
            if (root.pickMode === "zone") {
                root.finishZoneDraw()
            } else if (root.pickMode === "waypoint") {
                var name = root.nearestWaypoint(pickStart.x, pickStart.y)
                if (name !== "") {
                    root.pickedWaypoint = name
                    root.waypointPicked(name)
                }
            } else {
                root.pickedPose = { x: pickStart.x, y: pickStart.y, yaw: root.pickPoint.yaw }
                root.posePicked(pickStart.x, pickStart.y, root.pickPoint.yaw)
            }
            pickStart = null
            root.pickPoint = null
            root.zoneRect = null
            root.pickMode = ""
        }
    }

    // Pick-mode hint and cancel button.
    Rectangle {
        visible: root.pickMode !== ""
        anchors.top: parent.top
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.topMargin: 10
        z: 20
        radius: 10
        color: C.surfaceRaised
        border.color: C.accent
        border.width: 1
        implicitWidth: hintRow.implicitWidth + 20
        implicitHeight: hintRow.implicitHeight + 14

        Row {
            id: hintRow
            anchors.centerIn: parent
            spacing: 12
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: root.pickMode === "waypoint"
                      ? "Click a waypoint pin to select it"
                      : root.pickMode === "zone"
                      ? "Click and drag to close the lanes inside a zone"
                      : root.pickMode === "graph_vertex"
                      ? "Click the map to place a new waypoint"
                      : root.pickMode === "graph_lane"
                      ? (root._laneFromIndex < 0
                         ? "Click the first waypoint of the new lane"
                         : "Click the second waypoint to connect it")
                      : "Click and drag to set position + heading"
                color: C.text
                font.pixelSize: 12
            }
            Text {
                anchors.verticalCenter: parent.verticalCenter
                text: "✕ Cancel"
                color: C.err
                font.pixelSize: 12
                font.bold: true
                MouseArea {
                    anchors.fill: parent
                    anchors.margins: -4
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        root.pickMode = ""
                        root.pickPoint = null
                        root.zoneRect = null
                        root._laneFromIndex = -1
                        root.pickCancelled()
                    }
                }
            }
        }
    }

    // No-go zone controls: draw one, or delete the selected one.
    Row {
        anchors.left: parent.left
        anchors.top:  parent.top
        anchors.margins: 10
        spacing: 6
        z: 10

        Rectangle {
            width: zoneBtnRow.implicitWidth + 16; height: 30; radius: 6
            color: root.pickMode === "zone" ? C.accent
                   : (zoneBtnMa.containsMouse ? C.surfaceRaised : C.surface)
            border.color: C.border; border.width: 1
            opacity: 0.95
            Row {
                id: zoneBtnRow
                anchors.centerIn: parent
                spacing: 6
                Text {
                    text: "🚫"; font.pixelSize: 13
                    anchors.verticalCenter: parent.verticalCenter
                }
                Text {
                    text: "NO-GO ZONE"
                    color: root.pickMode === "zone" ? "#ffffff" : C.text
                    font.pixelSize: 11; font.bold: true
                    anchors.verticalCenter: parent.verticalCenter
                }
            }
            MouseArea {
                id: zoneBtnMa
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: root.pickMode = (root.pickMode === "zone") ? "" : "zone"
            }
        }

        Rectangle {
            visible: root.selectedZoneId >= 0
            width: deleteZoneRow.implicitWidth + 16; height: 30; radius: 6
            color: deleteZoneMa.containsMouse ? Qt.darker(C.err, 1.15) : C.err
            opacity: 0.95
            Row {
                id: deleteZoneRow
                anchors.centerIn: parent
                spacing: 6
                Text { text: "🗑"; font.pixelSize: 13; anchors.verticalCenter: parent.verticalCenter }
                Text {
                    text: "DELETE ZONE"
                    color: "#ffffff"; font.pixelSize: 11; font.bold: true
                    anchors.verticalCenter: parent.verticalCenter
                }
            }
            MouseArea {
                id: deleteZoneMa
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: root.deleteZone(root.selectedZoneId)
            }
        }
    }

    // ── nav_graph.yaml editor controls ──────────────────────────────────────
    property string graphStatusText: ""
    Connections {
        target: graphEd
        function onSaveResult(ok, message) { statusTimer.show(message) }
        function onLoadResult(ok, message) { statusTimer.show(message) }
    }
    Timer {
        id: statusTimer
        interval: 4000; repeat: false
        function show(msg) { root.graphStatusText = msg; restart() }
        onTriggered: root.graphStatusText = ""
    }

    Row {
        anchors.left: parent.left
        anchors.top:  parent.top
        anchors.margins: 10
        anchors.topMargin: 46
        spacing: 6
        z: 10

        Rectangle {
            width: editGraphRow.implicitWidth + 16; height: 30; radius: 6
            color: root.graphEditMode ? C.accent : (editGraphMa.containsMouse ? C.surfaceRaised : C.surface)
            border.color: C.border; border.width: 1
            opacity: 0.95
            Row {
                id: editGraphRow
                anchors.centerIn: parent
                spacing: 6
                Text { text: "🛠"; font.pixelSize: 13; anchors.verticalCenter: parent.verticalCenter }
                Text {
                    text: "EDIT GRAPH"
                    color: root.graphEditMode ? "#ffffff" : C.text
                    font.pixelSize: 11; font.bold: true
                    anchors.verticalCenter: parent.verticalCenter
                }
            }
            MouseArea {
                id: editGraphMa
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: {
                    if (root.graphEditMode) {
                        graphEd.stopEditing()
                        root.pickMode = ""; root._laneFromIndex = -1
                        root.selectedGraphVertex = -1; root.selectedGraphLane = null
                    } else {
                        graphEd.loadFromFile(mapProv.navGraphPath, "")
                    }
                }
            }
        }

        Rectangle {
            visible: root.graphEditMode
            width: newGraphRow.implicitWidth + 16; height: 30; radius: 6
            color: newGraphMa.containsMouse ? C.surfaceRaised : C.surface
            border.color: C.border; border.width: 1
            opacity: 0.95
            Row {
                id: newGraphRow
                anchors.centerIn: parent
                spacing: 6
                Text { text: "＋"; font.pixelSize: 13; anchors.verticalCenter: parent.verticalCenter }
                Text { text: "NEW"; color: C.text; font.pixelSize: 11; font.bold: true; anchors.verticalCenter: parent.verticalCenter }
            }
            MouseArea {
                id: newGraphMa
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: graphEd.newGraph("")
            }
        }

        Rectangle {
            visible: root.graphEditMode
            width: reloadGraphRow.implicitWidth + 16; height: 30; radius: 6
            color: reloadGraphMa.containsMouse ? C.surfaceRaised : C.surface
            border.color: C.border; border.width: 1
            opacity: 0.95
            Row {
                id: reloadGraphRow
                anchors.centerIn: parent
                spacing: 6
                Text { text: "↺"; font.pixelSize: 14; anchors.verticalCenter: parent.verticalCenter }
                Text { text: "RELOAD"; color: C.text; font.pixelSize: 11; font.bold: true; anchors.verticalCenter: parent.verticalCenter }
            }
            MouseArea {
                id: reloadGraphMa
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                // Discard every unsaved edit this session and reload the file from
                // disk -- the file itself is only touched by SAVE AS, so this is a
                // clean "undo everything, back to default" as long as you haven't saved.
                onClicked: {
                    root.pickMode = ""; root._laneFromIndex = -1
                    root.selectedGraphVertex = -1; root.selectedGraphLane = null
                    graphEd.loadFromFile(graphEd.sourcePath || mapProv.navGraphPath, "")
                }
            }
        }

        Rectangle {
            visible: root.graphEditMode
            width: addVertexRow.implicitWidth + 16; height: 30; radius: 6
            color: root.pickMode === "graph_vertex" ? C.accent : (addVertexMa.containsMouse ? C.surfaceRaised : C.surface)
            border.color: C.border; border.width: 1
            opacity: 0.95
            Row {
                id: addVertexRow
                anchors.centerIn: parent
                spacing: 6
                Text { text: "📍"; font.pixelSize: 12; anchors.verticalCenter: parent.verticalCenter }
                Text {
                    text: "WAYPOINT"
                    color: root.pickMode === "graph_vertex" ? "#ffffff" : C.text
                    font.pixelSize: 11; font.bold: true
                    anchors.verticalCenter: parent.verticalCenter
                }
            }
            MouseArea {
                id: addVertexMa
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: {
                    root._laneFromIndex = -1
                    root.pickMode = (root.pickMode === "graph_vertex") ? "" : "graph_vertex"
                }
            }
        }

        Rectangle {
            visible: root.graphEditMode
            width: addLaneRow.implicitWidth + 16; height: 30; radius: 6
            color: root.pickMode === "graph_lane" ? C.accent : (addLaneMa.containsMouse ? C.surfaceRaised : C.surface)
            border.color: C.border; border.width: 1
            opacity: 0.95
            Row {
                id: addLaneRow
                anchors.centerIn: parent
                spacing: 6
                Text { text: "↔"; font.pixelSize: 13; anchors.verticalCenter: parent.verticalCenter }
                Text {
                    text: "LANE"
                    color: root.pickMode === "graph_lane" ? "#ffffff" : C.text
                    font.pixelSize: 11; font.bold: true
                    anchors.verticalCenter: parent.verticalCenter
                }
            }
            MouseArea {
                id: addLaneMa
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: {
                    root._laneFromIndex = -1
                    root.pickMode = (root.pickMode === "graph_lane") ? "" : "graph_lane"
                }
            }
        }

        Rectangle {
            visible: root.graphEditMode
            width: saveGraphRow.implicitWidth + 16; height: 30; radius: 6
            color: saveGraphMa.containsMouse ? C.surfaceRaised : C.surface
            border.color: C.border; border.width: 1
            opacity: 0.95
            Row {
                id: saveGraphRow
                anchors.centerIn: parent
                spacing: 6
                Text { text: "💾"; font.pixelSize: 12; anchors.verticalCenter: parent.verticalCenter }
                Text { text: "SAVE AS"; color: C.text; font.pixelSize: 11; font.bold: true; anchors.verticalCenter: parent.verticalCenter }
            }
            MouseArea {
                id: saveGraphMa
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: {
                    graphSaveDialog.defaultPath = mapProv.navGraphPath.replace(/\.yaml$/, "_edited.yaml")
                    graphSaveDialog.open()
                }
            }
        }

        Rectangle {
            visible: root.graphEditMode && (root.selectedGraphVertex >= 0 || root.selectedGraphLane !== null)
            width: graphDeleteRow.implicitWidth + 16; height: 30; radius: 6
            color: graphDeleteMa.containsMouse ? Qt.darker(C.err, 1.15) : C.err
            opacity: 0.95
            Row {
                id: graphDeleteRow
                anchors.centerIn: parent
                spacing: 6
                Text { text: "🗑"; font.pixelSize: 13; anchors.verticalCenter: parent.verticalCenter }
                Text {
                    text: root.selectedGraphVertex >= 0 ? "DELETE WAYPOINT" : "DELETE LANE"
                    color: "#ffffff"; font.pixelSize: 11; font.bold: true
                    anchors.verticalCenter: parent.verticalCenter
                }
            }
            MouseArea {
                id: graphDeleteMa
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: {
                    if (root.selectedGraphVertex >= 0) {
                        graphEd.removeVertex(root.selectedGraphVertex)
                        root.selectedGraphVertex = -1
                    } else if (root.selectedGraphLane) {
                        graphEd.removeLane(root.selectedGraphLane.from, root.selectedGraphLane.to)
                        root.selectedGraphLane = null
                    }
                }
            }
        }

        Rectangle {
            visible: root.graphEditMode && root.selectedGraphVertex >= 0
            width: renameRow.implicitWidth + 16; height: 30; radius: 6
            color: renameMa.containsMouse ? C.surfaceRaised : C.surface
            border.color: C.border; border.width: 1
            opacity: 0.95
            Row {
                id: renameRow
                anchors.centerIn: parent
                spacing: 6
                Text { text: "✏"; font.pixelSize: 12; anchors.verticalCenter: parent.verticalCenter }
                Text { text: "RENAME"; color: C.text; font.pixelSize: 11; font.bold: true; anchors.verticalCenter: parent.verticalCenter }
            }
            MouseArea {
                id: renameMa
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: {
                    graphVertexDialog.editIndex = root.selectedGraphVertex
                    graphVertexDialog.open()
                }
            }
        }
    }

    // Graph editor status line (load/save feedback).
    Text {
        visible: root.graphStatusText !== ""
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        anchors.margins: 10
        z: 10
        text: root.graphStatusText
        color: C.text
        font.pixelSize: 11
        style: Text.Outline; styleColor: C.bg
    }

    GraphPromptDialog {
        id: graphVertexDialog
        anchors.centerIn: parent
        mode: "vertex"
        property int editIndex: -1
        onOpened: {
            if (editIndex >= 0 && root.graphVertices[editIndex]) {
                var v = root.graphVertices[editIndex]
                nameFieldPrefill(v.name, v.is_charger)
            }
        }
        function nameFieldPrefill(n, c) {
            // Dialog resets its own fields onOpened before this runs, so set after.
            Qt.callLater(function() {
                if (contentItem && contentItem.children) {
                    // Fields are private to GraphPromptDialog; use its own API instead.
                }
            })
        }
        onVertexConfirmed: (name, isCharger) => {
            if (editIndex >= 0) {
                graphEd.renameVertex(editIndex, name)
                graphEd.setVertexCharger(editIndex, isCharger)
            } else if (root._pendingVertexPos) {
                graphEd.addVertex(root._pendingVertexPos.x, root._pendingVertexPos.y, name, isCharger)
            }
            root._pendingVertexPos = null
            editIndex = -1
            root.pickMode = ""
        }
        onRejected: { root._pendingVertexPos = null; editIndex = -1 }
    }

    GraphPromptDialog {
        id: graphLaneDialog
        anchors.centerIn: parent
        mode: "lane"
        property int fromIndex: -1
        property int toIndex: -1
        onLaneConfirmed: (bidirectional) => {
            if (fromIndex >= 0 && toIndex >= 0)
                graphEd.addLane(fromIndex, toIndex, bidirectional)
            fromIndex = -1; toIndex = -1
        }
    }

    GraphPromptDialog {
        id: graphSaveDialog
        anchors.centerIn: parent
        mode: "save"
        onSaveConfirmed: (path) => graphEd.saveAs(path)
    }

    // Zoom and reset controls.
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
