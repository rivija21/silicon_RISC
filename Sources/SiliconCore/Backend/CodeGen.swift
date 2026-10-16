import Foundation

/// Where an assembly line came from.
public struct AsmOrigin: Hashable, Sendable {
    /// IR instruction id (0 for prologue/epilogue and other glue).
    public var inst: Int
    /// Syntax-tree node id.
    public var node: Int
    /// C source line (0 = none).
    public var line: Int
    public init(inst: Int, node: Int, line: Int) {
        self.inst = inst
        self.node = node
        self.line = line
    }
}

public struct AsmLine: Sendable {
    public enum Kind: Sendable { case label, instruction, directive, comment, blank }
    public var kind: Kind
    public var text: String
    public var origin: AsmOrigin?
    /// The function this line belongs to, if any.
    public var function: String?
    /// A short note shown to the right of the instruction.
    public var note: String = ""

    public init(_ kind: Kind, _ text: String, origin: AsmOrigin? = nil, function: String? = nil, note: String = "") {
        self.kind = kind
        self.text = text
        self.origin = origin
        self.function = function
        self.note = note
    }
}

/// The stack frame of one function. Offsets are from s0 (the frame pointer),
/// which holds the value sp had on entry; everything is below it.
public struct FrameLayout: Sendable {
    public struct Entry: Sendable {
        public var offset: Int   // negative: s0 + offset
        public var size: Int
        public var what: String
        public var kind: String  // "ra", "fp", "saved", "spill", "local"
    }
    public var function: String
    public var size = 0
    public var entries: [Entry] = []
    public var hasFrame = true
    public var slotOffset: [Int] = []
    public var spillOffset: [Int] = []
    public var savedOffset: [Int: Int] = [:]
}

/// Turns optimised IR plus a register allocation into RISC-V assembly text.
public final class CodeGen {
    private var out: [AsmLine] = []
    private var f: IRFunction!
    private var alloc: Allocation!
    private var frame: FrameLayout!
    private var origin: AsmOrigin?
    private var sourceLines: [String]
    private var lastLine = -1
    public private(set) var frames: [FrameLayout] = []

    public init(source: String) {
        sourceLines = source.components(separatedBy: "\n")
    }

    public static func label(_ fn: String, _ block: Block) -> String {
        ".L\(fn)_" + block.name.replacingOccurrences(of: ".", with: "_")
    }

    // MARK: Output helpers

    private func emit(_ text: String, note: String = "") {
        out.append(AsmLine(.instruction, "    " + text, origin: origin, function: f?.name, note: note))
    }

    private func directive(_ text: String) {
        out.append(AsmLine(.directive, "    " + text))
    }

    private func comment(_ text: String) {
        out.append(AsmLine(.comment, "    # " + text, origin: origin, function: f?.name))
    }

    private func fits12(_ v: Int) -> Bool { v >= -2048 && v <= 2047 }
    private func r(_ x: Int) -> String { Reg.name(x) }

    // MARK: Module

    public func generate(_ m: IRModule, allocations: [String: Allocation]) -> [AsmLine] {
        out = []
        if !m.data.isEmpty {
            directive(".data")
            for g in m.data { data(g) }
            out.append(AsmLine(.blank, ""))
        }
        directive(".text")
        for fn in m.functions {
            function(fn, allocations[fn.name]!)
            out.append(AsmLine(.blank, ""))
        }
        return out
    }

    private func data(_ g: GlobalData) {
        if g.align > 1 { directive(".align \(g.align.trailingZeroBitCount)") }
        if !g.label.hasPrefix(".") { directive(".globl \(g.label)") }
        if g.line > 0 {
            origin = AsmOrigin(inst: 0, node: 0, line: g.line)
            out.append(AsmLine(.label, "\(g.label):", origin: origin, note: g.comment))
        } else {
            out.append(AsmLine(.label, "\(g.label):", note: g.comment))
        }
        if g.comment == "string" {
            let bytes = g.items.compactMap { if case let .byte(b) = $0 { return b }; return nil }
            directive(".string \"\(CodeGen.escape(Array(bytes.dropLast())))\"")
            origin = nil
            return
        }
        var pendingWords: [String] = []
        var pendingBytes: [String] = []
        func flush() {
            if !pendingWords.isEmpty { directive(".word " + pendingWords.joined(separator: ", ")) }
            if !pendingBytes.isEmpty { directive(".byte " + pendingBytes.joined(separator: ", ")) }
            pendingWords = []
            pendingBytes = []
        }
        for it in g.items {
            switch it {
            case let .word(v):
                if !pendingBytes.isEmpty { flush() }
                pendingWords.append(String(v))
            case let .address(l, o):
                if !pendingBytes.isEmpty { flush() }
                pendingWords.append(o == 0 ? l : "\(l)+\(o)")
            case let .byte(b):
                if !pendingWords.isEmpty { flush() }
                pendingBytes.append(String(Int8(bitPattern: b)))
            case let .zero(n):
                flush()
                directive(".space \(n)")
            }
            if pendingWords.count >= 8 || pendingBytes.count >= 16 { flush() }
        }
        flush()
        origin = nil
    }

    public static func escape(_ bytes: [UInt8]) -> String {
        var s = ""
        for b in bytes {
            switch b {
            case 10: s += "\\n"
            case 9: s += "\\t"
            case 13: s += "\\r"
            case 0: s += "\\0"
            case 34: s += "\\\""
            case 92: s += "\\\\"
            case 32..<127: s.append(Character(UnicodeScalar(b)))
            default: s += String(format: "\\x%02x", b)
            }
        }
        return s
    }

    // MARK: Frame

    private func layoutFrame() {
        var fr = FrameLayout(function: f.name)
        let hasCalls = f.blocks.contains { $0.insts.contains { if case .call = $0.kind { return true }; return false } }
        let leaf = !hasCalls && f.slots.isEmpty && alloc.spillSlots == 0 && alloc.usedCalleeSaved.isEmpty
        fr.hasFrame = !leaf
        if leaf {
            frame = fr
            return
        }
        var off = 0
        func place(_ size: Int, _ align: Int, _ what: String, _ kind: String) -> Int {
            off += size
            off = (off + align - 1) / align * align
            fr.entries.append(.init(offset: -off, size: size, what: what, kind: kind))
            return -off
        }
        _ = place(4, 4, "return address (ra)", "ra")
        _ = place(4, 4, "caller's frame pointer (old s0)", "fp")
        for s in alloc.usedCalleeSaved {
            fr.savedOffset[s] = place(4, 4, "saved \(r(s)) (callee-saved)", "saved")
        }
        for k in 0..<alloc.spillSlots {
            let name = alloc.intervals.first { $0.location == .spill(k) }?.name ?? "?"
            fr.spillOffset.append(place(4, 4, "spill slot for \(name)", "spill"))
        }
        for s in f.slots {
            fr.slotOffset.append(place(s.size, max(s.align, s.size >= 4 ? 4 : 1), "\(s.name.dropFirst()) (\(s.size) bytes)", "local"))
        }
        fr.size = (off + 15) / 16 * 16
        frame = fr
    }

    private func spillOffset(_ k: Int) -> Int { frame.spillOffset[k] }

    // MARK: Operands

    /// Loads a constant into a register (`li` is a pseudo-instruction: addi, or lui + addi).
    private func li(_ rd: Int, _ v: Int32) {
        emit("li \(r(rd)), \(v)")
    }

    /// The register holding an operand, loading it into `scratch` if needed.
    private func src(_ o: Operand, _ scratch: Int) -> Int {
        switch o {
        case .imm(0): return Reg.zero
        case let .imm(c):
            li(scratch, c)
            return scratch
        case let .v(v):
            switch alloc.locations[v] {
            case let .reg(x)?: return x
            case let .spill(k)?:
                memOp("lw", scratch, Reg.s0, spillOffset(k), note: "reload \(f.vregs[v].name)")
                return scratch
            case nil:
                // Never defined (read of an uninitialised variable): any value will do.
                return Reg.zero
            }
        }
    }

    /// The register to compute a result into, and a closure that stores it if the value lives in memory.
    private func dst(_ v: Int) -> (Int, () -> Void) {
        switch alloc.locations[v] {
        case let .reg(x)?: return (x, {})
        case let .spill(k)?:
            return (Reg.t6, { self.memOp("sw", Reg.t6, Reg.s0, self.spillOffset(k), note: "spill \(self.f.vregs[v].name)") })
        case nil:
            return (Reg.t6, {})
        }
    }

    /// `op rd, offset(base)`, using t4 to build the address if the offset does not fit in 12 bits.
    private func memOp(_ op: String, _ rd: Int, _ base: Int, _ offset: Int, note: String = "") {
        if fits12(offset) {
            emit("\(op) \(r(rd)), \(offset)(\(r(base)))", note: note)
        } else {
            li(Reg.t4, Int32(offset))
            emit("add t4, \(r(base)), t4")
            emit("\(op) \(r(rd)), 0(t4)", note: note)
        }
    }

    /// Resolves an IR address into base register + 12-bit offset. Uses t6 when needed.
    private func address(_ a: Addr) -> (Int, Int) {
        switch a {
        case let .slot(s, o):
            let off = frame.slotOffset[s] + Int(o)
            if fits12(off) { return (Reg.s0, off) }
            li(Reg.t6, Int32(off))
            emit("add t6, s0, t6")
            return (Reg.t6, 0)
        case let .global(g, o):
            emit("la t6, \(g)")
            if fits12(Int(o)) { return (Reg.t6, Int(o)) }
            li(Reg.t4, o)
            emit("add t6, t6, t4")
            return (Reg.t6, 0)
        case let .reg(base, o):
            let b = src(base, Reg.t6)
            if fits12(Int(o)) { return (b, Int(o)) }
            li(Reg.t4, o)
            emit("add t6, \(r(b)), t4")
            return (Reg.t6, 0)
        }
    }

    // MARK: Functions

    private func function(_ fn: IRFunction, _ a: Allocation) {
        f = fn
        alloc = a
        lastLine = -1
        layoutFrame()
        frames.append(frame)
        origin = AsmOrigin(inst: 0, node: 0, line: fn.line)
        directive(".globl \(fn.name)")
        out.append(AsmLine(.label, "\(fn.name):", origin: origin, function: fn.name))
        prologue()
        // Copy incoming arguments out of a0–a7.
        for (k, p) in fn.params.enumerated() {
            switch a.locations[p] {
            case let .reg(x)?: if x != Reg.a(k) { emit("mv \(r(x)), \(r(Reg.a(k)))", note: "parameter \(fn.vregs[p].name)") }
            case let .spill(s)?: memOp("sw", Reg.a(k), Reg.s0, spillOffset(s), note: "parameter \(fn.vregs[p].name)")
            case nil: break
            }
        }
        for (bi, b) in fn.blocks.enumerated() {
            if bi > 0 || predecessorsOfEntry() {
                out.append(AsmLine(.label, "\(CodeGen.label(fn.name, b)):", function: fn.name))
            }
            for i in b.insts {
                origin = AsmOrigin(inst: i.id, node: i.origin, line: i.line)
                sourceComment(i.line)
                inst(i)
            }
            origin = AsmOrigin(inst: b.termID, node: b.termOrigin, line: b.termLine)
            sourceComment(b.termLine)
            terminator(b, next: bi + 1 < fn.blocks.count ? fn.blocks[bi + 1] : nil)
        }
        out.append(AsmLine(.label, ".L\(fn.name)_return:", function: fn.name))
        origin = AsmOrigin(inst: 0, node: 0, line: 0)
        epilogue()
        origin = nil
    }

    private func predecessorsOfEntry() -> Bool {
        guard let e = f.blocks.first else { return false }
        return f.blocks.contains { $0.term.successors.contains(e.id) }
    }

    private func sourceComment(_ line: Int) {
        guard line > 0, line != lastLine, line <= sourceLines.count else { return }
        lastLine = line
        let text = sourceLines[line - 1].trimmingCharacters(in: .whitespaces)
        if !text.isEmpty { comment("\(line): \(text)") }
    }

    private func prologue() {
        guard frame.hasFrame else {
            comment("leaf function: no calls, nothing on the stack, so no frame is needed")
            return
        }
        let size = frame.size
        if size <= 2047 {
            emit("addi sp, sp, -\(size)", note: "allocate the \(size)-byte frame")
            emit("sw ra, \(size - 4)(sp)", note: "save return address")
            emit("sw s0, \(size - 8)(sp)", note: "save caller's frame pointer")
            emit("addi s0, sp, \(size)", note: "s0 = frame pointer")
        } else {
            emit("sw ra, -4(sp)", note: "save return address")
            emit("sw s0, -8(sp)", note: "save caller's frame pointer")
            emit("mv s0, sp", note: "s0 = frame pointer")
            li(Reg.t6, Int32(size))
            emit("sub sp, sp, t6", note: "allocate the \(size)-byte frame")
        }
        for s in alloc.usedCalleeSaved {
            memOp("sw", s, Reg.s0, frame.savedOffset[s]!, note: "\(r(s)) is callee-saved")
        }
    }

    private func epilogue() {
        guard frame.hasFrame else {
            emit("ret")
            return
        }
        for s in alloc.usedCalleeSaved {
            memOp("lw", s, Reg.s0, frame.savedOffset[s]!, note: "restore \(r(s))")
        }
        let size = frame.size
        if size <= 2047 {
            emit("lw ra, \(size - 4)(sp)", note: "restore return address")
            emit("lw s0, \(size - 8)(sp)", note: "restore caller's frame pointer")
            emit("addi sp, sp, \(size)", note: "free the frame")
        } else {
            emit("lw ra, -4(s0)", note: "restore return address")
            emit("mv sp, s0", note: "free the frame")
            emit("lw s0, -8(sp)", note: "restore caller's frame pointer")
        }
        emit("ret")
    }

    // MARK: Instructions

    private static let rType: [IROp: String] = [
        .add: "add", .sub: "sub", .mul: "mul", .div: "div", .divu: "divu", .rem: "rem", .remu: "remu",
        .and: "and", .or: "or", .xor: "xor", .shl: "sll", .shr: "srl", .sar: "sra", .lt: "slt", .ltu: "sltu",
    ]
    private static let iType: [IROp: String] = [
        .add: "addi", .and: "andi", .or: "ori", .xor: "xori", .lt: "slti", .ltu: "sltiu",
    ]
    private static let shiftImm: [IROp: String] = [.shl: "slli", .shr: "srli", .sar: "srai"]

    private func inst(_ i: Inst) {
        switch i.kind {
        case let .bin(op, d, a, b):
            binary(op, d, a, b)
        case let .un(op, d, a):
            let x = src(a, Reg.t5)
            let (rd, finish) = dst(d)
            switch op {
            case .neg: emit("neg \(r(rd)), \(r(x))")
            case .not: emit("not \(r(rd)), \(r(x))")
            case .sext8:
                emit("slli \(r(rd)), \(r(x)), 24", note: "sign-extend")
                emit("srai \(r(rd)), \(r(rd)), 24", note: "the low byte")
            }
            finish()
        case let .mov(d, a):
            let (rd, finish) = dst(d)
            switch a {
            case let .imm(c): li(rd, c)
            case .v:
                let x = src(a, rd)
                if x != rd { emit("mv \(r(rd)), \(r(x))") }
            }
            finish()
        case let .load(d, a, size):
            let (base, off) = address(a)
            let (rd, finish) = dst(d)
            emit("\(size == 1 ? "lb" : "lw") \(r(rd)), \(off)(\(r(base)))")
            finish()
        case let .store(v, a, size):
            let x = src(v, Reg.t5)
            let (base, off) = address(a)
            emit("\(size == 1 ? "sb" : "sw") \(r(x)), \(off)(\(r(base)))")
        case let .addr(d, a):
            let (rd, finish) = dst(d)
            switch a {
            case let .slot(s, o):
                let off = frame.slotOffset[s] + Int(o)
                if fits12(off) { emit("addi \(r(rd)), s0, \(off)", note: "&\(f.slots[s].name.dropFirst())") }
                else {
                    li(rd, Int32(off))
                    emit("add \(r(rd)), s0, \(r(rd))", note: "&\(f.slots[s].name.dropFirst())")
                }
            case let .global(g, o):
                emit("la \(r(rd)), \(g)", note: "&\(g)")
                if o != 0 { addImm(rd, rd, o) }
            case let .reg(b, o):
                let x = src(b, Reg.t5)
                addImm(rd, x, o)
            }
            finish()
        case let .call(d, name, args):
            for (k, a) in args.enumerated() {
                let target = Reg.a(k)
                switch a {
                case let .imm(c): li(target, c)
                case let .v(v):
                    switch alloc.locations[v] {
                    case let .reg(x)?: emit("mv \(r(target)), \(r(x))")
                    case let .spill(s)?: memOp("lw", target, Reg.s0, spillOffset(s))
                    case nil: break
                    }
                }
            }
            emit("call \(name)")
            if let d {
                let (rd, finish) = dst(d)
                if rd != Reg.a0 { emit("mv \(r(rd)), a0", note: "result") }
                finish()
            }
        }
    }

    private func addImm(_ rd: Int, _ rs: Int, _ c: Int32) {
        if c == 0 {
            if rd != rs { emit("mv \(r(rd)), \(r(rs))") }
        } else if fits12(Int(c)) {
            emit("addi \(r(rd)), \(r(rs)), \(c)")
        } else {
            li(Reg.t4, c)
            emit("add \(r(rd)), \(r(rs)), t4")
        }
    }

    private func binary(_ op: IROp, _ d: Int, _ a: Operand, _ b: Operand) {
        var a = a, b = b
        if a.constant != nil && b.constant == nil && op.isCommutative { swap(&a, &b) }
        // Immediate forms: addi, andi, slti, slli…
        if case let .imm(c) = b {
            if let name = CodeGen.shiftImm[op] {
                let x = src(a, Reg.t5)
                let (rd, finish) = dst(d)
                emit("\(name) \(r(rd)), \(r(x)), \(c & 31)")
                finish()
                return
            }
            var opI = op, cI = c
            if op == .sub && c != Int32.min { opI = .add; cI = -c }
            if let name = CodeGen.iType[opI], fits12(Int(cI)) {
                let x = src(a, Reg.t5)
                let (rd, finish) = dst(d)
                emit("\(name) \(r(rd)), \(r(x)), \(cI)")
                finish()
                return
            }
            if (op == .eq || op == .ne), fits12(Int(c)) {
                let x = src(a, Reg.t5)
                let (rd, finish) = dst(d)
                let set = op == .eq ? "seqz" : "snez"
                if c == 0 {
                    emit("\(set) \(r(rd)), \(r(x))")
                } else {
                    emit("xori \(r(rd)), \(r(x)), \(c)", note: "zero if equal")
                    emit("\(set) \(r(rd)), \(r(rd))")
                }
                finish()
                return
            }
            if (op == .le || op == .leu) && c != Int32.max && fits12(Int(c) + 1) {
                // x <= c  ⟺  x < c + 1
                if op == .le || c != -1 {
                    let x = src(a, Reg.t5)
                    let (rd, finish) = dst(d)
                    emit("\(op == .le ? "slti" : "sltiu") \(r(rd)), \(r(x)), \(c + 1)")
                    finish()
                    return
                }
            }
        }
        let x = src(a, Reg.t5)
        let y = src(b, Reg.t6)
        let (rd, finish) = dst(d)
        switch op {
        case .gt: emit("slt \(r(rd)), \(r(y)), \(r(x))")
        case .gtu: emit("sltu \(r(rd)), \(r(y)), \(r(x))")
        case .le:
            emit("slt \(r(rd)), \(r(y)), \(r(x))", note: "b < a …")
            emit("xori \(r(rd)), \(r(rd)), 1", note: "… a <= b is the opposite")
        case .leu:
            emit("sltu \(r(rd)), \(r(y)), \(r(x))")
            emit("xori \(r(rd)), \(r(rd)), 1")
        case .ge:
            emit("slt \(r(rd)), \(r(x)), \(r(y))", note: "a < b …")
            emit("xori \(r(rd)), \(r(rd)), 1", note: "… a >= b is the opposite")
        case .geu:
            emit("sltu \(r(rd)), \(r(x)), \(r(y))")
            emit("xori \(r(rd)), \(r(rd)), 1")
        case .eq:
            emit("sub \(r(rd)), \(r(x)), \(r(y))")
            emit("seqz \(r(rd)), \(r(rd))")
        case .ne:
            emit("sub \(r(rd)), \(r(x)), \(r(y))")
            emit("snez \(r(rd)), \(r(rd))")
        default:
            emit("\(CodeGen.rType[op]!) \(r(rd)), \(r(x)), \(r(y))")
        }
        finish()
    }

    private func terminator(_ b: Block, next: Block?) {
        switch b.term {
        case let .jump(t):
            if next?.id != t { emit("j \(CodeGen.label(f.name, f.block(t)!))") }
        case let .branch(op, a, c, t, e):
            let x = src(a, Reg.t5)
            let y = src(c, Reg.t6)
            var (op, tt, ee) = (op, t, e)
            // Branch to the far block; fall through to the one laid out next.
            if next?.id == tt {
                op = op.negated
                swap(&tt, &ee)
            }
            let tl = CodeGen.label(f.name, f.block(tt)!)
            switch op {
            case .eq: emit(y == 0 ? "beqz \(r(x)), \(tl)" : "beq \(r(x)), \(r(y)), \(tl)")
            case .ne: emit(y == 0 ? "bnez \(r(x)), \(tl)" : "bne \(r(x)), \(r(y)), \(tl)")
            case .lt: emit("blt \(r(x)), \(r(y)), \(tl)")
            case .ge: emit("bge \(r(x)), \(r(y)), \(tl)")
            case .gt: emit("bgt \(r(x)), \(r(y)), \(tl)")
            case .le: emit("ble \(r(x)), \(r(y)), \(tl)")
            case .ltu: emit("bltu \(r(x)), \(r(y)), \(tl)")
            case .geu: emit("bgeu \(r(x)), \(r(y)), \(tl)")
            case .gtu: emit("bgtu \(r(x)), \(r(y)), \(tl)")
            case .leu: emit("bleu \(r(x)), \(r(y)), \(tl)")
            default: break
            }
            if next?.id != ee { emit("j \(CodeGen.label(f.name, f.block(ee)!))") }
        case let .ret(v):
            if let v {
                switch v {
                case let .imm(c): li(Reg.a0, c)
                case .v:
                    let x = src(v, Reg.a0)
                    if x != Reg.a0 { emit("mv a0, \(r(x))", note: "return value") }
                }
            }
            if next != nil { emit("j .L\(f.name)_return") }
        }
    }
}
