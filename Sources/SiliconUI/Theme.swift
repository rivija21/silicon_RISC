import AppKit
import SiliconCore

/// A classic lab-notebook look: warm parchment, ivory cards, ink-blue and crimson.
enum Theme {
    static let background = NSColor(hex: 0xF3EDE2)
    static let panel = NSColor(hex: 0xFFFDF8)
    static let panelAlt = NSColor(hex: 0xFAF5EA)
    static let border = NSColor(hex: 0xE3D9C6)
    static let borderStrong = NSColor(hex: 0xCDBFA5)
    static let grid = NSColor(hex: 0xEDF1EA)
    static let gridMajor = NSColor(hex: 0xD5DFD2)
    static let text = NSColor(hex: 0x2B2A33)
    static let secondary = NSColor(hex: 0x7C7364)
    static let faint = NSColor(hex: 0xA79F92)
    static let ink = NSColor(hex: 0x1F4E9E)
    static let crimson = NSColor(hex: 0xC0392B)
    static let heading = NSColor(hex: 0xA0472E)
    static let green = NSColor(hex: 0x1C7C6A)
    static let amber = NSColor(hex: 0xC27A1A)
    static let purple = NSColor(hex: 0x7B3F9E)
    static let selection = NSColor(hex: 0xFCE9B8)
    static let selectionStrong = NSColor(hex: 0xF6D57A)
    static let hover = NSColor(hex: 0xF4EEDF)
    static let errorFill = NSColor(hex: 0xF9DEDA)
    static let okFill = NSColor(hex: 0xDCEFE5)

    // Pipeline stages.
    static let stage: [NSColor] = [
        NSColor(hex: 0x2F6FC9), NSColor(hex: 0x2A8F62), NSColor(hex: 0xD27A22), NSColor(hex: 0x8552B8), NSColor(hex: 0xBF3D5A),
    ]
    static func stageFill(_ k: Int) -> NSColor { stage[k].blended(withFraction: 0.82, of: .white)! }

    // Syntax colours.
    static let keyword = NSColor(hex: 0x8E3A9E)
    static let number = NSColor(hex: 0x1F6FB2)
    static let string = NSColor(hex: 0xB5552B)
    static let comment = NSColor(hex: 0x6E8B5E)
    static let directive = NSColor(hex: 0xA0472E)
    static let punct = NSColor(hex: 0x6A6358)
    static let type = NSColor(hex: 0x1C7C6A)
    static let register = NSColor(hex: 0x1C7C6A)
    static let label = NSColor(hex: 0xA0472E)
    static let mnemonic = NSColor(hex: 0x1F4E9E)

    // Encoding fields.
    static let field: [String: NSColor] = [
        "opcode": NSColor(hex: 0xE8D5F0), "rd": NSColor(hex: 0xD3ECDF), "rs1": NSColor(hex: 0xD6E6F7), "rs2": NSColor(hex: 0xDCE3F9),
        "funct3": NSColor(hex: 0xF7E3C7), "funct7": NSColor(hex: 0xF5D9D2), "imm": NSColor(hex: 0xFFF1B8), "shamt": NSColor(hex: 0xFFF1B8),
    ]
    static func fieldColor(_ name: String) -> NSColor {
        if name.hasPrefix("imm") { return field["imm"]! }
        return field[name] ?? NSColor(hex: 0xEEEEEE)
    }
}

extension NSColor {
    convenience init(hex: UInt32, alpha: CGFloat = 1) {
        self.init(srgbRed: CGFloat((hex >> 16) & 0xFF) / 255, green: CGFloat((hex >> 8) & 0xFF) / 255,
                  blue: CGFloat(hex & 0xFF) / 255, alpha: alpha)
    }
    func alpha(_ a: CGFloat) -> NSColor { withAlphaComponent(a) }
}

enum Fonts {
    static func serif(_ size: CGFloat, _ weight: NSFont.Weight = .regular) -> NSFont {
        let base = NSFont.systemFont(ofSize: size, weight: weight)
        if let d = base.fontDescriptor.withDesign(.serif), let f = NSFont(descriptor: d, size: size) { return f }
        return base
    }
    static func mono(_ size: CGFloat = 12, _ weight: NSFont.Weight = .regular) -> NSFont {
        NSFont.monospacedSystemFont(ofSize: size, weight: weight)
    }
    static func ui(_ size: CGFloat = 12, _ weight: NSFont.Weight = .regular) -> NSFont {
        NSFont.systemFont(ofSize: size, weight: weight)
    }
    static let title = serif(18, .semibold)
    static let heading = serif(14, .semibold)
    static let subheading = serif(12.5, .semibold)
    static let body = ui(12)
    static let small = ui(11)
    static let tiny = ui(10)
    static let code = mono(12.5)
    static let codeSmall = mono(11)
    static let codeTiny = mono(10)
}

// MARK: - Drawing helpers

extension NSAttributedString {
    convenience init(_ s: String, _ font: NSFont, _ color: NSColor = Theme.text) {
        self.init(string: s, attributes: [.font: font, .foregroundColor: color])
    }
}

extension NSMutableAttributedString {
    func add(_ s: String, _ font: NSFont, _ color: NSColor = Theme.text) {
        append(NSAttributedString(s, font, color))
    }
}

func drawText(_ s: String, at p: NSPoint, _ font: NSFont, _ color: NSColor = Theme.text) {
    NSAttributedString(s, font, color).draw(at: p)
}

func textSize(_ s: String, _ font: NSFont) -> NSSize {
    NSAttributedString(s, font).size()
}

/// Draws text so that it fits the rect, centred vertically, truncated with an ellipsis.
func drawText(_ s: String, in r: NSRect, _ font: NSFont, _ color: NSColor = Theme.text, align: NSTextAlignment = .left) {
    let p = NSMutableParagraphStyle()
    p.alignment = align
    p.lineBreakMode = .byTruncatingTail
    let a = NSAttributedString(string: s, attributes: [.font: font, .foregroundColor: color, .paragraphStyle: p])
    let h = a.size().height
    a.draw(with: NSRect(x: r.minX, y: r.midY - h / 2, width: r.width, height: h + 2), options: [.usesLineFragmentOrigin, .truncatesLastVisibleLine])
}

func drawAttributed(_ a: NSAttributedString, in r: NSRect) {
    a.draw(with: r, options: [.usesLineFragmentOrigin, .truncatesLastVisibleLine])
}

func fillRounded(_ r: NSRect, _ radius: CGFloat, _ color: NSColor) {
    color.setFill()
    NSBezierPath(roundedRect: r, xRadius: radius, yRadius: radius).fill()
}

func strokeRounded(_ r: NSRect, _ radius: CGFloat, _ color: NSColor, _ width: CGFloat = 1) {
    color.setStroke()
    let p = NSBezierPath(roundedRect: r.insetBy(dx: width / 2, dy: width / 2), xRadius: radius, yRadius: radius)
    p.lineWidth = width
    p.stroke()
}

func line(_ a: NSPoint, _ b: NSPoint, _ color: NSColor, _ width: CGFloat = 1, dash: [CGFloat]? = nil) {
    color.setStroke()
    let p = NSBezierPath()
    p.move(to: a)
    p.line(to: b)
    p.lineWidth = width
    if let dash { p.setLineDash(dash, count: dash.count, phase: 0) }
    p.stroke()
}

func arrowHead(at tip: NSPoint, from: NSPoint, _ color: NSColor, size: CGFloat = 6) {
    let ang = atan2(tip.y - from.y, tip.x - from.x)
    let p = NSBezierPath()
    p.move(to: tip)
    p.line(to: NSPoint(x: tip.x - size * cos(ang - 0.45), y: tip.y - size * sin(ang - 0.45)))
    p.line(to: NSPoint(x: tip.x - size * cos(ang + 0.45), y: tip.y - size * sin(ang + 0.45)))
    p.close()
    color.setFill()
    p.fill()
}

/// A softly shadowed ivory card, the basic panel of the app.
func drawCard(_ r: NSRect, radius: CGFloat = 8, fill: NSColor = Theme.panel) {
    NSGraphicsContext.saveGraphicsState()
    let sh = NSShadow()
    sh.shadowColor = NSColor(hex: 0x6B5A3A, alpha: 0.10)
    sh.shadowOffset = NSSize(width: 0, height: -1)
    sh.shadowBlurRadius = 3
    sh.set()
    fillRounded(r, radius, fill)
    NSGraphicsContext.restoreGraphicsState()
    strokeRounded(r, radius, Theme.border)
}

/// A small rounded label like "load-use" or "L1 hit".
func drawPill(_ s: String, at p: NSPoint, font: NSFont = Fonts.tiny, fill: NSColor, text: NSColor = Theme.text) -> NSRect {
    let sz = textSize(s, font)
    let r = NSRect(x: p.x, y: p.y, width: sz.width + 10, height: sz.height + 3)
    fillRounded(r, r.height / 2, fill)
    drawText(s, at: NSPoint(x: r.minX + 5, y: r.minY + 1.5), font, text)
    return r
}

/// Flipped base view: y grows downwards, like reading a page.
class FlippedView: NSView {
    override var isFlipped: Bool { true }
}

func formatCount(_ n: Int) -> String {
    let f = NumberFormatter()
    f.numberStyle = .decimal
    f.groupingSeparator = ","
    return f.string(from: NSNumber(value: n)) ?? String(n)
}

func hex8(_ v: UInt32) -> String { String(format: "%08x", v) }
func hexAddr(_ v: UInt32) -> String { "0x" + String(format: "%08x", v) }

func binaryString(_ v: UInt32, bits: Int) -> String {
    (0..<bits).reversed().map { (v >> UInt32($0)) & 1 == 1 ? "1" : "0" }.joined()
}

/// Colours an assembly line: mnemonic, registers, numbers, labels, comments.
func highlightAsm(_ text: String, font: NSFont = Fonts.code, dim: Bool = false) -> NSAttributedString {
    let out = NSMutableAttributedString()
    var body = text
    var comment = ""
    if let h = text.firstIndex(of: "#") {
        body = String(text[..<h])
        comment = String(text[h...])
    }
    let trimmed = body.trimmingCharacters(in: .whitespaces)
    let lead = String(body.prefix(while: { $0 == " " || $0 == "\t" }))
    out.add(lead, font)
    if trimmed.hasSuffix(":") {
        out.add(trimmed, Fonts.mono(font.pointSize, .semibold), Theme.label)
    } else if trimmed.hasPrefix(".") {
        let parts = trimmed.split(separator: " ", maxSplits: 1).map(String.init)
        out.add(parts[0], font, Theme.directive)
        if parts.count > 1 { out.add(" " + parts[1], font, Theme.string) }
    } else if !trimmed.isEmpty {
        let parts = trimmed.split(separator: " ", maxSplits: 1).map(String.init)
        out.add(parts[0], Fonts.mono(font.pointSize, .semibold), dim ? Theme.secondary : Theme.mnemonic)
        if parts.count > 1 {
            out.add(" ", font)
            var tok = ""
            func flush() {
                guard !tok.isEmpty else { return }
                let color: NSColor
                if Reg.parse(tok) != nil { color = Theme.register }
                else if tok.first.map({ $0.isNumber || $0 == "-" }) == true { color = Theme.number }
                else if tok.hasPrefix("%") { color = Theme.purple }
                else { color = Theme.label }
                out.add(tok, font, color)
                tok = ""
            }
            for c in parts[1] {
                if c == "," || c == "(" || c == ")" || c == " " {
                    flush()
                    out.add(String(c), font, Theme.punct)
                } else {
                    tok.append(c)
                }
            }
            flush()
        }
    }
    if !comment.isEmpty { out.add(comment, Fonts.mono(font.pointSize - 0.5), Theme.comment) }
    return out
}
