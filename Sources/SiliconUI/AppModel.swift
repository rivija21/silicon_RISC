import AppKit
import SiliconCore

/// The ten stops on the way from C source to silicon.
enum Station: Int, CaseIterable {
    case source, tokens, tree, symbols, ir, asm, machine, link, pipeline, memory

    var title: String {
        switch self {
        case .source: return "Source"
        case .tokens: return "Tokens"
        case .tree: return "Syntax tree"
        case .symbols: return "Symbols"
        case .ir: return "IR & optimiser"
        case .asm: return "Assembly"
        case .machine: return "Machine code"
        case .link: return "Linking"
        case .pipeline: return "Pipeline"
        case .memory: return "Memory"
        }
    }

    var layer: String {
        switch self {
        case .source, .tokens, .tree, .symbols: return "front end"
        case .ir: return "middle end"
        case .asm, .machine, .link: return "back end"
        case .pipeline: return "microarchitecture"
        case .memory: return "memory hierarchy"
        }
    }

    var symbol: String {
        switch self {
        case .source: return "doc.text"
        case .tokens: return "textformat.abc"
        case .tree: return "point.3.connected.trianglepath.dotted"
        case .symbols: return "tablecells"
        case .ir: return "arrow.triangle.branch"
        case .asm: return "chevron.left.forwardslash.chevron.right"
        case .machine: return "01.square"
        case .link: return "link"
        case .pipeline: return "cpu"
        case .memory: return "memorychip"
        }
    }

    /// Hand-written assembly skips the compiler stations.
    var needsC: Bool { [.tokens, .tree, .symbols, .ir].contains(self) }
}

struct Selection: Equatable {
    var line: Int?
    var node: Int?
    var pc: UInt32?
    var irInst: Int?
    /// Source text to select in the editor.
    var range: SourceRange?
    /// Which view made the selection, so it does not scroll itself.
    var origin: Station?
}

enum ModelEvent {
    case compiled
    case stepped
    case selection
    case station
    case config
    case running
}

final class AppModel {
    private(set) var source: String
    private(set) var isAssembly: Bool
    var options: CompileOptions { didSet { if options != oldValue { scheduleCompile(delay: 0) } } }
    var pipelineConfig: PipelineConfig { didSet { if pipelineConfig != oldValue { resetMachine(); notify(.config) } } }
    private(set) var compilation: Compilation?
    /// The last compilation that produced an executable (views keep showing it while you type an error).
    private(set) var good: Compilation?
    private(set) var pipeline: Pipeline?
    private(set) var station: Station = .source
    var selection = Selection() { didSet { if selection != oldValue { notify(.selection) } } }
    var irSnapshot: Int? { didSet { notify(.selection) } }
    var focusFunction: String? { didSet { if focusFunction != oldValue { notify(.selection) } } }
    private(set) var running = false
    /// Clock speed while running, in cycles per second.
    var speed: Double = 12
    var exampleName: String = ""
    var undoManager = UndoManager()

    private var observers: [(ModelEvent) -> Void] = []
    private var compileWork: DispatchWorkItem?
    private var timer: Timer?
    private var carry = 0.0
    private var lastTick = Date()

    init() {
        let d = UserDefaults.standard
        let first = Examples.all[0]
        source = d.string(forKey: "source") ?? first.source
        isAssembly = d.bool(forKey: "isAssembly")
        exampleName = d.string(forKey: "example") ?? first.name
        var opts = CompileOptions.o1
        if let arr = d.array(forKey: "passes") as? [String] {
            opts.passes = Set(arr.compactMap(PassID.init(rawValue:)))
        }
        let limit = d.integer(forKey: "registerLimit")
        opts.registerLimit = limit > 0 ? limit : nil
        options = opts
        var pc = PipelineConfig()
        pc.forwarding = d.object(forKey: "forwarding") as? Bool ?? true
        pc.branchStage = BranchStage(rawValue: d.string(forKey: "branchStage") ?? "") ?? .ex
        pc.predictor = PredictorKind(rawValue: d.string(forKey: "predictor") ?? "") ?? .notTaken
        pipelineConfig = pc
        if let s = Station(rawValue: d.integer(forKey: "station")), d.object(forKey: "station") != nil { station = s }
        speed = d.object(forKey: "speed") as? Double ?? 12
    }

    func save() {
        let d = UserDefaults.standard
        d.set(source, forKey: "source")
        d.set(isAssembly, forKey: "isAssembly")
        d.set(exampleName, forKey: "example")
        d.set(options.passes.map { $0.rawValue }, forKey: "passes")
        d.set(options.registerLimit ?? 0, forKey: "registerLimit")
        d.set(pipelineConfig.forwarding, forKey: "forwarding")
        d.set(pipelineConfig.branchStage.rawValue, forKey: "branchStage")
        d.set(pipelineConfig.predictor.rawValue, forKey: "predictor")
        d.set(station.rawValue, forKey: "station")
        d.set(speed, forKey: "speed")
    }

    func observe(_ f: @escaping (ModelEvent) -> Void) { observers.append(f) }
    func notify(_ e: ModelEvent) { for o in observers { o(e) } }

    // MARK: Source and compiling

    func setSource(_ s: String, fromEditor: Bool = true) {
        guard s != source else { return }
        source = s
        scheduleCompile(delay: fromEditor ? 0.45 : 0)
    }

    func load(_ ex: Example) {
        pause()
        exampleName = ex.name
        isAssembly = ex.isAssembly
        source = ex.source
        selection = Selection()
        irSnapshot = nil
        focusFunction = nil
        compileNow()
        setStation(ex.isAssembly && ex.station.needsC ? .asm : ex.station)
    }

    func loadFile(_ url: URL) {
        guard let s = try? String(contentsOf: url, encoding: .utf8) else { return }
        pause()
        exampleName = url.lastPathComponent
        isAssembly = ["s", "asm", "S"].contains(url.pathExtension)
        source = s
        selection = Selection()
        compileNow()
    }

    func setAssemblyMode(_ on: Bool) {
        guard on != isAssembly else { return }
        isAssembly = on
        if on && station.needsC { station = .asm }
        compileNow()
    }

    func scheduleCompile(delay: Double) {
        compileWork?.cancel()
        let w = DispatchWorkItem { [weak self] in self?.compileNow() }
        compileWork = w
        DispatchQueue.main.asyncAfter(deadline: .now() + delay, execute: w)
    }

    func compileNow() {
        compileWork?.cancel()
        let c = isAssembly ? Compilation.assemble(source) : Compilation.compile(source, options: options)
        compilation = c
        if c.succeeded {
            good = c
            resetMachine()
        } else if good == nil || good!.isAssembly != isAssembly {
            good = nil
            pipeline = nil
        }
        if let s = irSnapshot, s >= (good?.snapshots.count ?? 0) { irSnapshot = nil }
        notify(.compiled)
        save()
    }

    /// The compilation the views should display.
    var shown: Compilation? { compilation?.succeeded == true ? compilation : (good ?? compilation) }
    var hasErrors: Bool { compilation?.errors.isEmpty == false }

    // MARK: Station

    func setStation(_ s: Station) {
        guard s != station else { return }
        station = s
        notify(.station)
        save()
    }

    // MARK: Running the machine

    func resetMachine() {
        pause()
        if let exe = good?.executable {
            pipeline = Pipeline(exe, config: pipelineConfig)
        } else {
            pipeline = nil
        }
        notify(.stepped)
    }

    func stepCycle(_ n: Int = 1) {
        guard let p = pipeline, !p.halted else { return }
        for _ in 0..<n where p.step() {}
        notify(.stepped)
        if p.halted { pause() }
    }

    /// Steps until the next instruction completes (leaves WB).
    func stepInstruction() {
        guard let p = pipeline, !p.halted else { return }
        let target = p.stats.retired + 1
        var guardCount = 0
        while p.stats.retired < target && p.step() && guardCount < 10_000 { guardCount += 1 }
        notify(.stepped)
    }

    /// Steps until an instruction from a different C line completes.
    func stepLine() {
        guard let p = pipeline, !p.halted, let exe = good?.executable else { return }
        func lineOf(_ seq: Int?) -> Int? {
            guard let s = seq, let pc = p.seqPC[s] else { return nil }
            return exe.instruction(at: pc).flatMap(sourceLine)
        }
        let startLine = lineOf(p.lastRecord?.retired) ?? -1
        var n = 0
        while p.step() && n < 200_000 {
            n += 1
            if let r = p.lastRecord?.retired, let l = lineOf(r), l > 0, l != startLine { break }
        }
        notify(.stepped)
    }

    func stepBack() {
        guard let p = pipeline, p.cycle > 0, let exe = good?.executable else { return }
        pause()
        let target = p.cycle - 1
        let q = Pipeline(exe, config: pipelineConfig)
        while q.cycle < target && q.step() {}
        pipeline = q
        notify(.stepped)
    }

    func runToEnd() {
        guard let p = pipeline, !p.halted else { return }
        pause()
        let t0 = Date()
        while !p.halted && Date().timeIntervalSince(t0) < 4 {
            for _ in 0..<20_000 where p.step() {}
        }
        notify(.stepped)
    }

    func togglePlay() { running ? pause() : play() }

    func play() {
        guard let p = pipeline, !p.halted else { return }
        running = true
        carry = 0
        lastTick = Date()
        let t = Timer(timeInterval: 1.0 / 30, repeats: true) { [weak self] _ in self?.tick() }
        RunLoop.main.add(t, forMode: .common)
        timer = t
        notify(.running)
    }

    func pause() {
        guard running else { return }
        running = false
        timer?.invalidate()
        timer = nil
        notify(.running)
    }

    private func tick() {
        guard let p = pipeline else { pause(); return }
        let now = Date()
        carry += now.timeIntervalSince(lastTick) * speed
        lastTick = now
        let n = Int(carry)
        carry -= Double(n)
        if n > 0 {
            let t0 = Date()
            var k = 0
            while k < n && p.step() {
                k += 1
                if k & 1023 == 0 && Date().timeIntervalSince(t0) > 0.025 { break }
            }
            notify(.stepped)
        }
        if p.halted { pause() }
    }

    // MARK: Lookups

    /// Source line (C, or the assembly line in assembly mode) an instruction came from.
    func sourceLine(_ ins: LinkedInstruction) -> Int? {
        guard ins.object == "main.o" else { return nil }
        if good?.isAssembly == true { return ins.asmLine + 1 }
        if let l = ins.origin?.line, l > 0 { return l }
        return nil
    }

    func line(ofPC pc: UInt32) -> Int? {
        good?.executable?.instruction(at: pc).flatMap(sourceLine)
    }

    /// For every C line with an instruction in the pipeline right now: which stages.
    var stagesByLine: [Int: [Int]] {
        guard let p = pipeline, let rec = p.lastRecord, let exe = good?.executable else { return [:] }
        var out: [Int: [Int]] = [:]
        for (k, s) in rec.stages.enumerated() where s.seq >= 0 && s.status != .flushed {
            if let ins = exe.instruction(at: s.pc), let l = sourceLine(ins) {
                out[l, default: []].append(k)
            }
        }
        return out
    }

    func select(line: Int?, origin: Station) {
        selection = Selection(line: line, node: nil, pc: nil, irInst: nil, origin: origin)
    }
}
