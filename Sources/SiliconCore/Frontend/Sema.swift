import Foundation

/// One lexical scope, kept for the symbol-table view.
public struct ScopeInfo: Sendable {
    public var function: String?
    public var depth: Int
    public var line: Int
    public var symbols: [Symbol]
}

public struct StringLiteral: Sendable {
    public var label: String
    public var bytes: [UInt8]
}

public struct SemaResult {
    public var symbols: [Symbol] = []
    public var scopes: [ScopeInfo] = []
    public var strings: [StringLiteral] = []
    /// Library functions the program calls (directly or through printf).
    public var libraryUsed: Set<String> = []
    public var diagnostics: [Diagnostic] = []
    public var hasErrors: Bool { diagnostics.contains { $0.severity == .error } }
}

/// Semantic analysis: resolves every name to its declaration, works out the
/// type of every expression, inserts the implicit C conversions (array → pointer,
/// char → int, int → unsigned) as type annotations, and rejects programs that
/// are syntactically fine but meaningless (`5 = x`, calling with the wrong
/// number of arguments, `break` outside a loop…).
public final class Sema {
    private let prog: Program
    private var result = SemaResult()
    private var scopes: [[String: Symbol]] = [[:]]
    private var scopeInfo: [ScopeInfo] = []
    private var nextSymbol = 0
    private var currentFunction: FunctionDecl?
    private var loopDepth = 0
    private var switchDepth = 0
    private var nameCounts: [String: Int] = [:]
    private var stringCount = 0
    private var stringLabels: [[UInt8]: String] = [:]

    public static func analyze(_ prog: Program, library: Bool = false) -> SemaResult {
        let s = Sema(prog, library: library)
        s.run()
        return s.result
    }

    /// Compiling the C library itself: library functions may be defined and there is no main.
    private let isLibrary: Bool

    private init(_ prog: Program, library: Bool) {
        self.prog = prog
        isLibrary = library
    }

    private struct Failure: Error { let d: Diagnostic }

    private func fail(_ msg: String, _ range: SourceRange, _ line: Int) -> Failure {
        Failure(d: Diagnostic(.error, .semantic, msg, range: range, line: line))
    }
    private func fail(_ msg: String, _ e: Expr) -> Failure { fail(msg, e.range, e.line) }

    private func warn(_ msg: String, _ range: SourceRange, _ line: Int) {
        result.diagnostics.append(Diagnostic(.warning, .semantic, msg, range: range, line: line))
    }

    private func newSymbol(_ name: String, _ kind: Symbol.Kind, _ type: CType, _ range: SourceRange, _ line: Int) -> Symbol {
        nextSymbol += 1
        let s = Symbol(id: nextSymbol, name: name, kind: kind, type: type, range: range, line: line, depth: scopes.count - 1)
        result.symbols.append(s)
        return s
    }

    private func lookup(_ name: String) -> Symbol? {
        for s in scopes.reversed() {
            if let sym = s[name] { return sym }
        }
        return nil
    }

    private func pushScope(line: Int) {
        scopes.append([:])
        scopeInfo.append(ScopeInfo(function: currentFunction?.name, depth: scopes.count - 1, line: line, symbols: []))
    }

    private func popScope() {
        scopes.removeLast()
        // Find the most recent open record at this depth and close it.
        if let i = scopeInfo.lastIndex(where: { $0.depth == scopes.count }) {
            result.scopes.append(scopeInfo.remove(at: i))
        }
    }

    private func declare(_ sym: Symbol) {
        scopes[scopes.count - 1][sym.name] = sym
        if let i = scopeInfo.lastIndex(where: { $0.depth == scopes.count - 1 }) {
            scopeInfo[i].symbols.append(sym)
        }
    }

    private func run() {
        do {
            try analyzeProgram()
        } catch let f as Failure {
            result.diagnostics.append(f.d)
        } catch {}
    }

    private func analyzeProgram() throws {
        var fileScope = ScopeInfo(function: nil, depth: 0, line: 1, symbols: [])
        for item in prog.items {
            switch item {
            case .structDef:
                continue
            case let .function(f):
                try declareFunction(f, &fileScope)
                if f.body != nil { try analyzeFunction(f) }
            case let .globals(decls):
                for d in decls {
                    try checkObjectType(d.type, d.nameRange, d.line, name: d.name)
                    if let existing = scopes[0][d.name] {
                        throw fail(existing.kind == .function ? "'\(d.name)' is already a function" : "global '\(d.name)' is already defined on line \(existing.line)", d.nameRange, d.line)
                    }
                    if Library.byName[d.name] != nil {
                        throw fail("'\(d.name)' is a library function name", d.nameRange, d.line)
                    }
                    try resolveArraySize(d)
                    let sym = newSymbol(d.name, .global, d.type, d.nameRange, d.line)
                    d.symbol = sym
                    scopes[0][d.name] = sym
                    fileScope.symbols.append(sym)
                    if let ini = d.initializer { try checkGlobalInitializer(ini, d.type, d) }
                }
            }
        }
        if isLibrary {
            result.scopes.insert(fileScope, at: 0)
            return
        }
        guard let main = scopes[0]["main"], main.kind == .function, main.isDefined else {
            result.diagnostics.append(Diagnostic(.error, .semantic, "the program needs a main function: int main() { ... }"))
            return
        }
        if case let .function(r, ps) = main.type {
            if !ps.isEmpty {
                warn("main's parameters are ignored (there is no command line); they will be 0", main.declRange, main.line)
            }
            if r != .int && r != .void {
                throw fail("main should return int", main.declRange, main.line)
            }
        }
        for sym in scopes[0].values where sym.kind == .function && !sym.isDefined {
            if referencedFunctions.contains(sym.name) {
                result.diagnostics.append(Diagnostic(.error, .semantic, "function '\(sym.name)' is declared but never defined (the linker would report: undefined reference to '\(sym.name)')", range: sym.declRange, line: sym.line))
            }
        }
        result.scopes.insert(fileScope, at: 0)
    }

    private var referencedFunctions: Set<String> = []

    private func checkObjectType(_ t: CType, _ range: SourceRange, _ line: Int, name: String) throws {
        switch t {
        case .void: throw fail("variable '\(name)' cannot have type void", range, line)
        case let .structure(s) where !s.isComplete:
            throw fail("struct \(s.name) is not defined (only declared), so '\(name)' has no known size", range, line)
        case let .array(e, _): try checkObjectType(e, range, line, name: name)
        default: break
        }
    }

    private func declareFunction(_ f: FunctionDecl, _ fileScope: inout ScopeInfo) throws {
        let type = CType.function(f.returnType, f.params.map { $0.type })
        if f.returnType.isArray { throw fail("a function cannot return an array", f.nameRange, f.line) }
        if f.returnType.isStruct { throw fail("returning a struct by value is not supported; return a pointer", f.nameRange, f.line) }
        if Library.byName[f.name] != nil && !isLibrary {
            throw fail("'\(f.name)' is a built-in library function and cannot be redefined", f.nameRange, f.line)
        }
        if f.name == "printf" { throw fail("'printf' is built in and cannot be redefined", f.nameRange, f.line) }
        if let existing = scopes[0][f.name] {
            guard existing.kind == .function else {
                throw fail("'\(f.name)' is already a global variable", f.nameRange, f.line)
            }
            if existing.type != type {
                throw fail("'\(f.name)' was declared on line \(existing.line) as \(existing.type.spelled(f.name)), which does not match", f.nameRange, f.line)
            }
            if existing.isDefined && f.body != nil {
                throw fail("function '\(f.name)' is already defined on line \(existing.line)", f.nameRange, f.line)
            }
            if f.body != nil { existing.isDefined = true }
            f.symbol = existing
            return
        }
        let sym = newSymbol(f.name, .function, type, f.nameRange, f.line)
        sym.isDefined = f.body != nil
        f.symbol = sym
        scopes[0][f.name] = sym
        fileScope.symbols.append(sym)
    }

    private func resolveArraySize(_ d: VarDecl) throws {
        guard case let .array(e, n) = d.type, n < 0 else { return }
        switch d.initializer {
        case let .list(items, _)?:
            d.type = .array(e, max(1, items.count))
        case let .expr(x)?:
            if case let .string(bytes) = x.kind, e == .char {
                d.type = .array(.char, bytes.count + 1)
            } else {
                throw fail("cannot work out the size of array '\(d.name)' from this initialiser", d.nameRange, d.line)
            }
        case nil:
            throw fail("array '\(d.name)' needs a size, e.g. \(d.name)[10]", d.nameRange, d.line)
        }
    }

    // MARK: Globals

    private func isConstantAddress(_ e: Expr) -> Bool {
        switch e.kind {
        case .string: return true
        case let .unary(.addr, x):
            if case let .ident(n) = x.kind, let s = lookup(n), s.kind == .global { return true }
            return false
        case let .ident(n):
            if let s = lookup(n), s.kind == .global, s.type.isArray { return true }
            return false
        case let .cast(_, x): return isConstantAddress(x)
        default: return false
        }
    }

    private func checkGlobalInitializer(_ ini: Initializer, _ t: CType, _ d: VarDecl) throws {
        switch (ini, t) {
        case let (.list(items, r), .array(e, n)):
            if items.count > n { throw fail("too many initialisers for '\(d.name)' (it holds \(n))", r, d.line) }
            for i in items { try checkGlobalInitializer(i, e, d) }
        case let (.list(items, r), .structure(s)):
            if items.count > s.fields.count { throw fail("too many initialisers for struct \(s.name)", r, d.line) }
            for (i, f) in zip(items, s.fields) { try checkGlobalInitializer(i, f.type, d) }
        case let (.list(items, r), _):
            if items.count != 1 { throw fail("a scalar needs exactly one initialiser", r, d.line) }
            try checkGlobalInitializer(items[0], t, d)
        case let (.expr(e), .array(.char, n)):
            guard case let .string(bytes) = e.kind else { throw fail("a char array needs a string or {...} initialiser", e) }
            if bytes.count > n { throw fail("the string is longer than '\(d.name)' (\(bytes.count) > \(n))", e) }
        case let (.expr(e), .array):
            throw fail("an array needs a {...} initialiser", e)
        case let (.expr(e), .structure):
            throw fail("a struct needs a {...} initialiser", e)
        case let (.expr(e), _):
            try check(e)
            if t.isPointer && isConstantAddress(e) {
                if case .string = e.kind, t != .pointer(.char) { throw fail("a string can only initialise a char *", e) }
                try checkAssignable(t, e, what: "initialise '\(d.name)'")
                return
            }
            guard Parser.constantValue(e) != nil else {
                throw fail("a global's initial value must be a constant (globals are filled in before main runs)", e)
            }
            try checkAssignable(t, e, what: "initialise '\(d.name)'")
        }
    }

    // MARK: Functions

    private func analyzeFunction(_ f: FunctionDecl) throws {
        currentFunction = f
        nameCounts = [:]
        f.locals = []
        f.hasCalls = false
        defer { currentFunction = nil }
        pushScope(line: f.line)
        for p in f.params {
            if p.name.isEmpty { throw fail("parameter needs a name in a function definition", p.range, p.line) }
            if scopes[scopes.count - 1][p.name] != nil { throw fail("duplicate parameter '\(p.name)'", p.nameRange, p.line) }
            try checkObjectType(p.type, p.nameRange, p.line, name: p.name)
            let s = newSymbol(p.name, .param, p.type, p.nameRange, p.line)
            s.function = f.name
            s.uniqueName = uniqueName(p.name)
            p.symbol = s
            declare(s)
            f.locals.append(s)
        }
        if f.params.count > 8 {
            throw fail("more than 8 parameters: our calling convention passes arguments only in a0–a7", f.nameRange, f.line)
        }
        guard let body = f.body, case let .block(items) = body.kind else { return }
        // The body shares the parameters' scope, as in C.
        for s in items { try stmt(s) }
        popScope()
        if f.returnType != .void && f.name != "main" && !alwaysReturns(body) {
            warn("'\(f.name)' may reach the end without returning a value", f.nameRange, f.line)
        }
    }

    private func uniqueName(_ n: String) -> String {
        let k = nameCounts[n, default: 0]
        nameCounts[n] = k + 1
        return k == 0 ? n : "\(n).\(k)"
    }

    private func alwaysReturns(_ s: Stmt) -> Bool {
        switch s.kind {
        case .returnStmt: return true
        case let .block(items): return items.last.map(alwaysReturns) ?? false
        case let .ifElse(_, a, b?): return alwaysReturns(a) && alwaysReturns(b)
        case let .whileLoop(c, _):
            if let v = Parser.constantValue(c), v != 0 { return true }
            return false
        case let .forLoop(_, c, _, _): return c == nil
        case let .expr(e):
            if case .call("exit", _) = e.kind { return true }
            return false
        default: return false
        }
    }

    // MARK: Statements

    private func stmt(_ s: Stmt) throws {
        switch s.kind {
        case let .block(items):
            pushScope(line: s.line)
            for i in items { try stmt(i) }
            popScope()
        case let .decl(decls):
            for d in decls { try localDecl(d) }
        case let .expr(e):
            try check(e)
        case let .ifElse(c, a, b):
            try condition(c)
            try stmt(a)
            if let b { try stmt(b) }
        case let .whileLoop(c, body):
            try condition(c)
            loopDepth += 1
            try stmt(body)
            loopDepth -= 1
        case let .doWhile(body, c):
            loopDepth += 1
            try stmt(body)
            loopDepth -= 1
            try condition(c)
        case let .forLoop(ini, c, step, body):
            pushScope(line: s.line)
            if let ini { try stmt(ini) }
            if let c { try condition(c) }
            if let step { try check(step) }
            loopDepth += 1
            try stmt(body)
            loopDepth -= 1
            popScope()
        case let .switchCase(v, cases):
            try check(v)
            guard v.type.decayed.isInteger else { throw fail("switch needs an integer value", v) }
            var seen: [Int32: Int] = [:]
            var hasDefault = false
            switchDepth += 1
            for c in cases {
                if let cv = c.value {
                    try check(cv)
                    guard let k = Parser.constantValue(cv) else { throw fail("a case label must be a constant", cv) }
                    c.constant = Int32(truncatingIfNeeded: k)
                    if let l = seen[c.constant] { throw fail("case \(c.constant) already appears on line \(l)", cv) }
                    seen[c.constant] = c.line
                } else {
                    if hasDefault { throw fail("a switch can only have one default", c.range, c.line) }
                    hasDefault = true
                }
                pushScope(line: c.line)
                for b in c.body { try stmt(b) }
                popScope()
            }
            switchDepth -= 1
        case let .returnStmt(e):
            guard let f = currentFunction else { return }
            if let e {
                try check(e)
                if f.returnType == .void {
                    if e.type != .void { throw fail("'\(f.name)' returns void, so it cannot return a value", e) }
                } else {
                    try checkAssignable(f.returnType, e, what: "return from '\(f.name)'")
                }
            } else if f.returnType != .void && f.name != "main" {
                throw fail("'\(f.name)' must return a \(f.returnType)", s.range, s.line)
            }
        case .breakStmt:
            if loopDepth == 0 && switchDepth == 0 { throw fail("'break' is only allowed inside a loop or switch", s.range, s.line) }
        case .continueStmt:
            if loopDepth == 0 { throw fail("'continue' is only allowed inside a loop", s.range, s.line) }
        case .empty:
            break
        }
    }

    private func condition(_ c: Expr) throws {
        try check(c)
        guard c.type.decayed.isScalar else { throw fail("a condition must be a number or a pointer, not \(c.type)", c) }
        if case .assign(nil, _, _) = c.kind {
            warn("'=' in a condition assigns; did you mean '=='?", c.range, c.line)
        }
    }

    private func localDecl(_ d: VarDecl) throws {
        guard let f = currentFunction else { return }
        try resolveArraySize(d)
        try checkObjectType(d.type, d.nameRange, d.line, name: d.name)
        if let prev = scopes[scopes.count - 1][d.name] {
            throw fail("'\(d.name)' is already declared in this scope (line \(prev.line))", d.nameRange, d.line)
        }
        if d.type.size > 256 * 1024 {
            throw fail("local '\(d.name)' is \(d.type.size) bytes — too big for the stack; make it a global", d.nameRange, d.line)
        }
        // Check the initialiser before declaring the name: in `int x = x;` the right x is the outer one.
        if let ini = d.initializer { try checkLocalInitializer(ini, d.type, d) }
        let s = newSymbol(d.name, .local, d.type, d.nameRange, d.line)
        s.function = f.name
        s.uniqueName = uniqueName(d.name)
        d.symbol = s
        declare(s)
        f.locals.append(s)
    }

    private func checkLocalInitializer(_ ini: Initializer, _ t: CType, _ d: VarDecl) throws {
        switch (ini, t) {
        case let (.list(items, r), .array(e, n)):
            if items.count > n { throw fail("too many initialisers for '\(d.name)' (it holds \(n))", r, d.line) }
            for i in items { try checkLocalInitializer(i, e, d) }
        case let (.list(items, r), .structure(s)):
            if items.count > s.fields.count { throw fail("too many initialisers for struct \(s.name)", r, d.line) }
            for (i, f) in zip(items, s.fields) { try checkLocalInitializer(i, f.type, d) }
        case let (.list(items, r), _):
            if items.count != 1 { throw fail("a scalar needs exactly one initialiser", r, d.line) }
            try checkLocalInitializer(items[0], t, d)
        case let (.expr(e), .array(.char, n)):
            if case let .string(bytes) = e.kind {
                if bytes.count > n { throw fail("the string is longer than '\(d.name)' (\(bytes.count) > \(n))", e) }
                try check(e)
                return
            }
            throw fail("a char array needs a string or {...} initialiser", e)
        case let (.expr(e), .array):
            throw fail("an array needs a {...} initialiser", e)
        case let (.expr(e), _):
            try check(e)
            try checkAssignable(t, e, what: "initialise '\(d.name)'")
        }
    }

    // MARK: Expressions

    private func isNullConstant(_ e: Expr) -> Bool {
        switch e.kind {
        case let .number(v): return v == 0
        case let .cast(t, x): return (t.isPointer || t.isInteger) && isNullConstant(x)
        default: return false
        }
    }

    /// Can a value of e's type be stored in something of type `t`?
    private func checkAssignable(_ t: CType, _ e: Expr, what: String) throws {
        let s = e.type.decayed
        if t.isInteger && s.isInteger { return }
        if case let .structure(a) = t, case let .structure(b) = s, a === b { return }
        if let tp = t.pointee, t.isPointer {
            if isNullConstant(e) { return }
            if let sp = s.pointee, s.isPointer {
                if tp == sp || tp.isVoid || sp.isVoid { return }
                warn("\(what): converting \(s) to \(t) (different pointer types)", e.range, e.line)
                return
            }
            if s.isInteger { throw fail("cannot \(what) with an integer: \(t) needs a pointer (cast with (\(t)) if you really mean it)", e) }
        }
        if t.isInteger && s.isPointer { throw fail("cannot \(what) with a pointer: \(t) is an integer (cast with (\(t)) if you really mean it)", e) }
        if s.isVoid { throw fail("cannot \(what) with a void value", e) }
        throw fail("cannot \(what): \(s) is not compatible with \(t)", e)
    }

    /// The "usual arithmetic conversions": char promotes to int; if either side is unsigned, both become unsigned.
    private func arith(_ a: CType, _ b: CType) -> CType {
        (a == .uint || b == .uint) ? .uint : .int
    }
    private func promote(_ a: CType) -> CType { a == .uint ? .uint : .int }

    private func requireLValue(_ e: Expr, _ what: String) throws {
        guard e.isLValue else { throw fail("\(what) needs a variable or memory location on the left, not an expression", e) }
        if e.type.isArray { throw fail("\(what): an array cannot be assigned as a whole; assign its elements", e) }
    }

    private func markAddressTaken(_ e: Expr) {
        switch e.kind {
        case .ident: e.symbol?.addressTaken = true
        case let .member(base, _, false): markAddressTaken(base)
        case let .index(base, _) where base.type.isArray: markAddressTaken(base)
        default: break
        }
    }

    private func internString(_ bytes: [UInt8]) -> String {
        if let l = stringLabels[bytes] { return l }
        let l = ".LC\(stringCount)"
        stringCount += 1
        stringLabels[bytes] = l
        result.strings.append(StringLiteral(label: l, bytes: bytes))
        return l
    }

    private func check(_ e: Expr) throws {
        switch e.kind {
        case .number:
            // The parser set int or unsigned already.
            e.isLValue = false
        case let .string(bytes):
            e.type = .array(.char, bytes.count + 1)
            e.label = internString(bytes)
        case let .ident(name):
            guard let s = lookup(name) else {
                if name == "NULL" {
                    let zero = Expr(id: -e.id, kind: .number(0), range: e.range, line: e.line)
                    e.kind = .cast(.pointer(.void), zero)
                    try check(e)
                    return
                }
                if Library.byName[name] != nil || name == "printf" {
                    throw fail("'\(name)' is a function: call it with \(name)(...)", e)
                }
                throw fail("'\(name)' is not declared", e)
            }
            if s.kind == .function { throw fail("'\(name)' is a function; function pointers are not supported", e) }
            e.symbol = s
            e.type = s.type
            e.isLValue = true
        case let .binary(op, a, b):
            try check(a)
            try check(b)
            try binary(e, op, a, b)
        case let .unary(op, a):
            try check(a)
            let t = a.type.decayed
            switch op {
            case .neg, .plus, .bitNot:
                guard t.isInteger else { throw fail("unary '\(op.rawValue)' needs a number, not \(t)", e) }
                e.type = promote(t)
                e.opType = e.type
            case .not:
                guard t.isScalar else { throw fail("'!' needs a number or pointer, not \(t)", e) }
                e.type = .int
            case .deref:
                guard let p = t.pointee, t.isPointer else { throw fail("'*' needs a pointer, but this is \(t)", e) }
                if p.isVoid { throw fail("cannot dereference a void * (cast it to a real pointer type first)", e) }
                e.type = p
                e.isLValue = true
            case .addr:
                guard a.isLValue || a.type.isArray else { throw fail("'&' needs a variable or memory location", e) }
                markAddressTaken(a)
                e.type = .pointer(a.type)
            }
        case let .assign(op, lhs, rhs):
            try check(lhs)
            try check(rhs)
            try requireLValue(lhs, "assignment")
            if let op {
                let lt = lhs.type, rt = rhs.type.decayed
                if (op == .add || op == .sub) && lt.isPointer {
                    guard rt.isInteger else { throw fail("pointer \(op.rawValue)= needs an integer on the right", e) }
                    e.opType = lt
                } else {
                    guard lt.isInteger && rt.isInteger else { throw fail("'\(op.rawValue)=' needs numbers on both sides", e) }
                    e.opType = (op == .shl || op == .shr) ? promote(lt) : arith(lt, rt)
                }
            } else {
                try checkAssignable(lhs.type, rhs, what: "assign to '\(describe(lhs))'")
            }
            e.type = lhs.type
        case let .incDec(_, inc, a):
            try check(a)
            try requireLValue(a, inc ? "'++'" : "'--'")
            guard a.type.isScalar else { throw fail("'\(inc ? "++" : "--")' needs a number or pointer", e) }
            if let p = a.type.pointee, p.isVoid { throw fail("cannot do arithmetic on a void *", e) }
            e.type = a.type
            e.opType = a.type.isPointer ? a.type : promote(a.type)
        case let .call(name, args):
            try call(e, name, args)
        case let .index(base, idx):
            try check(base)
            try check(idx)
            var b = base.type.decayed, i = idx.type.decayed
            if b.isInteger && i.isPointer {
                // C allows 3[a]. Swap so the pointer is first.
                e.kind = .index(idx, base)
                swap(&b, &i)
            }
            guard let elem = b.pointee, b.isPointer else { throw fail("only arrays and pointers can be indexed with [ ]; this is \(base.type)", e) }
            guard i.isInteger else { throw fail("an array index must be an integer", idx) }
            if elem.isVoid { throw fail("cannot index a void *", e) }
            e.type = elem
            e.isLValue = true
        case let .member(base, field, arrow):
            try check(base)
            let st: StructDef
            if arrow {
                guard case let .pointer(.structure(s)) = base.type.decayed else {
                    throw fail("'->' needs a pointer to a struct, but this is \(base.type)", e)
                }
                st = s
            } else {
                guard case let .structure(s) = base.type else {
                    if case .pointer(.structure) = base.type { throw fail("this is a pointer: use '->' instead of '.'", e) }
                    throw fail("'.' needs a struct, but this is \(base.type)", e)
                }
                st = s
            }
            guard let f = st.field(field) else { throw fail("struct \(st.name) has no field '\(field)'", e) }
            e.type = f.type
            e.fieldOffset = f.offset
            e.isLValue = true
        case let .ternary(c, a, b):
            try check(c)
            try check(a)
            try check(b)
            guard c.type.decayed.isScalar else { throw fail("the ?: condition must be a number or pointer", c) }
            let at = a.type.decayed, bt = b.type.decayed
            if at.isInteger && bt.isInteger {
                e.type = arith(at, bt)
            } else if at.isPointer && bt.isPointer {
                e.type = at.pointee!.isVoid ? bt : at
            } else if at.isPointer && isNullConstant(b) {
                e.type = at
            } else if bt.isPointer && isNullConstant(a) {
                e.type = bt
            } else if at.isVoid && bt.isVoid {
                e.type = .void
            } else if case let .structure(x) = at, case let .structure(y) = bt, x === y {
                throw fail("?: on structs is not supported", e)
            } else {
                throw fail("the two sides of ?: have incompatible types (\(at) and \(bt))", e)
            }
        case let .sizeofType(t):
            if t.isVoid { throw fail("sizeof(void) is not allowed", e) }
            e.type = .uint
        case let .sizeofExpr(a):
            try check(a)
            e.type = .uint
            e.kind = .sizeofType(a.type)
        case let .cast(t, a):
            try check(a)
            let s = a.type.decayed
            if t.isVoid { e.type = .void; return }
            guard t.isScalar else { throw fail("can only cast to a number or pointer type, not \(t)", e) }
            guard s.isScalar else { throw fail("cannot cast \(a.type) to \(t)", e) }
            e.type = t
        case let .comma(a, b):
            try check(a)
            try check(b)
            e.type = b.type.decayed
        }
    }

    private func describe(_ e: Expr) -> String {
        switch e.kind {
        case let .ident(n): return n
        case let .index(b, _): return describe(b) + "[…]"
        case let .member(b, f, arrow): return describe(b) + (arrow ? "->" : ".") + f
        case let .unary(.deref, b): return "*" + describe(b)
        default: return "expression"
        }
    }

    private func binary(_ e: Expr, _ op: BinaryOp, _ a: Expr, _ b: Expr) throws {
        let at = a.type.decayed, bt = b.type.decayed
        switch op {
        case .add:
            if at.isPointer && bt.isInteger {
                try requireComplete(at, e)
                e.type = at; e.opType = at
            } else if at.isInteger && bt.isPointer {
                try requireComplete(bt, e)
                e.type = bt; e.opType = bt
            } else if at.isInteger && bt.isInteger {
                e.type = arith(at, bt); e.opType = e.type
            } else {
                throw fail("cannot add \(at) and \(bt)", e)
            }
        case .sub:
            if at.isPointer && bt.isInteger {
                try requireComplete(at, e)
                e.type = at; e.opType = at
            } else if at.isPointer && bt.isPointer {
                guard at.pointee == bt.pointee else { throw fail("cannot subtract pointers of different types (\(at) and \(bt))", e) }
                try requireComplete(at, e)
                e.type = .int; e.opType = at
            } else if at.isInteger && bt.isInteger {
                e.type = arith(at, bt); e.opType = e.type
            } else {
                throw fail("cannot subtract \(bt) from \(at)", e)
            }
        case .mul, .div, .rem, .bitAnd, .bitOr, .bitXor:
            guard at.isInteger && bt.isInteger else { throw fail("'\(op.rawValue)' needs numbers on both sides (got \(at) and \(bt))", e) }
            e.type = arith(at, bt); e.opType = e.type
            if (op == .div || op == .rem), let v = Parser.constantValue(b), v == 0 {
                warn("division by zero (RISC-V gives \(op == .div ? "-1" : "the dividend") instead of crashing)", e.range, e.line)
            }
        case .shl, .shr:
            guard at.isInteger && bt.isInteger else { throw fail("'\(op.rawValue)' needs numbers on both sides", e) }
            e.type = promote(at); e.opType = e.type
            if let v = Parser.constantValue(b), v < 0 || v > 31 {
                warn("shifting by \(v): RISC-V only uses the low 5 bits of the shift amount", e.range, e.line)
            }
        case .lt, .gt, .le, .ge, .eq, .ne:
            if at.isInteger && bt.isInteger {
                e.opType = arith(at, bt)
                if e.opType == .uint && (at != bt) && (op != .eq && op != .ne) {
                    if (at == .int && !isNonNegativeConstant(a)) || (bt == .int && !isNonNegativeConstant(b)) {
                        warn("comparing a signed and an unsigned value: the signed one is treated as unsigned", e.range, e.line)
                    }
                }
            } else if at.isPointer && bt.isPointer {
                if at.pointee != bt.pointee && !(at.pointee!.isVoid || bt.pointee!.isVoid) {
                    warn("comparing different pointer types (\(at) and \(bt))", e.range, e.line)
                }
                e.opType = .uint
            } else if at.isPointer && isNullConstant(b) || bt.isPointer && isNullConstant(a) {
                e.opType = .uint
            } else {
                throw fail("cannot compare \(at) with \(bt)", e)
            }
            e.type = .int
        case .logAnd, .logOr:
            guard at.isScalar && bt.isScalar else { throw fail("'\(op.rawValue)' needs numbers or pointers", e) }
            e.type = .int
        }
    }

    private func isNonNegativeConstant(_ e: Expr) -> Bool {
        if let v = Parser.constantValue(e) { return v >= 0 }
        return false
    }

    private func requireComplete(_ p: CType, _ e: Expr) throws {
        guard let t = p.pointee else { return }
        if t.isVoid { throw fail("cannot do arithmetic on a void * (cast it to char * first)", e) }
        if case let .structure(s) = t, !s.isComplete { throw fail("struct \(s.name) is incomplete", e) }
    }

    private func call(_ e: Expr, _ name: String, _ args: [Expr]) throws {
        for a in args { try check(a) }
        currentFunction?.hasCalls = true
        if name == "printf" {
            try printf(e, args)
            return
        }
        let params: [CType], ret: CType
        if let s = lookup(name), s.kind == .function, case let .function(r, ps) = s.type {
            params = ps
            ret = r
            referencedFunctions.insert(name)
        } else if let s = lookup(name), s.kind != .function {
            throw fail("'\(name)' is a variable, not a function", e)
        } else if let lib = Library.byName[name] {
            params = lib.params
            ret = lib.returnType
            result.libraryUsed.insert(name)
        } else {
            throw fail("function '\(name)' is not declared (declare or define it above this line)", e)
        }
        if args.count != params.count {
            throw fail("'\(name)' takes \(params.count) argument\(params.count == 1 ? "" : "s") but \(args.count) \(args.count == 1 ? "was" : "were") given", e)
        }
        for (i, (a, p)) in zip(args, params).enumerated() {
            try checkAssignable(p, a, what: "pass argument \(i + 1) of '\(name)'")
        }
        e.type = ret
    }

    private func printf(_ e: Expr, _ args: [Expr]) throws {
        guard let fmt = args.first, case let .string(bytes) = fmt.kind else {
            throw fail("printf needs a string literal as its format, e.g. printf(\"%d\\n\", x)", e)
        }
        let pieces: [PrintfPiece]
        switch PrintfFormat.parse(bytes) {
        case let .success(p): pieces = p
        case let .failure(err): throw fail(err.message, fmt)
        }
        var ai = 1
        for piece in pieces {
            switch piece {
            case let .text(t):
                if t.count == 1 { result.libraryUsed.insert("print_char") } else {
                    _ = internString(t)
                    result.libraryUsed.insert("print_str")
                }
            case let .conversion(c):
                guard ai < args.count else { throw fail("printf: the format has more % conversions than arguments", e) }
                let a = args[ai], t = a.type.decayed
                ai += 1
                switch c {
                case "d", "i":
                    guard t.isInteger else { throw fail("printf %\(c) needs an integer, but argument \(ai - 1) is \(t)", a) }
                    result.libraryUsed.insert("print_int")
                case "u":
                    guard t.isInteger else { throw fail("printf %u needs an integer", a) }
                    result.libraryUsed.insert("print_uint")
                case "x", "X", "p":
                    guard t.isScalar else { throw fail("printf %\(c) needs an integer or pointer", a) }
                    result.libraryUsed.insert("print_hex")
                case "c":
                    guard t.isInteger else { throw fail("printf %c needs a character", a) }
                    result.libraryUsed.insert("print_char")
                case "s":
                    guard t == .pointer(.char) else { throw fail("printf %s needs a char * string, but argument \(ai - 1) is \(t)", a) }
                    result.libraryUsed.insert("print_str")
                default: break
                }
            }
        }
        if ai < args.count {
            throw fail("printf has \(args.count - ai) more argument\(args.count - ai == 1 ? "" : "s") than % conversions", args[ai])
        }
        e.type = .int
    }
}
