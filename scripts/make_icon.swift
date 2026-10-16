// Draws the app icon and writes Resources/AppIcon.icns.
// Usage: swift scripts/make_icon.swift
import AppKit

func color(_ hex: UInt32, _ a: CGFloat = 1) -> CGColor {
    CGColor(srgbRed: CGFloat((hex >> 16) & 0xFF) / 255, green: CGFloat((hex >> 8) & 0xFF) / 255,
            blue: CGFloat(hex & 0xFF) / 255, alpha: a)
}

func drawIcon(size: Int) -> Data {
    let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: size, pixelsHigh: size, bitsPerSample: 8, samplesPerPixel: 4,
                               hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
    NSGraphicsContext.saveGraphicsState()
    NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
    let ctx = NSGraphicsContext.current!.cgContext
    let s = CGFloat(size) / 1024
    ctx.scaleBy(x: s, y: s)

    // Parchment card.
    let card = CGRect(x: 100, y: 100, width: 824, height: 824)
    let shape = CGPath(roundedRect: card, cornerWidth: 185, cornerHeight: 185, transform: nil)
    ctx.saveGState()
    ctx.setShadow(offset: CGSize(width: 0, height: -10 * s), blur: 26 * s, color: color(0x3A2A10, 0.35))
    ctx.addPath(shape)
    ctx.setFillColor(color(0xFFFDF8))
    ctx.fillPath()
    ctx.restoreGState()
    ctx.saveGState()
    ctx.addPath(shape)
    ctx.clip()
    let g = CGGradient(colorsSpace: CGColorSpace(name: CGColorSpace.sRGB), colors: [color(0xFFFDF6), color(0xF1E7D3)] as CFArray, locations: nil)!
    ctx.drawLinearGradient(g, start: CGPoint(x: 512, y: 924), end: CGPoint(x: 512, y: 100), options: [])
    ctx.setStrokeColor(color(0xDCE6D8))
    ctx.setLineWidth(2)
    for v in stride(from: 100.0, through: 924, by: 41.2) {
        ctx.move(to: CGPoint(x: v, y: 100)); ctx.addLine(to: CGPoint(x: v, y: 924))
        ctx.move(to: CGPoint(x: 100, y: v)); ctx.addLine(to: CGPoint(x: 924, y: v))
    }
    ctx.strokePath()

    // The pipeline staircase: four instructions flowing through five stages.
    let stages: [UInt32] = [0x2F6FC9, 0x2A8F62, 0xD27A22, 0x8552B8, 0xBF3D5A]
    let cw: CGFloat = 78, step: CGFloat = 84, ch: CGFloat = 50
    let x0: CGFloat = 512 - (8 * step - (step - cw)) / 2
    for row in 0..<4 {
        for k in 0..<5 {
            let r = CGRect(x: x0 + CGFloat(row + k) * step, y: 750 - CGFloat(row) * 62, width: cw, height: ch)
            ctx.addPath(CGPath(roundedRect: r, cornerWidth: 10, cornerHeight: 10, transform: nil))
            ctx.setFillColor(color(stages[k], 0.4 + 0.2 * CGFloat(row)))
            ctx.fillPath()
        }
    }
    // A chip with pins, holding a "C".
    let chip = CGRect(x: 382, y: 190, width: 260, height: 260)
    ctx.setFillColor(color(0x1F4E9E))
    for k in 0..<5 {
        let o = chip.minX + 33 + CGFloat(k) * 46
        ctx.fill(CGRect(x: o, y: chip.minY - 34, width: 18, height: 38))
        ctx.fill(CGRect(x: o, y: chip.maxY - 4, width: 18, height: 38))
        let oy = chip.minY + 33 + CGFloat(k) * 46
        ctx.fill(CGRect(x: chip.minX - 34, y: oy, width: 38, height: 18))
        ctx.fill(CGRect(x: chip.maxX - 4, y: oy, width: 38, height: 18))
    }
    ctx.addPath(CGPath(roundedRect: chip, cornerWidth: 34, cornerHeight: 34, transform: nil))
    ctx.fillPath()
    ctx.setStrokeColor(color(0xFFFFFF, 0.25))
    ctx.setLineWidth(6)
    ctx.addPath(CGPath(roundedRect: chip.insetBy(dx: 22, dy: 22), cornerWidth: 20, cornerHeight: 20, transform: nil))
    ctx.strokePath()
    ctx.restoreGState()
    NSGraphicsContext.restoreGraphicsState()
    // Text drawn with AppKit (needs a flipped-aware context).
    NSGraphicsContext.saveGraphicsState()
    NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
    let font: NSFont = {
        let base = NSFont.systemFont(ofSize: 190 * s, weight: .bold)
        if let d = base.fontDescriptor.withDesign(.serif), let f = NSFont(descriptor: d, size: 190 * s) { return f }
        return base
    }()
    let a = NSAttributedString(string: "C", attributes: [.font: font, .foregroundColor: NSColor.white])
    let sz = a.size()
    a.draw(at: NSPoint(x: 512 * s - sz.width / 2, y: 320 * s - sz.height / 2))
    NSGraphicsContext.restoreGraphicsState()
    return rep.representation(using: .png, properties: [:])!
}

let fm = FileManager.default
let iconset = "Resources/AppIcon.iconset"
try? fm.removeItem(atPath: iconset)
try! fm.createDirectory(atPath: iconset, withIntermediateDirectories: true)
for (name, px) in [("16x16", 16), ("16x16@2x", 32), ("32x32", 32), ("32x32@2x", 64), ("128x128", 128), ("128x128@2x", 256),
                   ("256x256", 256), ("256x256@2x", 512), ("512x512", 512), ("512x512@2x", 1024)] {
    try! drawIcon(size: px).write(to: URL(fileURLWithPath: "\(iconset)/icon_\(name).png"))
}
let p = Process()
p.executableURL = URL(fileURLWithPath: "/usr/bin/iconutil")
p.arguments = ["-c", "icns", iconset, "-o", "Resources/AppIcon.icns"]
try! p.run()
p.waitUntilExit()
try? fm.removeItem(atPath: iconset)
print("wrote Resources/AppIcon.icns")
