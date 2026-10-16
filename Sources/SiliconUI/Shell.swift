import AppKit
import SiliconCore

/// The row of numbered stations from Source to Memory.
final class JourneyBar: NSView {
    weak var model: AppModel?
    private var rects: [NSRect] = []
    private var hovered: Int? { didSet { if hovered != oldValue { needsDisplay = true } } }

    override init(frame: NSRect) {
        super.init(frame: frame)
        addTrackingArea(NSTrackingArea(rect: .zero, options: [.mouseMoved, .mouseEnteredAndExited, .activeInKeyWindow, .inVisibleRect], owner: self))
    }
    required init?(coder: NSCoder) { fatalError() }

    override var isFlipped: Bool { true }

    private func status(_ s: Station) -> Int {
        // 0 ok, 1 failed here, 2 not reached / skipped
        guard let m = model, let c = m.compilation else { return 2 }
        if c.isAssembly && s.needsC { return 2 }
        let f = c.failedStage
        let order: [Station: Stage] = [.tokens: .lexer, .tree: .parser, .symbols: .semantic, .ir: .ir, .asm: .codegen, .machine: .assembler, .link: .linker]
        guard let fs = f else { return 0 }
        let stagesInOrder: [Stage] = [.lexer, .preprocessor, .parser, .semantic, .ir, .codegen, .assembler, .linker]
        let fi = stagesInOrder.firstIndex(of: fs) ?? 0
        if let st = order[s] {
            let si = stagesInOrder.firstIndex(of: st) ?? 0
            if si == fi { return 1 }
            if si > fi { return 2 }
            return 0
        }
        if s == .source { return 0 }
        return m.good != nil ? 0 : 2
    }

    override func draw(_ dirtyRect: NSRect) {
        Theme.background.setFill()
        bounds.fill()
        guard let m = model else { return }
        let all = Station.allCases
        let pad: CGFloat = 14
        let w = (bounds.width - pad * 2) / CGFloat(all.count)
        rects = []
        let cy: CGFloat = 20
        // The rail.
        line(NSPoint(x: pad + w / 2, y: cy), NSPoint(x: bounds.width - pad - w / 2, y: cy), Theme.borderStrong, 2)
        // Layer brackets.
        let groups: [(String, ClosedRange<Int>)] = [("front end", 0...3), ("middle end", 4...4), ("back end", 5...7), ("hardware", 8...9)]
        for (name, range) in groups {
            let x0 = pad + CGFloat(range.lowerBound) * w + 6, x1 = pad + CGFloat(range.upperBound + 1) * w - 6
            let y = bounds.height - 9
            line(NSPoint(x: x0, y: y - 4), NSPoint(x: x0, y: y), Theme.borderStrong)
            line(NSPoint(x: x0, y: y), NSPoint(x: x1, y: y), Theme.borderStrong)
            line(NSPoint(x: x1, y: y - 4), NSPoint(x: x1, y: y), Theme.borderStrong)
            let sz = textSize(name, Fonts.tiny)
            let lr = NSRect(x: (x0 + x1) / 2 - sz.width / 2 - 5, y: y - 6, width: sz.width + 10, height: 12)
            Theme.background.setFill()
            lr.fill()
            drawText(name, at: NSPoint(x: lr.minX + 5, y: lr.minY - 1), Fonts.tiny, Theme.faint)
        }
        for (k, s) in all.enumerated() {
            let x = pad + CGFloat(k) * w
            let r = NSRect(x: x, y: 2, width: w, height: bounds.height - 16)
            rects.append(r)
            let st = status(s)
            let isSel = m.station == s
            let c = NSPoint(x: r.midX, y: cy)
            let rad: CGFloat = isSel ? 13 : 11
            let circle = NSRect(x: c.x - rad, y: c.y - rad, width: rad * 2, height: rad * 2)
            let fill: NSColor = isSel ? Theme.ink : st == 1 ? Theme.crimson : st == 2 ? NSColor(hex: 0xE7E0D2) : (hovered == k ? Theme.hover : Theme.panel)
            fillRounded(circle, rad, fill)
            strokeRounded(circle, rad, isSel ? Theme.ink : st == 1 ? Theme.crimson : Theme.borderStrong, 1.5)
            let numColor: NSColor = isSel || st == 1 ? .white : st == 2 ? Theme.faint : Theme.ink
            drawText("\(k + 1)", in: circle, Fonts.ui(isSel ? 12 : 11, .bold), numColor, align: .center)
            drawText(s.title, in: NSRect(x: r.minX, y: cy + 15, width: r.width, height: 15), Fonts.ui(11, isSel ? .bold : .medium),
                     isSel ? Theme.ink : st == 2 ? Theme.faint : st == 1 ? Theme.crimson : Theme.text, align: .center)
        }
    }

    override func mouseMoved(with event: NSEvent) {
        let p = convert(event.locationInWindow, from: nil)
        hovered = rects.firstIndex { $0.contains(p) }
    }
    override func mouseExited(with event: NSEvent) { hovered = nil }
    override func mouseDown(with event: NSEvent) {
        let p = convert(event.locationInWindow, from: nil)
        if let k = rects.firstIndex(where: { $0.contains(p) }) { model?.setStation(Station.allCases[k]) }
    }
}

/// The explanation under the station view.
final class LessonStrip: FlippedView {
    var station: Station = .source { didSet { needsDisplay = true } }

    override func draw(_ dirtyRect: NSRect) {
        let r = bounds.insetBy(dx: 0.5, dy: 0.5)
        drawCard(r, radius: 8, fill: NSColor(hex: 0xFBF6EA))
        let (title, body) = Lessons.text(station)
        let icon = NSImage(systemSymbolName: "book", accessibilityDescription: nil)?
            .withSymbolConfiguration(.init(pointSize: 13, weight: .medium))?.tinted(Theme.heading)
        icon?.draw(in: NSRect(x: 12, y: 10, width: 16, height: 14))
        drawText(title, at: NSPoint(x: 34, y: 8), Fonts.serif(13, .semibold), Theme.heading)
        let p = NSMutableParagraphStyle()
        p.lineSpacing = 2
        NSAttributedString(string: body, attributes: [.font: Fonts.ui(11.5), .foregroundColor: Theme.text, .paragraphStyle: p])
            .draw(with: NSRect(x: 14, y: 28, width: bounds.width - 28, height: bounds.height - 32), options: [.usesLineFragmentOrigin, .truncatesLastVisibleLine])
    }
}

/// Program output and problems, under the editor.
final class ConsoleView: NSView {
    let tabs = Segmented(["Output", "Problems"])
    let outputView = NSTextView()
    let outputScroll = NSScrollView()
    let problems = ListPanel()
    weak var model: AppModel?
    var onSelectLine: ((Int, SourceRange?) -> Void)?
    private var lastText = ""

    override init(frame: NSRect) {
        super.init(frame: frame)
        outputView.isEditable = false
        outputView.font = Fonts.mono(11.5)
        outputView.backgroundColor = NSColor(hex: 0xFFFEFA)
        outputView.textColor = Theme.text
        outputView.textContainerInset = NSSize(width: 8, height: 6)
        outputView.isVerticallyResizable = true
        outputView.autoresizingMask = [.width]
        outputScroll.documentView = outputView
        outputScroll.hasVerticalScroller = true
        outputScroll.autohidesScrollers = true
        outputScroll.borderType = .noBorder
        addSubview(tabs)
        addSubview(outputScroll)
        addSubview(problems)
        problems.isHidden = true
        tabs.onChange = { [weak self] k in
            self?.outputScroll.isHidden = k != 0
            self?.problems.isHidden = k != 1
        }
        problems.list.onClick = { [weak self] _, r in
            if let l = r.line { self?.onSelectLine?(l, nil) }
        }
    }
    required init?(coder: NSCoder) { fatalError() }

    override var isFlipped: Bool { true }

    override func layout() {
        super.layout()
        let s = tabs.intrinsicContentSize
        tabs.frame = NSRect(x: 8, y: 6, width: s.width, height: 24)
        outputScroll.frame = NSRect(x: 1, y: 36, width: bounds.width - 2, height: bounds.height - 37)
        problems.frame = outputScroll.frame
        outputView.frame.size.width = outputScroll.contentSize.width
    }

    override func draw(_ dirtyRect: NSRect) {
        drawCard(bounds.insetBy(dx: 0.5, dy: 0.5))
        guard let m = model else { return }
        var status = ""
        var color = Theme.secondary
        if let p = m.pipeline {
            if let f = p.output.fault { status = "stopped: \(f)"; color = Theme.crimson }
            else if p.halted { status = "exited with code \(p.output.exitCode ?? 0) after \(formatCount(p.stats.cycles)) cycles"; color = Theme.green }
            else if p.cycle > 0 { status = "running… cycle \(formatCount(p.cycle))" }
            else { status = "ready" }
        } else if m.hasErrors {
            status = "fix the problems to run"
            color = Theme.crimson
        }
        drawText(status, in: NSRect(x: tabs.frame.maxX + 10, y: 10, width: bounds.width - tabs.frame.maxX - 20, height: 16), Fonts.small, color, align: .right)
        line(NSPoint(x: 1, y: 35.5), NSPoint(x: bounds.width - 1, y: 35.5), Theme.border)
    }

    func reload(_ model: AppModel) {
        self.model = model
        let text = model.pipeline?.output.text ?? ""
        if text != lastText {
            lastText = text
            outputView.string = text.isEmpty ? "" : text
            outputView.scrollToEndOfDocument(nil)
        }
        if text.isEmpty {
            outputView.string = model.pipeline == nil ? "" : "(nothing printed yet — press Run)"
            outputView.textColor = Theme.faint
            lastText = ""
        } else {
            outputView.textColor = Theme.text
        }
        let diags = model.compilation?.diagnostics ?? []
        let problemsCount = diags.filter { $0.severity != .note }.count
        tabs.options = ["Output", problemsCount > 0 ? "Problems (\(problemsCount))" : "Problems"]
        var rows: [Row] = []
        for d in diags {
            let (label, fill, fg): (String, NSColor, NSColor) = d.severity == .error ? ("error", Theme.errorFill, Theme.crimson) : d.severity == .warning ? ("warning", NSColor(hex: 0xFFF1C9), Theme.amber) : ("note", NSColor(hex: 0xE6EEF9), Theme.ink)
            var r = Row([Cell(NSAttributedString(label, Fonts.ui(10, .bold), fg), width: 60, badge: fill),
                         Cell(d.line > 0 ? "\(d.line)" : "", Fonts.mono(10.5), Theme.secondary, width: 34, align: .right),
                         Cell(NSAttributedString(d.message, Fonts.small, Theme.text), leftPad: 8)], height: 24)
            r.line = d.line > 0 ? d.line : nil
            rows.append(r)
        }
        if rows.isEmpty { rows = [Row([Cell("No problems.", Fonts.small, Theme.faint)])] }
        problems.set(rows)
        if model.hasErrors && tabs.selected == 0 && (model.compilation?.errors.count ?? 0) > 0 {
            tabs.selected = 1
            outputScroll.isHidden = true
            problems.isHidden = false
        }
        needsDisplay = true
    }
}

/// Title, example picker, language and optimisation level, and the clock controls.
final class TopBar: NSView {
    weak var model: AppModel?
    let examples = PopUp(Examples.all.map { $0.name })
    let lang = Segmented(["C", "Assembly"])
    let opt = Segmented(["-O0", "-O1"])
    let reset = TButton("", symbol: "arrow.counterclockwise")
    let back = TButton("", symbol: "backward.frame")
    let stepCycle = TButton("Cycle", symbol: "forward.frame")
    let stepInst = TButton("Instruction", symbol: "forward")
    let stepLine = TButton("Line", symbol: "text.line.first.and.arrowtriangle.forward")
    let play = TButton("Run", symbol: "play.fill", style: .primary)
    let toEnd = TButton("", symbol: "forward.end.fill")
    let speed = NSSlider(value: 1, minValue: 0, maxValue: 5, target: nil, action: nil)
    let speedLabel = makeLabel("", Fonts.tiny, Theme.secondary)

    static let speeds: [Double] = [2, 6, 20, 80, 400, 5000]

    override init(frame: NSRect) {
        super.init(frame: frame)
        for v in [examples, lang, opt, reset, back, stepCycle, stepInst, stepLine, play, toEnd, speed, speedLabel] as [NSView] { addSubview(v) }
        reset.toolTip = "Reset the machine to cycle 0"
        back.toolTip = "One cycle back"
        stepCycle.toolTip = "Advance one clock cycle"
        stepInst.toolTip = "Run until the next instruction completes"
        stepLine.toolTip = "Run until an instruction from another C line completes"
        toEnd.toolTip = "Run to the end of the program"
        speed.numberOfTickMarks = 6
        speed.allowsTickMarkValuesOnly = true
        speed.controlSize = .small
        speed.target = self
        speed.action = #selector(speedChanged)
        examples.onChange = { [weak self] k in self?.model?.load(Examples.all[k]) }
        lang.onChange = { [weak self] k in self?.model?.setAssemblyMode(k == 1) }
        opt.onChange = { [weak self] k in
            guard let m = self?.model else { return }
            var o = m.options
            o.passes = k == 0 ? [] : Set(PassID.allCases)
            m.irSnapshot = nil
            m.options = o
        }
        reset.action = { [weak self] in self?.model?.resetMachine() }
        back.action = { [weak self] in self?.model?.stepBack() }
        stepCycle.action = { [weak self] in self?.model?.stepCycle() }
        stepInst.action = { [weak self] in self?.model?.stepInstruction() }
        stepLine.action = { [weak self] in self?.model?.stepLine() }
        play.action = { [weak self] in self?.model?.togglePlay() }
        toEnd.action = { [weak self] in self?.model?.runToEnd() }
    }
    required init?(coder: NSCoder) { fatalError() }

    override var isFlipped: Bool { true }

    @objc private func speedChanged() {
        let k = Int(speed.doubleValue.rounded())
        model?.speed = TopBar.speeds[k]
        refresh()
    }

    override func layout() {
        super.layout()
        let y: CGFloat = 14
        var x: CGFloat = 196
        examples.frame = NSRect(x: x, y: y - 1, width: 220, height: 26)
        x += 228
        let ls = lang.intrinsicContentSize
        lang.frame = NSRect(x: x, y: y, width: ls.width, height: 24)
        x += ls.width + 8
        let os = opt.intrinsicContentSize
        opt.frame = NSRect(x: x, y: y, width: os.width, height: 24)
        // Clock controls on the right.
        var rx = bounds.width - 12
        func place(_ v: NSView, _ w: CGFloat) {
            rx -= w
            v.frame = NSRect(x: rx, y: y - 1, width: w, height: 26)
            rx -= 5
        }
        speedLabel.frame = NSRect(x: bounds.width - 92, y: y + 22, width: 80, height: 12)
        place(speed, 80)
        place(toEnd, 32)
        place(play, play.intrinsicContentSize.width + 6)
        place(stepLine, stepLine.intrinsicContentSize.width)
        place(stepInst, stepInst.intrinsicContentSize.width)
        place(stepCycle, stepCycle.intrinsicContentSize.width)
        place(back, 32)
        place(reset, 32)
        speedLabel.alignment = .center
    }

    override func draw(_ dirtyRect: NSRect) {
        Theme.panel.setFill()
        bounds.fill()
        line(NSPoint(x: 0, y: bounds.height - 0.5), NSPoint(x: bounds.width, y: bounds.height - 0.5), Theme.border)
        // A small chip-like emblem.
        let em = NSRect(x: 14, y: 12, width: 28, height: 28)
        fillRounded(em, 6, Theme.ink)
        for k in 0..<4 {
            let o = CGFloat(k) * 6 + 6
            line(NSPoint(x: em.minX + o, y: em.minY - 3), NSPoint(x: em.minX + o, y: em.minY), Theme.ink, 1.5)
            line(NSPoint(x: em.minX + o, y: em.maxY), NSPoint(x: em.minX + o, y: em.maxY + 3), Theme.ink, 1.5)
        }
        drawText("C", in: em, Fonts.serif(15, .bold), .white, align: .center)
        drawText("Silicon Path", at: NSPoint(x: 50, y: 9), Fonts.serif(18, .semibold), Theme.text)
        drawText("from C to RISC-V hardware", at: NSPoint(x: 51, y: 31), Fonts.tiny, Theme.secondary)
    }

    func refresh() {
        guard let m = model else { return }
        if let k = Examples.all.firstIndex(where: { $0.name == m.exampleName }) { examples.button.selectItem(at: k) }
        lang.selected = m.isAssembly ? 1 : 0
        opt.enabledMask = m.isAssembly ? [false, false] : nil
        let all = m.options.passes.count == PassID.allCases.count, none = m.options.passes.isEmpty
        opt.options = all || none ? ["-O0", "-O1"] : ["-O0", "-O1", "custom"]
        opt.selected = none ? 0 : all ? 1 : 2
        let canRun = m.pipeline != nil && !(m.pipeline!.halted)
        for b in [stepCycle, stepInst, stepLine, play, toEnd] { b.isEnabled = canRun }
        back.isEnabled = (m.pipeline?.cycle ?? 0) > 0
        reset.isEnabled = m.pipeline != nil
        play.title = m.running ? "Pause" : "Run"
        play.symbol = m.running ? "pause.fill" : "play.fill"
        let k = TopBar.speeds.firstIndex(of: m.speed) ?? 1
        speed.doubleValue = Double(k)
        speedLabel.stringValue = m.speed >= 1000 ? "full speed" : "\(Int(m.speed)) cycles/s"
        needsLayout = true
    }
}
