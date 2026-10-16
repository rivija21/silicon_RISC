import AppKit
import SiliconCore

/// Colours one IR instruction's text.
func irAttributed(_ text: String, font: NSFont = Fonts.code) -> NSAttributedString {
    let out = NSMutableAttributedString()
    var tok = ""
    func flush() {
        guard !tok.isEmpty else { return }
        let c: NSColor
        if tok.hasPrefix("%") { c = tok.hasPrefix("%t") && tok.dropFirst(2).allSatisfy({ $0.isNumber }) ? Theme.secondary : Theme.green }
        else if tok.hasPrefix("[$") || tok.hasPrefix("$") { c = Theme.purple }
        else if tok.hasPrefix("[@") || tok.hasPrefix("@") { c = Theme.label }
        else if ["load.w", "load.b", "store.w", "store.b", "call", "goto", "if", "else", "return", "(char)"].contains(tok) { c = Theme.mnemonic }
        else if tok.first?.isNumber == true || (tok.first == "-" && tok.count > 1) { c = Theme.number }
        else if tok.contains(".") || tok.first?.isLetter == true { c = Theme.label }
        else { c = Theme.punct }
        let f = ["call", "goto", "if", "else", "return", "load.w", "load.b", "store.w", "store.b"].contains(tok) ? Fonts.mono(font.pointSize, .semibold) : font
        out.add(tok, f, c)
        tok = ""
    }
    for ch in text {
        if ch == " " || ch == "," || ch == "(" || ch == ")" || ch == "]" || ch == "←" {
            flush()
            out.add(String(ch), font, Theme.punct)
        } else if ch == "[" {
            flush()
            out.add("[", font, Theme.punct)
        } else {
            tok.append(ch)
        }
    }
    flush()
    return out
}

// MARK: - Pass list

final class PassList: FlippedView {
    struct Entry {
        var title: String
        var pass: PassID?
        var snapshot: Int?
        var delta: Int
        var changed: Bool
        var enabled: Bool
        var isFinal: Bool
    }
    var entries: [Entry] = []
    var selected = 0
    var onSelect: ((Int) -> Void)?
    var onToggle: ((PassID, Bool) -> Void)?
    var onPreset: ((Bool) -> Void)?
    private var rects: [NSRect] = []
    private var checkRects: [NSRect] = []
    private var presetRects: [NSRect] = []

    override func draw(_ dirtyRect: NSRect) {
        Theme.panelAlt.setFill()
        bounds.fill()
        line(NSPoint(x: bounds.maxX - 0.5, y: 0), NSPoint(x: bounds.maxX - 0.5, y: bounds.maxY), Theme.border)
        drawText("Optimiser passes", at: NSPoint(x: 12, y: 10), Fonts.subheading, Theme.heading)
        drawText("click a step to see the IR after it", at: NSPoint(x: 12, y: 28), Fonts.tiny, Theme.secondary)
        presetRects = []
        var px: CGFloat = 12
        for (k, t) in ["-O0  none", "-O1  all"].enumerated() {
            let w = textSize(t, Fonts.small).width + 16
            let r = NSRect(x: px, y: 46, width: w, height: 20)
            fillRounded(r, 5, Theme.panel)
            strokeRounded(r, 5, Theme.borderStrong)
            drawText(t, in: r, Fonts.ui(11, .medium), Theme.ink, align: .center)
            presetRects.append(r)
            px += w + 6
            _ = k
        }
        rects = []
        checkRects = []
        var y: CGFloat = 76
        for (k, e) in entries.enumerated() {
            let h: CGFloat = e.pass == nil ? 34 : 40
            let r = NSRect(x: 8, y: y, width: bounds.width - 16, height: h)
            rects.append(r)
            if k == selected {
                drawCard(r, radius: 6, fill: NSColor(hex: 0xE8EFF9))
                strokeRounded(r, 6, Theme.ink.alpha(0.5))
            } else if e.snapshot != nil {
                fillRounded(r, 6, Theme.panel.alpha(0.6))
            }
            var tx = r.minX + 10
            if let _ = e.pass {
                let cb = NSRect(x: r.minX + 8, y: r.minY + 8, width: 14, height: 14)
                checkRects.append(cb)
                fillRounded(cb, 3.5, e.enabled ? Theme.ink : Theme.panel)
                strokeRounded(cb, 3.5, e.enabled ? Theme.ink : Theme.borderStrong)
                if e.enabled {
                    let p = NSBezierPath()
                    p.move(to: NSPoint(x: cb.minX + 3.2, y: cb.midY))
                    p.line(to: NSPoint(x: cb.minX + 6, y: cb.maxY - 3.5))
                    p.line(to: NSPoint(x: cb.maxX - 3, y: cb.minY + 3.5))
                    p.lineWidth = 1.8
                    NSColor.white.setStroke()
                    p.stroke()
                }
                tx = cb.maxX + 8
            } else {
                checkRects.append(.zero)
            }
            let fg = e.enabled || e.pass == nil ? Theme.text : Theme.faint
            drawText(e.title, in: NSRect(x: tx, y: r.minY + 5, width: r.maxX - tx - 8, height: 16), Fonts.ui(11.5, .semibold), fg)
            var note: String
            if e.snapshot == nil { note = e.pass == nil ? "" : "off" }
            else if e.pass == nil && !e.isFinal { note = "\(e.delta) instructions" }
            else if !e.changed { note = "no change" }
            else { note = e.delta == 0 ? "rewrote instructions" : (e.delta < 0 ? "\(e.delta) instructions" : "+\(e.delta) instructions") }
            let nc: NSColor = e.delta < 0 ? Theme.green : (e.changed ? Theme.amber : Theme.faint)
            drawText(note, in: NSRect(x: tx, y: r.minY + 21, width: r.maxX - tx - 8, height: 14), Fonts.tiny, e.snapshot == nil ? Theme.faint : nc)
            y += h + 4
        }
    }

    override func mouseDown(with event: NSEvent) {
        let p = convert(event.locationInWindow, from: nil)
        for (k, r) in presetRects.enumerated() where r.contains(p) {
            onPreset?(k == 1)
            return
        }
        for (k, r) in rects.enumerated() where r.contains(p) {
            if let pass = entries[k].pass, checkRects[k].insetBy(dx: -4, dy: -4).contains(p) {
                onToggle?(pass, !entries[k].enabled)
                return
            }
            if entries[k].snapshot != nil {
                selected = k
                onSelect?(k)
                needsDisplay = true
            }
            return
        }
    }
}

// MARK: - Control-flow graph

final class CFGCanvas: FlippedView {
    var function: IRFunction?
    var changed: Set<Int> = []
    var selectedLine: Int?
    private var boxes: [Int: NSRect] = [:]
    private var order: [Int] = []
    private var size = NSSize(width: 400, height: 300)
    let boxW: CGFloat = 330

    override var intrinsicContentSize: NSSize { size }

    func recompute() {
        boxes = [:]
        order = []
        guard let f = function else { return }
        var y: CGFloat = 20
        let lineH: CGFloat = 15
        let x: CGFloat = 70
        for b in f.blocks {
            let n = min(b.insts.count, 8) + (b.insts.count > 8 ? 1 : 0) + 1
            let h = 24 + CGFloat(n) * lineH + 6
            boxes[b.id] = NSRect(x: x, y: y, width: boxW, height: h)
            order.append(b.id)
            y += h + 34
        }
        size = NSSize(width: x + boxW + 150, height: y + 10)
        invalidateIntrinsicContentSize()
        needsDisplay = true
    }

    override func draw(_ dirtyRect: NSRect) {
        Theme.panel.setFill()
        dirtyRect.fill()
        // Graph-paper background.
        for gx in stride(from: CGFloat(0), to: bounds.width, by: 20) { line(NSPoint(x: gx, y: 0), NSPoint(x: gx, y: bounds.height), Theme.grid) }
        for gy in stride(from: CGFloat(0), to: bounds.height, by: 20) { line(NSPoint(x: 0, y: gy), NSPoint(x: bounds.width, y: gy), Theme.grid) }
        guard let f = function else { return }
        // Edges first.
        var rightLane: CGFloat = 0, leftLane: CGFloat = 0
        for (k, id) in order.enumerated() {
            guard let b = f.block(id), let r = boxes[id] else { continue }
            let succ = b.term.successors
            for (si, s) in succ.enumerated() {
                guard let t = boxes[s] else { continue }
                let label: String
                if case .branch = b.term { label = si == 0 ? "true" : "false" } else { label = "" }
                let color = label == "true" ? Theme.green : label == "false" ? Theme.crimson : Theme.secondary
                let isNext = k + 1 < order.count && order[k + 1] == s
                if isNext {
                    let a = NSPoint(x: r.midX + (succ.count > 1 ? -40 : 0), y: r.maxY)
                    let e = NSPoint(x: t.midX + (succ.count > 1 ? -40 : 0), y: t.minY)
                    line(a, e, color, 1.6)
                    arrowHead(at: e, from: a, color)
                    if !label.isEmpty { drawText(label, at: NSPoint(x: a.x + 5, y: a.y + 8), Fonts.tiny, color) }
                } else if t.minY > r.minY {
                    rightLane += 14
                    let lx = r.maxX + 12 + rightLane
                    let p = NSBezierPath()
                    p.move(to: NSPoint(x: r.maxX, y: r.maxY - 10))
                    p.line(to: NSPoint(x: lx, y: r.maxY - 10))
                    p.line(to: NSPoint(x: lx, y: t.minY + 12))
                    p.line(to: NSPoint(x: t.maxX, y: t.minY + 12))
                    p.lineWidth = 1.6
                    color.setStroke()
                    p.stroke()
                    arrowHead(at: NSPoint(x: t.maxX, y: t.minY + 12), from: NSPoint(x: lx, y: t.minY + 12), color)
                    if !label.isEmpty { drawText(label, at: NSPoint(x: r.maxX + 4, y: r.maxY - 24), Fonts.tiny, color) }
                } else {
                    // A back edge: this is a loop.
                    leftLane += 12
                    let lx = r.minX - 12 - leftLane
                    let p = NSBezierPath()
                    p.move(to: NSPoint(x: r.minX, y: r.maxY - 10))
                    p.line(to: NSPoint(x: lx, y: r.maxY - 10))
                    p.line(to: NSPoint(x: lx, y: t.minY + 12))
                    p.line(to: NSPoint(x: t.minX, y: t.minY + 12))
                    p.lineWidth = 1.8
                    let loopColor = label.isEmpty ? Theme.ink : color
                    loopColor.setStroke()
                    p.setLineDash([5, 3], count: 2, phase: 0)
                    p.stroke()
                    arrowHead(at: NSPoint(x: t.minX, y: t.minY + 12), from: NSPoint(x: lx, y: t.minY + 12), loopColor)
                    drawText(label.isEmpty ? "loop" : label, at: NSPoint(x: lx - 2, y: (r.maxY + t.minY) / 2), Fonts.tiny, loopColor)
                }
            }
        }
        for id in order {
            guard let b = f.block(id), let r = boxes[id] else { continue }
            let hasSel = selectedLine != nil && (b.insts.contains { $0.line == selectedLine } || b.termLine == selectedLine)
            drawCard(r, radius: 6, fill: hasSel ? NSColor(hex: 0xFFF8E3) : Theme.panel)
            if hasSel { strokeRounded(r, 6, Theme.amber, 1.5) }
            let head = NSRect(x: r.minX, y: r.minY, width: r.width, height: 22)
            fillRounded(head, 6, NSColor(hex: 0xEFE7D8))
            NSColor(hex: 0xEFE7D8).setFill()
            NSRect(x: r.minX, y: r.minY + 12, width: r.width, height: 10).fill()
            drawText(b.name + ":", at: NSPoint(x: r.minX + 8, y: r.minY + 4), Fonts.mono(11, .semibold), Theme.label)
            var y = r.minY + 26
            for i in b.insts.prefix(8) {
                let a = irAttributed(IRPrinter.inst(i, f), font: Fonts.mono(10))
                if changed.contains(i.id) {
                    Theme.okFill.setFill()
                    NSRect(x: r.minX + 3, y: y, width: r.width - 6, height: 15).fill()
                }
                drawAttributed(a, in: NSRect(x: r.minX + 10, y: y + 1, width: r.width - 16, height: 14))
                y += 15
            }
            if b.insts.count > 8 {
                drawText("… \(b.insts.count - 8) more", at: NSPoint(x: r.minX + 10, y: y + 1), Fonts.mono(10), Theme.faint)
                y += 15
            }
            drawAttributed(irAttributed(IRPrinter.term(b.term, f), font: Fonts.mono(10, .semibold)), in: NSRect(x: r.minX + 10, y: y + 1, width: r.width - 16, height: 14))
        }
    }
}

// MARK: - IR view

final class IRView: NSView, StationView {
    let passes = PassList()
    let listing = ListPanel()
    let cfg = CFGCanvas()
    lazy var cfgPanel = CanvasPanel(cfg)
    let mode = Segmented(["Listing", "Control-flow graph"])
    let functionPicker = PopUp(["main"])
    let explanation = NSTextField(wrappingLabelWithString: "")
    let empty = EmptyNote()
    weak var model: AppModel?
    private var functions: [String] = []

    override init(frame: NSRect) {
        super.init(frame: frame)
        for v in [passes, listing, cfgPanel, mode, functionPicker, explanation, empty] as [NSView] { addSubview(v) }
        explanation.font = Fonts.small
        explanation.textColor = Theme.secondary
        cfgPanel.isHidden = true
        mode.onChange = { [weak self] k in
            self?.listing.isHidden = k == 1
            self?.cfgPanel.isHidden = k == 0
            self?.functionPicker.isHidden = k == 0
            self?.needsLayout = true
        }
        functionPicker.isHidden = true
        functionPicker.onChange = { [weak self] k in
            guard let self, k < self.functions.count else { return }
            self.model?.focusFunction = self.functions[k]
        }
        passes.onSelect = { [weak self] k in
            guard let self, let snap = self.passes.entries[k].snapshot else { return }
            self.model?.irSnapshot = snap
        }
        passes.onToggle = { [weak self] p, on in
            guard let m = self?.model else { return }
            var o = m.options
            if on { o.passes.insert(p) } else { o.passes.remove(p) }
            m.irSnapshot = nil
            m.options = o
        }
        passes.onPreset = { [weak self] all in
            guard let m = self?.model else { return }
            var o = m.options
            o.passes = all ? Set(PassID.allCases) : []
            m.irSnapshot = nil
            m.options = o
        }
        listing.list.onClick = { [weak self] _, r in
            self?.model?.selection = Selection(line: r.line, irInst: r.irInst, origin: .ir)
        }
        listing.list.highlight = { [weak self] r in
            guard let m = self?.model else { return nil }
            if let l = m.selection.line, r.line == l { return Theme.selection }
            return nil
        }
    }
    required init?(coder: NSCoder) { fatalError() }

    override var isFlipped: Bool { true }

    override func layout() {
        super.layout()
        let pw: CGFloat = 250
        passes.frame = NSRect(x: 0, y: 0, width: pw, height: bounds.height)
        let x = pw + 10
        let ms = mode.intrinsicContentSize
        mode.frame = NSRect(x: x, y: 8, width: ms.width, height: 24)
        functionPicker.frame = NSRect(x: mode.frame.maxX + 10, y: 7, width: 170, height: 26)
        explanation.frame = NSRect(x: x, y: 38, width: bounds.width - x - 12, height: 44)
        let top: CGFloat = 86
        listing.frame = NSRect(x: pw, y: top, width: bounds.width - pw, height: bounds.height - top)
        cfgPanel.frame = listing.frame
        empty.frame = bounds
    }

    override func draw(_ dirtyRect: NSRect) {
        Theme.panel.setFill()
        bounds.fill()
        line(NSPoint(x: 250, y: 85.5), NSPoint(x: bounds.width, y: 85.5), Theme.border)
    }

    func reload(_ model: AppModel, _ event: ModelEvent) {
        self.model = model
        guard !model.isAssembly, let c = model.shown, !c.snapshots.isEmpty else {
            empty.isHidden = false
            empty.title = "No IR"
            empty.message = model.isAssembly ? "Hand-written assembly skips the compiler." : "Fix the errors in the program first."
            empty.needsDisplay = true
            return
        }
        empty.isHidden = true
        let snaps = c.snapshots
        let shownIndex = min(model.irSnapshot ?? snaps.count - 1, snaps.count - 1)
        // Pass list entries: all passes, with the ones that ran pointing at their snapshot.
        var entries: [PassList.Entry] = [PassList.Entry(title: "Unoptimised IR", pass: nil, snapshot: 0, delta: snaps[0].instructions,
                                                        changed: false, enabled: true, isFinal: false)]
        var selectedEntry = 0
        for p in PassID.allCases {
            let k = snaps.firstIndex { $0.pass == p }
            let delta = k.map { snaps[$0].instructions - snaps[$0 - 1].instructions } ?? 0
            entries.append(PassList.Entry(title: p.shortTitle + " — " + p.title.components(separatedBy: " (").first!.replacingOccurrences(of: "Promote locals to registers", with: "locals → registers"),
                                          pass: p, snapshot: k, delta: delta, changed: k.map { snaps[$0].changed } ?? false,
                                          enabled: c.options.passes.contains(p), isFinal: false))
            if k == shownIndex { selectedEntry = entries.count - 1 }
        }
        if let last = snaps.last, last.pass == nil, snaps.count > 1 {
            let k = snaps.count - 1
            entries.append(PassList.Entry(title: "Repeat until nothing changes", pass: nil, snapshot: k,
                                          delta: last.instructions - snaps[k - 1].instructions, changed: last.changed, enabled: true, isFinal: true))
            if k == shownIndex { selectedEntry = entries.count - 1 }
        }
        if shownIndex == 0 { selectedEntry = 0 }
        passes.entries = entries
        passes.selected = selectedEntry
        passes.needsDisplay = true

        let snap = snaps[shownIndex]
        let prev = shownIndex > 0 ? snaps[shownIndex - 1].module : nil
        if let p = snap.pass {
            explanation.stringValue = p.explanation
        } else if shownIndex == 0 {
            explanation.stringValue = "Three-address code straight from the syntax tree, as -O0 would make it: every variable has a stack slot ($x), every read is a load and every write a store. %t registers are compiler temporaries; there are as many as needed."
        } else {
            explanation.stringValue = "Passes feed each other — folding a constant makes code dead, removing code empties blocks — so the cheap passes repeat until the IR stops changing. This is what the back end receives."
        }
        // Instructions that are new or rewritten compared with the previous step.
        var changed = Set<Int>()
        var removed = 0
        if let prev {
            var before: [Int: InstKind] = [:]
            for f in prev.functions { for b in f.blocks { for i in b.insts { before[i.id] = i.kind } } }
            var after = Set<Int>()
            for f in snap.module.functions {
                for b in f.blocks {
                    for i in b.insts {
                        after.insert(i.id)
                        if before[i.id] != i.kind { changed.insert(i.id) }
                    }
                }
            }
            removed = before.keys.filter { !after.contains($0) }.count
        }
        functions = snap.module.functions.map { $0.name }
        let focus = model.focusFunction.flatMap { functions.contains($0) ? $0 : nil } ?? functions.first { $0 == "main" } ?? functions.first ?? ""
        functionPicker.set(functions, selected: functions.firstIndex(of: focus) ?? 0)
        cfg.function = snap.module.functions.first { $0.name == focus }
        cfg.changed = changed
        cfg.selectedLine = model.selection.line
        cfg.recompute()
        cfgPanel.needsLayout = true

        var rows: [Row] = []
        let summary = "\(snap.instructions) instructions" + (prev != nil ? " · \(changed.count) new or rewritten (green) · \(removed) removed" : "")
        rows.append(Row([Cell(summary, Fonts.small, Theme.secondary)], height: 20))
        for f in snap.module.functions {
            rows.append(.spacer(4))
            let params = f.params.map { f.vregs[$0].name }.joined(separator: ", ")
            var hdr = Row.header("function \(f.name)(\(params))", detail: f.slots.isEmpty ? "" : "stack slots: " + f.slots.map { "\($0.name) \($0.size)B" }.joined(separator: ", "))
            hdr.line = f.line
            rows.append(hdr)
            let preds = f.predecessors()
            for b in f.blocks {
                let from = (preds[b.id] ?? []).compactMap { f.block($0)?.name }
                var br = Row([Cell(NSAttributedString(b.name + ":", Fonts.mono(12, .semibold), Theme.label), width: 200),
                              Cell(from.isEmpty ? "" : "← from " + from.joined(separator: ", "), Fonts.small, Theme.faint)], height: 22)
                br.kind = .section
                rows.append(br)
                for i in b.insts {
                    var r = Row([Cell(irAttributed(IRPrinter.inst(i, f)), leftPad: 18), Cell(i.line > 0 ? "line \(i.line)" : "", Fonts.mono(10), Theme.faint, width: 60, align: .right)])
                    r.line = i.line
                    r.irInst = i.id
                    if changed.contains(i.id) { r.background = Theme.okFill }
                    rows.append(r)
                }
                var tr = Row([Cell(irAttributed(IRPrinter.term(b.term, f)), leftPad: 18), Cell(b.termLine > 0 ? "line \(b.termLine)" : "", Fonts.mono(10), Theme.faint, width: 60, align: .right)])
                tr.line = b.termLine
                tr.irInst = b.termID
                rows.append(tr)
            }
        }
        if event != .selection || listing.list.rows.isEmpty {
            listing.set(rows)
        } else {
            listing.list.needsDisplay = true
        }
        if event == .selection, model.selection.origin != .ir, let l = model.selection.line,
           let k = listing.list.rows.firstIndex(where: { $0.line == l && $0.irInst != nil }) {
            listing.list.reveal(k)
        }
    }
}
