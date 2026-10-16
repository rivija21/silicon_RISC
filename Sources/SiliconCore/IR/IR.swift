import Foundation

// The intermediate representation: three-address code in basic blocks.
//
// Each instruction does one small thing (`%t3 = %i * 4`). Values live in an
// unlimited supply of virtual registers (%t1, %x …); the register allocator
// later maps them onto the 32 real RISC-V registers. Instructions are grouped
// into basic blocks: straight-line code that is entered only at the top and
// left only at the bottom, by a jump, a conditional branch or a return.

/// An instruction operand: a virtual register or a constant.
public enum Operand: Hashable, Sendable {
    case v(Int)
    case imm(Int32)

    public var vreg: Int? { if case let .v(r) = self { return r }; return nil }
    public var constant: Int32? { if case let .imm(c) = self { return c }; return nil }
}

/// A memory address: a stack slot, a global symbol or a register, plus a byte offset.
public enum Addr: Hashable, Sendable {
    case slot(Int, Int32)
    case global(String, Int32)
    case reg(Operand, Int32)

    public func adding(_ k: Int32) -> Addr {
        switch self {
        case let .slot(s, o): return .slot(s, o &+ k)
        case let .global(g, o): return .global(g, o &+ k)
        case let .reg(r, o): return .reg(r, o &+ k)
        }
    }
    public var baseVReg: Int? { if case let .reg(.v(r), _) = self { return r }; return nil }
}

public enum IROp: String, Sendable, CaseIterable {
    case add, sub, mul, div, divu, rem, remu, and, or, xor, shl, shr, sar
    case eq, ne, lt, le, gt, ge, ltu, leu, gtu, geu

    public var symbol: String {
        switch self {
        case .add: return "+"
        case .sub: return "-"
        case .mul: return "*"
        case .div: return "/"
        case .divu: return "/u"
        case .rem: return "%"
        case .remu: return "%u"
        case .and: return "&"
        case .or: return "|"
        case .xor: return "^"
        case .shl: return "<<"
        case .shr: return ">>u"
        case .sar: return ">>"
        case .eq: return "=="
        case .ne: return "!="
        case .lt: return "<"
        case .le: return "<="
        case .gt: return ">"
        case .ge: return ">="
        case .ltu: return "<u"
        case .leu: return "<=u"
        case .gtu: return ">u"
        case .geu: return ">=u"
        }
    }

    public var isComparison: Bool {
        switch self {
        case .eq, .ne, .lt, .le, .gt, .ge, .ltu, .leu, .gtu, .geu: return true
        default: return false
        }
    }
    public var isCommutative: Bool {
        switch self {
        case .add, .mul, .and, .or, .xor, .eq, .ne: return true
        default: return false
        }
    }
    /// a op b == b swapped(op) a
    public var swapped: IROp {
        switch self {
        case .lt: return .gt
        case .gt: return .lt
        case .le: return .ge
        case .ge: return .le
        case .ltu: return .gtu
        case .gtu: return .ltu
        case .leu: return .geu
        case .geu: return .leu
        default: return self
        }
    }
    /// !(a op b) == a negated(op) b
    public var negated: IROp {
        switch self {
        case .eq: return .ne
        case .ne: return .eq
        case .lt: return .ge
        case .ge: return .lt
        case .gt: return .le
        case .le: return .gt
        case .ltu: return .geu
        case .geu: return .ltu
        case .gtu: return .leu
        case .leu: return .gtu
        default: return self
        }
    }

    /// Evaluates the operation on constants, with RISC-V semantics.
    public func fold(_ a: Int32, _ b: Int32) -> Int32 {
        let ua = UInt32(bitPattern: a), ub = UInt32(bitPattern: b)
        switch self {
        case .add: return a &+ b
        case .sub: return a &- b
        case .mul: return a &* b
        case .div: return RV.div(a, b)
        case .divu: return Int32(bitPattern: RV.divu(ua, ub))
        case .rem: return RV.rem(a, b)
        case .remu: return Int32(bitPattern: RV.remu(ua, ub))
        case .and: return a & b
        case .or: return a | b
        case .xor: return a ^ b
        case .shl: return Int32(bitPattern: ua << (ub & 31))
        case .shr: return Int32(bitPattern: ua >> (ub & 31))
        case .sar: return a >> Int32(ub & 31)
        case .eq: return a == b ? 1 : 0
        case .ne: return a != b ? 1 : 0
        case .lt: return a < b ? 1 : 0
        case .le: return a <= b ? 1 : 0
        case .gt: return a > b ? 1 : 0
        case .ge: return a >= b ? 1 : 0
        case .ltu: return ua < ub ? 1 : 0
        case .leu: return ua <= ub ? 1 : 0
        case .gtu: return ua > ub ? 1 : 0
        case .geu: return ua >= ub ? 1 : 0
        }
    }
}

public enum UnOp: String, Sendable {
    /// -x
    case neg
    /// ~x
    case not
    /// sign-extend the low byte (conversion to char)
    case sext8 = "sext.b"

    public func fold(_ a: Int32) -> Int32 {
        switch self {
        case .neg: return 0 &- a
        case .not: return ~a
        case .sext8: return Int32(Int8(truncatingIfNeeded: a))
        }
    }
}

public enum InstKind: Hashable, Sendable {
    case bin(IROp, Int, Operand, Operand)
    case un(UnOp, Int, Operand)
    case mov(Int, Operand)
    /// size 1 (char, sign-extended) or 4
    case load(Int, Addr, Int)
    case store(Operand, Addr, Int)
    /// dst = the address itself
    case addr(Int, Addr)
    case call(Int?, String, [Operand])
}

public struct Inst: Sendable {
    public var kind: InstKind
    /// Stable identity, kept when a pass rewrites the instruction, so views can link IR ↔ assembly.
    public var id: Int
    /// The syntax-tree node it came from, and its source line.
    public var origin: Int
    public var line: Int

    public var def: Int? {
        switch kind {
        case let .bin(_, d, _, _), let .un(_, d, _), let .mov(d, _), let .load(d, _, _), let .addr(d, _): return d
        case let .call(d, _, _): return d
        case .store: return nil
        }
    }

    public var uses: [Int] {
        func ops(_ o: [Operand]) -> [Int] { o.compactMap { $0.vreg } }
        func addr(_ a: Addr) -> [Int] { a.baseVReg.map { [$0] } ?? [] }
        switch kind {
        case let .bin(_, _, a, b): return ops([a, b])
        case let .un(_, _, a), let .mov(_, a): return ops([a])
        case let .load(_, a, _), let .addr(_, a): return addr(a)
        case let .store(v, a, _): return ops([v]) + addr(a)
        case let .call(_, _, args): return ops(args)
        }
    }

    /// Instructions with no effect other than writing their destination.
    public var isPure: Bool {
        switch kind {
        case .bin, .un, .mov, .addr, .load: return true
        case .store, .call: return false
        }
    }

    /// Rewrites every operand that reads a virtual register.
    public mutating func mapUses(_ f: (Operand) -> Operand) {
        func ma(_ a: Addr) -> Addr {
            if case let .reg(r, o) = a { return .reg(f(r), o) }
            return a
        }
        switch kind {
        case let .bin(op, d, a, b): kind = .bin(op, d, f(a), f(b))
        case let .un(op, d, a): kind = .un(op, d, f(a))
        case let .mov(d, a): kind = .mov(d, f(a))
        case let .load(d, a, s): kind = .load(d, ma(a), s)
        case let .store(v, a, s): kind = .store(f(v), ma(a), s)
        case let .addr(d, a): kind = .addr(d, ma(a))
        case let .call(d, n, args): kind = .call(d, n, args.map(f))
        }
    }
}

public enum Terminator: Hashable, Sendable {
    case jump(Int)
    /// if a op b goto t else goto f
    case branch(IROp, Operand, Operand, Int, Int)
    case ret(Operand?)

    public var successors: [Int] {
        switch self {
        case let .jump(t): return [t]
        case let .branch(_, _, _, t, f): return t == f ? [t] : [t, f]
        case .ret: return []
        }
    }
    public var uses: [Int] {
        switch self {
        case let .branch(_, a, b, _, _): return [a, b].compactMap { $0.vreg }
        case let .ret(v?): return v.vreg.map { [$0] } ?? []
        default: return []
        }
    }
    public mutating func mapUses(_ f: (Operand) -> Operand) {
        switch self {
        case let .branch(op, a, b, t, e): self = .branch(op, f(a), f(b), t, e)
        case let .ret(v?): self = .ret(f(v))
        default: break
        }
    }
    public mutating func mapTargets(_ f: (Int) -> Int) {
        switch self {
        case let .jump(t): self = .jump(f(t))
        case let .branch(op, a, b, t, e): self = .branch(op, a, b, f(t), f(e))
        case .ret: break
        }
    }
}

public final class Block: @unchecked Sendable {
    public let id: Int
    public var name: String
    public var insts: [Inst] = []
    public var term: Terminator = .ret(nil)
    public var termID = 0
    public var termOrigin = 0
    public var termLine = 0

    init(id: Int, name: String) {
        self.id = id
        self.name = name
    }

    func copy() -> Block {
        let b = Block(id: id, name: name)
        b.insts = insts
        b.term = term
        b.termID = termID
        b.termOrigin = termOrigin
        b.termLine = termLine
        return b
    }
}

/// A stack slot: memory for a local variable, an array, or a struct.
public struct Slot: Sendable {
    public var name: String
    public var size: Int
    public var align: Int
    public var symbolID: Int?
    /// Scalars may be promoted to registers by mem2reg; arrays and structs may not.
    public var isScalar: Bool
}

public struct VRegInfo: Sendable {
    public var name: String
    /// Named after a C variable (vs. a compiler temporary).
    public var isVariable: Bool
}

/// Hands out unique instruction ids for a whole compilation.
public final class IDGen: @unchecked Sendable {
    private var n: Int
    public init(_ start: Int = 0) { n = start }
    public func next() -> Int {
        n += 1
        return n
    }
}

public final class IRFunction: @unchecked Sendable {
    public let name: String
    public var params: [Int] = []
    public var blocks: [Block] = []
    public var slots: [Slot] = []
    public var vregs: [VRegInfo] = [VRegInfo(name: "%invalid", isVariable: false)]
    public var returnsValue = true
    public var line = 0
    public var nextBlock = 0
    public let ids: IDGen

    init(name: String, ids: IDGen) {
        self.name = name
        self.ids = ids
    }

    public func newVReg(_ name: String? = nil, variable: Bool = false) -> Int {
        let n = vregs.count
        vregs.append(VRegInfo(name: name ?? "%t\(n)", isVariable: variable))
        return n
    }

    public func block(_ id: Int) -> Block? { blocks.first { $0.id == id } }

    public func copy() -> IRFunction {
        let f = IRFunction(name: name, ids: ids)
        f.params = params
        f.blocks = blocks.map { $0.copy() }
        f.slots = slots
        f.vregs = vregs
        f.returnsValue = returnsValue
        f.line = line
        f.nextBlock = nextBlock
        return f
    }

    public var instructionCount: Int { blocks.reduce(0) { $0 + $1.insts.count + 1 } }

    public func predecessors() -> [Int: [Int]] {
        var p: [Int: [Int]] = [:]
        for b in blocks {
            p[b.id, default: []] += []
            for s in b.term.successors { p[s, default: []].append(b.id) }
        }
        return p
    }
}

/// Initial contents of a global in the data section.
public enum DataItem: Sendable, Hashable {
    case word(Int32)
    case byte(UInt8)
    case zero(Int)
    case address(String, Int32)
}

public struct GlobalData: Sendable {
    public var label: String
    public var align: Int
    public var items: [DataItem]
    public var size: Int
    public var line: Int
    public var comment: String
}

public final class IRModule: @unchecked Sendable {
    public var functions: [IRFunction] = []
    public var data: [GlobalData] = []

    public func copy() -> IRModule {
        let m = IRModule()
        m.functions = functions.map { $0.copy() }
        m.data = data
        return m
    }
    public var instructionCount: Int { functions.reduce(0) { $0 + $1.instructionCount } }
}

// MARK: - Printing

public enum IRPrinter {
    public static func operand(_ o: Operand, _ f: IRFunction) -> String {
        switch o {
        case let .v(r): return f.vregs[r].name
        case let .imm(c): return String(c)
        }
    }

    public static func addr(_ a: Addr, _ f: IRFunction) -> String {
        func off(_ o: Int32) -> String { o == 0 ? "" : o > 0 ? " + \(o)" : " - \(-Int(o))" }
        switch a {
        case let .slot(s, o): return "[\(f.slots[s].name)\(off(o))]"
        case let .global(g, o): return "[@\(g)\(off(o))]"
        case let .reg(r, o): return "[\(operand(r, f))\(off(o))]"
        }
    }

    public static func inst(_ i: Inst, _ f: IRFunction) -> String {
        func v(_ r: Int) -> String { f.vregs[r].name }
        func o(_ x: Operand) -> String { operand(x, f) }
        switch i.kind {
        case let .bin(op, d, a, b): return "\(v(d)) = \(o(a)) \(op.symbol) \(o(b))"
        case let .un(op, d, a):
            switch op {
            case .neg: return "\(v(d)) = -\(o(a))"
            case .not: return "\(v(d)) = ~\(o(a))"
            case .sext8: return "\(v(d)) = (char) \(o(a))"
            }
        case let .mov(d, a): return "\(v(d)) = \(o(a))"
        case let .load(d, a, s): return "\(v(d)) = load\(s == 1 ? ".b" : ".w") \(addr(a, f))"
        case let .store(x, a, s): return "store\(s == 1 ? ".b" : ".w") \(addr(a, f)) ← \(o(x))"
        case let .addr(d, a): return "\(v(d)) = &\(addr(a, f))"
        case let .call(d, n, args):
            let c = "call \(n)(\(args.map(o).joined(separator: ", ")))"
            return d.map { "\(v($0)) = \(c)" } ?? c
        }
    }

    public static func term(_ t: Terminator, _ f: IRFunction) -> String {
        func name(_ id: Int) -> String { f.block(id)?.name ?? "?\(id)" }
        switch t {
        case let .jump(b): return "goto \(name(b))"
        case let .branch(op, a, b, t, e):
            return "if \(operand(a, f)) \(op.symbol) \(operand(b, f)) goto \(name(t)) else \(name(e))"
        case let .ret(v): return v.map { "return \(operand($0, f))" } ?? "return"
        }
    }

    public static func function(_ f: IRFunction) -> String {
        var s = "function \(f.name)(\(f.params.map { f.vregs[$0].name }.joined(separator: ", ")))\n"
        if !f.slots.isEmpty {
            s += "  stack slots: " + f.slots.map { "\($0.name) (\($0.size) B)" }.joined(separator: ", ") + "\n"
        }
        for b in f.blocks {
            s += "\(b.name):\n"
            for i in b.insts { s += "    \(inst(i, f))\n" }
            s += "    \(term(b.term, f))\n"
        }
        return s
    }

    public static func module(_ m: IRModule) -> String {
        m.functions.map(function).joined(separator: "\n")
    }
}
