import Foundation

public struct CompileOptions: Equatable, Sendable {
    public var passes: Set<PassID>
    /// Pretend the machine has only this many allocatable registers (to watch spilling).
    public var registerLimit: Int?
    /// Compiling a library member: no main needed, library names may be defined.
    public var isLibrary: Bool

    public init(passes: Set<PassID> = Set(PassID.allCases), registerLimit: Int? = nil, isLibrary: Bool = false) {
        self.passes = passes
        self.registerLimit = registerLimit
        self.isLibrary = isLibrary
    }

    public static let o0 = CompileOptions(passes: [])
    public static let o1 = CompileOptions()
}

/// One run of the whole tool chain, keeping every intermediate result so
/// the app can show each stage: tokens → syntax tree → symbols → IR (after
/// each pass) → register allocation → assembly → object file → executable.
public final class Compilation: @unchecked Sendable {
    public let source: String
    public let options: CompileOptions
    public let name: String
    public let lines: LineTable
    public private(set) var isAssembly = false

    public private(set) var lex: LexResult?
    public private(set) var program: Program?
    public private(set) var sema: SemaResult?
    public private(set) var snapshots: [PassSnapshot] = []
    public private(set) var ir: IRModule?
    public private(set) var allocations: [String: Allocation] = [:]
    public private(set) var frames: [FrameLayout] = []
    public private(set) var asm: [AsmLine] = []
    public private(set) var object: ObjectFile?
    public private(set) var executable: Executable?
    public private(set) var diagnostics: [Diagnostic] = []
    /// The first stage that failed, if any.
    public private(set) var failedStage: Stage?
    public private(set) var timings: [(Stage, Double)] = []

    public var errors: [Diagnostic] { diagnostics.filter { $0.severity == .error } }
    public var succeeded: Bool { executable != nil }

    private init(source: String, name: String, options: CompileOptions) {
        self.source = source
        self.name = name
        self.options = options
        lines = LineTable(source)
    }

    private func fail(_ s: Stage) { if failedStage == nil { failedStage = s } }

    private func time<T>(_ s: Stage, _ body: () -> T) -> T {
        let t0 = Date()
        let r = body()
        timings.append((s, Date().timeIntervalSince(t0)))
        return r
    }

    /// Compiles a C program and links it into an executable.
    public static func compile(_ source: String, options: CompileOptions = .o1) -> Compilation {
        let c = compileObject(source, name: "main.o", options: options)
        guard let obj = c.object, c.failedStage == nil else { return c }
        c.link(obj)
        return c
    }

    /// Assembles and links a hand-written assembly program.
    public static func assemble(_ source: String) -> Compilation {
        let c = Compilation(source: source, name: "main.o", options: .o1)
        c.isAssembly = true
        let obj = c.time(.assembler) { Assembler.assemble(text: source, name: "main.o") }
        c.object = obj
        c.asm = obj.lines
        c.diagnostics += obj.diagnostics
        if obj.hasErrors {
            c.fail(.assembler)
            return c
        }
        if obj.symbols["main"] != nil && !obj.globalSymbols.contains("main") {
            obj.symbols["main"]!.isGlobal = true
        }
        c.link(obj)
        return c
    }

    private func link(_ obj: ObjectFile) {
        let lib = Runtime.library
        let (exe, d) = time(.linker) { Linker.link([lib.crt0, obj], archive: lib.members) }
        diagnostics += d
        if exe == nil { fail(.linker) }
        executable = exe
    }

    /// Runs everything up to and including the assembler.
    public static func compileObject(_ source: String, name: String, options: CompileOptions) -> Compilation {
        let c = Compilation(source: source, name: name, options: options)
        let lx = c.time(.lexer) { Lexer.lex(source) }
        c.lex = lx
        c.diagnostics += lx.diagnostics
        if lx.hasErrors { c.fail(.lexer); return c }

        let (prog, pd) = c.time(.parser) { Parser.parse(lx) }
        c.diagnostics += pd
        guard let prog else { c.fail(.parser); return c }
        c.program = prog

        let sema = c.time(.semantic) { Sema.analyze(prog, library: options.isLibrary) }
        c.sema = sema
        c.diagnostics += sema.diagnostics
        if sema.hasErrors { c.fail(.semantic); return c }

        let ids = IDGen()
        let snaps = c.time(.ir) { () -> [PassSnapshot] in
            let m = Lowering.lower(prog, sema, ids: ids)
            return Optimizer.run(m, enabled: options.passes)
        }
        c.snapshots = snaps
        let final = snaps.last!.module
        c.ir = final

        let gen = CodeGen(source: source)
        c.asm = c.time(.codegen) { () -> [AsmLine] in
            for f in final.functions {
                c.allocations[f.name] = RegisterAllocator.allocate(f, registerLimit: options.registerLimit)
            }
            return gen.generate(final, allocations: c.allocations)
        }
        c.frames = gen.frames
        c.describeLocations(prog, final)

        let obj = c.time(.assembler) { Assembler.assemble(c.asm, name: name) }
        c.object = obj
        c.diagnostics += obj.diagnostics
        if obj.hasErrors { c.fail(.assembler) }
        return c
    }

    /// Fills Symbol.location ("s0-20", "s1", "spilled", "@0x…") for the symbol-table view.
    private func describeLocations(_ prog: Program, _ m: IRModule) {
        for fd in prog.functions {
            guard let f = m.functions.first(where: { $0.name == fd.name }),
                  let a = allocations[fd.name], let fr = frames.first(where: { $0.function == fd.name }) else { continue }
            for s in fd.locals {
                if let k = f.slots.firstIndex(where: { $0.symbolID == s.id }), k < fr.slotOffset.count {
                    s.location = "stack: \(fr.slotOffset[k])(s0)"
                } else if let v = f.vregs.firstIndex(where: { $0.name == "%" + s.uniqueName && $0.isVariable }) {
                    switch a.locations[v] {
                    case let .reg(r)?: s.location = "register \(Reg.name(r))"
                    case let .spill(k)?: s.location = k < fr.spillOffset.count ? "spilled: \(fr.spillOffset[k])(s0)" : "spilled"
                    case nil: s.location = "optimised away"
                    }
                } else {
                    s.location = "optimised away"
                }
            }
        }
        for s in prog.globals.compactMap({ $0.symbol }) { s.location = ".data (\(s.name))" }
    }

    // MARK: Lookups used by the views

    /// Assembly lines generated for a C source line.
    public func asmLines(forSourceLine line: Int) -> [Int] {
        asm.indices.filter { asm[$0].origin?.line == line && asm[$0].kind == .instruction }
    }
}
