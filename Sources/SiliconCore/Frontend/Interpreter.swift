import Foundation

/// Runs a checked C program directly from its syntax tree, without compiling it.
///
/// This is the *reference*: the compiled RISC-V program must print exactly what
/// the interpreter prints. Every variable lives in memory here (no registers),
/// and the arithmetic comes from `RV`, so it agrees with the hardware on corner cases.
public final class Interpreter {
    private let prog: Program
    private let sema: SemaResult
    private let mem = Memory()
    private var globalAddr: [Int: UInt32] = [:]
    private var stringAddr: [String: UInt32] = [:]
    private var functions: [String: FunctionDecl] = [:]
    private var frames: [[Int: UInt32]] = []
    private var sp: UInt32 = MemoryMap.stackTop & ~0xF
    private var brk: UInt32 = MemoryMap.heapBase
    private var randNext: UInt32 = 1
    private var out = ProgramOutput()
    private var steps = 0
    private let stepLimit: Int

    private struct Exit: Error { let code: Int32 }
    private struct Fault: Error { let message: String }
    private enum Flow { case normal, breakLoop, continueLoop, returned(Int32) }

    public static func run(_ prog: Program, _ sema: SemaResult, stepLimit: Int = 20_000_000) -> ProgramOutput {
        let i = Interpreter(prog, sema, stepLimit: stepLimit)
        return i.execute()
    }

    private init(_ prog: Program, _ sema: SemaResult, stepLimit: Int) {
        self.prog = prog
        self.sema = sema
        self.stepLimit = stepLimit
    }

    private func execute() -> ProgramOutput {
        do {
            try layoutData()
            for f in prog.functions { functions[f.name] = f }
            guard let main = functions["main"] else { throw Fault(message: "no main") }
            let code = try call(main, Array(repeating: 0, count: main.params.count))
            out.exitCode = main.returnType == .void ? 0 : code
        } catch let e as Exit {
            out.exitCode = e.code
        } catch let f as Fault {
            out.fault = f.message
        } catch let m as MemoryFault {
            out.fault = m.message
        } catch {
            out.fault = "\(error)"
        }
        return out
    }

    // MARK: Data

    private func layoutData() throws {
        var addr = MemoryMap.dataBase
        func align(_ a: Int) { addr = (addr + UInt32(a) - 1) & ~(UInt32(a) - 1) }
        for g in prog.globals {
            guard let s = g.symbol else { continue }
            align(max(4, g.type.align))
            globalAddr[s.id] = addr
            addr += UInt32(g.type.size)
        }
        for s in sema.strings {
            stringAddr[s.label] = addr
            for (i, b) in s.bytes.enumerated() { try mem.store8(addr + UInt32(i), b) }
            try mem.store8(addr + UInt32(s.bytes.count), 0)
            addr += UInt32(s.bytes.count + 1)
        }
        for g in prog.globals {
            guard let s = g.symbol, let a = globalAddr[s.id], let ini = g.initializer else { continue }
            try initialize(a, g.type, ini, global: true)
        }
    }

    /// Writes an initialiser into memory. Elements without an initialiser become zero.
    private func initialize(_ addr: UInt32, _ t: CType, _ ini: Initializer, global: Bool) throws {
        switch (ini, t) {
        case let (.list(items, _), .array(e, n)):
            for k in 0..<n {
                let ea = addr + UInt32(k * e.size)
                if k < items.count { try initialize(ea, e, items[k], global: global) } else { try zero(ea, e.size) }
            }
        case let (.list(items, _), .structure(s)):
            try zero(addr, s.size)
            for (i, f) in zip(items, s.fields) { try initialize(addr + UInt32(f.offset), f.type, i, global: global) }
        case let (.list(items, _), _):
            try initialize(addr, t, items[0], global: global)
        case let (.expr(e), .array(.char, n)):
            guard case let .string(bytes) = e.kind else { return }
            for k in 0..<n { try mem.store8(addr + UInt32(k), k < bytes.count ? bytes[k] : 0) }
        case let (.expr(e), _):
            let v = try eval(e)
            try store(t, addr, convert(v, to: t))
        }
    }

    private func zero(_ addr: UInt32, _ n: Int) throws {
        for k in 0..<n { try mem.store8(addr + UInt32(k), 0) }
    }

    // MARK: Memory helpers

    private func load(_ t: CType, _ addr: UInt32) throws -> Int32 {
        switch t {
        case .char: return Int32(Int8(bitPattern: try mem.load8(addr)))
        case .array, .structure, .function: return Int32(bitPattern: addr)
        default: return Int32(bitPattern: try mem.load32(addr))
        }
    }

    private func store(_ t: CType, _ addr: UInt32, _ v: Int32) throws {
        switch t {
        case .char: try mem.store8(addr, UInt8(truncatingIfNeeded: v))
        case let .structure(s):
            // Struct assignment copies the bytes from the source address.
            let src = UInt32(bitPattern: v)
            for k in 0..<UInt32(s.size) { try mem.store8(addr + k, try mem.load8(src + k)) }
        default: try mem.store32(addr, UInt32(bitPattern: v))
        }
    }

    private func convert(_ v: Int32, to t: CType) -> Int32 {
        t == .char ? Int32(Int8(truncatingIfNeeded: v)) : v
    }

    private func tick() throws {
        steps += 1
        if steps > stepLimit { throw Fault(message: "step limit reached (infinite loop?)") }
    }

    // MARK: Calls

    private func call(_ f: FunctionDecl, _ args: [Int32]) throws -> Int32 {
        try tick()
        if frames.count > 2000 { throw Fault(message: "stack overflow (recursion too deep)") }
        let savedSP = sp
        var frame: [Int: UInt32] = [:]
        for s in f.locals {
            let size = UInt32(max(4, s.type.size))
            sp = (sp - size) & ~(UInt32(max(4, s.type.align)) - 1)
            frame[s.id] = sp
        }
        if sp < MemoryMap.stackBottom + 64 { throw Fault(message: "stack overflow") }
        frames.append(frame)
        defer {
            frames.removeLast()
            sp = savedSP
        }
        for (p, v) in zip(f.params, args) {
            if let s = p.symbol, let a = frame[s.id] { try store(s.type, a, convert(v, to: s.type)) }
        }
        guard let body = f.body else { return 0 }
        if case let .returned(v) = try exec(body) { return v }
        return 0
    }

    private func callLibrary(_ name: String, _ a: [Int32]) throws -> Int32 {
        func u(_ i: Int) -> UInt32 { UInt32(bitPattern: a[i]) }
        switch name {
        case "print_int": out.append(String(a[0]))
        case "print_uint": out.append(String(u(0)))
        case "print_hex": out.append(Format.hex(u(0)))
        case "print_char", "putchar": out.append([UInt8(truncatingIfNeeded: a[0])]); return a[0]
        case "print_str": out.append(try mem.cString(u(0), limit: 100_000))
        case "puts":
            out.append(try mem.cString(u(0), limit: 100_000))
            out.append([10])
        case "exit": throw Exit(code: a[0])
        case "malloc": return Int32(bitPattern: sbrk((u(0) &+ 7) & ~7))
        case "calloc":
            let n = (u(0) &* u(1) &+ 7) & ~7
            let p = sbrk(n)
            if p != 0 { try zero(p, Int(n)) }
            return Int32(bitPattern: p)
        case "free": break
        case "abs": return a[0] < 0 ? 0 &- a[0] : a[0]
        case "rand":
            randNext = randNext &* 1_103_515_245 &+ 12345
            return Int32((randNext >> 16) & 32767)
        case "srand": randNext = u(0)
        case "strlen": return Int32(try mem.cString(u(0), limit: 1 << 22).count)
        case "strcmp":
            var p = u(0), q = u(1)
            while true {
                let x = try mem.load8(p), y = try mem.load8(q)
                if x != y || x == 0 { return Int32(x) - Int32(y) }
                p &+= 1
                q &+= 1
            }
        case "strcpy":
            var p = u(0), q = u(1)
            while true {
                let b = try mem.load8(q)
                try mem.store8(p, b)
                if b == 0 { break }
                p &+= 1
                q &+= 1
            }
            return a[0]
        case "memset":
            for k in 0..<u(2) { try mem.store8(u(0) &+ k, UInt8(truncatingIfNeeded: a[1])) }
            return a[0]
        case "memcpy":
            for k in 0..<u(2) { try mem.store8(u(0) &+ k, try mem.load8(u(1) &+ k)) }
            return a[0]
        default:
            throw Fault(message: "unknown library function \(name)")
        }
        return 0
    }

    private func sbrk(_ n: UInt32) -> UInt32 {
        let p = brk
        if brk &+ n > MemoryMap.heapLimit || brk &+ n < brk { return 0 }
        brk &+= n
        return p
    }

    private func printf(_ args: [Expr]) throws -> Int32 {
        guard case let .string(bytes) = args[0].kind, case let .success(pieces) = PrintfFormat.parse(bytes) else { return 0 }
        // All arguments are evaluated before anything is printed, as in C.
        var values: [Int32] = []
        for a in args.dropFirst() { values.append(try eval(a)) }
        var ai = 0
        for piece in pieces {
            switch piece {
            case let .text(t): out.append(t)
            case let .conversion(c):
                let v = values[ai]
                ai += 1
                switch c {
                case "d", "i": out.append(String(v))
                case "u": out.append(String(UInt32(bitPattern: v)))
                case "x": out.append(String(UInt32(bitPattern: v), radix: 16))
                case "X": out.append(String(UInt32(bitPattern: v), radix: 16).uppercased())
                case "p": out.append(Format.hex(UInt32(bitPattern: v)))
                case "c": out.append([UInt8(truncatingIfNeeded: v)])
                case "s": out.append(try mem.cString(UInt32(bitPattern: v), limit: 100_000))
                default: break
                }
            }
        }
        return 0
    }

    // MARK: Statements

    private func exec(_ s: Stmt) throws -> Flow {
        try tick()
        switch s.kind {
        case let .block(items):
            for i in items {
                let f = try exec(i)
                if case .normal = f { continue }
                return f
            }
            return .normal
        case let .decl(decls):
            for d in decls {
                guard let sym = d.symbol, let a = frames.last?[sym.id], let ini = d.initializer else { continue }
                try initialize(a, d.type, ini, global: false)
            }
            return .normal
        case let .expr(e):
            _ = try eval(e)
            return .normal
        case let .ifElse(c, a, b):
            if try eval(c) != 0 { return try exec(a) }
            if let b { return try exec(b) }
            return .normal
        case let .whileLoop(c, body):
            while try eval(c) != 0 {
                switch try exec(body) {
                case .breakLoop: return .normal
                case let .returned(v): return .returned(v)
                default: break
                }
            }
            return .normal
        case let .doWhile(body, c):
            repeat {
                switch try exec(body) {
                case .breakLoop: return .normal
                case let .returned(v): return .returned(v)
                default: break
                }
            } while try eval(c) != 0
            return .normal
        case let .forLoop(ini, c, step, body):
            if let ini { _ = try exec(ini) }
            while true {
                if let c, try eval(c) == 0 { break }
                switch try exec(body) {
                case .breakLoop: return .normal
                case let .returned(v): return .returned(v)
                default: break
                }
                if let step { _ = try eval(step) }
            }
            return .normal
        case let .switchCase(v, cases):
            let x = try eval(v)
            var start = cases.firstIndex { $0.value != nil && $0.constant == x }
            if start == nil { start = cases.firstIndex { $0.value == nil } }
            guard let first = start else { return .normal }
            for c in cases[first...] {
                for st in c.body {
                    switch try exec(st) {
                    case .normal: continue
                    case .breakLoop: return .normal
                    case let f: return f
                    }
                }
            }
            return .normal
        case let .returnStmt(e):
            guard let e else { return .returned(0) }
            var v = try eval(e)
            if let f = frames.isEmpty ? nil : currentReturnType(), f == .char { v = convert(v, to: .char) }
            return .returned(v)
        case .breakStmt: return .breakLoop
        case .continueStmt: return .continueLoop
        case .empty: return .normal
        }
    }

    private var returnTypes: [CType] = []
    private func currentReturnType() -> CType? { returnTypes.last }

    // MARK: Expressions

    private func address(_ e: Expr) throws -> UInt32 {
        switch e.kind {
        case .ident:
            guard let s = e.symbol else { throw Fault(message: "unresolved \(e.kind)") }
            if s.kind == .global { return globalAddr[s.id]! }
            return frames.last![s.id]!
        case let .unary(.deref, p):
            return UInt32(bitPattern: try eval(p))
        case let .index(b, i):
            let base = UInt32(bitPattern: try eval(b))
            let idx = try eval(i)
            return base &+ UInt32(bitPattern: idx &* Int32(e.type.size))
        case let .member(b, _, arrow):
            let base = arrow ? UInt32(bitPattern: try eval(b)) : try address(b)
            return base &+ UInt32(e.fieldOffset)
        case .string:
            return stringAddr[e.label]!
        case let .cast(_, x):
            return try address(x)
        default:
            throw Fault(message: "not an lvalue")
        }
    }

    private func arith(_ op: BinaryOp, _ a: Int32, _ b: Int32, _ opType: CType) -> Int32 {
        if let p = opType.pointee, opType.isPointer {
            let size = Int32(p.size)
            switch op {
            case .add: return a &+ b &* size
            case .sub: return a &- b &* size
            default: break
            }
        }
        return RV.binary(op, a, b, unsigned: opType == .uint)
    }

    private func eval(_ e: Expr) throws -> Int32 {
        switch e.kind {
        case let .number(v): return Int32(truncatingIfNeeded: v)
        case .string: return Int32(bitPattern: try address(e))
        case .ident: return try load(e.type, try address(e))
        case let .binary(op, a, b):
            switch op {
            case .logAnd: return try eval(a) != 0 && eval(b) != 0 ? 1 : 0
            case .logOr: return try eval(a) != 0 || eval(b) != 0 ? 1 : 0
            default: break
            }
            let x = try eval(a), y = try eval(b)
            let at = a.type.decayed, bt = b.type.decayed
            if op == .sub && at.isPointer && bt.isPointer {
                return RV.div(x &- y, Int32(at.pointee!.size))
            }
            if op == .add && at.isInteger && bt.isPointer {
                return arith(.add, y, x, e.opType)
            }
            return arith(op, x, y, e.opType)
        case let .unary(op, a):
            switch op {
            case .neg: return 0 &- (try eval(a))
            case .plus: return try eval(a)
            case .bitNot: return ~(try eval(a))
            case .not: return try eval(a) == 0 ? 1 : 0
            case .deref: return try load(e.type, try address(e))
            case .addr: return Int32(bitPattern: try address(a))
            }
        case let .assign(op, lhs, rhs):
            let addr = try address(lhs)
            var v: Int32
            if let op {
                let old = try load(lhs.type, addr)
                v = arith(op, old, try eval(rhs), e.opType)
            } else {
                v = try eval(rhs)
            }
            if lhs.type.isStruct {
                try store(lhs.type, addr, v)
                return Int32(bitPattern: addr)
            }
            v = convert(v, to: lhs.type)
            try store(lhs.type, addr, v)
            return v
        case let .incDec(isPrefix, inc, a):
            let addr = try address(a)
            let old = try load(a.type, addr)
            let new = convert(arith(inc ? .add : .sub, old, 1, e.opType), to: a.type)
            try store(a.type, addr, new)
            return isPrefix ? new : old
        case let .call(name, args):
            if name == "printf" { return try printf(args) }
            var vals: [Int32] = []
            for a in args { vals.append(try eval(a)) }
            if let f = functions[name] {
                for (k, p) in f.params.enumerated() { vals[k] = convert(vals[k], to: p.type) }
                returnTypes.append(f.returnType)
                defer { returnTypes.removeLast() }
                return try call(f, vals)
            }
            if let lib = Library.byName[name] {
                for (k, p) in lib.params.enumerated() { vals[k] = convert(vals[k], to: p) }
                return try callLibrary(name, vals)
            }
            throw Fault(message: "undefined function \(name)")
        case .index, .member:
            return try load(e.type, try address(e))
        case let .ternary(c, a, b):
            return try eval(c) != 0 ? try eval(a) : try eval(b)
        case let .sizeofType(t): return Int32(t.size)
        case .sizeofExpr: return Int32(e.type.size)
        case let .cast(t, a):
            let v = try eval(a)
            return t.isVoid ? 0 : convert(v, to: t)
        case let .comma(a, b):
            _ = try eval(a)
            return try eval(b)
        }
    }
}
