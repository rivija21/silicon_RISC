import AppKit

/// One cell of a listing row.
struct Cell {
    var text: NSAttributedString
    /// nil = take the remaining width.
    var width: CGFloat?
    var align: NSTextAlignment = .left
    /// A rounded background behind the text (a badge).
    var badge: NSColor?
    var leftPad: CGFloat = 0

    init(_ text: NSAttributedString, width: CGFloat? = nil, align: NSTextAlignment = .left, badge: NSColor? = nil, leftPad: CGFloat = 0) {
        self.text = text
        self.width = width
        self.align = align
        self.badge = badge
        self.leftPad = leftPad
    }

    init(_ s: String, _ font: NSFont = Fonts.code, _ color: NSColor = Theme.text, width: CGFloat? = nil, align: NSTextAlignment = .left) {
        text = NSAttributedString(s, font, color)
        self.width = width
        self.align = align
    }
}

struct Row {
    enum Kind { case normal, header, section, spacer }
    var cells: [Cell]
    var kind: Kind = .normal
    var height: CGFloat = 19
    var background: NSColor?
    /// Payload for click handling and highlight matching.
    var tag: Int = -1
    var line: Int?
    var pc: UInt32?
    var node: Int?
    var irInst: Int?
    /// A coloured strip at the left edge (stage colour, etc.).
    var marker: NSColor?
    var indent: CGFloat = 0
    var tooltip: String?

    init(_ cells: [Cell], kind: Kind = .normal, height: CGFloat = 19, tag: Int = -1) {
        self.cells = cells
        self.kind = kind
        self.height = height
        self.tag = tag
    }

    static func header(_ s: String, detail: String = "") -> Row {
        let a = NSMutableAttributedString()
        a.add(s, Fonts.subheading, Theme.heading)
        if !detail.isEmpty { a.add("   " + detail, Fonts.small, Theme.secondary) }
        var r = Row([Cell(a)], kind: .header, height: 28)
        r.background = nil
        return r
    }

    static func spacer(_ h: CGFloat = 8) -> Row { Row([], kind: .spacer, height: h) }
}

/// A fast, custom-drawn list of rows (used for IR, assembly, machine code, tables…).
final class ListView: FlippedView {
    var rows: [Row] = [] {
        didSet {
            offsets = []
            var y: CGFloat = 6
            for r in rows {
                offsets.append(y)
                y += r.height
            }
            contentHeight = y + 8
            invalidateIntrinsicContentSize()
            frame.size.height = max(contentHeight, superview?.bounds.height ?? 0)
            needsDisplay = true
        }
    }
    private(set) var offsets: [CGFloat] = []
    private(set) var contentHeight: CGFloat = 0
    var padding: CGFloat = 10
    /// Returns a highlight colour for a row (selection, execution…).
    var highlight: ((Row) -> NSColor?)?
    var onClick: ((Int, Row) -> Void)?
    var onHover: ((Int?) -> Void)?
    var hovered: Int? { didSet { if hovered != oldValue { needsDisplay = true; onHover?(hovered) } } }
    var minWidth: CGFloat = 0

    override init(frame: NSRect) {
        super.init(frame: frame)
        addTrackingArea(NSTrackingArea(rect: .zero, options: [.mouseMoved, .mouseEnteredAndExited, .activeInKeyWindow, .inVisibleRect], owner: self))
    }
    required init?(coder: NSCoder) { fatalError() }

    func index(at y: CGFloat) -> Int? {
        var lo = 0, hi = offsets.count - 1
        guard hi >= 0 else { return nil }
        while lo < hi {
            let mid = (lo + hi + 1) / 2
            if offsets[mid] <= y { lo = mid } else { hi = mid - 1 }
        }
        guard y >= offsets[lo], y < offsets[lo] + rows[lo].height else { return nil }
        return lo
    }

    func rect(ofRow k: Int) -> NSRect {
        NSRect(x: 0, y: offsets[k], width: bounds.width, height: rows[k].height)
    }

    /// Scrolls so that row k is visible (centred if it was far away).
    func reveal(_ k: Int) {
        guard k >= 0, k < rows.count, let sv = enclosingScrollView else { return }
        let r = rect(ofRow: k)
        let vis = sv.contentView.documentVisibleRect
        if r.minY < vis.minY + 4 || r.maxY > vis.maxY - 4 {
            let y = max(0, min(r.midY - vis.height / 2, bounds.height - vis.height))
            sv.contentView.scroll(to: NSPoint(x: vis.minX, y: y))
            sv.reflectScrolledClipView(sv.contentView)
        }
    }

    override func mouseMoved(with event: NSEvent) {
        let p = convert(event.locationInWindow, from: nil)
        hovered = index(at: p.y).flatMap { rows[$0].kind == .normal ? $0 : nil }
    }
    override func mouseExited(with event: NSEvent) { hovered = nil }

    override func mouseDown(with event: NSEvent) {
        let p = convert(event.locationInWindow, from: nil)
        if let k = index(at: p.y), rows[k].kind == .normal { onClick?(k, rows[k]) }
    }

    override func draw(_ dirtyRect: NSRect) {
        Theme.panel.setFill()
        dirtyRect.fill()
        guard !rows.isEmpty else { return }
        let first = index(at: max(6, dirtyRect.minY)) ?? 0
        var k = max(0, first - 1)
        while k < rows.count {
            let y = offsets[k]
            if y > dirtyRect.maxY { break }
            drawRow(rows[k], k, y)
            k += 1
        }
    }

    private func drawRow(_ r: Row, _ k: Int, _ y: CGFloat) {
        let full = NSRect(x: 0, y: y, width: bounds.width, height: r.height)
        switch r.kind {
        case .spacer: return
        case .header:
            if let c = r.cells.first {
                let h = c.text.size().height
                c.text.draw(at: NSPoint(x: padding + r.indent, y: y + r.height - h - 4))
            }
            line(NSPoint(x: padding, y: y + r.height - 1.5), NSPoint(x: bounds.width - padding, y: y + r.height - 1.5), Theme.border)
            return
        case .section:
            Theme.panelAlt.setFill()
            full.fill()
        case .normal:
            break
        }
        if let bg = r.background {
            bg.setFill()
            full.fill()
        }
        if let h = highlight?(r) {
            h.setFill()
            full.fill()
        }
        if hovered == k {
            Theme.hover.alpha(0.7).setFill()
            full.fill()
        }
        if let m = r.marker {
            m.setFill()
            NSRect(x: 0, y: y + 1, width: 4, height: r.height - 2).fill()
        }
        var x = padding + r.indent
        let fixed = r.cells.compactMap { $0.width }.reduce(0, +)
        let flexCount = r.cells.filter { $0.width == nil }.count
        let flexW = max(40, (bounds.width - padding * 2 - r.indent - fixed) / CGFloat(max(1, flexCount)))
        for c in r.cells {
            let w = c.width ?? flexW
            let cellR = NSRect(x: x + c.leftPad, y: y, width: w - c.leftPad - 4, height: r.height)
            if let b = c.badge {
                let tw = min(cellR.width, c.text.size().width + 10)
                let bx = c.align == .right ? cellR.maxX - tw : c.align == .center ? cellR.midX - tw / 2 : cellR.minX
                fillRounded(NSRect(x: bx, y: y + 2, width: tw, height: r.height - 4), 4, b)
                let th = c.text.size().height
                c.text.draw(with: NSRect(x: bx + 5, y: y + (r.height - th) / 2, width: tw - 6, height: th + 2), options: [.usesLineFragmentOrigin, .truncatesLastVisibleLine])
            } else {
                let p = NSMutableParagraphStyle()
                p.alignment = c.align
                p.lineBreakMode = .byTruncatingTail
                let m = NSMutableAttributedString(attributedString: c.text)
                m.addAttribute(.paragraphStyle, value: p, range: NSRange(location: 0, length: m.length))
                let th = m.size().height
                m.draw(with: NSRect(x: cellR.minX, y: y + (r.height - th) / 2, width: cellR.width, height: th + 2), options: [.usesLineFragmentOrigin, .truncatesLastVisibleLine])
            }
            x += w
        }
    }
}

/// A ListView inside a scroll view, sized to fill.
final class ListPanel: NSView {
    let scroll = NSScrollView()
    let list = ListView(frame: .zero)

    override init(frame: NSRect) {
        super.init(frame: frame)
        scroll.documentView = list
        scroll.hasVerticalScroller = true
        scroll.autohidesScrollers = true
        scroll.drawsBackground = true
        scroll.backgroundColor = Theme.panel
        scroll.borderType = .noBorder
        addSubview(scroll)
    }
    required init?(coder: NSCoder) { fatalError() }

    override func layout() {
        super.layout()
        scroll.frame = bounds
        list.frame = NSRect(x: 0, y: 0, width: max(scroll.contentSize.width, list.minWidth),
                            height: max(list.contentHeight, scroll.contentSize.height))
    }

    func set(_ rows: [Row]) {
        list.rows = rows
        needsLayout = true
        layoutSubtreeIfNeeded()
    }
}

/// A scrollable canvas for custom diagrams.
final class CanvasPanel: NSView {
    let scroll = NSScrollView()
    let canvas: NSView

    init(_ canvas: NSView) {
        self.canvas = canvas
        super.init(frame: .zero)
        scroll.documentView = canvas
        scroll.hasVerticalScroller = true
        scroll.hasHorizontalScroller = true
        scroll.autohidesScrollers = true
        scroll.drawsBackground = true
        scroll.backgroundColor = Theme.panel
        scroll.borderType = .noBorder
        addSubview(scroll)
    }
    required init?(coder: NSCoder) { fatalError() }

    override func layout() {
        super.layout()
        scroll.frame = bounds
        let want = canvas.intrinsicContentSize
        canvas.frame = NSRect(x: 0, y: 0, width: max(want.width, scroll.contentSize.width), height: max(want.height, scroll.contentSize.height))
    }
}
