import Foundation

/// Translates the checked syntax tree into IR, the way an unoptimising
/// compiler (`-O0`) does: every local variable and parameter gets a stack
/// slot, every read is a `load` and every write a `store`. The optimiser
/// passes (starting with mem2reg) then clean this up.
///
/// Control flow becomes basic blocks: `if`, loops, `&&`/`||`, `?:` and
/// `switch` all turn into conditional branches between blocks.
public final class Lowering {
    private let prog: Program
    private let sema: SemaResult
    private let module = IRModule()
    private let ids: IDGen

    private var f: IRFunction!
    private var fn: FunctionDecl!
    private var cur: Block?
    private var pending: [Int: Block] = [:]
    private var slotOf: [Int: Int] = [:]
    private var breakTargets: [Int] = []
    private var continueTargets: [Int] = []
    private var nameCounts: [String: Int] = [:]
    private var origin = 0
    private var line = 0

    public static func lower(_ prog: Program, _ sema: SemaResult, ids: IDGen = IDGen()) -> IRModule {
        let l = Lowering(prog, sema, ids)
        l.run()
        return l.module
    }

    private init(_ prog: Program, _ sema: SemaResult, _ ids: IDGen) {
        self.prog = prog
        self.sema = sema
        self.ids = ids
    }

    private func run() {
        for g in prog.globals { lowerGlobal(g) }
        for s in sema.strings {
            module.data.append(GlobalData(label: s.label, align: 1, items: s.bytes.map { .byte($0) } + [.byte(0)],
                                          size: s.bytes.count + 1, line: 0, comment: "string"))
        }
        for fd in prog.functions { lowerFunction(fd) }
    }

    // MARK: Blocks and emission

    private func nextID() -> Int { ids.next() }

    private func newBlock(_ name: String) -> Int {
        let k = nameCounts[name, default: 0]
        nameCounts[name] = k + 1
        let b = Block(id: f.nextBlock, name: k == 0 ? name : "\(name).\(k)")
        f.nextBlock += 1
        pending[b.id] = b
        return b.id
    }

    /// Makes `id` the current block. If the previous block is still open, it falls through with a jump.
    private func start(_ id: Int) {
        if cur != nil { terminate(.jump(id)) }
        guard let b = pending.removeValue(forKey: id) else { return }
        f.blocks.append(b)
        cur = b
    }

    private func current() -> Block {
        if let c = cur { return c }
        // Code after return/break/continue: unreachable, but it still needs a home.
        let id = newBlock("dead")
        start(id)
        return cur!
    }

    private func emit(_ k: InstKind) {
        current().insts.append(Inst(kind: k, id: nextID(), origin: origin, line: line))
    }

    private func terminate(_ t: Terminator) {
        let b = current()
        b.term = t
        b.termID = nextID()
        b.termOrigin = origin
        b.termLine = line
        cur = nil
    }

    private func jump(_ id: Int) {
        if cur != nil { terminate(.jump(id)) }
    }

    private func temp() -> Int { f.newVReg() }

    private func bin(_ op: IROp, _ a: Operand, _ b: Operand) -> Operand {
        let d = temp()
        emit(.bin(op, d, a, b))
        return .v(d)
    }

    private func un(_ op: UnOp, _ a: Operand) -> Operand {
        let d = temp()
        emit(.un(op, d, a))
        return .v(d)
    }

    private func at<T>(_ e: Expr, _ body: () -> T) -> T {
        let (so, sl) = (origin, line)
        origin = e.id
        line = e.line
        defer { (origin, line) = (so, sl) }
        return body()
    }

    // MARK: Functions

    private func lowerFunction(_ d: FunctionDecl) {
        f = IRFunction(name: d.name, ids: ids)
        fn = d
        f.returnsValue = d.returnType != .void
        f.line = d.line
        pending = [:]
        slotOf = [:]
        nameCounts = [:]
        cur = nil
        origin = d.id
        line = d.line
        start(newBlock("entry"))

        // Every local and parameter starts life in its own stack slot.
        for s in d.locals {
            slotOf[s.id] = f.slots.count
            f.slots.append(Slot(name: "$" + s.uniqueName, size: s.type.size, align: max(1, s.type.align),
                                symbolID: s.id, isScalar: s.type.isScalar))
        }
        for p in d.params {
            guard let s = p.symbol else { continue }
            let v = f.newVReg("%" + s.uniqueName, variable: true)
            f.params.append(v)
            line = p.line
            emit(.store(.v(v), .slot(slotOf[s.id]!, 0), s.type == .char ? 1 : 4))
        }
        line = d.line
        if let body = d.body { stmt(body) }
        if cur != nil {
            // Falling off the end: void functions return; main returns 0 (as C99 says).
            if let body = d.body, case .block = body.kind { line = lastLine(body) }
            terminate(.ret(f.returnsValue ? .imm(0) : nil))
        }
        removeUnreachable()
        module.functions.append(f)
    }

    private func lastLine(_ s: Stmt) -> Int {
        if case let .block(items) = s.kind, let l = items.last { return max(l.line, lastLine(l)) }
        return s.line
    }

    private func removeUnreachable() {
        guard let entry = f.blocks.first else { return }
        var seen: Set<Int> = [entry.id]
        var work = [entry.id]
        while let id = work.popLast() {
            for s in f.block(id)?.term.successors ?? [] where !seen.contains(s) {
                seen.insert(s)
                work.append(s)
            }
        }
        f.blocks.removeAll { !seen.contains($0.id) }
    }

    // MARK: Globals

    private func lowerGlobal(_ g: VarDecl) {
        guard let s = g.symbol else { return }
        var items: [DataItem] = []
        dataItems(g.type, g.initializer, &items)
        // Merge runs of zeros.
        var merged: [DataItem] = []
        for it in items {
            if case let .zero(n) = it, case let .zero(m)? = merged.last {
                merged[merged.count - 1] = .zero(n + m)
            } else if case let .zero(n) = it, n == 0 {
                continue
            } else {
                merged.append(it)
            }
        }
        module.data.append(GlobalData(label: s.uniqueName, align: max(4, g.type.align), items: merged,
                                      size: g.type.size, line: g.line, comment: g.type.spelled(g.name)))
    }

    private func dataItems(_ t: CType, _ ini: Initializer?, _ out: inout [DataItem]) {
        guard let ini else {
            out.append(.zero(t.size))
            return
        }
        switch (ini, t) {
        case let (.list(items, _), .array(e, n)):
            for i in items.prefix(n) { dataItems(e, i, &out) }
            if items.count < n { out.append(.zero(e.size * (n - items.count))) }
        case let (.list(items, _), .structure(s)):
            var offset = 0
            for (k, fld) in s.fields.enumerated() {
                if fld.offset > offset { out.append(.zero(fld.offset - offset)) }
                dataItems(fld.type, k < items.count ? items[k] : nil, &out)
                offset = fld.offset + fld.type.size
            }
            if s.size > offset { out.append(.zero(s.size - offset)) }
        case let (.list(items, _), _):
            dataItems(t, items.first, &out)
        case let (.expr(e), .array(.char, n)):
            if case let .string(bytes) = e.kind {
                out += bytes.prefix(n).map { .byte($0) }
                if bytes.count < n { out.append(.zero(n - bytes.count)) }
            }
        case let (.expr(e), _):
            if t.isPointer, let target = constantAddress(e) {
                out.append(.address(target, 0))
                return
            }
            let v = Int32(truncatingIfNeeded: Parser.constantValue(e) ?? 0)
            if t == .char { out.append(.byte(UInt8(truncatingIfNeeded: v))) } else { out.append(.word(v)) }
        }
    }

    private func constantAddress(_ e: Expr) -> String? {
        switch e.kind {
        case .string: return e.label
        case let .unary(.addr, x):
            if case .ident = x.kind, let s = x.symbol { return s.uniqueName }
            return nil
        case .ident:
            if let s = e.symbol, s.type.isArray { return s.uniqueName }
            return nil
        case let .cast(_, x): return constantAddress(x)
        default: return nil
        }
    }

    // MARK: Statements

    private func stmt(_ s: Stmt) {
        let (so, sl) = (origin, line)
        origin = s.id
        line = s.line
        defer { (origin, line) = (so, sl) }
        switch s.kind {
        case let .block(items):
            for i in items { stmt(i) }
        case let .decl(decls):
            for d in decls {
                guard let sym = d.symbol, let ini = d.initializer, let slot = slotOf[sym.id] else { continue }
                line = d.line
                initLocal(.slot(slot, 0), d.type, ini)
            }
        case let .expr(e):
            _ = value(e, want: false)
        case let .ifElse(c, a, b):
            let then = newBlock("if.then")
            let end = newBlock("if.end")
            let els = b != nil ? newBlock("if.else") : end
            cond(c, then, els)
            start(then)
            stmt(a)
            jump(end)
            if let b {
                start(els)
                stmt(b)
                jump(end)
            }
            start(end)
        case let .whileLoop(c, body):
            let head = newBlock("while.cond"), bodyB = newBlock("while.body"), end = newBlock("while.end")
            start(head)
            cond(c, bodyB, end)
            start(bodyB)
            loop(break: end, continue: head) { stmt(body) }
            jump(head)
            start(end)
        case let .doWhile(body, c):
            let bodyB = newBlock("do.body"), test = newBlock("do.cond"), end = newBlock("do.end")
            start(bodyB)
            loop(break: end, continue: test) { stmt(body) }
            start(test)
            cond(c, bodyB, end)
            start(end)
        case let .forLoop(ini, c, step, body):
            if let ini { stmt(ini) }
            let head = newBlock("for.cond"), bodyB = newBlock("for.body")
            let stepB = newBlock("for.step"), end = newBlock("for.end")
            start(head)
            if let c { cond(c, bodyB, end) } else { jump(bodyB) }
            start(bodyB)
            loop(break: end, continue: stepB) { stmt(body) }
            start(stepB)
            if let step { _ = value(step, want: false) }
            jump(head)
            start(end)
        case let .switchCase(x, cases):
            lowerSwitch(x, cases)
        case let .returnStmt(e):
            if let e {
                var v = value(e)
                if fn.returnType == .char { v = un(.sext8, v) }
                terminate(.ret(f.returnsValue ? v : nil))
            } else {
                terminate(.ret(f.returnsValue ? .imm(0) : nil))
            }
        case .breakStmt:
            terminate(.jump(breakTargets.last!))
        case .continueStmt:
            terminate(.jump(continueTargets.last!))
        case .empty:
            break
        }
    }

    private func loop(break b: Int, continue c: Int, _ body: () -> Void) {
        breakTargets.append(b)
        continueTargets.append(c)
        body()
        breakTargets.removeLast()
        continueTargets.removeLast()
    }

    /// A switch becomes a chain of compare-and-branch tests, then the case
    /// bodies laid out in order so that a missing `break` falls through.
    private func lowerSwitch(_ x: Expr, _ cases: [SwitchCase]) {
        var v = value(x)
        if case .imm = v {
            let r = temp()
            emit(.mov(r, v))
            v = .v(r)
        }
        let end = newBlock("switch.end")
        let bodies = cases.map { newBlock($0.value == nil ? "switch.default" : "switch.case") }
        for (k, c) in cases.enumerated() where c.value != nil {
            let next = newBlock("switch.test")
            line = c.line
            terminate(.branch(.eq, v, .imm(c.constant), bodies[k], next))
            start(next)
        }
        jump(cases.firstIndex { $0.value == nil }.map { bodies[$0] } ?? end)
        breakTargets.append(end)
        for (k, c) in cases.enumerated() {
            start(bodies[k])
            for s in c.body { stmt(s) }
        }
        breakTargets.removeLast()
        start(end)
    }

    // MARK: Local initialisers

    private func initLocal(_ a: Addr, _ t: CType, _ ini: Initializer) {
        switch (ini, t) {
        case let (.list(items, _), .array(e, n)):
            for k in 0..<min(n, items.count) { initLocal(a.adding(Int32(k * e.size)), e, items[k]) }
            if items.count < n { zero(a.adding(Int32(items.count * e.size)), (n - items.count) * e.size, e.align) }
        case let (.list(items, _), .structure(s)):
            zero(a, s.size, s.align)
            for (k, fld) in s.fields.enumerated() where k < items.count {
                initLocal(a.adding(Int32(fld.offset)), fld.type, items[k])
            }
        case let (.list(items, _), _):
            initLocal(a, t, items[0])
        case let (.expr(e), .array(.char, n)):
            guard case let .string(bytes) = e.kind else { return }
            at(e) {
                if n <= 64 {
                    for k in 0..<n { emit(.store(.imm(k < bytes.count ? Int32(Int8(bitPattern: bytes[k])) : 0), a.adding(Int32(k)), 1)) }
                } else {
                    let copy = min(n, bytes.count + 1)
                    emit(.call(nil, "memcpy", [addrValue(a), addrValue(.global(e.label, 0)), .imm(Int32(copy))]))
                    if n > copy { zero(a.adding(Int32(copy)), n - copy, 1) }
                }
            }
        case let (.expr(e), _):
            at(e) {
                if t.isStruct {
                    copyStruct(a, value(e), t.size, t.align)
                } else {
                    storeValue(t, a, value(e))
                }
            }
        }
    }

    private func zero(_ a: Addr, _ size: Int, _ align: Int) {
        if size > 64 {
            emit(.call(nil, "memset", [addrValue(a), .imm(0), .imm(Int32(size))]))
            return
        }
        var k = 0
        if align >= 4 {
            while k + 4 <= size {
                emit(.store(.imm(0), a.adding(Int32(k)), 4))
                k += 4
            }
        }
        while k < size {
            emit(.store(.imm(0), a.adding(Int32(k)), 1))
            k += 1
        }
    }

    private func copyStruct(_ dst: Addr, _ src: Operand, _ size: Int, _ align: Int) {
        let step = align >= 4 ? 4 : 1
        var k = 0
        while k < size {
            let t = temp()
            emit(.load(t, .reg(src, Int32(k)), step))
            emit(.store(.v(t), dst.adding(Int32(k)), step))
            k += step
        }
    }

    // MARK: Values

    private func addrValue(_ a: Addr) -> Operand {
        switch a {
        case let .reg(r, 0): return r
        case let .reg(r, k): return bin(.add, r, .imm(k))
        default:
            let d = temp()
            emit(.addr(d, a))
            return .v(d)
        }
    }

    private func loadValue(_ t: CType, _ a: Addr) -> Operand {
        switch t {
        case .array, .structure, .function: return addrValue(a)
        default:
            let d = temp()
            emit(.load(d, a, t == .char ? 1 : 4))
            return .v(d)
        }
    }

    private func storeValue(_ t: CType, _ a: Addr, _ v: Operand) {
        if t.isStruct {
            copyStruct(a, v, t.size, t.align)
            return
        }
        emit(.store(v, a, t == .char ? 1 : 4))
    }

    private func scale(_ idx: Operand, _ size: Int) -> Operand {
        size == 1 ? idx : bin(.mul, idx, .imm(Int32(size)))
    }

    private func arithOp(_ op: BinaryOp, _ unsigned: Bool) -> IROp {
        switch op {
        case .add: return .add
        case .sub: return .sub
        case .mul: return .mul
        case .div: return unsigned ? .divu : .div
        case .rem: return unsigned ? .remu : .rem
        case .shl: return .shl
        case .shr: return unsigned ? .shr : .sar
        case .bitAnd: return .and
        case .bitOr: return .or
        case .bitXor: return .xor
        case .lt: return unsigned ? .ltu : .lt
        case .gt: return unsigned ? .gtu : .gt
        case .le: return unsigned ? .leu : .le
        case .ge: return unsigned ? .geu : .ge
        case .eq: return .eq
        case .ne: return .ne
        case .logAnd, .logOr: return .and
        }
    }

    /// `a op b` where the operation happens in `opType` (int, unsigned, or pointer arithmetic).
    private func arith(_ op: BinaryOp, _ a: Operand, _ b: Operand, _ opType: CType, bIsPointer: Bool = false) -> Operand {
        if let p = opType.pointee, opType.isPointer, op == .add || op == .sub {
            if bIsPointer { return bin(.add, b, scale(a, p.size)) }
            return bin(op == .add ? .add : .sub, a, scale(b, p.size))
        }
        return bin(arithOp(op, opType == .uint), a, b)
    }

    private func value(_ e: Expr, want: Bool = true) -> Operand {
        at(e) { valueAt(e, want: want) }
    }

    private func valueAt(_ e: Expr, want: Bool) -> Operand {
        switch e.kind {
        case let .number(v):
            return .imm(Int32(truncatingIfNeeded: v))
        case .string:
            return addrValue(.global(e.label, 0))
        case .ident, .index, .member, .unary(.deref, _):
            return loadValue(e.type, address(e))
        case let .binary(op, a, b):
            if op == .logAnd || op == .logOr { return boolValue(e) }
            let at = a.type.decayed, bt = b.type.decayed
            let x = value(a), y = value(b)
            if op == .sub && at.isPointer && bt.isPointer {
                let diff = bin(.sub, x, y)
                let size = at.pointee!.size
                if size == 1 { return diff }
                // Pointer differences are exact multiples, so a shift is enough for powers of two.
                if size & (size - 1) == 0 { return bin(.sar, diff, .imm(Int32(size.trailingZeroBitCount))) }
                return bin(.div, diff, .imm(Int32(size)))
            }
            return arith(op, x, y, e.opType, bIsPointer: at.isInteger && bt.isPointer)
        case let .unary(op, a):
            switch op {
            case .neg: return un(.neg, value(a))
            case .plus: return value(a)
            case .bitNot: return un(.not, value(a))
            case .not: return bin(.eq, value(a), .imm(0))
            case .addr: return addrValue(address(a))
            case .deref: return loadValue(e.type, address(e))
            }
        case let .assign(op, lhs, rhs):
            let a = address(lhs)
            if lhs.type.isStruct {
                copyStruct(a, value(rhs), lhs.type.size, lhs.type.align)
                return want ? addrValue(a) : .imm(0)
            }
            var v: Operand
            if let op {
                let old = loadValue(lhs.type, a)
                v = arith(op, old, value(rhs), e.opType)
            } else {
                v = value(rhs)
            }
            storeValue(lhs.type, a, v)
            if want && lhs.type == .char { v = un(.sext8, v) }
            return v
        case let .incDec(isPrefix, inc, x):
            let a = address(x)
            let old = loadValue(x.type, a)
            let step = Int32(x.type.pointee?.size ?? 1)
            let new = bin(inc ? .add : .sub, old, .imm(step))
            storeValue(x.type, a, new)
            if isPrefix { return (want && x.type == .char) ? un(.sext8, new) : new }
            return old
        case let .call(name, args):
            if name == "printf" {
                printf(args)
                return .imm(0)
            }
            let vals = args.map { value($0) }
            if e.type == .void {
                emit(.call(nil, name, vals))
                return .imm(0)
            }
            let d = temp()
            emit(.call(d, name, vals))
            return .v(d)
        case let .ternary(c, a, b):
            let t = newBlock("cond.true"), fl = newBlock("cond.false"), end = newBlock("cond.end")
            let r = e.type.isVoid ? 0 : temp()
            cond(c, t, fl)
            start(t)
            let va = value(a)
            if r != 0 { emit(.mov(r, va)) }
            jump(end)
            start(fl)
            let vb = value(b)
            if r != 0 { emit(.mov(r, vb)) }
            jump(end)
            start(end)
            return r != 0 ? .v(r) : .imm(0)
        case let .sizeofType(t):
            return .imm(Int32(t.size))
        case .sizeofExpr:
            return .imm(Int32(e.type.size))
        case let .cast(t, a):
            let v = value(a, want: !t.isVoid)
            return t == .char ? un(.sext8, v) : v
        case let .comma(a, b):
            _ = value(a, want: false)
            return value(b, want: want)
        }
    }

    /// `a && b` or `a || b` used as a value: branch, then put 1 or 0 in a register.
    private func boolValue(_ e: Expr) -> Operand {
        let t = newBlock("bool.true"), fl = newBlock("bool.false"), end = newBlock("bool.end")
        let r = temp()
        cond(e, t, fl)
        start(t)
        emit(.mov(r, .imm(1)))
        jump(end)
        start(fl)
        emit(.mov(r, .imm(0)))
        jump(end)
        start(end)
        return .v(r)
    }

    /// printf with a constant format is expanded right here into simple print calls.
    private func printf(_ args: [Expr]) {
        guard case let .string(bytes) = args[0].kind, case let .success(pieces) = PrintfFormat.parse(bytes) else { return }
        // C evaluates every argument before the call, so do that first (left to right),
        // then print the pieces. A function called in an argument prints before printf does.
        let values = args.dropFirst().map { value($0) }
        var ai = 0
        for piece in pieces {
            switch piece {
            case let .text(t):
                if t.count == 1 {
                    emit(.call(nil, "print_char", [.imm(Int32(t[0]))]))
                } else {
                    let label = sema.strings.first { $0.bytes == t }?.label ?? ".LC?"
                    emit(.call(nil, "print_str", [addrValue(.global(label, 0))]))
                }
            case let .conversion(c):
                let v = values[ai]
                ai += 1
                switch c {
                case "d", "i": emit(.call(nil, "print_int", [v]))
                case "u": emit(.call(nil, "print_uint", [v]))
                case "x": emit(.call(nil, "__printx", [v, .imm(0)]))
                case "X": emit(.call(nil, "__printx", [v, .imm(1)]))
                case "p": emit(.call(nil, "print_hex", [v]))
                case "c": emit(.call(nil, "print_char", [v]))
                case "s": emit(.call(nil, "print_str", [v]))
                default: break
                }
            }
        }
    }

    // MARK: Addresses

    private func address(_ e: Expr) -> Addr {
        at(e) { addressAt(e) }
    }

    private func addressAt(_ e: Expr) -> Addr {
        switch e.kind {
        case .ident:
            guard let s = e.symbol else { return .global("?", 0) }
            if s.kind == .global { return .global(s.uniqueName, 0) }
            return .slot(slotOf[s.id]!, 0)
        case let .unary(.deref, p):
            return .reg(value(p), 0)
        case let .index(b, i):
            let base = value(b)
            let off = scale(value(i), e.type.size)
            return .reg(bin(.add, base, off), 0)
        case let .member(b, _, arrow):
            if arrow { return .reg(value(b), Int32(e.fieldOffset)) }
            return address(b).adding(Int32(e.fieldOffset))
        case .string:
            return .global(e.label, 0)
        case let .cast(_, x):
            return address(x)
        default:
            return .reg(value(e), 0)
        }
    }

    // MARK: Conditions

    /// Lowers a condition straight into branches. `&&` and `||` short-circuit:
    /// the right side is only evaluated when it can still change the answer.
    private func cond(_ e: Expr, _ t: Int, _ fl: Int) {
        at(e) {
            switch e.kind {
            case let .binary(.logAnd, a, b):
                let mid = newBlock("and.rhs")
                cond(a, mid, fl)
                start(mid)
                cond(b, t, fl)
            case let .binary(.logOr, a, b):
                let mid = newBlock("or.rhs")
                cond(a, t, mid)
                start(mid)
                cond(b, t, fl)
            case let .unary(.not, a):
                cond(a, fl, t)
            case let .binary(op, a, b) where op.isComparison:
                let x = value(a), y = value(b)
                terminate(.branch(arithOp(op, e.opType == .uint), x, y, t, fl))
            case let .number(v):
                terminate(.jump(v != 0 ? t : fl))
            default:
                let v = value(e)
                terminate(.branch(.ne, v, .imm(0), t, fl))
            }
        }
    }
}
