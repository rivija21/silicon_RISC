import Foundation

/// The optimiser passes, in the order they run.
public enum PassID: String, CaseIterable, Sendable {
    case mem2reg, constFold, simplify, copyProp, cse, dce, cfg

    public var title: String {
        switch self {
        case .mem2reg: return "Promote locals to registers (mem2reg)"
        case .constFold: return "Constant propagation & folding"
        case .simplify: return "Algebraic simplification & strength reduction"
        case .copyProp: return "Copy propagation"
        case .cse: return "Common subexpressions & redundant loads"
        case .dce: return "Dead code elimination"
        case .cfg: return "Control-flow cleanup"
        }
    }

    public var shortTitle: String {
        switch self {
        case .mem2reg: return "mem2reg"
        case .constFold: return "Const fold"
        case .simplify: return "Simplify"
        case .copyProp: return "Copy prop"
        case .cse: return "CSE"
        case .dce: return "DCE"
        case .cfg: return "CFG"
        }
    }

    public var explanation: String {
        switch self {
        case .mem2reg:
            return "A local scalar whose address is never taken (no &x) cannot be reached through a pointer, so it does not need memory at all. Its loads and stores become plain register moves. This is the biggest single win: it removes most lw/sw instructions."
        case .constFold:
            return "Tracks which virtual registers hold a known constant on every path (a data-flow analysis over the control-flow graph), substitutes the constants, and computes constant expressions at compile time: 3 * 4 becomes 12. Branches whose outcome is known become plain jumps."
        case .simplify:
            return "Applies algebraic identities (x + 0 = x, x * 1 = x, x - x = 0) and replaces expensive operations by cheap ones: x * 8 → x << 3, unsigned x / 16 → x >> 4, and signed x / 4 → shifts with a rounding fix-up. It also folds constant offsets into addresses, so a[3] becomes a single lw with offset 12."
        case .copyProp:
            return "After %a = %b, later uses of %a can read %b directly. The copy itself then becomes dead and DCE removes it."
        case .cse:
            return "If the same expression is computed twice in a block with unchanged operands, the second one reuses the first result. Loads are reused until the next store or call, and a load right after a store to the same address gets the stored value."
        case .dce:
            return "Removes instructions whose result is never used. Liveness analysis runs backwards over the control-flow graph to find which registers are still needed."
        case .cfg:
            return "Removes unreachable blocks, skips blocks that only jump elsewhere, and merges a block into its predecessor when it is the only way in."
        }
    }
}

public struct PassSnapshot {
    /// nil for the unoptimised IR and for the final cleanup round.
    public var pass: PassID?
    public var title: String
    public var module: IRModule
    public var changed: Bool
    public var instructions: Int
}

public enum Optimizer {
    /// Runs the enabled passes and returns the IR after each one.
    public static func run(_ input: IRModule, enabled: Set<PassID>) -> [PassSnapshot] {
        var snaps = [PassSnapshot(pass: nil, title: "Unoptimised IR (straight from the syntax tree)", module: input.copy(),
                                  changed: false, instructions: input.instructionCount)]
        let m = input.copy()
        for p in PassID.allCases where enabled.contains(p) {
            var changed = false
            for f in m.functions { changed = apply(p, f) || changed }
            snaps.append(PassSnapshot(pass: p, title: p.title, module: m.copy(), changed: changed, instructions: m.instructionCount))
        }
        // One pass often creates work for another (folding a constant makes code dead,
        // removing code makes blocks empty…), so the cheap passes repeat until nothing changes.
        let cleanup: [PassID] = [.constFold, .simplify, .copyProp, .cse, .dce, .cfg].filter { enabled.contains($0) }
        if !cleanup.isEmpty {
            var any = false
            for _ in 0..<6 {
                var changed = false
                for p in cleanup {
                    for f in m.functions { changed = apply(p, f) || changed }
                }
                any = any || changed
                if !changed { break }
            }
            snaps.append(PassSnapshot(pass: nil, title: "Repeat until nothing changes", module: m.copy(), changed: any,
                                      instructions: m.instructionCount))
        }
        return snaps
    }

    public static func apply(_ p: PassID, _ f: IRFunction) -> Bool {
        switch p {
        case .mem2reg: return Mem2Reg.run(f)
        case .constFold: return ConstProp.run(f)
        case .simplify: return Simplify.run(f)
        case .copyProp: return CopyProp.run(f)
        case .cse: return CSE.run(f)
        case .dce: return DCE.run(f)
        case .cfg: return CFGSimplify.run(f)
        }
    }
}

// MARK: - Analyses

public enum Liveness {
    public struct Result {
        public var liveIn: [Int: Set<Int>]
        public var liveOut: [Int: Set<Int>]
    }

    /// Classic backward data-flow: in[b] = use[b] ∪ (out[b] − def[b]), out[b] = ∪ in[successors].
    public static func analyze(_ f: IRFunction) -> Result {
        var use: [Int: Set<Int>] = [:], def: [Int: Set<Int>] = [:]
        for b in f.blocks {
            var u = Set<Int>(), d = Set<Int>()
            for i in b.insts {
                for r in i.uses where !d.contains(r) { u.insert(r) }
                if let x = i.def { d.insert(x) }
            }
            for r in b.term.uses where !d.contains(r) { u.insert(r) }
            use[b.id] = u
            def[b.id] = d
        }
        var liveIn: [Int: Set<Int>] = [:], liveOut: [Int: Set<Int>] = [:]
        for b in f.blocks {
            liveIn[b.id] = []
            liveOut[b.id] = []
        }
        var changed = true
        while changed {
            changed = false
            for b in f.blocks.reversed() {
                var out = Set<Int>()
                for s in b.term.successors { out.formUnion(liveIn[s] ?? []) }
                let inn = use[b.id]!.union(out.subtracting(def[b.id]!))
                if out != liveOut[b.id] || inn != liveIn[b.id] {
                    liveOut[b.id] = out
                    liveIn[b.id] = inn
                    changed = true
                }
            }
        }
        return Result(liveIn: liveIn, liveOut: liveOut)
    }
}

/// Blocks in reverse post-order (each block before its successors, loops aside).
func reversePostOrder(_ f: IRFunction) -> [Block] {
    guard let entry = f.blocks.first else { return [] }
    var seen: Set<Int> = []
    var order: [Int] = []
    func visit(_ id: Int) {
        guard !seen.contains(id) else { return }
        seen.insert(id)
        for s in f.block(id)?.term.successors ?? [] { visit(s) }
        order.append(id)
    }
    visit(entry.id)
    return order.reversed().compactMap { f.block($0) }
}

func defCounts(_ f: IRFunction) -> [Int: Int] {
    var c: [Int: Int] = [:]
    for b in f.blocks {
        for i in b.insts { if let d = i.def { c[d, default: 0] += 1 } }
    }
    return c
}

// MARK: - mem2reg

enum Mem2Reg {
    static func run(_ f: IRFunction) -> Bool {
        var promotable = Set(f.slots.indices.filter { f.slots[$0].isScalar })
        func check(_ a: Addr, size: Int?) {
            if case let .slot(s, off) = a {
                if off != 0 { promotable.remove(s) }
                if let size, size != f.slots[s].size { promotable.remove(s) }
            }
        }
        for b in f.blocks {
            for i in b.insts {
                switch i.kind {
                case let .addr(_, a): if case let .slot(s, _) = a { promotable.remove(s) }
                case let .load(_, a, size): check(a, size: size)
                case let .store(_, a, size): check(a, size: size)
                default: break
                }
            }
        }
        guard !promotable.isEmpty else { return false }

        // A parameter's slot can reuse the incoming parameter register itself.
        var reg: [Int: Int] = [:]
        var paramStore: Set<Int> = []
        if let entry = f.blocks.first {
            for i in entry.insts {
                guard case let .store(.v(p), .slot(s, 0), _) = i.kind, f.params.contains(p), promotable.contains(s), reg[s] == nil else { continue }
                reg[s] = p
                paramStore.insert(i.id)
            }
        }
        for s in promotable.sorted() where reg[s] == nil {
            reg[s] = f.newVReg("%" + f.slots[s].name.dropFirst(), variable: true)
        }
        for b in f.blocks {
            var out: [Inst] = []
            for var i in b.insts {
                switch i.kind {
                case let .load(d, .slot(s, 0), _) where reg[s] != nil:
                    i.kind = .mov(d, .v(reg[s]!))
                case let .store(v, .slot(s, 0), size) where reg[s] != nil:
                    if paramStore.contains(i.id) {
                        // A char parameter still needs truncating to 8 bits.
                        if size == 1 { i.kind = .un(.sext8, reg[s]!, v) } else { continue }
                    } else {
                        i.kind = size == 1 ? .un(.sext8, reg[s]!, v) : .mov(reg[s]!, v)
                    }
                default: break
                }
                out.append(i)
            }
            b.insts = out
        }
        // Remove the now-unused slots, renumbering the rest.
        var newIndex: [Int: Int] = [:]
        var kept: [Slot] = []
        for (k, s) in f.slots.enumerated() where !promotable.contains(k) {
            newIndex[k] = kept.count
            kept.append(s)
        }
        f.slots = kept
        func remap(_ a: Addr) -> Addr {
            if case let .slot(s, o) = a { return .slot(newIndex[s]!, o) }
            return a
        }
        for b in f.blocks {
            for k in b.insts.indices {
                switch b.insts[k].kind {
                case let .load(d, a, s): b.insts[k].kind = .load(d, remap(a), s)
                case let .store(v, a, s): b.insts[k].kind = .store(v, remap(a), s)
                case let .addr(d, a): b.insts[k].kind = .addr(d, remap(a))
                default: break
                }
            }
        }
        return true
    }
}

// MARK: - Constant propagation

enum ConstProp {
    enum Lat: Equatable {
        case top
        case const(Int32)
        case bottom

        func meet(_ o: Lat) -> Lat {
            switch (self, o) {
            case (.top, _): return o
            case (_, .top): return self
            case let (.const(a), .const(b)): return a == b ? self : .bottom
            default: return .bottom
            }
        }
    }

    static func evaluate(_ i: Inst, _ st: [Lat]) -> Lat {
        func val(_ o: Operand) -> Lat {
            switch o {
            case let .imm(c): return .const(c)
            case let .v(r): return st[r]
            }
        }
        switch i.kind {
        case let .bin(op, _, a, b):
            let x = val(a), y = val(b)
            if case let .const(p) = x, case let .const(q) = y { return .const(op.fold(p, q)) }
            if (op == .mul || op == .and), x == .const(0) || y == .const(0) { return .const(0) }
            if x == .bottom || y == .bottom { return .bottom }
            return .top
        case let .un(op, _, a):
            switch val(a) {
            case let .const(c): return .const(op.fold(c))
            case let l: return l
            }
        case let .mov(_, a): return val(a)
        default: return .bottom
        }
    }

    static func transfer(_ b: Block, _ inState: [Lat]) -> [Lat] {
        var st = inState
        for i in b.insts {
            if let d = i.def { st[d] = evaluate(i, st) }
        }
        return st
    }

    static func run(_ f: IRFunction) -> Bool {
        let n = f.vregs.count
        let order = reversePostOrder(f)
        guard let entry = order.first else { return false }
        let preds = f.predecessors()
        var outs: [Int: [Lat]] = [:]
        func inState(_ b: Block) -> [Lat] {
            if b.id == entry.id {
                // Parameters (and anything read before it is written) are unknown on entry.
                var s = [Lat](repeating: .bottom, count: n)
                if let p = preds[b.id], !p.isEmpty {
                    for q in p { if let o = outs[q] { for k in 0..<n { s[k] = s[k].meet(o[k]) } } }
                }
                return s
            }
            var s = [Lat](repeating: .top, count: n)
            for q in preds[b.id] ?? [] {
                if let o = outs[q] { for k in 0..<n { s[k] = s[k].meet(o[k]) } }
            }
            return s
        }
        var changed = true
        var rounds = 0
        while changed && rounds < 50 {
            changed = false
            rounds += 1
            for b in order {
                let o = transfer(b, inState(b))
                if outs[b.id] != o {
                    outs[b.id] = o
                    changed = true
                }
            }
        }
        // Rewrite using the facts.
        var modified = false
        for b in order {
            var st = inState(b)
            func sub(_ o: Operand) -> Operand {
                if case let .v(r) = o, case let .const(c) = st[r] {
                    modified = true
                    return .imm(c)
                }
                return o
            }
            for k in b.insts.indices {
                var i = b.insts[k]
                let result = evaluate(i, st)
                i.mapUses(sub)
                if let d = i.def, case let .const(c) = result, i.isPure {
                    if case .mov(_, .imm(c)) = i.kind {} else {
                        i.kind = .mov(d, .imm(c))
                        modified = true
                    }
                }
                if let d = i.def { st[d] = result }
                b.insts[k] = i
            }
            b.term.mapUses(sub)
            if case let .branch(op, .imm(x), .imm(y), t, e) = b.term {
                b.term = .jump(op.fold(x, y) != 0 ? t : e)
                modified = true
            }
        }
        return modified
    }
}

// MARK: - Algebraic simplification and strength reduction

enum Simplify {
    static func log2(_ c: Int32) -> Int32? {
        guard c > 0, c & (c - 1) == 0 else { return nil }
        return Int32(c.trailingZeroBitCount)
    }

    static func run(_ f: IRFunction) -> Bool {
        var changed = false
        for b in f.blocks {
            // Definitions made earlier in this block whose operands are still unchanged.
            var avail: [Int: InstKind] = [:]
            func invalidate(_ r: Int) {
                avail[r] = nil
                for (k, v) in avail {
                    let tmp = Inst(kind: v, id: 0, origin: 0, line: 0)
                    if tmp.uses.contains(r) { avail[k] = nil }
                }
            }
            func fold(_ a: Addr) -> Addr {
                guard case let .reg(.v(t), off) = a, let def = avail[t] else { return a }
                // Only fold while the offset still fits a 12-bit load/store immediate.
                switch def {
                case let .bin(.add, _, x, .imm(k)) where abs(Int(off) + Int(k)) < 2048: return .reg(x, off &+ k)
                case let .addr(_, .slot(s, o)) where abs(Int(o) + Int(off)) < 2048: return .slot(s, o &+ off)
                case let .addr(_, .global(g, o)) where abs(Int(o) + Int(off)) < 2048: return .global(g, o &+ off)
                case let .addr(_, .reg(x, o)) where abs(Int(o) + Int(off)) < 2048: return .reg(x, o &+ off)
                default: return a
                }
            }
            var out: [Inst] = []
            for var i in b.insts {
                let before = i.kind
                switch i.kind {
                case let .load(d, a, s): i.kind = .load(d, fold(a), s)
                case let .store(v, a, s): i.kind = .store(v, fold(a), s)
                case let .addr(d, a):
                    let fa = fold(a)
                    if case let .reg(x, 0) = fa { i.kind = .mov(d, x) }
                    else if case let .reg(x, k) = fa { i.kind = .bin(.add, d, x, .imm(k)) }
                    else { i.kind = .addr(d, fa) }
                default: break
                }
                var extra: [Inst] = []
                if case let .bin(op, d, a, b) = i.kind {
                    if let k = simplify(op, d, a, b, f, avail, &extra) { i.kind = k }
                }
                if case let .un(.sext8, d, .v(a)) = i.kind, let def = avail[a] {
                    // Already a sign-extended byte: the conversion does nothing.
                    switch def {
                    case .un(.sext8, _, _), .load(_, _, 1): i.kind = .mov(d, .v(a))
                    default: break
                    }
                }
                if i.kind != before { changed = true }
                for var e in extra {
                    e.id = f.ids.next()
                    e.origin = i.origin
                    e.line = i.line
                    out.append(e)
                    if let d = e.def {
                        invalidate(d)
                        avail[d] = e.kind
                    }
                }
                out.append(i)
                if let d = i.def {
                    invalidate(d)
                    if !i.uses.contains(d) { avail[d] = i.kind }
                }
            }
            b.insts = out
        }
        return changed
    }

    /// Returns a cheaper replacement for `d = a op b`, or nil.
    static func simplify(_ op: IROp, _ d: Int, _ a: Operand, _ b: Operand, _ f: IRFunction,
                         _ avail: [Int: InstKind], _ extra: inout [Inst]) -> InstKind? {
        var a = a, b = b
        if a.constant != nil, b.constant == nil, op.isCommutative { swap(&a, &b) }
        if let x = a.vreg, x == b.vreg {
            switch op {
            case .sub, .xor, .ne, .lt, .gt, .ltu, .gtu: return .mov(d, .imm(0))
            case .and, .or: return .mov(d, a)
            case .eq, .le, .ge, .leu, .geu: return .mov(d, .imm(1))
            default: break
            }
        }
        guard let c = b.constant else {
            if a.constant != nil, op.isComparison, b.vreg != nil {
                return .bin(op.swapped, d, b, a)
            }
            return nil
        }
        switch op {
        case .add, .or, .xor, .shl, .shr, .sar:
            if c == 0 { return .mov(d, a) }
            if op == .add, let x = a.vreg, case let .bin(.add, _, y, .imm(k))? = avail[x] {
                // (y + k) + c → y + (k + c)
                return .bin(.add, d, y, .imm(k &+ c))
            }
        case .sub:
            if c == 0 { return .mov(d, a) }
            if c != Int32.min { return .bin(.add, d, a, .imm(-c)) }
        case .mul:
            if c == 0 { return .mov(d, .imm(0)) }
            if c == 1 { return .mov(d, a) }
            if c == -1 { return .un(.neg, d, a) }
            if let k = log2(c) { return .bin(.shl, d, a, .imm(k)) }
        case .div:
            if c == 1 { return .mov(d, a) }
            if c == -1 { return .un(.neg, d, a) }
            if let k = log2(c), k > 0, a.vreg != nil {
                // Signed division rounds toward zero, but an arithmetic shift rounds down.
                // Add (2^k − 1) first when a is negative: t = (a >> 31) >>> (32 − k).
                let s = f.newVReg(), t = f.newVReg(), u = f.newVReg()
                extra.append(Inst(kind: .bin(.sar, s, a, .imm(31)), id: 0, origin: 0, line: 0))
                extra.append(Inst(kind: .bin(.shr, t, .v(s), .imm(32 - k)), id: 0, origin: 0, line: 0))
                extra.append(Inst(kind: .bin(.add, u, a, .v(t)), id: 0, origin: 0, line: 0))
                return .bin(.sar, d, .v(u), .imm(k))
            }
        case .divu:
            if c == 1 { return .mov(d, a) }
            if let k = log2(c) { return .bin(.shr, d, a, .imm(k)) }
            if c < 0, UInt32(bitPattern: c) == 0x8000_0000 { return .bin(.shr, d, a, .imm(31)) }
        case .remu:
            if c == 1 { return .mov(d, .imm(0)) }
            if let _ = log2(c) { return .bin(.and, d, a, .imm(c - 1)) }
        case .rem:
            if c == 1 || c == -1 { return .mov(d, .imm(0)) }
        case .and:
            if c == 0 { return .mov(d, .imm(0)) }
            if c == -1 { return .mov(d, a) }
        case .ltu:
            if c == 0 { return .mov(d, .imm(0)) }
        case .geu:
            if c == 0 { return .mov(d, .imm(1)) }
        default:
            break
        }
        return nil
    }
}

// MARK: - Copy propagation

enum CopyProp {
    static func run(_ f: IRFunction) -> Bool {
        var changed = false
        // Global: a register written exactly once by a copy of another single-assignment
        // register (or of an unmodified parameter) can be replaced everywhere.
        let counts = defCounts(f)
        // A parameter already holds its incoming value, so it is single-valued only if never assigned.
        let params = Set(f.params)
        func stable(_ r: Int) -> Bool { params.contains(r) ? (counts[r] ?? 0) == 0 : (counts[r] ?? 0) == 1 }
        var global: [Int: Operand] = [:]
        for b in f.blocks {
            for i in b.insts {
                if case let .mov(d, s) = i.kind, counts[d] == 1, !params.contains(d) {
                    if case let .v(x) = s, x != d, stable(x) { global[d] = s }
                }
            }
        }
        func resolve(_ o: Operand) -> Operand {
            var o = o
            var hops = 0
            while case let .v(r) = o, let n = global[r], hops < 50 {
                o = n
                hops += 1
            }
            return o
        }
        if !global.isEmpty {
            for b in f.blocks {
                for k in b.insts.indices {
                    let before = b.insts[k].kind
                    b.insts[k].mapUses(resolve)
                    if b.insts[k].kind != before { changed = true }
                }
                let t = b.term
                b.term.mapUses(resolve)
                if b.term != t { changed = true }
            }
        }
        // Local: within a block, follow copies until either side is overwritten.
        for b in f.blocks {
            var copies: [Int: Operand] = [:]
            func sub(_ o: Operand) -> Operand {
                if case let .v(r) = o, let s = copies[r] { return s }
                return o
            }
            for k in b.insts.indices {
                let before = b.insts[k].kind
                b.insts[k].mapUses(sub)
                if b.insts[k].kind != before { changed = true }
                if let d = b.insts[k].def {
                    copies[d] = nil
                    for (key, v) in copies where v == .v(d) { copies[key] = nil }
                    if case let .mov(_, s) = b.insts[k].kind, s != .v(d) { copies[d] = s }
                }
            }
            let t = b.term
            b.term.mapUses(sub)
            if b.term != t { changed = true }
        }
        // Coalesce "%t = a + b; %x = %t" into "%x = a + b" when %t is used nowhere else.
        var useCount: [Int: Int] = [:]
        for b in f.blocks {
            for i in b.insts { for u in i.uses { useCount[u, default: 0] += 1 } }
            for u in b.term.uses { useCount[u, default: 0] += 1 }
        }
        let defs = defCounts(f)
        for b in f.blocks where b.insts.count >= 2 {
            var k = 1
            while k < b.insts.count {
                if case let .mov(x, .v(t)) = b.insts[k].kind, x != t, useCount[t] == 1, defs[t] == 1,
                   !f.vregs[t].isVariable, b.insts[k - 1].def == t, !f.params.contains(t) {
                    var prev = b.insts[k - 1]
                    switch prev.kind {
                    case let .bin(op, _, a, c): prev.kind = .bin(op, x, a, c)
                    case let .un(op, _, a): prev.kind = .un(op, x, a)
                    case let .load(_, a, s): prev.kind = .load(x, a, s)
                    case let .addr(_, a): prev.kind = .addr(x, a)
                    case let .call(_, n, args): prev.kind = .call(x, n, args)
                    case let .mov(_, a): prev.kind = .mov(x, a)
                    case .store: k += 1; continue
                    }
                    b.insts[k - 1] = prev
                    b.insts.remove(at: k)
                    changed = true
                    continue
                }
                k += 1
            }
        }
        // Remove copies to self.
        for b in f.blocks {
            let n = b.insts.count
            b.insts.removeAll { if case let .mov(d, .v(s)) = $0.kind { return d == s }; return false }
            if b.insts.count != n { changed = true }
        }
        return changed
    }
}

// MARK: - Common subexpression elimination

enum CSE {
    static func key(_ k: InstKind) -> InstKind? {
        switch k {
        case let .bin(op, _, a, b):
            if op.isCommutative, "\(a)" > "\(b)" { return .bin(op, 0, b, a) }
            return .bin(op, 0, a, b)
        case let .un(op, _, a): return .un(op, 0, a)
        case let .addr(_, a): return .addr(0, a)
        case let .load(_, a, s): return .load(0, a, s)
        default: return nil
        }
    }

    static func run(_ f: IRFunction) -> Bool {
        var changed = false
        for b in f.blocks {
            var table: [InstKind: Operand] = [:]
            func invalidate(_ r: Int) {
                for (k, v) in table {
                    let tmp = Inst(kind: k, id: 0, origin: 0, line: 0)
                    if tmp.uses.contains(r) || v == .v(r) { table[k] = nil }
                }
            }
            func dropLoads() {
                for k in table.keys { if case .load = k { table[k] = nil } }
            }
            for idx in b.insts.indices {
                let i = b.insts[idx]
                switch i.kind {
                case .store(let v, let a, let s):
                    dropLoads()
                    if s == 4 { table[.load(0, a, 4)] = v }
                    continue
                case .call(let d, _, _):
                    dropLoads()
                    if let d { invalidate(d) }
                    continue
                default: break
                }
                guard let d = i.def else { continue }
                if let k = key(i.kind), let prev = table[k], prev != .v(d) {
                    b.insts[idx].kind = .mov(d, prev)
                    changed = true
                    invalidate(d)
                    continue
                }
                invalidate(d)
                if let k = key(i.kind), !i.uses.contains(d) { table[k] = .v(d) }
            }
        }
        return changed
    }
}

// MARK: - Dead code elimination

enum DCE {
    static func run(_ f: IRFunction) -> Bool {
        var changed = false
        var again = true
        while again {
            again = false
            let live = Liveness.analyze(f)
            for b in f.blocks {
                var alive = live.liveOut[b.id] ?? []
                for r in b.term.uses { alive.insert(r) }
                var keep: [Inst] = []
                for i in b.insts.reversed() {
                    if let d = i.def, i.isPure, !alive.contains(d) {
                        again = true
                        changed = true
                        continue
                    }
                    if let d = i.def { alive.remove(d) }
                    for u in i.uses { alive.insert(u) }
                    keep.append(i)
                }
                b.insts = keep.reversed()
            }
        }
        return changed
    }
}

// MARK: - Control-flow cleanup

enum CFGSimplify {
    static func run(_ f: IRFunction) -> Bool {
        var changed = false
        var again = true
        var rounds = 0
        while again && rounds < 100 {
            again = false
            rounds += 1
            for b in f.blocks {
                if case let .branch(op, x, y, t, e) = b.term {
                    if t == e { b.term = .jump(t); again = true }
                    else if case let .imm(p) = x, case let .imm(q) = y { b.term = .jump(op.fold(p, q) != 0 ? t : e); again = true }
                }
            }
            // Jump threading through empty blocks.
            guard let entry = f.blocks.first else { return changed }
            var forward: [Int: Int] = [:]
            for b in f.blocks where b.id != entry.id && b.insts.isEmpty {
                if case let .jump(t) = b.term, t != b.id { forward[b.id] = t }
            }
            func final(_ id: Int) -> Int {
                var x = id, hops = 0
                while let n = forward[x], hops < 64 { x = n; hops += 1 }
                return x
            }
            if !forward.isEmpty {
                for b in f.blocks {
                    let t = b.term
                    b.term.mapTargets(final)
                    if b.term != t { again = true }
                }
            }
            // Unreachable blocks.
            var seen: Set<Int> = [entry.id]
            var work = [entry.id]
            while let id = work.popLast() {
                for s in f.block(id)?.term.successors ?? [] where !seen.contains(s) {
                    seen.insert(s)
                    work.append(s)
                }
            }
            let n = f.blocks.count
            f.blocks.removeAll { !seen.contains($0.id) }
            if f.blocks.count != n { again = true }
            // Merge a block into its only predecessor.
            let preds = f.predecessors()
            for a in f.blocks {
                guard case let .jump(t) = a.term, t != a.id, t != entry.id, preds[t]?.count == 1,
                      let bb = f.block(t) else { continue }
                a.insts += bb.insts
                a.term = bb.term
                a.termID = bb.termID
                a.termOrigin = bb.termOrigin
                a.termLine = bb.termLine
                f.blocks.removeAll { $0.id == t }
                again = true
                break
            }
            changed = changed || again
        }
        return changed
    }
}
