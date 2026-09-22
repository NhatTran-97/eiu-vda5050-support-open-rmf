import QtQuick

// Line chart of one measure over the last minutes, with a grid, the latest value, an optional limit line and a crosshair readout.
Rectangle {
    id: chart

    property string title: ""
    // Appended to every value shown.
    property string unit: ""
    // Seconds ago and value of each sample, oldest first; a null value is an interval without data.
    property var ages: []
    property var values: []
    // A level the measure is judged against; 0 draws none.
    property real limit: 0
    property string limitLabel: ""
    property int decimals: 1
    property color lineColor: C.cyan
    // Scale of the text and spacing; the parent raises it as the page grows.
    property real s: 1.0

    readonly property int lastIndex: {
        for (var i = values.length - 1; i >= 0; i--)
            if (values[i] !== null && values[i] !== undefined) return i
        return -1
    }
    readonly property bool hasData: lastIndex >= 0
    readonly property real span: ages.length > 0 ? Math.max(1, ages[0]) : 1
    readonly property real yMax: {
        var m = limit > 0 ? limit * 1.15 : 0
        for (var i = 0; i < values.length; i++)
            if (values[i] !== null && values[i] !== undefined && values[i] > m) m = values[i]
        return niceMax(m)
    }
    property int hoverIndex: -1

    function niceMax(v) {
        if (!(v > 0)) return 1
        var e = Math.pow(10, Math.floor(Math.log(v) / Math.LN10))
        var f = v / e
        return (f <= 1 ? 1 : f <= 2 ? 2 : f <= 5 ? 5 : 10) * e
    }
    function fmt(v) {
        if (v === null || v === undefined) return "no data"
        return Number(v).toFixed(v >= 1000 ? 0 : decimals) + (unit ? " " + unit : "")
    }
    function agoText(s) {
        if (s < 1) return "now"
        return s >= 90 ? Math.round(s / 60) + " min ago" : Math.round(s) + " s ago"
    }
    function xOf(age, left, width) { return left + width * (1 - age / span) }
    function yOf(v, topY, height) { return topY + height * (1 - Math.min(v, yMax) / yMax) }

    implicitHeight: 230 * s
    radius: 12 * s
    color: C.surfaceAlt
    border.color: C.border
    border.width: 1
    onAgesChanged: canvas.requestPaint()
    onValuesChanged: canvas.requestPaint()
    onLimitChanged: canvas.requestPaint()
    onHoverIndexChanged: canvas.requestPaint()
    onSChanged: canvas.requestPaint()

    Text {
        id: titleText
        anchors.left: parent.left; anchors.top: parent.top
        anchors.leftMargin: 16 * chart.s; anchors.topMargin: 14 * chart.s
        text: chart.title.toUpperCase()
        color: C.textDim
        font.pixelSize: 13 * chart.s; font.bold: true; font.letterSpacing: 1.0
    }
    Text {
        anchors.right: parent.right; anchors.top: parent.top
        anchors.rightMargin: 16 * chart.s; anchors.topMargin: 9 * chart.s
        text: chart.hasData ? chart.fmt(chart.values[chart.lastIndex]) : "—"
        color: C.text
        font.family: "IBM Plex Mono"; font.pixelSize: 24 * chart.s; font.bold: true
    }

    Canvas {
        id: canvas
        anchors.fill: parent
        anchors.topMargin: 46 * chart.s
        anchors.leftMargin: 8 * chart.s; anchors.rightMargin: 14 * chart.s; anchors.bottomMargin: 8 * chart.s
        renderTarget: Canvas.Image

        readonly property real plotLeft: 58 * chart.s
        readonly property real plotRight: width - 8 * chart.s
        readonly property real plotTop: 8 * chart.s
        readonly property real plotBottom: height - 26 * chart.s

        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()

        onPaint: {
            var ctx = getContext("2d")
            ctx.reset()
            var left = plotLeft, w = plotRight - plotLeft
            var topY = plotTop, h = plotBottom - plotTop
            ctx.font = (12 * chart.s) + "px 'IBM Plex Sans'"

            // Hairline grid with the values it stands for.
            ctx.lineWidth = 1
            ctx.strokeStyle = C.border
            ctx.fillStyle = C.textDim
            ctx.textAlign = "right"
            ctx.textBaseline = "middle"
            for (var g = 0; g <= 2; g++) {
                var gy = Math.round(topY + h * (1 - g / 2)) + 0.5
                ctx.beginPath(); ctx.moveTo(left, gy); ctx.lineTo(left + w, gy); ctx.stroke()
                var tick = chart.yMax * g / 2
                ctx.fillText(tick >= 1000 ? tick.toFixed(0) : Number(tick.toPrecision(3)).toString(), left - 8 * chart.s, gy)
            }
            ctx.textBaseline = "alphabetic"
            ctx.textAlign = "left"
            ctx.fillText(chart.ages.length > 0 ? "−" + (chart.span >= 90 ? Math.round(chart.span / 60) + " min" : Math.round(chart.span) + " s") : "", left, plotBottom + 17 * chart.s)
            ctx.textAlign = "right"
            ctx.fillText("now", left + w, plotBottom + 17 * chart.s)

            if (!chart.hasData) return

            // The level the measure is judged against.
            if (chart.limit > 0) {
                var ly = Math.round(chart.yOf(chart.limit, topY, h)) + 0.5
                ctx.strokeStyle = C.warn
                ctx.beginPath(); ctx.moveTo(left, ly); ctx.lineTo(left + w, ly); ctx.stroke()
                if (chart.limitLabel !== "") {
                    ctx.fillStyle = C.textDim
                    ctx.textAlign = "left"
                    ctx.fillText(chart.limitLabel, left + 6 * chart.s, ly - 5 * chart.s)
                }
            }

            // The measure: a 2 px line that breaks where an interval had no data.
            ctx.lineWidth = 2
            ctx.lineJoin = "round"
            ctx.lineCap = "round"
            ctx.strokeStyle = chart.lineColor
            var pen = false
            ctx.beginPath()
            for (var i = 0; i < chart.values.length; i++) {
                var v = chart.values[i]
                if (v === null || v === undefined) { pen = false; continue }
                var x = chart.xOf(chart.ages[i], left, w), y = chart.yOf(v, topY, h)
                if (pen) ctx.lineTo(x, y); else ctx.moveTo(x, y)
                pen = true
            }
            ctx.stroke()

            // Crosshair on the sample under the pointer.
            if (chart.hoverIndex >= 0 && chart.values[chart.hoverIndex] !== null && chart.values[chart.hoverIndex] !== undefined) {
                var hx = Math.round(chart.xOf(chart.ages[chart.hoverIndex], left, w)) + 0.5
                ctx.lineWidth = 1
                ctx.strokeStyle = C.textDim
                ctx.beginPath(); ctx.moveTo(hx, topY); ctx.lineTo(hx, plotBottom); ctx.stroke()
            }

            // The latest sample, or the one under the pointer, as a dot ringed in the surface colour.
            var mark = (chart.hoverIndex >= 0 && chart.values[chart.hoverIndex] !== null && chart.values[chart.hoverIndex] !== undefined)
                       ? chart.hoverIndex : chart.lastIndex
            var mx = chart.xOf(chart.ages[mark], left, w), my = chart.yOf(chart.values[mark], topY, h)
            ctx.fillStyle = C.surfaceAlt
            ctx.beginPath(); ctx.arc(mx, my, 4.5 * Math.max(1, chart.s * 0.9) + 2, 0, Math.PI * 2); ctx.fill()
            ctx.fillStyle = chart.lineColor
            ctx.beginPath(); ctx.arc(mx, my, 4.5 * Math.max(1, chart.s * 0.9), 0, Math.PI * 2); ctx.fill()
        }
    }

    Text {
        anchors.centerIn: canvas
        visible: !chart.hasData
        text: "Waiting for reports"
        color: C.textDim
        font.pixelSize: 14 * chart.s
    }

    // The pointer picks the nearest sample; it never takes clicks or drags, so the page still scrolls.
    MouseArea {
        anchors.fill: canvas
        hoverEnabled: true
        acceptedButtons: Qt.NoButton
        onPositionChanged: function (mouse) {
            if (chart.ages.length === 0) { chart.hoverIndex = -1; return }
            var w = canvas.plotRight - canvas.plotLeft
            var age = (1 - (mouse.x - canvas.plotLeft) / w) * chart.span
            var best = -1, gap = 1e9
            for (var i = 0; i < chart.ages.length; i++) {
                if (chart.values[i] === null || chart.values[i] === undefined) continue
                var d = Math.abs(chart.ages[i] - age)
                if (d < gap) { gap = d; best = i }
            }
            chart.hoverIndex = best
        }
        onExited: chart.hoverIndex = -1
    }

    Rectangle {
        id: readout
        visible: chart.hoverIndex >= 0
        z: 2
        readonly property real px: chart.hoverIndex >= 0
                                   ? canvas.x + chart.xOf(chart.ages[chart.hoverIndex], canvas.plotLeft, canvas.plotRight - canvas.plotLeft) : 0
        x: Math.max(6, Math.min(chart.width - width - 6, px - width / 2))
        y: 46 * chart.s + 4
        width: readoutRow.implicitWidth + 24 * chart.s
        height: 32 * chart.s
        radius: 8
        color: C.surfaceRaised
        border.color: C.border
        Row {
            id: readoutRow
            anchors.centerIn: parent
            spacing: 9 * chart.s
            Rectangle { anchors.verticalCenter: parent.verticalCenter; width: 14 * chart.s; height: 2; radius: 1; color: chart.lineColor }
            Text {
                text: chart.hoverIndex >= 0 ? chart.fmt(chart.values[chart.hoverIndex]) : ""
                color: C.text
                font.family: "IBM Plex Mono"; font.pixelSize: 14 * chart.s; font.bold: true
            }
            Text {
                text: chart.hoverIndex >= 0 ? chart.agoText(chart.ages[chart.hoverIndex]) : ""
                color: C.textDim
                font.pixelSize: 13 * chart.s
            }
        }
    }
}
