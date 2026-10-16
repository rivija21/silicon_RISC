import AppKit

/// A soft, rounded button in the app's style. Can show an SF Symbol and act as a toggle.
final class TButton: NSView {
    enum Style { case plain, primary, toggle, link }
    var title: String { didSet { invalidateIntrinsicContentSize(); needsDisplay = true } }
    var symbol: String? { didSet { needsDisplay = true } }
    var style: Style
    var isOn = false { didSet { needsDisplay = true } }
    var isEnabled = true { didSet { needsDisplay = true } }
    var action: (() -> Void)?
    var font = Fonts.ui(12, .medium)
    private var hover = false
    private var pressed = false

    init(_ title: String, symbol: String? = nil, style: Style = .plain, action: (() -> Void)? = nil) {
        self.title = title
        self.symbol = symbol
        self.style = style
        self.action = action
        super.init(frame: .zero)
        addTrackingArea(NSTrackingArea(rect: .zero, options: [.mouseEnteredAndExited, .activeInActiveApp, .inVisibleRect], owner: self))
    }
    required init?(coder: NSCoder) { fatalError() }

    override var intrinsicContentSize: NSSize {
        let w = title.isEmpty ? 0 : textSize(title, font).width
        let iconW: CGFloat = symbol == nil ? 0 : (title.isEmpty ? 16 : 20)
        return NSSize(width: w + iconW + (title.isEmpty ? 14 : 20), height: 26)
    }

    override func mouseEntered(with event: NSEvent) { hover = true; needsDisplay = true }
    override func mouseExited(with event: NSEvent) { hover = false; needsDisplay = true }
    override func mouseDown(with event: NSEvent) {
        guard isEnabled else { return }
        pressed = true
        needsDisplay = true
    }
    override func mouseUp(with event: NSEvent) {
        guard isEnabled else { return }
        pressed = false
        needsDisplay = true
        if bounds.contains(convert(event.locationInWindow, from: nil)) { action?() }
    }

    override func draw(_ dirtyRect: NSRect) {
        let r = bounds.insetBy(dx: 0.5, dy: 0.5)
        var fill = Theme.panel
        var fg = isEnabled ? Theme.text : Theme.faint
        var border = Theme.borderStrong
        switch style {
        case .primary:
            fill = isEnabled ? Theme.ink : Theme.faint
            fg = .white
            border = fill
            if hover && isEnabled { fill = Theme.ink.blended(withFraction: 0.15, of: .black)! }
        case .toggle:
            if isOn {
                fill = NSColor(hex: 0xE2EAF6)
                fg = Theme.ink
                border = Theme.ink.alpha(0.55)
            } else if hover && isEnabled { fill = Theme.hover }
        case .plain:
            if hover && isEnabled { fill = Theme.hover }
        case .link:
            fill = .clear
            border = .clear
            fg = isEnabled ? Theme.ink : Theme.faint
        }
        if pressed { fill = fill.blended(withFraction: 0.1, of: .black)! }
        if style != .link {
            fillRounded(r, 6, fill)
            strokeRounded(r, 6, border)
        }
        var x = (bounds.width - intrinsicContentSize.width) / 2 + (title.isEmpty ? 7 : 10)
        if let symbol, let img = NSImage(systemSymbolName: symbol, accessibilityDescription: title) {
            let cfg = NSImage.SymbolConfiguration(pointSize: 12, weight: .medium)
            let im = img.withSymbolConfiguration(cfg) ?? img
            let tinted = im.tinted(fg)
            let sz = tinted.size
            tinted.draw(in: NSRect(x: x + (16 - sz.width) / 2, y: (bounds.height - sz.height) / 2, width: sz.width, height: sz.height))
            x += title.isEmpty ? 16 : 20
        }
        if !title.isEmpty {
            let sz = textSize(title, font)
            drawText(title, at: NSPoint(x: x, y: (bounds.height - sz.height) / 2), font, fg)
        }
    }
}

extension NSImage {
    func tinted(_ color: NSColor) -> NSImage {
        let img = NSImage(size: size)
        img.lockFocus()
        draw(in: NSRect(origin: .zero, size: size))
        color.set()
        NSRect(origin: .zero, size: size).fill(using: .sourceAtop)
        img.unlockFocus()
        img.isTemplate = false
        return img
    }
}

/// A row of mutually exclusive options.
final class Segmented: NSView {
    var options: [String] { didSet { invalidateIntrinsicContentSize(); needsDisplay = true } }
    var selected: Int { didSet { needsDisplay = true } }
    var onChange: ((Int) -> Void)?
    var font = Fonts.ui(11.5, .medium)
    var enabledMask: [Bool]?

    init(_ options: [String], selected: Int = 0, onChange: ((Int) -> Void)? = nil) {
        self.options = options
        self.selected = selected
        self.onChange = onChange
        super.init(frame: .zero)
    }
    required init?(coder: NSCoder) { fatalError() }

    private var widths: [CGFloat] { options.map { textSize($0, font).width + 18 } }
    override var intrinsicContentSize: NSSize { NSSize(width: widths.reduce(0, +) + 4, height: 26) }

    override func draw(_ dirtyRect: NSRect) {
        let r = bounds.insetBy(dx: 0.5, dy: 0.5)
        fillRounded(r, 7, NSColor(hex: 0xEFE8DA))
        strokeRounded(r, 7, Theme.border)
        let total = widths.reduce(0, +)
        let scale = (bounds.width - 4) / max(1, total)
        var x: CGFloat = 2
        for (k, o) in options.enumerated() {
            let w = widths[k] * scale
            let seg = NSRect(x: x, y: 2, width: w, height: bounds.height - 4)
            let enabled = enabledMask?[k] ?? true
            if k == selected {
                drawCard(seg, radius: 5)
            }
            drawText(o, in: seg, font, !enabled ? Theme.faint : k == selected ? Theme.ink : Theme.secondary, align: .center)
            x += w
        }
    }

    override func mouseDown(with event: NSEvent) {
        let p = convert(event.locationInWindow, from: nil)
        let total = widths.reduce(0, +)
        let scale = (bounds.width - 4) / max(1, total)
        var x: CGFloat = 2
        for (k, w0) in widths.enumerated() {
            let w = w0 * scale
            if p.x >= x && p.x < x + w {
                if enabledMask?[k] ?? true, k != selected {
                    selected = k
                    onChange?(k)
                }
                return
            }
            x += w
        }
    }
}

/// A checkbox with a label, drawn to match.
final class Check: NSView {
    var title: String
    var isOn: Bool { didSet { needsDisplay = true } }
    var onChange: ((Bool) -> Void)?
    var font = Fonts.ui(11.5)

    init(_ title: String, isOn: Bool, onChange: ((Bool) -> Void)? = nil) {
        self.title = title
        self.isOn = isOn
        self.onChange = onChange
        super.init(frame: .zero)
    }
    required init?(coder: NSCoder) { fatalError() }

    override var intrinsicContentSize: NSSize { NSSize(width: textSize(title, font).width + 24, height: 20) }

    override func draw(_ dirtyRect: NSRect) {
        let box = NSRect(x: 1, y: (bounds.height - 14) / 2, width: 14, height: 14)
        fillRounded(box, 3.5, isOn ? Theme.ink : Theme.panel)
        strokeRounded(box, 3.5, isOn ? Theme.ink : Theme.borderStrong)
        if isOn {
            let p = NSBezierPath()
            p.move(to: NSPoint(x: box.minX + 3.2, y: box.midY))
            p.line(to: NSPoint(x: box.minX + 6, y: box.minY + 3.5))
            p.line(to: NSPoint(x: box.maxX - 3, y: box.maxY - 3.5))
            p.lineWidth = 1.8
            NSColor.white.setStroke()
            p.stroke()
        }
        drawText(title, in: NSRect(x: 21, y: 0, width: bounds.width - 21, height: bounds.height), font, Theme.text)
    }

    override func mouseDown(with event: NSEvent) {
        isOn.toggle()
        onChange?(isOn)
    }
}

/// A styled pop-up menu button.
final class PopUp: NSView {
    let button = NSPopUpButton(frame: .zero, pullsDown: false)
    var onChange: ((Int) -> Void)?

    init(_ items: [String], selected: Int = 0, onChange: ((Int) -> Void)? = nil) {
        self.onChange = onChange
        super.init(frame: .zero)
        button.addItems(withTitles: items)
        button.selectItem(at: selected)
        button.font = Fonts.ui(12)
        button.bezelStyle = .flexiblePush
        button.controlSize = .regular
        button.target = self
        button.action = #selector(changed)
        addSubview(button)
    }
    required init?(coder: NSCoder) { fatalError() }

    @objc private func changed() { onChange?(button.indexOfSelectedItem) }

    func set(_ items: [String], selected: Int) {
        button.removeAllItems()
        button.addItems(withTitles: items)
        button.selectItem(at: selected)
    }

    override var intrinsicContentSize: NSSize { NSSize(width: button.intrinsicContentSize.width, height: 26) }
    override func layout() {
        super.layout()
        button.frame = bounds.insetBy(dx: 0, dy: -1)
    }
}

/// A label (non-editable, no background).
func makeLabel(_ s: String, _ font: NSFont = Fonts.body, _ color: NSColor = Theme.text) -> NSTextField {
    let l = NSTextField(labelWithString: s)
    l.font = font
    l.textColor = color
    l.lineBreakMode = .byTruncatingTail
    return l
}

/// Lays subviews out left to right with their intrinsic widths.
final class HStack: NSView {
    var spacing: CGFloat = 6
    var items: [NSView] = []
    var fixedWidths: [ObjectIdentifier: CGFloat] = [:]

    func set(_ views: [NSView]) {
        for v in items { v.removeFromSuperview() }
        items = views
        for v in views { addSubview(v) }
        needsLayout = true
    }

    override var intrinsicContentSize: NSSize {
        let w = items.reduce(CGFloat(0)) { $0 + width(of: $1) } + spacing * CGFloat(max(0, items.count - 1))
        return NSSize(width: w, height: 26)
    }

    private func width(of v: NSView) -> CGFloat {
        if let f = fixedWidths[ObjectIdentifier(v)] { return f }
        let w = v.intrinsicContentSize.width
        return w > 0 ? w : 60
    }

    override func layout() {
        super.layout()
        var x: CGFloat = 0
        for v in items {
            let w = width(of: v)
            let h = min(bounds.height, max(20, v.intrinsicContentSize.height > 0 ? v.intrinsicContentSize.height : 26))
            v.frame = NSRect(x: x, y: (bounds.height - h) / 2, width: w, height: h)
            x += w + spacing
        }
    }
}

/// A view that paints the parchment background with a faint paper grain.
final class ParchmentView: NSView {
    override func draw(_ dirtyRect: NSRect) {
        Theme.background.setFill()
        dirtyRect.fill()
    }
}

/// A titled card that hosts one content view.
final class CardView: NSView {
    var title: String { didSet { needsDisplay = true } }
    var subtitle: String = "" { didSet { needsDisplay = true } }
    let content: NSView
    var accessory: NSView? {
        didSet {
            oldValue?.removeFromSuperview()
            if let a = accessory { addSubview(a) }
            needsLayout = true
        }
    }
    var headerHeight: CGFloat { title.isEmpty ? 0 : 32 }

    init(_ title: String, content: NSView) {
        self.title = title
        self.content = content
        super.init(frame: .zero)
        addSubview(content)
    }
    required init?(coder: NSCoder) { fatalError() }

    override var isFlipped: Bool { true }

    override func layout() {
        super.layout()
        content.frame = NSRect(x: 1, y: headerHeight, width: bounds.width - 2, height: bounds.height - headerHeight - 1)
        if let a = accessory {
            let sz = a.intrinsicContentSize
            a.frame = NSRect(x: bounds.width - sz.width - 10, y: 4, width: sz.width, height: 24)
        }
    }

    override func draw(_ dirtyRect: NSRect) {
        drawCard(bounds.insetBy(dx: 0.5, dy: 0.5))
        guard !title.isEmpty else { return }
        drawText(title, at: NSPoint(x: 12, y: 8), Fonts.heading, Theme.heading)
        if !subtitle.isEmpty {
            let w = textSize(title, Fonts.heading).width
            let maxW = bounds.width - w - 30 - (accessory?.intrinsicContentSize.width ?? 0) - 10
            drawText(subtitle, in: NSRect(x: 22 + w, y: 9, width: max(0, maxW), height: 16), Fonts.small, Theme.secondary)
        }
        line(NSPoint(x: 1, y: headerHeight - 0.5), NSPoint(x: bounds.width - 1, y: headerHeight - 0.5), Theme.border)
    }
}
