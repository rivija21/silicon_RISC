import AppKit
import SiliconCore

func sizeText(_ b: Int) -> String { b >= 1024 ? "\(b / 1024) KiB" : "\(b) B" }

/// Registers → L1 → L2 → DRAM, with live statistics on each level.
final class HierarchyCanvas: FlippedView {
    weak var model: AppModel?
    var selectedCache = 1
    var onSelectCache: ((Int) -> Void)?
    private var cacheRects: [Int: NSRect] = [:]

    override func draw(_ dirtyRect: NSRect) {
        Theme.panelAlt.setFill()
        bounds.fill()
        line(NSPoint(x: bounds.maxX - 0.5, y: 0), NSPoint(x: bounds.maxX - 0.5, y: bounds.height), Theme.border)
        guard let m = model, let p = m.pipeline else { return }
        let h = p.hierarchy
        drawText("Memory hierarchy", at: NSPoint(x: 12, y: 10), Fonts.subheading, Theme.heading)
        drawText("small & fast at the top, big & slow at the bottom", at: NSPoint(x: 12, y: 28), Fonts.tiny, Theme.secondary)
        let W = bounds.width - 24
        var y: CGFloat = 50
        cacheRects = [:]
        let lastFetch = h.recent.last { $0.kind == .fetch }
        let lastData = h.recent.last { $0.kind != .fetch }
        func level(_ r: NSRect, _ title: String, _ detail: String, _ stats: CacheStats?, fill: NSColor, lit: Bool, selectable: Int? = nil) {
            drawCard(r, radius: 7, fill: fill)
            if let s = selectable, s == selectedCache { strokeRounded(r, 7, Theme.ink, 2) }
            if lit { strokeRounded(r.insetBy(dx: -2, dy: -2), 9, Theme.amber, 2) }
            drawText(title, at: NSPoint(x: r.minX + 8, y: r.minY + 5), Fonts.ui(11.5, .bold), Theme.text)
            drawText(detail, in: NSRect(x: r.minX + 8, y: r.minY + 21, width: r.width - 14, height: 13), Fonts.tiny, Theme.secondary)
            if let s = stats {
                let rate = s.accesses == 0 ? "—" : String(format: "%.1f%% hits", 100 * s.hitRate)
                drawText(rate, in: NSRect(x: r.minX + 8, y: r.minY + 35, width: r.width - 14, height: 15), Fonts.mono(11, .bold), s.accesses == 0 ? Theme.faint : s.hitRate > 0.9 ? Theme.green : s.hitRate > 0.7 ? Theme.amber : Theme.crimson)
                drawText("\(formatCount(s.hits)) / \(formatCount(s.accesses))", in: NSRect(x: r.minX + 8, y: r.minY + 51, width: r.width - 14, height: 13), Fonts.mono(9.5), Theme.secondary)
                let bar = NSRect(x: r.minX + 8, y: r.maxY - 9, width: r.width - 16, height: 4)
                fillRounded(bar, 2, Theme.errorFill)
                fillRounded(NSRect(x: bar.minX, y: bar.minY, width: bar.width * CGFloat(s.hitRate), height: 4), 2, Theme.green)
            }
            if let s = selectable { cacheRects[s] = r }
        }
        let regR = NSRect(x: 12 + W * 0.2, y: y, width: W * 0.6, height: 40)
        level(regR, "Registers", "32 × 4 bytes", nil, fill: NSColor(hex: 0xE5F1EA), lit: false)
        y = regR.maxY + 22
        let cfg = h.config
        let half = (W - 10) / 2
        let iR = NSRect(x: 12, y: y, width: half, height: 74)
        let dR = NSRect(x: 12 + half + 10, y: y, width: half, height: 74)
        let disabled = !cfg.enabled
        level(iR, "L1 I-cache", disabled ? "off (perfect memory)" : "\(sizeText(cfg.l1i.size)), \(cfg.l1i.associativityName)", disabled ? nil : h.l1i.stats,
              fill: Theme.panel, lit: lastFetch?.time == p.cycle && lastFetch?.servedBy != "L1", selectable: 0)
        level(dR, "L1 D-cache", disabled ? "off (perfect memory)" : "\(sizeText(cfg.l1d.size)), \(cfg.l1d.associativityName)", disabled ? nil : h.l1d.stats,
              fill: Theme.panel, lit: lastData?.time == p.cycle && lastData?.servedBy != "L1", selectable: 1)
        line(NSPoint(x: regR.midX - 30, y: regR.maxY), NSPoint(x: iR.midX, y: iR.minY), Theme.borderStrong, 1.5)
        line(NSPoint(x: regR.midX + 30, y: regR.maxY), NSPoint(x: dR.midX, y: dR.minY), Theme.borderStrong, 1.5)
        drawText("1 cycle", at: NSPoint(x: dR.maxX - 40, y: dR.minY - 14), Fonts.tiny, Theme.secondary)
        y = iR.maxY + 24
        if let l2 = h.l2 {
            let r = NSRect(x: 12 + W * 0.1, y: y, width: W * 0.8, height: 74)
            level(r, "L2 cache (unified)", "\(sizeText(l2.config.size)), \(l2.config.associativityName)", l2.stats, fill: Theme.panel,
                  lit: (lastData?.time == p.cycle && lastData?.servedBy == "DRAM") || (lastFetch?.time == p.cycle && lastFetch?.servedBy == "DRAM"), selectable: 2)
            line(NSPoint(x: iR.midX, y: iR.maxY), NSPoint(x: r.midX - 40, y: r.minY), Theme.borderStrong, 1.5)
            line(NSPoint(x: dR.midX, y: dR.maxY), NSPoint(x: r.midX + 40, y: r.minY), Theme.borderStrong, 1.5)
            drawText("+\(l2.config.latency) cycles", at: NSPoint(x: r.maxX - 62, y: r.minY - 14), Fonts.tiny, Theme.secondary)
            y = r.maxY + 24
        }
        let dram = NSRect(x: 12, y: y, width: W, height: 48)
        drawCard(dram, radius: 7, fill: NSColor(hex: 0xF1E9DA))
        drawText("Main memory (DRAM)", at: NSPoint(x: dram.minX + 8, y: dram.minY + 6), Fonts.ui(11.5, .bold), Theme.text)
        drawText(disabled ? "every access takes 1 cycle (ideal)" : "+\(cfg.dramLatency) cycles · \(formatCount(h.dramAccesses)) accesses", in: NSRect(x: dram.minX + 8, y: dram.minY + 24, width: dram.width - 16, height: 14), Fonts.small, Theme.secondary)
        if h.l2 != nil {
            line(NSPoint(x: dram.midX, y: dram.minY - 24), NSPoint(x: dram.midX, y: dram.minY), Theme.borderStrong, 1.5)
        } else {
            line(NSPoint(x: iR.midX, y: iR.maxY), NSPoint(x: dram.midX - 30, y: dram.minY), Theme.borderStrong, 1.5)
            line(NSPoint(x: dR.midX, y: dR.maxY), NSPoint(x: dram.midX + 30, y: dram.minY), Theme.borderStrong, 1.5)
        }
        y = dram.maxY + 16
        // AMAT.
        if !disabled {
            let amat = h.amat(h.l1d)
            let l2part = h.l2.map { "\($0.config.latency) + \(String(format: "%.2f", $0.stats.missRate)) × \(cfg.dramLatency)" } ?? "\(cfg.dramLatency)"
            drawText("Average memory access time (data)", at: NSPoint(x: 12, y: y), Fonts.ui(11, .semibold), Theme.heading)
            y += 16
            let s = "AMAT = 1 + \(String(format: "%.3f", h.l1d.stats.missRate)) × (\(l2part))"
            drawText(s, in: NSRect(x: 12, y: y, width: W, height: 14), Fonts.mono(10), Theme.text)
            y += 15
            drawText(String(format: "     = %.2f cycles per access", amat), in: NSRect(x: 12, y: y, width: W, height: 14), Fonts.mono(10, .bold), Theme.ink)
            y += 22
            let total = h.stallCycles.values.reduce(0, +)
            drawText("Cycles lost waiting for memory: \(formatCount(total))", in: NSRect(x: 12, y: y, width: W, height: 14), Fonts.small, Theme.secondary)
        }
    }

    override func mouseDown(with event: NSEvent) {
        let p = convert(event.locationInWindow, from: nil)
        for (k, r) in cacheRects where r.contains(p) {
            onSelectCache?(k)
            return
        }
    }
}

/// Every line of one cache: sets down, ways across.
final class CacheGridCanvas: FlippedView {
    var cache: Cache?
    var now = 0
    var highlightSet: Int?
    private var size = NSSize(width: 300, height: 300)
    let rowH: CGFloat = 20

    override var intrinsicContentSize: NSSize { size }

    func recompute(width: CGFloat) {
        let sets = cache?.config.sets ?? 0
        size = NSSize(width: max(width, 120 + CGFloat(cache?.config.ways ?? 1) * 120), height: 128 + CGFloat(sets) * rowH + 20)
        invalidateIntrinsicContentSize()
        needsDisplay = true
    }

    override func draw(_ dirtyRect: NSRect) {
        Theme.panel.setFill()
        dirtyRect.fill()
        guard let c = cache else { return }
        let cfg = c.config
        drawText(cfg.name, at: NSPoint(x: 14, y: 10), Fonts.subheading, Theme.heading)
        let desc = "\(sizeText(cfg.size)) · \(cfg.blockSize)-byte blocks · \(cfg.sets) sets × \(cfg.ways) way\(cfg.ways == 1 ? "" : "s") (\(cfg.associativityName)) · \(cfg.replacement.rawValue) · \(cfg.write.rawValue.lowercased())"
        drawText(desc, in: NSRect(x: 14, y: 28, width: bounds.width - 28, height: 14), Fonts.small, Theme.secondary)
        // Address breakdown of the last access.
        let ay: CGFloat = 50
        if let e = c.last {
            let (tag, set, off) = c.split(e.address)
            let bits = [(cfg.tagBits, "tag", String(tag, radix: 16), Theme.fieldColor("rs1")),
                        (cfg.indexBits, "set index", "\(set)", Theme.fieldColor("funct3")),
                        (cfg.offsetBits, "offset", "\(off)", Theme.fieldColor("rd"))]
            let total = bounds.width - 28 - 92 - 200
            var x: CGFloat = 14
            drawText("last access", at: NSPoint(x: x, y: ay + 2), Fonts.tiny, Theme.secondary)
            drawText(hexAddr(e.address), at: NSPoint(x: x, y: ay + 15), Fonts.mono(11, .semibold), Theme.ink)
            x += 92
            for (n, name, val, color) in bits where n > 0 {
                let w = max(56, total * CGFloat(n) / 32)
                let r = NSRect(x: x, y: ay, width: w, height: 30)
                color.setFill()
                r.fill()
                Theme.borderStrong.setStroke()
                NSBezierPath(rect: r).stroke()
                drawText("\(name) (\(n) bits)", in: NSRect(x: r.minX + 4, y: r.minY + 2, width: w - 6, height: 12), Fonts.tiny, Theme.secondary)
                drawText(name == "tag" ? "0x" + val : val, in: NSRect(x: r.minX + 4, y: r.minY + 14, width: w - 6, height: 14), Fonts.mono(10.5, .semibold), Theme.text)
                x += w
            }
            let verdict: String
            if e.hit { verdict = "HIT in way \(e.way)" }
            else { verdict = "MISS (\(e.missKind?.rawValue ?? ""))" + (e.evicted != nil ? (e.writeback ? ", evicted a dirty block" : ", evicted a block") : "") }
            drawText(verdict, in: NSRect(x: x + 10, y: ay + 8, width: bounds.width - x - 16, height: 16), Fonts.ui(11, .bold), e.hit ? Theme.green : Theme.crimson)
        } else {
            drawText("No accesses yet — step the program.", at: NSPoint(x: 14, y: ay + 8), Fonts.small, Theme.faint)
        }
        // Grid.
        let gy: CGFloat = 96
        let setW: CGFloat = 46
        let wayW = (bounds.width - 28 - setW) / CGFloat(cfg.ways)
        drawText("set", in: NSRect(x: 14, y: gy, width: setW - 6, height: 14), Fonts.mono(9.5, .semibold), Theme.faint, align: .right)
        for w in 0..<cfg.ways {
            drawText("way \(w)", in: NSRect(x: 14 + setW + CGFloat(w) * wayW, y: gy, width: wayW, height: 14), Fonts.mono(9.5, .semibold), Theme.faint, align: .center)
        }
        let y0 = gy + 18
        let last = c.last
        for s in 0..<cfg.sets {
            let y = y0 + CGFloat(s) * rowH
            if y > dirtyRect.maxY + rowH || y + rowH < dirtyRect.minY - rowH { continue }
            let isLastSet = last?.set == s
            if isLastSet {
                Theme.selection.setFill()
                NSRect(x: 10, y: y, width: bounds.width - 20, height: rowH).fill()
            }
            drawText("\(s)", in: NSRect(x: 14, y: y, width: setW - 6, height: rowH), Fonts.mono(10, isLastSet ? .bold : .regular), isLastSet ? Theme.text : Theme.secondary, align: .right)
            for w in 0..<cfg.ways {
                let l = c.line(set: s, way: w)
                let r = NSRect(x: 14 + setW + CGFloat(w) * wayW + 2, y: y + 2, width: wayW - 4, height: rowH - 4)
                guard l.valid else {
                    fillRounded(r, 3, NSColor(hex: 0xF4F1EA))
                    drawText("empty", in: r, Fonts.tiny, Theme.faint, align: .center)
                    continue
                }
                let age = now - l.lastTouch
                var fill = NSColor(hex: 0xE6EEF9)
                if age <= 0 { fill = (last?.hit ?? false) && isLastSet && last?.way == w ? NSColor(hex: 0xBFE5CF) : NSColor(hex: 0xFFD9A8) }
                else if age < 30 { fill = NSColor(hex: 0xE6EEF9).blended(withFraction: 0.5 * (1 - CGFloat(age) / 30), of: NSColor(hex: 0xBFE5CF))! }
                fillRounded(r, 3, fill)
                if isLastSet && last?.way == w { strokeRounded(r, 3, last?.hit == true ? Theme.green : Theme.crimson, 1.6) }
                let base = c.blockAddress(set: s, tag: l.tag)
                let txt = wayW > 150 ? "tag \(String(l.tag, radix: 16))  ·  \(String(format: "%08x", base))" : "\(String(format: "%08x", base))"
                drawText(txt, in: r.insetBy(dx: 5, dy: 0), Fonts.mono(9.5), Theme.text)
                if l.dirty {
                    drawText("D", in: NSRect(x: r.maxX - 14, y: r.minY, width: 12, height: r.height), Fonts.mono(9, .bold), Theme.crimson, align: .center)
                }
            }
        }
    }
}

final class CacheStatsCanvas: FlippedView {
    var cache: Cache?
    var recent: [MemoryAccess] = []

    override func draw(_ dirtyRect: NSRect) {
        Theme.panelAlt.setFill()
        bounds.fill()
        line(NSPoint(x: 0.5, y: 0), NSPoint(x: 0.5, y: bounds.height), Theme.border)
        guard let c = cache else { return }
        let s = c.stats
        var y: CGFloat = 10
        drawText("Misses by cause (the 3 Cs)", at: NSPoint(x: 12, y: y), Fonts.subheading, Theme.heading)
        y += 22
        let total = max(1, s.misses)
        for (name, n, color, note) in [("Compulsory", s.compulsory, Theme.ink, "first touch of a block"),
                                       ("Capacity", s.capacity, Theme.amber, "cache too small for the working set"),
                                       ("Conflict", s.conflict, Theme.crimson, "too many blocks map to one set")] {
            drawText(name, at: NSPoint(x: 12, y: y), Fonts.ui(11, .semibold), Theme.text)
            drawText(formatCount(n), in: NSRect(x: 12, y: y, width: bounds.width - 24, height: 14), Fonts.mono(11, .semibold), color, align: .right)
            y += 15
            let bar = NSRect(x: 12, y: y, width: bounds.width - 24, height: 6)
            fillRounded(bar, 3, Theme.border)
            fillRounded(NSRect(x: bar.minX, y: y, width: bar.width * CGFloat(n) / CGFloat(total), height: 6), 3, color)
            y += 9
            drawText(note, at: NSPoint(x: 12, y: y), Fonts.tiny, Theme.secondary)
            y += 18
        }
        drawText("Write-backs of dirty blocks: \(formatCount(s.writebacks))", at: NSPoint(x: 12, y: y), Fonts.small, Theme.secondary)
        y += 24
        line(NSPoint(x: 10, y: y), NSPoint(x: bounds.width - 10, y: y), Theme.border)
        y += 8
        drawText("Recent accesses", at: NSPoint(x: 12, y: y), Fonts.subheading, Theme.heading)
        y += 20
        for a in recent.suffix(Int((bounds.height - y) / 15)).reversed() {
            let c: NSColor = a.servedBy == "L1" ? Theme.green : a.servedBy == "L2" ? Theme.amber : Theme.crimson
            drawText(a.kind == .fetch ? "fetch" : a.kind.rawValue, at: NSPoint(x: 12, y: y), Fonts.mono(9.5), Theme.secondary)
            drawText(hex8(a.address), at: NSPoint(x: 54, y: y), Fonts.mono(9.5), Theme.text)
            drawText(a.servedBy == "L1" ? "hit" : "\(a.servedBy) +\(a.penalty)", in: NSRect(x: 12, y: y, width: bounds.width - 24, height: 13), Fonts.mono(9.5, .semibold), c, align: .right)
            y += 15
        }
    }
}

final class MemoryView: NSView, StationView {
    let hierarchy = HierarchyCanvas()
    let grid = CacheGridCanvas()
    lazy var gridPanel = CanvasPanel(grid)
    let stats = CacheStatsCanvas()
    let which = Segmented(["L1 I-cache", "L1 D-cache", "L2"])
    let sizeP = PopUp([])
    let blockP = PopUp([])
    let waysP = PopUp([])
    let replP = PopUp(Replacement.allCases.map { $0.rawValue })
    let writeP = PopUp(WritePolicy.allCases.map { $0.rawValue })
    let enabled = Check("Caches on", isOn: true)
    let l2On = Check("L2", isOn: true)
    let empty = EmptyNote()
    weak var model: AppModel?
    private var selected = 1
    private let sizes = [64, 128, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768]
    private let blocks = [4, 8, 16, 32, 64, 128]
    private let waysList = [1, 2, 4, 8, 16]

    override init(frame: NSRect) {
        super.init(frame: frame)
        for v in [hierarchy, gridPanel, stats, which, sizeP, blockP, waysP, replP, writeP, enabled, l2On, empty] as [NSView] { addSubview(v) }
        which.onChange = { [weak self] k in
            self?.selected = k
            self?.hierarchy.selectedCache = k
            if let m = self?.model { self?.reload(m, .config) }
        }
        hierarchy.onSelectCache = { [weak self] k in
            self?.selected = k
            self?.which.selected = k
            self?.hierarchy.selectedCache = k
            if let m = self?.model { self?.reload(m, .config) }
        }
        sizeP.onChange = { [weak self] k in self?.edit { $0.size = self!.sizes[k] } }
        blockP.onChange = { [weak self] k in self?.edit { $0.blockSize = self!.blocks[k] } }
        waysP.onChange = { [weak self] k in self?.edit { $0.ways = self!.waysList[k] } }
        replP.onChange = { [weak self] k in self?.edit { $0.replacement = Replacement.allCases[k] } }
        writeP.onChange = { [weak self] k in self?.edit { $0.write = WritePolicy.allCases[k] } }
        enabled.onChange = { [weak self] on in
            guard let m = self?.model else { return }
            var h = m.pipelineConfig.hierarchy
            h.enabled = on
            if on && h.dramLatency == 0 { h = HierarchyConfig.teaching }
            m.pipelineConfig.hierarchy = h
        }
        l2On.onChange = { [weak self] on in
            guard let m = self?.model else { return }
            var h = m.pipelineConfig.hierarchy
            h.l2 = on ? HierarchyConfig.teaching.l2 : nil
            m.pipelineConfig.hierarchy = h
            if !on && self?.selected == 2 { self?.selected = 1 }
        }
    }
    required init?(coder: NSCoder) { fatalError() }

    override var isFlipped: Bool { true }

    private func edit(_ f: (inout CacheConfig) -> Void) {
        guard let m = model else { return }
        var h = m.pipelineConfig.hierarchy
        var c = selected == 0 ? h.l1i : selected == 1 ? h.l1d : (h.l2 ?? HierarchyConfig.teaching.l2!)
        f(&c)
        // Keep the configuration legal: blocks fit, ways fit.
        if c.blockSize > c.size { c.blockSize = c.size }
        while c.ways > c.size / c.blockSize { c.ways /= 2 }
        switch selected {
        case 0: h.l1i = c
        case 1: h.l1d = c
        default: h.l2 = c
        }
        m.pipelineConfig.hierarchy = h
    }

    override func layout() {
        super.layout()
        let hw: CGFloat = 270, sw: CGFloat = 220
        hierarchy.frame = NSRect(x: 0, y: 0, width: hw, height: bounds.height)
        let cx = hw + 10
        let ws = which.intrinsicContentSize
        which.frame = NSRect(x: cx, y: 8, width: ws.width, height: 24)
        enabled.frame = NSRect(x: which.frame.maxX + 14, y: 10, width: enabled.intrinsicContentSize.width, height: 20)
        l2On.frame = NSRect(x: enabled.frame.maxX + 10, y: 10, width: l2On.intrinsicContentSize.width, height: 20)
        var x = cx
        for (p, w) in [(sizeP, 92.0), (blockP, 88.0), (waysP, 96.0), (replP, 86.0), (writeP, 250.0)] {
            p.frame = NSRect(x: x, y: 38, width: CGFloat(w), height: 26)
            x += CGFloat(w) + 6
        }
        gridPanel.frame = NSRect(x: hw, y: 70, width: bounds.width - hw - sw, height: bounds.height - 70)
        stats.frame = NSRect(x: bounds.width - sw, y: 70, width: sw, height: bounds.height - 70)
        grid.recompute(width: gridPanel.bounds.width - 4)
        empty.frame = bounds
    }

    override func draw(_ dirtyRect: NSRect) {
        Theme.panel.setFill()
        bounds.fill()
        line(NSPoint(x: 270, y: 69.5), NSPoint(x: bounds.width, y: 69.5), Theme.border)
    }

    func reload(_ model: AppModel, _ event: ModelEvent) {
        self.model = model
        hierarchy.model = model
        guard let p = model.pipeline else {
            empty.isHidden = false
            empty.title = "No program running"
            empty.message = "Fix the errors in the program first."
            empty.needsDisplay = true
            return
        }
        empty.isHidden = true
        let h = p.hierarchy
        if h.l2 == nil && selected == 2 { selected = 1 }
        which.selected = selected
        which.enabledMask = [true, true, h.l2 != nil]
        hierarchy.selectedCache = selected
        enabled.isOn = h.config.enabled
        l2On.isOn = h.l2 != nil
        let cache = selected == 0 ? h.l1i : selected == 1 ? h.l1d : h.l2!
        let cfg = cache.config
        sizeP.set(sizes.map { "size " + sizeText($0) }, selected: sizes.firstIndex(of: cfg.size) ?? 3)
        blockP.set(blocks.map { "block \($0) B" }, selected: blocks.firstIndex(of: cfg.blockSize) ?? 2)
        let wl = waysList.filter { $0 <= cfg.blocks }
        waysP.set(wl.map { $0 == 1 ? "direct-mapped" : "\($0)-way" }, selected: wl.firstIndex(of: cfg.ways) ?? 0)
        replP.button.selectItem(at: Replacement.allCases.firstIndex(of: cfg.replacement) ?? 0)
        writeP.button.selectItem(at: WritePolicy.allCases.firstIndex(of: cfg.write) ?? 0)
        for v in [sizeP, blockP, waysP, replP, writeP] { v.button.isEnabled = h.config.enabled }
        grid.cache = h.config.enabled ? cache : nil
        grid.now = p.cycle
        grid.recompute(width: gridPanel.bounds.width - 4)
        gridPanel.needsLayout = true
        if let e = cache.last, model.running == false {
            let y = 114 + CGFloat(e.set) * grid.rowH
            grid.scrollToVisible(NSRect(x: 0, y: y - 30, width: 10, height: 80))
        }
        stats.cache = h.config.enabled ? cache : nil
        stats.recent = h.recent.filter { selected == 0 ? $0.kind == .fetch : selected == 1 ? $0.kind != .fetch : $0.servedBy != "L1" }
        hierarchy.needsDisplay = true
        grid.needsDisplay = true
        stats.needsDisplay = true
    }
}
