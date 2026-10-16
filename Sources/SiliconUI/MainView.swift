import AppKit
import SiliconCore

final class MainView: NSView, NSSplitViewDelegate {
    let model: AppModel
    let top = TopBar()
    let journey = JourneyBar()
    let split = NSSplitView()
    let leftSplit = NSSplitView()
    let editorCard = NSView()
    let editor = CodeEditor(frame: .zero)
    let console = ConsoleView()
    let right = FlippedView()
    let stationHost = NSView()
    let lesson = LessonStrip()
    var views: [Station: StationView] = [:]
    private var editorTitle = makeLabel("", Fonts.subheading, Theme.heading)
    private let stale = StaleBanner()

    init(model: AppModel) {
        self.model = model
        super.init(frame: .zero)
        wantsLayer = true
        layer?.backgroundColor = Theme.background.cgColor
        top.model = model
        journey.model = model
        console.model = model
        addSubview(top)
        addSubview(journey)

        split.isVertical = true
        split.dividerStyle = .thin
        split.delegate = self
        leftSplit.isVertical = false
        leftSplit.dividerStyle = .thin
        leftSplit.delegate = self
        editorCard.wantsLayer = true
        editorCard.layer?.backgroundColor = Theme.panel.cgColor
        editorCard.layer?.cornerRadius = 8
        editorCard.layer?.borderColor = Theme.border.cgColor
        editorCard.layer?.borderWidth = 1
        editorCard.layer?.masksToBounds = true
        editorCard.addSubview(editor)
        editorCard.addSubview(editorTitle)
        leftSplit.addArrangedSubview(editorCard)
        leftSplit.addArrangedSubview(console)
        split.addArrangedSubview(leftSplit)
        split.addArrangedSubview(right)
        addSubview(split)

        stationHost.wantsLayer = true
        stationHost.layer?.backgroundColor = Theme.panel.cgColor
        stationHost.layer?.cornerRadius = 8
        stationHost.layer?.borderColor = Theme.border.cgColor
        stationHost.layer?.borderWidth = 1
        stationHost.layer?.masksToBounds = true
        right.addSubview(stationHost)
        right.addSubview(lesson)
        right.addSubview(stale)
        stale.isHidden = true

        let make: [(Station, StationView)] = [
            (.source, OverviewView()), (.tokens, TokensView()), (.tree, TreeView()), (.symbols, SymbolsView()),
            (.ir, IRView()), (.asm, AsmView()), (.machine, MachineCodeView()), (.link, LinkView()),
            (.pipeline, PipelineView()), (.memory, MemoryView()),
        ]
        for (s, v) in make {
            views[s] = v
            v.isHidden = true
            stationHost.addSubview(v)
        }

        editor.onChange = { [weak model] s in model?.setSource(s) }
        editor.onSelectLine = { [weak model] l in model?.select(line: l, origin: .source) }
        console.onSelectLine = { [weak self] l, _ in
            self?.model.select(line: l, origin: .source)
            self?.editor.reveal(line: l)
        }
        model.observe { [weak self] e in self?.handle(e) }
        editor.setText(model.source, assembly: model.isAssembly)
    }
    required init?(coder: NSCoder) { fatalError() }

    override var isFlipped: Bool { true }

    private var didInitialSplit = false

    override func layout() {
        super.layout()
        top.frame = NSRect(x: 0, y: 0, width: bounds.width, height: 50)
        journey.frame = NSRect(x: 0, y: 52, width: bounds.width, height: 70)
        split.frame = NSRect(x: 10, y: 124, width: bounds.width - 20, height: bounds.height - 134)
        if !didInitialSplit && split.bounds.width > 0 {
            didInitialSplit = true
            split.setPosition(max(420, split.bounds.width * 0.36), ofDividerAt: 0)
            leftSplit.setPosition(leftSplit.bounds.height - 190, ofDividerAt: 0)
        }
        editorTitle.frame = NSRect(x: 12, y: editorCard.bounds.height - 26, width: editorCard.bounds.width - 24, height: 18)
        editor.frame = NSRect(x: 0, y: 0, width: editorCard.bounds.width, height: editorCard.bounds.height - 30)
        let lh: CGFloat = 96
        let bannerH: CGFloat = stale.isHidden ? 0 : 34
        stale.frame = NSRect(x: 8, y: 0, width: right.bounds.width - 8, height: 28)
        stationHost.frame = NSRect(x: 8, y: bannerH, width: right.bounds.width - 8, height: right.bounds.height - lh - 8 - bannerH)
        lesson.frame = NSRect(x: 8, y: right.bounds.height - lh, width: right.bounds.width - 8, height: lh)
        for v in views.values { v.frame = stationHost.bounds }
    }

    // MARK: Split view

    func splitView(_ splitView: NSSplitView, constrainMinCoordinate proposedMinimumPosition: CGFloat, ofSubviewAt dividerIndex: Int) -> CGFloat {
        splitView === split ? 330 : 160
    }
    func splitView(_ splitView: NSSplitView, constrainMaxCoordinate proposedMaximumPosition: CGFloat, ofSubviewAt dividerIndex: Int) -> CGFloat {
        splitView === split ? splitView.bounds.width - 620 : splitView.bounds.height - 90
    }
    func splitView(_ splitView: NSSplitView, shouldAdjustSizeOfSubview view: NSView) -> Bool {
        splitView === split ? view === right : view === editorCard
    }
    func splitViewDidResizeSubviews(_ notification: Notification) { needsLayout = true }

    // MARK: Model events

    func start() {
        model.compileNow()
        showStation()
    }

    /// Warns when a station shows the last program that compiled, not the text in the editor.
    private func updateStale() {
        var show = false
        if let c = model.compilation, let failed = c.failedStage, model.good != nil {
            let stageOrder: [Stage] = [.lexer, .preprocessor, .parser, .semantic, .ir, .codegen, .assembler, .linker]
            let need: [Station: Stage] = [.tokens: .lexer, .tree: .parser, .symbols: .semantic, .ir: .ir, .asm: .codegen,
                                          .machine: .assembler, .link: .linker, .pipeline: .linker, .memory: .linker]
            if let st = need[model.station], let a = stageOrder.firstIndex(of: st), let b = stageOrder.firstIndex(of: failed), a >= b {
                show = !(model.station == .tokens && failed != .lexer)
            }
            let line = c.errors.first?.line ?? 0
            stale.text = "Showing the last version that compiled. \(failed.rawValue) stopped at an error" + (line > 0 ? " on line \(line)" : "") + " — fix it to update this view."
        }
        if stale.isHidden == show {
            stale.isHidden = !show
            needsLayout = true
        }
        stale.needsDisplay = true
    }

    private func showStation() {
        updateStale()
        for (s, v) in views { v.isHidden = s != model.station }
        lesson.station = model.station
        journey.needsDisplay = true
        if let v = views[model.station] {
            v.reload(model, .compiled)
        }
    }

    private func handle(_ e: ModelEvent) {
        switch e {
        case .compiled:
            if editor.text != model.source { editor.setText(model.source, assembly: model.isAssembly) }
            else if editor.isAssembly != model.isAssembly { editor.setText(model.source, assembly: model.isAssembly) }
            editor.errors = model.compilation?.diagnostics ?? []
            editorTitle.stringValue = model.isAssembly ? "Assembly source" : "C source"
            for (s, v) in views where s == model.station { v.reload(model, .compiled) }
            for (s, v) in views where s != model.station { if !v.isHidden { v.reload(model, .compiled) } }
            dirtyOthers = Set(Station.allCases).subtracting([model.station])
            editor.stages = model.stagesByLine
            console.reload(model)
            top.refresh()
            journey.needsDisplay = true
            updateStale()
        case .stepped, .running, .config:
            editor.stages = model.stagesByLine
            console.reload(model)
            top.refresh()
            views[model.station]?.reload(model, e)
            if e == .config { dirtyOthers = Set(Station.allCases).subtracting([model.station]) }
            journey.needsDisplay = true
        case .selection:
            editor.selectedLine = model.selection.line
            if model.selection.origin != .source {
                if let r = model.selection.range { editor.select(range: r) }
                if let l = model.selection.line { editor.reveal(line: l) }
            }
            views[model.station]?.reload(model, .selection)
        case .station:
            if dirtyOthers.contains(model.station) {
                dirtyOthers.remove(model.station)
                views[model.station]?.reload(model, .compiled)
            }
            showStation()
            views[model.station]?.reload(model, .stepped)
            views[model.station]?.reload(model, .selection)
        }
    }

    private var dirtyOthers: Set<Station> = []
}

final class StaleBanner: FlippedView {
    var text = ""
    override func draw(_ dirtyRect: NSRect) {
        fillRounded(bounds.insetBy(dx: 0.5, dy: 0.5), 7, NSColor(hex: 0xFBE3DE))
        strokeRounded(bounds.insetBy(dx: 0.5, dy: 0.5), 7, Theme.crimson.alpha(0.4))
        drawText("⚠︎  " + text, in: NSRect(x: 12, y: 0, width: bounds.width - 24, height: bounds.height), Fonts.ui(11.5, .medium), Theme.crimson)
    }
}
