import QtQuick

// Icon of the tool header: strokes drawn on a 24 x 24 grid in the colour given. No image file, so it
// follows the theme and stays sharp whatever the scale of the screen.
Canvas {
    id: icon
    property string name: ""
    property color tint: Theme.text

    width: 22
    height: 22
    onNameChanged: requestPaint()
    onTintChanged: requestPaint()
    onWidthChanged: requestPaint()
    onVisibleChanged: if (visible) requestPaint()

    onPaint: {
        const c = getContext("2d")
        c.reset()
        c.scale(width / 24, height / 24)
        c.lineWidth = 1.7
        c.lineCap = "round"
        c.lineJoin = "round"
        c.strokeStyle = String(icon.tint)
        c.fillStyle = String(icon.tint)
        // A broken line through the points (x0, y0, x1, y1...), closed or not; a circle; a rectangle.
        const line = (points, closed) => {
            c.beginPath()
            c.moveTo(points[0], points[1])
            for (let i = 2; i < points.length; i += 2)
                c.lineTo(points[i], points[i + 1])
            if (closed)
                c.closePath()
            c.stroke()
        }
        const circle = (x, y, r, filled) => {
            c.beginPath()
            c.arc(x, y, r, 0, 2 * Math.PI, false)
            if (filled)
                c.fill()
            else
                c.stroke()
        }
        const box = (x, y, w, h) => {
            c.beginPath()
            c.rect(x, y, w, h)
            c.stroke()
        }
        // An ellipse in the rectangle (x, y, w, h): a circle seen at an angle.
        const oval = (x, y, w, h) => {
            c.beginPath()
            c.ellipse(x, y, w, h)
            c.stroke()
        }
        switch (icon.name) {
        case "pointer":
            line([6, 3, 6, 19, 10.5, 15, 13.5, 21, 15.8, 20, 12.8, 14, 18.5, 14], true)
            break
        case "wire":
            line([5, 18.5, 19, 5.5], false)
            circle(5, 18.5, 2, true)
            circle(19, 5.5, 2, true)
            break
        case "element":
            line([9, 3, 9, 10], false)
            line([9, 14, 9, 21], false)
            circle(9, 12, 1.8, false)
            line([15, 7, 21, 7], false)
            line([18, 4, 18, 10], false)
            break
        case "source":
            circle(12, 12, 8, false)
            c.beginPath()
            c.moveTo(7, 12)
            c.quadraticCurveTo(9.5, 5.5, 12, 12)
            c.quadraticCurveTo(14.5, 18.5, 17, 12)
            c.stroke()
            break
        case "move":
            line([12, 3, 12, 21], false)
            line([3, 12, 21, 12], false)
            line([9.5, 5.5, 12, 3, 14.5, 5.5], false)
            line([9.5, 18.5, 12, 21, 14.5, 18.5], false)
            line([5.5, 9.5, 3, 12, 5.5, 14.5], false)
            line([18.5, 9.5, 21, 12, 18.5, 14.5], false)
            break
        case "cut":
            circle(6, 7, 2.6, false)
            circle(6, 17, 2.6, false)
            line([8.2, 8.4, 20, 17], false)
            line([8.2, 15.6, 20, 7], false)
            break
        case "duplicate":
            box(9, 9, 11, 11)
            line([15, 6, 15, 4, 4, 4, 4, 15, 6, 15], false)
            break
        case "rotate":
            c.beginPath()
            c.arc(12, 12, 7.5, 0.25 * Math.PI, 1.75 * Math.PI, false)
            c.stroke()
            line([13.2, 7.2, 17.3, 6.7, 17.6, 2.6], false)
            break
        case "mirror":
            line([12, 3, 12, 6], false)
            line([12, 9, 12, 12], false)
            line([12, 15, 12, 18], false)
            line([12, 20.5, 12, 21], false)
            line([8.5, 7, 8.5, 17, 3, 17], true)
            line([15.5, 7, 15.5, 17, 21, 17], true)
            break
        case "align":
            line([5, 3, 5, 21], false)
            box(8, 6, 11, 4)
            box(8, 14, 7, 4)
            break
        case "delete":
            line([4, 7, 20, 7], false)
            line([9, 7, 9, 4, 15, 4, 15, 7], false)
            line([6, 7, 7, 20, 17, 20, 18, 7], false)
            line([10, 11, 10, 16], false)
            line([14, 11, 14, 16], false)
            break
        case "group":
            box(7, 7, 5, 5)
            box(12, 12, 5, 5)
            line([3, 7, 3, 3, 7, 3], false)
            line([17, 3, 21, 3, 21, 7], false)
            line([21, 17, 21, 21, 17, 21], false)
            line([7, 21, 3, 21, 3, 17], false)
            break
        case "ungroup":
            box(4, 4, 7, 7)
            box(13, 13, 7, 7)
            line([15, 5, 19, 9], false)
            line([19, 5, 15, 9], false)
            break
        case "constraint":
            circle(8.5, 12, 4.5, false)
            circle(15.5, 12, 4.5, false)
            break
        case "plane":
            line([3, 18, 8, 10, 21, 10, 16, 18], true)
            line([12, 14, 12, 3], false)
            line([10, 5, 12, 3, 14, 5], false)
            break
        case "pivot":
            circle(12, 12, 5.5, false)
            circle(12, 12, 1.5, true)
            line([12, 2, 12, 6.5], false)
            line([12, 17.5, 12, 22], false)
            line([2, 12, 6.5, 12], false)
            line([17.5, 12, 22, 12], false)
            break
        case "measure":
            box(3, 8, 18, 8)
            line([7, 8, 7, 12], false)
            line([11, 8, 11, 11], false)
            line([15, 8, 15, 12], false)
            line([19, 8, 19, 11], false)
            break
        case "views":
            line([12, 3, 20, 7.5, 20, 16.5, 12, 21, 4, 16.5, 4, 7.5], true)
            line([4, 7.5, 12, 12, 20, 7.5], false)
            line([12, 12, 12, 21], false)
            break
        case "fit":
            line([4, 9, 4, 4, 9, 4], false)
            line([15, 4, 20, 4, 20, 9], false)
            line([20, 15, 20, 20, 15, 20], false)
            line([9, 20, 4, 20, 4, 15], false)
            box(9, 9, 6, 6)
            break
        case "display":
            c.beginPath()
            c.moveTo(2, 12)
            c.quadraticCurveTo(12, 1.5, 22, 12)
            c.quadraticCurveTo(12, 22.5, 2, 12)
            c.stroke()
            circle(12, 12, 3, false)
            break
        case "pattern":
            circle(12, 12, 9, false)
            c.beginPath()
            c.moveTo(11, 12)
            c.quadraticCurveTo(20, 3, 20.5, 12)
            c.quadraticCurveTo(20, 21, 11, 12)
            c.quadraticCurveTo(5, 9, 5.5, 12)
            c.quadraticCurveTo(5, 15, 11, 12)
            c.stroke()
            break
        case "lobes":
            c.beginPath()
            c.moveTo(9, 12)
            c.bezierCurveTo(12, 2, 22, 5, 22, 12)
            c.bezierCurveTo(22, 19, 12, 22, 9, 12)
            c.bezierCurveTo(6, 8, 2, 9.5, 2, 12)
            c.bezierCurveTo(2, 14.5, 6, 16, 9, 12)
            c.stroke()
            break
        case "sweep":
            line([4, 4, 4, 20, 20, 20], false)
            c.beginPath()
            c.moveTo(6.5, 7)
            c.bezierCurveTo(11, 7, 11, 16.5, 12.5, 16.5)
            c.bezierCurveTo(14, 16.5, 14, 7, 19.5, 7)
            c.stroke()
            break
        case "smith":
            circle(12, 12, 9, false)
            line([3, 12, 21, 12], false)
            circle(16.5, 12, 4.5, false)
            c.beginPath()
            c.arc(21, 3, 9, 0.5 * Math.PI, Math.PI, false)
            c.stroke()
            c.beginPath()
            c.arc(21, 21, 9, Math.PI, 1.5 * Math.PI, false)
            c.stroke()
            break
        case "check":
            line([4, 12.5, 9.5, 18, 20, 6], false)
            break
        case "play":
            line([8, 5, 19, 12, 8, 19], true)
            break
        case "stop":
            box(7, 7, 10, 10)
            break
        case "pause":
            box(7, 6, 3.5, 12)
            box(13.5, 6, 3.5, 12)
            break
        case "step":
            line([6, 5, 15, 12, 6, 19], true)
            line([18.5, 5, 18.5, 19], false)
            break
        case "colours":
            box(4, 4, 16, 16)
            line([4, 12, 12, 4], false)
            line([4, 20, 20, 4], false)
            line([12, 20, 20, 12], false)
            break
        case "fieldlines":
            c.beginPath()
            c.moveTo(3, 7)
            c.quadraticCurveTo(12, 1, 21, 7)
            c.moveTo(3, 17)
            c.quadraticCurveTo(12, 23, 21, 17)
            c.stroke()
            line([3, 12, 21, 12], false)
            line([13.5, 9.5, 16.5, 12, 13.5, 14.5], false)
            break
        case "contours":
            circle(12, 12, 9, false)
            circle(12, 12, 5.5, false)
            circle(12, 12, 2, false)
            break
        case "arrows":
            line([5, 19, 19, 5], false)
            line([11, 5, 19, 5, 19, 13], false)
            break
        case "grid":
            line([9, 4, 9, 20], false)
            line([15, 4, 15, 20], false)
            line([4, 9, 20, 9], false)
            line([4, 15, 20, 15], false)
            break
        case "brush":
            line([20, 4, 11, 13], false)
            c.beginPath()
            c.moveTo(11, 13)
            c.quadraticCurveTo(6, 13.5, 6.5, 18)
            c.quadraticCurveTo(6, 20, 4, 20.5)
            c.quadraticCurveTo(11.5, 21.5, 13, 15)
            c.closePath()
            c.stroke()
            break
        case "eraser":
            line([9, 20, 4, 15, 14, 5, 20, 11, 11, 20], true)
            line([8, 11, 14, 17], false)
            line([11, 20, 20, 20], false)
            break
        case "rect":
            box(4, 6, 16, 12)
            break
        // Shapes of the antenna designer, by nature: wires, surfaces, solids.
        case "dipole":
            line([12, 3, 12, 10], false)
            line([12, 14, 12, 21], false)
            circle(12, 12, 1.8, false)
            break
        case "loop":
            circle(12, 11, 7.5, false)
            circle(12, 18.5, 1.8, true)
            break
        case "helix":
            line([6, 21, 18, 18, 6, 15, 18, 12, 6, 9, 18, 6, 6, 3], false)
            break
        case "plate":
            line([2.5, 17, 8, 8, 21.5, 8, 16, 17], true)
            break
        case "discflat":
            oval(3, 7.5, 18, 9)
            break
        case "box":
            line([4, 9, 15, 9, 15, 20, 4, 20], true)
            line([4, 9, 9, 4, 20, 4, 15, 9], false)
            line([20, 4, 20, 15, 15, 20], false)
            break
        case "cylinder":
            oval(5, 3, 14, 6)
            oval(5, 15, 14, 6)
            line([5, 6, 5, 18], false)
            line([19, 6, 19, 18], false)
            break
        // A curved wire, a parabolic reflector with its focus.
        case "curve":
            c.beginPath()
            c.arc(12, 19, 10, Math.PI * 1.08, Math.PI * 1.92, false)
            c.stroke()
            circle(2.6, 16.5, 1.4, true)
            circle(21.4, 16.5, 1.4, true)
            break
        case "dish": {
            const dish = []
            for (let q = -9; q <= 9; q += 1.5)
                dish.push(4 + q * q / 13, 12 + q)
            line(dish, false)
            // Its focus, a quarter of 13 from the vertex, and the arm of the feed.
            circle(4 + 13 / 4, 12, 1.6, true)
            line([4 + 13 / 4, 12, 21, 12], false)
            break
        }
        case "cone":
            oval(4, 15, 16, 6)
            line([4, 18, 12, 3, 20, 18], false)
            break
        case "sphere":
            circle(12, 12, 8, false)
            oval(4, 9.5, 16, 5)
            break
        case "disc":
            circle(12, 12, 8, false)
            break
        case "pointb":
            circle(12, 12, 7, false)
            circle(12, 12, 2.2, true)
            break
        case "filter":
            line([4, 5, 20, 5, 14, 12, 14, 19, 10, 17, 10, 12], true)
            break
        case "caret":
            c.beginPath()
            c.moveTo(4, 8)
            c.lineTo(20, 8)
            c.lineTo(12, 18)
            c.closePath()
            c.fill()
            break
        }
    }
}
