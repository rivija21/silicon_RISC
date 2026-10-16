import AppKit
import SiliconCore

/// Renders the main window offscreen, for checking layouts without a screen.
public enum Snapshot {
    /// Compiles and runs every built-in example on both CPU models and checks them against the interpreter.
    public static func verifyExamples() -> Bool {
        var ok = true
        for ex in Examples.all {
            let c = ex.isAssembly ? Compilation.assemble(ex.source) : Compilation.compile(ex.source, options: .o1)
            guard let exe = c.executable else {
                print("FAIL \(ex.name): \(c.errors.map { $0.description })")
                ok = false
                continue
            }
            let isa = ISASimulator(exe)
            isa.run()
            let p = Pipeline(exe)
            p.recordHistory = false
            p.run()
            var verdict = isa.output == p.output && isa.instret == p.stats.retired && isa.output.fault == nil
            if let prog = c.program, let sema = c.sema {
                let ref = Interpreter.run(prog, sema)
                verdict = verdict && ref == isa.output
            }
            for opt in [CompileOptions.o0, CompileOptions(registerLimit: 3)] where !ex.isAssembly {
                let c2 = Compilation.compile(ex.source, options: opt)
                if let e2 = c2.executable {
                    let s2 = ISASimulator(e2)
                    s2.run()
                    verdict = verdict && s2.output == isa.output
                } else { verdict = false }
            }
            ok = ok && verdict
            let warn = c.diagnostics.filter { $0.severity == .warning }.map { $0.description }
            print("\(verdict ? "ok  " : "FAIL") \(ex.name): \(isa.instret) instructions, \(p.stats.cycles) cycles, CPI \(String(format: "%.2f", p.stats.cpi)) · output \(isa.output.text.prefix(60).debugDescription)\(warn.isEmpty ? "" : " · warnings: \(warn)")")
        }
        return ok
    }

    /// `script` is a list of commands: "example:N", "station:N", "cycles:N", "end", "select:LINE", "opt:0|1", "regs:N", "asm".
    public static func render(_ script: [String], to path: String, size: NSSize = NSSize(width: 1480, height: 920)) {
        _ = NSApplication.shared
        NSApp.setActivationPolicy(.prohibited)
        NSApp.appearance = NSAppearance(named: .aqua)
        let model = AppModel()
        model.pipelineConfig = PipelineConfig()
        var o = CompileOptions.o1
        o.registerLimit = nil
        model.options = o
        let window = NSWindow(contentRect: NSRect(origin: NSPoint(x: -4000, y: -4000), size: size), styleMask: [.titled, .resizable], backing: .buffered, defer: false)
        let main = MainView(model: model)
        window.contentView = main
        main.frame = NSRect(origin: .zero, size: size)
        main.layoutSubtreeIfNeeded()
        main.start()
        for cmd in script {
            let parts = cmd.split(separator: ":").map(String.init)
            let arg = parts.count > 1 ? Int(parts[1]) ?? 0 : 0
            switch parts[0] {
            case "example": model.load(Examples.all[arg])
            case "station": model.setStation(Station.allCases[arg])
            case "cycles": model.stepCycle(arg)
            case "end": model.runToEnd()
            case "select": model.selection = Selection(line: arg, origin: .source)
            case "opt":
                var o = model.options
                o.passes = arg == 0 ? [] : Set(PassID.allCases)
                model.options = o
                model.compileNow()
            case "regs":
                var o = model.options
                o.registerLimit = arg == 0 ? nil : arg
                model.options = o
                model.compileNow()
            case "snapshot": model.irSnapshot = arg
            case "caches":
                var h = model.pipelineConfig.hierarchy
                h.enabled = arg == 1
                model.pipelineConfig.hierarchy = h
            case "fwd": model.pipelineConfig.forwarding = arg == 1
            case "branch": model.pipelineConfig.branchStage = BranchStage.allCases[arg]
            case "pred": model.pipelineConfig.predictor = PredictorKind.allCases[arg]
            case "source":
                model.setSource(parts.dropFirst().joined(separator: ":").replacingOccurrences(of: "\\n", with: "\n"), fromEditor: false)
                model.compileNow()
            default: break
            }
            RunLoop.current.run(until: Date().addingTimeInterval(0.05))
            main.layoutSubtreeIfNeeded()
        }
        RunLoop.current.run(until: Date().addingTimeInterval(0.2))
        main.needsLayout = true
        main.layoutSubtreeIfNeeded()
        main.display()
        guard let rep = main.bitmapImageRepForCachingDisplay(in: main.bounds) else { return }
        main.cacheDisplay(in: main.bounds, to: rep)
        if let data = rep.representation(using: .png, properties: [:]) {
            try? data.write(to: URL(fileURLWithPath: path))
        }
    }
}
