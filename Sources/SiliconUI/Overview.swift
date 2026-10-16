import AppKit
import SiliconCore

enum Lessons {
    static func text(_ s: Station) -> (String, String) {
        switch s {
        case .source:
            return ("The whole journey",
                    "A compiler is a pipeline of translations. The front end checks what the program means, the middle end improves it, and the back end turns it into RISC-V instructions. The assembler and linker produce bytes, and the CPU (a 5-stage pipeline with caches) runs them. Click any station above to look inside it.")
        case .tokens:
            return ("Lexical analysis — characters become tokens",
                    "The lexer reads characters and groups them into tokens: keywords, names, numbers, strings and punctuation. Spaces and comments are thrown away. The tiny preprocessor here replaces #define names with their text as they are read; that is why N in your code may show up as a number.")
        case .tree:
            return ("Parsing — tokens become a syntax tree",
                    "A recursive-descent parser has one function per grammar rule (statement, expression, term…). Operator precedence decides the shape: a + b * c becomes +(a, *(b, c)). After semantic analysis every expression node also carries its C type.")
        case .symbols:
            return ("Semantic analysis — names and types",
                    "Every name is looked up in nested scopes, every expression gets a type, and C's implicit conversions are worked out (char → int, int → unsigned, array → pointer). Mistakes like calling with the wrong number of arguments are caught here. The last column shows where the back end finally put each variable.")
        case .ir:
            return ("Intermediate representation and the optimiser",
                    "The tree is lowered into three-address code inside basic blocks, which form a control-flow graph. Each optimisation pass rewrites the IR without changing what the program computes. Switch passes off to see what each one contributes. Then compare the Assembly and the Pipeline cycle count at -O0 and -O1.")
        case .asm:
            return ("Code generation — RISC-V assembly (P&H chapter 2)",
                    "Instruction selection maps each IR operation onto RV32IM instructions. The register allocator then maps the unlimited virtual registers onto real ones: values alive across a call go into callee-saved s-registers, and the rest go into t-registers. The calling convention puts arguments in a0–a7 and the return address in ra, and every function builds a stack frame.")
        case .machine:
            return ("Machine code — instruction formats (P&H §2.5)",
                    "The assembler encodes every instruction as a 32-bit word in one of six formats (R, I, S, B, U, J). The opcode and register fields always sit at the same bit positions, so the decoder can start reading registers before it knows the instruction. Pseudo-instructions such as li, la, mv and call expand into real ones.")
        case .link:
            return ("Linking and loading",
                    "Each source file becomes an object file with holes: addresses it cannot know yet. The linker places the code at 0x00400000 and the data at 0x10010000 (the RARS layout). It pulls in the library members your program needs and patches every relocation with the final address. crt0 is the real entry point: it calls main.")
        case .pipeline:
            return ("The pipelined datapath (P&H §4.5–4.9)",
                    "Five instructions are in flight at once, one per stage. Hazards break the rhythm. A value needed before it is written is forwarded from EX/MEM or MEM/WB, except right after a load, where the hazard unit inserts a one-cycle bubble. A taken branch flushes the instructions fetched behind it, and a better predictor flushes less often. Watch the CPI.")
        case .memory:
            return ("The memory hierarchy (P&H chapter 5)",
                    "Caches exploit locality: recently used data is likely to be used again (temporal), and so is its neighbour (spatial). Each address splits into tag | set index | block offset. A miss is compulsory (first touch), capacity (the cache is too small) or conflict (too many blocks map to one set). Change the size, block size and associativity, then re-run.")
        }
    }
}

/// Station 1: the whole compile at a glance.
final class OverviewView: NSView, StationView {
    let panel = ListPanel()
    weak var model: AppModel?

    override init(frame: NSRect) {
        super.init(frame: frame)
        addSubview(panel)
        panel.list.onClick = { [weak self] _, r in
            guard r.tag >= 0, let s = Station(rawValue: r.tag) else { return }
            self?.model?.setStation(s)
        }
    }
    required init?(coder: NSCoder) { fatalError() }

    override func layout() {
        super.layout()
        panel.frame = bounds
    }

    func reload(_ model: AppModel, _ event: ModelEvent) {
        self.model = model
        guard event == .compiled || panel.list.rows.isEmpty || event == .stepped else { return }
        guard let c = model.compilation else { return }
        var rows: [Row] = []
        rows.append(.header(model.exampleName.isEmpty ? "Your program" : model.exampleName,
                            detail: c.isAssembly ? "hand-written assembly" : (c.options.passes.isEmpty ? "compiled at -O0" : c.options.passes.count == PassID.allCases.count ? "compiled at -O1" : "custom optimisation passes")))
        let failed = c.failedStage
        func stage(_ n: Int, _ st: Station?, _ name: String, _ detail: String, _ stg: Stage?, ok: Bool) {
            let t = stg.flatMap { s in c.timings.first { $0.0 == s }?.1 }
            let mark = ok ? "✓" : (failed == stg ? "✗" : "·")
            let markColor = ok ? Theme.green : failed == stg ? Theme.crimson : Theme.faint
            var r = Row([Cell(NSAttributedString(mark, Fonts.ui(13, .bold), markColor), width: 26, align: .center),
                         Cell(NSAttributedString("\(n)  \(name)", Fonts.ui(12.5, .semibold), ok ? Theme.text : Theme.faint), width: 190),
                         Cell(detail, Fonts.small, ok ? Theme.text : Theme.faint),
                         Cell(t.map { String(format: "%.1f ms", $0 * 1000) } ?? "", Fonts.mono(10), Theme.faint, width: 70, align: .right)], height: 30)
            r.tag = st?.rawValue ?? -1
            r.tooltip = st.map { "Open the \($0.title) view" }
            rows.append(r)
        }
        if !c.isAssembly {
            let lex = c.lex
            stage(1, .tokens, "Lexer", lex.map { "\($0.tokens.count - 1) tokens · \($0.macros.count) #define macro\($0.macros.count == 1 ? "" : "s")" } ?? "", .lexer, ok: lex != nil && !(lex!.hasErrors))
            stage(2, .tree, "Parser", c.program.map { "\($0.nodeCount) syntax-tree nodes · \($0.functions.count) function\($0.functions.count == 1 ? "" : "s") · \($0.globals.count) global\($0.globals.count == 1 ? "" : "s")" } ?? "", .parser, ok: c.program != nil)
            let warns = c.diagnostics.filter { $0.severity == .warning }.count
            stage(3, .symbols, "Semantic analysis", c.sema.map { "\($0.symbols.count) symbols · \($0.strings.count) string literal\($0.strings.count == 1 ? "" : "s")" + (warns > 0 ? " · \(warns) warning\(warns == 1 ? "" : "s")" : "") } ?? "", .semantic, ok: c.sema != nil && !c.sema!.hasErrors)
            if let first = c.snapshots.first, let last = c.snapshots.last {
                let pct = first.instructions > 0 ? 100 * (first.instructions - last.instructions) / first.instructions : 0
                stage(4, .ir, "IR & optimiser", "\(first.instructions) → \(last.instructions) IR instructions (\(pct)% fewer) · \(c.options.passes.count) of \(PassID.allCases.count) passes on", .ir, ok: true)
            } else {
                stage(4, .ir, "IR & optimiser", "", .ir, ok: false)
            }
            let vregs = c.allocations.values.reduce(0) { $0 + $1.intervals.count }
            let spills = c.allocations.values.reduce(0) { $0 + $1.spillSlots }
            let saved = Set(c.allocations.values.flatMap { $0.usedCalleeSaved }).count
            stage(5, .asm, "Register allocation", c.asm.isEmpty ? "" : "\(vregs) virtual registers → real registers · \(spills) spilled · \(saved) callee-saved register\(saved == 1 ? "" : "s") used", .codegen, ok: !c.asm.isEmpty)
            let asmCount = c.asm.filter { $0.kind == .instruction }.count
            stage(6, .asm, "Code generation", c.asm.isEmpty ? "" : "\(asmCount) assembly instructions for \(c.ir?.functions.count ?? 0) function\(c.ir?.functions.count == 1 ? "" : "s")", .codegen, ok: !c.asm.isEmpty)
        }
        if let o = c.object {
            let pseudo = o.listing.filter { $0.pseudo != nil }.count
            stage(c.isAssembly ? 1 : 7, .machine, "Assembler", "\(o.listing.count) machine instructions (\(pseudo) from pseudo-instructions) · \(o.text.count) B code · \(o.data.count) B data · \(o.relocations.count) relocations", .assembler, ok: !o.hasErrors)
        } else {
            stage(c.isAssembly ? 1 : 7, .machine, "Assembler", "", .assembler, ok: false)
        }
        if let e = c.executable {
            let libs = e.objects.filter { $0.isLibrary }.map { $0.name }
            stage(c.isAssembly ? 2 : 8, .link, "Linker", "\(e.objects.count) objects (\(libs.joined(separator: ", "))) · \(e.text.count) B code at 0x00400000 · \(e.data.count) B data", .linker, ok: true)
        } else {
            stage(c.isAssembly ? 2 : 8, .link, "Linker", "", .linker, ok: false)
        }
        if let p = model.pipeline {
            let s = p.stats
            let state = p.halted ? (p.output.fault != nil ? "stopped with a fault" : "finished") : (s.cycles == 0 ? "ready — press Run or Step" : "running")
            stage(c.isAssembly ? 3 : 9, .pipeline, "Pipeline", "\(state) · \(formatCount(s.cycles)) cycles · \(formatCount(s.retired)) instructions" + (s.retired > 0 ? String(format: " · CPI %.2f", s.cpi) : ""), nil, ok: true)
            let h = p.hierarchy
            if h.config.enabled {
                stage(c.isAssembly ? 4 : 10, .memory, "Memory hierarchy", String(format: "I-cache %.1f%% hits · D-cache %.1f%% hits", 100 * h.l1i.stats.hitRate, 100 * h.l1d.stats.hitRate) + (h.l2.map { String(format: " · L2 %.1f%%", 100 * $0.stats.hitRate) } ?? ""), nil, ok: true)
            }
        }
        let problems = c.diagnostics.filter { $0.severity != .note }
        if !problems.isEmpty {
            rows.append(.spacer(10))
            rows.append(.header("Problems", detail: "click one to jump to the line"))
            for d in problems {
                var r = Row([Cell(NSAttributedString(d.severity == .error ? "error" : "warning", Fonts.ui(10.5, .bold), d.severity == .error ? Theme.crimson : Theme.amber), width: 64, badge: d.severity == .error ? Theme.errorFill : NSColor(hex: 0xFFF1C9)),
                             Cell(d.line > 0 ? "line \(d.line)" : "", Fonts.mono(10.5), Theme.secondary, width: 64),
                             Cell(NSAttributedString("\(d.stage.rawValue): \(d.message)", Fonts.small, Theme.text))], height: 24)
                r.line = d.line
                rows.append(r)
            }
        }
        if let p = model.pipeline, !p.output.text.isEmpty {
            rows.append(.spacer(10))
            rows.append(.header("Program output so far"))
            for l in p.output.text.components(separatedBy: "\n").prefix(40) {
                rows.append(Row([Cell(l, Fonts.mono(11.5), Theme.text)], height: 17))
            }
        }
        panel.set(rows)
    }
}
