import AppKit
import SiliconCore

func registerColor(_ loc: Location) -> NSColor {
    switch loc {
    case let .reg(r):
        if Reg.callerSaved.contains(r) { return NSColor(hex: 0x4A86D0) }
        if Reg.argumentTemps.contains(r) { return NSColor(hex: 0x8A6BC4) }
        return NSColor(hex: 0x2E9467)
    case .spill: return Theme.crimson
    }
}

/// Live intervals from the linear-scan register allocator, as a Gantt chart.
final class IntervalCanvas: FlippedView {
    var alloc: Allocation?
    var function: IRFunction?
    var selectedLine: Int?
    private var size = NSSize(width: 300, height: 200)
    let rowH: CGFloat = 17
    let labelW: CGFloat = 78

    override var intrinsicContentSize: NSSize { size }

    func recompute(width: CGFloat) {
        let n = alloc?.intervals.count ?? 0
        size = NSSize(width: max(260, width), height: 44 + CGFloat(n) * rowH + 30)
        invalidateIntrinsicContentSize()
        needsDisplay = true
    }

    override func draw(_ dirtyRect: NSRect) {
        Theme.panel.setFill()
        dirtyRect.fill()
        guard let a = alloc, let f = function, a.maxPosition > 0 else {
            drawText("No virtual registers.", at: NSPoint(x: 12, y: 12), Fonts.small, Theme.faint)
            return
        }
        let x0 = labelW, x1 = bounds.width - 12
        func xp(_ p: Int) -> CGFloat { x0 + (x1 - x0) * CGFloat(p) / CGFloat(a.maxPosition) }
        // Block boundaries.
        for b in f.blocks {
            guard let s = a.blockStart[b.id] else { continue }
            let x = xp(s - 1)
            line(NSPoint(x: x, y: 26), NSPoint(x: x, y: bounds.height - 20), Theme.grid, 1)
        }
        var lx: CGFloat = 8
        for (name, c) in [("t-reg", NSColor(hex: 0x4A86D0)), ("a-reg (as temp)", NSColor(hex: 0x8A6BC4)), ("s-reg (callee-saved)", NSColor(hex: 0x2E9467)), ("spilled", Theme.crimson)] {
            fillRounded(NSRect(x: lx, y: 8, width: 10, height: 8), 2, c)
            drawText(name, at: NSPoint(x: lx + 13, y: 5), Fonts.tiny, Theme.secondary)
            lx += textSize(name, Fonts.tiny).width + 26
        }
        for c in a.callPositions {
            let x = xp(c)
            line(NSPoint(x: x, y: 24), NSPoint(x: x, y: bounds.height - 18), Theme.amber.alpha(0.7), 1, dash: [3, 2])
            drawText("call", at: NSPoint(x: x - 9, y: bounds.height - 16), Fonts.tiny, Theme.amber)
        }
        var y: CGFloat = 30
        for iv in a.intervals {
            let name = iv.name
            drawText(name, in: NSRect(x: 6, y: y, width: labelW - 10, height: rowH), Fonts.mono(10.5), f.vregs[iv.vreg].isVariable ? Theme.green : Theme.secondary, align: .right)
            let bx = xp(iv.start), bw = max(4, xp(iv.end) - bx)
            let r = NSRect(x: bx, y: y + 2.5, width: bw, height: rowH - 5)
            let c = registerColor(iv.location)
            if case .spill = iv.location {
                fillRounded(r, 3, c.alpha(0.18))
                strokeRounded(r, 3, c, 1)
                NSGraphicsContext.saveGraphicsState()
                NSBezierPath(roundedRect: r, xRadius: 3, yRadius: 3).addClip()
                var hx = r.minX - r.height
                while hx < r.maxX {
                    line(NSPoint(x: hx, y: r.maxY), NSPoint(x: hx + r.height, y: r.minY), c.alpha(0.5), 1)
                    hx += 5
                }
                NSGraphicsContext.restoreGraphicsState()
            } else {
                fillRounded(r, 3, c.alpha(0.85))
            }
            let label: String
            switch iv.location {
            case let .reg(x): label = Reg.name(x)
            case let .spill(k): label = "spill \(k)"
            }
            let lw = textSize(label, Fonts.mono(9.5, .semibold)).width
            if bw > lw + 6 {
                let isSpill: Bool = { if case .spill = iv.location { return true }; return false }()
                drawText(label, at: NSPoint(x: bx + 4, y: y + 3), Fonts.mono(9.5, .semibold), isSpill ? c : .white)
            } else {
                drawText(label, at: NSPoint(x: bx + bw + 3, y: y + 3), Fonts.mono(9.5, .semibold), c)
            }
            y += rowH
        }
    }
}

/// The stack frame of one function, drawn from the frame pointer downwards.
final class FrameCanvas: FlippedView {
    var frameLayout: FrameLayout?
    private var size = NSSize(width: 300, height: 200)

    override var intrinsicContentSize: NSSize { size }

    private func h(_ e: FrameLayout.Entry) -> CGFloat { min(80, max(20, CGFloat(e.size) / 4 * 9)) }

    func recompute(width: CGFloat) {
        let total = (frameLayout?.entries ?? []).reduce(CGFloat(0)) { $0 + h($1) }
        size = NSSize(width: max(260, width), height: total + 110)
        invalidateIntrinsicContentSize()
        needsDisplay = true
    }

    override func draw(_ dirtyRect: NSRect) {
        Theme.panel.setFill()
        dirtyRect.fill()
        guard let fr = frameLayout else { return }
        guard fr.hasFrame else {
            drawText("Leaf function: no stack frame at all.", at: NSPoint(x: 14, y: 14), Fonts.body, Theme.text)
            drawText("It makes no calls and keeps every value in registers,", at: NSPoint(x: 14, y: 34), Fonts.small, Theme.secondary)
            drawText("so ra and sp are never touched.", at: NSPoint(x: 14, y: 50), Fonts.small, Theme.secondary)
            return
        }
        let x = 96.0, w = min(220, bounds.width - 110)
        var y: CGFloat = 30
        drawText("higher addresses — the caller's frame", at: NSPoint(x: x, y: 8), Fonts.tiny, Theme.faint)
        line(NSPoint(x: x, y: y), NSPoint(x: x + w, y: y), Theme.text, 1.5)
        drawText("s0 →", at: NSPoint(x: 52, y: y - 7), Fonts.mono(11, .semibold), Theme.ink)
        let colors: [String: NSColor] = ["ra": NSColor(hex: 0xF6E2C8), "fp": NSColor(hex: 0xF3E9D6), "saved": NSColor(hex: 0xDDEEE3),
                                         "spill": NSColor(hex: 0xF7DCD8), "local": NSColor(hex: 0xE3ECF8)]
        for e in fr.entries {
            let eh = h(e)
            let r = NSRect(x: x, y: y, width: w, height: eh)
            (colors[e.kind] ?? Theme.panelAlt).setFill()
            r.fill()
            Theme.borderStrong.setStroke()
            NSBezierPath(rect: r).stroke()
            drawText(e.what, in: r.insetBy(dx: 8, dy: 0), Fonts.small, Theme.text)
            drawText("\(e.offset)(s0)", in: NSRect(x: 8, y: y, width: x - 14, height: min(eh, 20)), Fonts.mono(10), Theme.secondary, align: .right)
            y += eh
        }
        let pad = fr.size - (fr.entries.map { -$0.offset }.max() ?? 0)
        if pad > 0 {
            let r = NSRect(x: x, y: y, width: w, height: 14)
            NSColor(hex: 0xF4F1EA).setFill()
            r.fill()
            drawText("padding to a multiple of 16", in: r.insetBy(dx: 8, dy: 0), Fonts.tiny, Theme.faint)
            y += 14
        }
        line(NSPoint(x: x, y: y), NSPoint(x: x + w, y: y), Theme.text, 1.5)
        drawText("sp →", at: NSPoint(x: 52, y: y - 7), Fonts.mono(11, .semibold), Theme.ink)
        drawText("frame size \(fr.size) bytes (sp = s0 − \(fr.size))", at: NSPoint(x: x, y: y + 8), Fonts.small, Theme.secondary)
        drawText("lower addresses — the stack grows down", at: NSPoint(x: x, y: y + 24), Fonts.tiny, Theme.faint)
    }
}

final class AsmView: NSView, StationView {
    let listing = ListPanel()
    let intervals = IntervalCanvas()
    lazy var intervalPanel = CanvasPanel(intervals)
    let frameCanvas = FrameCanvas()
    lazy var framePanel = CanvasPanel(frameCanvas)
    let functionPicker = PopUp(["main"])
    let regs = Segmented(["All registers", "6", "3"])
    let empty = EmptyNote()
    weak var model: AppModel?
    private var functions: [String] = []
    private var pcsByAsmLine: [Int: [UInt32]] = [:]

    override init(frame: NSRect) {
        super.init(frame: frame)
        for v in [listing, intervalPanel, framePanel, functionPicker, regs, empty] as [NSView] { addSubview(v) }
        functionPicker.onChange = { [weak self] k in
            guard let self, k < self.functions.count else { return }
            self.model?.focusFunction = self.functions[k]
        }
        regs.onChange = { [weak self] k in
            guard let m = self?.model else { return }
            var o = m.options
            o.registerLimit = [nil, 6, 3][k]
            m.options = o
        }
        listing.list.onClick = { [weak self] _, r in
            guard let self, let m = self.model else { return }
            m.selection = Selection(line: r.line, pc: r.pc, origin: .asm)
            if let fn = m.shown?.asm[safe: r.tag]?.function { m.focusFunction = fn }
        }
        listing.list.highlight = { [weak self] r in
            guard let m = self?.model else { return nil }
            if let pc = m.selection.pc, r.pc == pc { return Theme.selectionStrong.alpha(0.6) }
            if let l = m.selection.line, r.line == l { return Theme.selection }
            return nil
        }
    }
    required init?(coder: NSCoder) { fatalError() }

    override var isFlipped: Bool { true }

    override func layout() {
        super.layout()
        let rw = min(420, max(320, bounds.width * 0.42))
        let lw = bounds.width - rw
        listing.frame = NSRect(x: 0, y: 0, width: lw, height: bounds.height)
        functionPicker.frame = NSRect(x: lw + 12, y: 34, width: 150, height: 26)
        let rs = regs.intrinsicContentSize
        regs.frame = NSRect(x: bounds.width - rs.width - 12, y: 35, width: rs.width, height: 24)
        let half = (bounds.height - 66) * 0.55
        intervalPanel.frame = NSRect(x: lw, y: 66, width: rw, height: half)
        framePanel.frame = NSRect(x: lw, y: 66 + half + 30, width: rw, height: bounds.height - 66 - half - 30)
        intervals.recompute(width: rw - 4)
        frameCanvas.recompute(width: rw - 4)
        empty.frame = bounds
    }

    override func draw(_ dirtyRect: NSRect) {
        Theme.panel.setFill()
        bounds.fill()
        let lw = listing.frame.width
        line(NSPoint(x: lw + 0.5, y: 0), NSPoint(x: lw + 0.5, y: bounds.height), Theme.border)
        drawText("Register allocation", at: NSPoint(x: lw + 12, y: 9), Fonts.subheading, Theme.heading)
        drawText("linear scan over live intervals", at: NSPoint(x: lw + 150, y: 11), Fonts.tiny, Theme.secondary)
        let fy = framePanel.frame.minY - 24
        line(NSPoint(x: lw, y: fy - 4.5), NSPoint(x: bounds.width, y: fy - 4.5), Theme.border)
        drawText("Stack frame", at: NSPoint(x: lw + 12, y: fy), Fonts.subheading, Theme.heading)
        drawText("what the prologue builds", at: NSPoint(x: lw + 104, y: fy + 2), Fonts.tiny, Theme.secondary)
    }

    func reload(_ model: AppModel, _ event: ModelEvent) {
        self.model = model
        guard let c = model.shown, !c.asm.isEmpty else {
            empty.isHidden = false
            empty.title = "No assembly yet"
            empty.message = "Fix the errors in the program first."
            empty.needsDisplay = true
            return
        }
        empty.isHidden = true
        regs.selected = [nil, 6, 3].firstIndex(of: c.options.registerLimit) ?? 0
        regs.enabledMask = c.isAssembly ? [false, false, false] : nil
        if event == .compiled || event == .config || listing.list.rows.isEmpty {
            pcsByAsmLine = [:]
            for ins in c.executable?.instructions ?? [] where ins.object == "main.o" {
                pcsByAsmLine[ins.asmLine, default: []].append(ins.address)
            }
            var rows: [Row] = []
            for (k, l) in c.asm.enumerated() {
                var r: Row
                switch l.kind {
                case .blank:
                    r = Row([], height: 8)
                case .comment:
                    r = Row([Cell(NSAttributedString(l.text.trimmingCharacters(in: .whitespaces), Fonts.mono(11), Theme.comment), leftPad: c.isAssembly ? 0 : 6)], height: 18)
                case .label:
                    r = Row([Cell(highlightAsm(l.text.trimmingCharacters(in: .whitespaces))), Cell(l.note, Fonts.small, Theme.faint, width: 200)], height: 21)
                default:
                    let pcs = pcsByAsmLine[k] ?? []
                    let addr = pcs.first.map { String(format: "%06x", $0 & 0xFFFFFF) } ?? ""
                    r = Row([Cell(addr, Fonts.mono(10), Theme.faint, width: 54),
                             Cell(highlightAsm(l.text.trimmingCharacters(in: .whitespaces)), leftPad: 14),
                             Cell(l.note, Fonts.small, Theme.secondary, width: 210)])
                    r.pc = pcs.first
                }
                r.tag = k
                r.line = c.isAssembly ? k + 1 : (l.origin?.line).flatMap { $0 > 0 ? $0 : nil }
                rows.append(r)
            }
            listing.set(rows)
        }
        // Mark rows whose instructions are in the pipeline now.
        if let p = model.pipeline, let rec = p.lastRecord {
            var stageOf: [UInt32: Int] = [:]
            for (k, s) in rec.stages.enumerated() where s.seq >= 0 && s.status != .flushed { stageOf[s.pc] = k }
            for k in listing.list.rows.indices {
                let pcs = pcsByAsmLine[listing.list.rows[k].tag] ?? []
                listing.list.rows[k].marker = pcs.compactMap { stageOf[$0] }.max().map { Theme.stage[$0] }
            }
        }
        listing.list.needsDisplay = true

        functions = c.ir?.functions.map { $0.name } ?? []
        let focus = model.focusFunction.flatMap { functions.contains($0) ? $0 : nil } ?? (functions.contains("main") ? "main" : functions.first)
        functionPicker.isHidden = functions.isEmpty
        functionPicker.set(functions, selected: focus.flatMap { functions.firstIndex(of: $0) } ?? 0)
        intervals.alloc = focus.flatMap { c.allocations[$0] }
        intervals.function = c.ir?.functions.first { $0.name == focus }
        frameCanvas.frameLayout = c.frames.first { $0.function == focus }
        intervals.recompute(width: intervalPanel.bounds.width - 4)
        frameCanvas.recompute(width: framePanel.bounds.width - 4)
        intervalPanel.needsLayout = true
        framePanel.needsLayout = true
        if c.isAssembly {
            intervals.alloc = nil
            frameCanvas.frameLayout = nil
        }

        if event == .selection, model.selection.origin != .asm {
            if let pc = model.selection.pc, let k = listing.list.rows.firstIndex(where: { $0.pc == pc }) {
                listing.list.reveal(k)
            } else if let l = model.selection.line, let k = listing.list.rows.firstIndex(where: { $0.line == l && $0.pc != nil }) {
                listing.list.reveal(k)
            }
        }
        if event == .stepped, model.running == false, let p = model.pipeline, let rec = p.lastRecord,
           let ex = rec.stages.indices.reversed().first(where: { rec.stages[$0].seq >= 0 }), model.station == .asm {
            if let k = listing.list.rows.firstIndex(where: { $0.pc == rec.stages[ex].pc }) { listing.list.reveal(k) }
        }
    }
}

extension Array {
    subscript(safe i: Int) -> Element? { i >= 0 && i < count ? self[i] : nil }
}
