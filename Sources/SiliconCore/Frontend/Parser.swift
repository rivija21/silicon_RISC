import Foundation

/// Recursive-descent parser: tokens in, abstract syntax tree out.
///
/// Each grammar rule is one function. Binary operators use one function
/// per precedence level, from `comma` (loosest) down to `primary` (tightest).
/// The parser stops at the first syntax error.
public struct Parser {
    private var toks: [Token]
    private var p = 0
    private var nextID = 0
    private var structs: [String: StructDef] = [:]
    private var structList: [StructDef] = []
    /// typedef scopes: names that mean a type. Innermost last.
    private var typedefs: [[String: CType]] = [[:]]
    private var warnings: [Diagnostic] = []

    public static func parse(_ lex: LexResult) -> (Program?, [Diagnostic]) {
        var ps = Parser(toks: lex.tokens)
        do {
            let prog = try ps.program()
            return (prog, ps.warnings)
        } catch let d as Diagnostic {
            return (nil, ps.warnings + [d])
        } catch {
            return (nil, [Diagnostic(.error, .parser, "\(error)")])
        }
    }

    private init(toks: [Token]) { self.toks = toks }

    // MARK: Helpers

    private var cur: Token { toks[p] }
    private func peek(_ k: Int = 1) -> Token { toks[min(p + k, toks.count - 1)] }
    private var atEnd: Bool { cur.kind == .eof }

    private mutating func id() -> Int {
        nextID += 1
        return nextID
    }

    private func fail(_ msg: String, at t: Token? = nil) -> Diagnostic {
        let t = t ?? cur
        return Diagnostic(.error, .parser, msg, range: t.range, line: t.line)
    }

    @discardableResult
    private mutating func advance() -> Token {
        let t = cur
        if p < toks.count - 1 { p += 1 }
        return t
    }

    private mutating func accept(_ punct: String) -> Bool {
        if cur.isPunct(punct) {
            advance()
            return true
        }
        return false
    }

    private mutating func acceptKeyword(_ k: String) -> Bool {
        if cur.isKeyword(k) {
            advance()
            return true
        }
        return false
    }

    @discardableResult
    private mutating func expect(_ punct: String, _ what: String? = nil) throws -> Token {
        if cur.isPunct(punct) { return advance() }
        let found = cur.kind == .eof ? "the end of the file" : "'\(cur.text)'"
        // Point a missing ';' at the end of the previous token, which is where it belongs.
        if punct == ";" && p > 0 {
            let prev = toks[p - 1]
            throw Diagnostic(.error, .parser, "expected ';' after \(what ?? "this"), found \(found)",
                             range: SourceRange(prev.range.end, prev.range.end + 1), line: prev.line)
        }
        throw fail("expected '\(punct)'\(what.map { " " + $0 } ?? ""), found \(found)")
    }

    private mutating func expectIdentifier(_ what: String) throws -> Token {
        if cur.kind == .identifier { return advance() }
        if cur.kind == .keyword { throw fail("'\(cur.text)' is a keyword and cannot be used as \(what)") }
        throw fail("expected \(what), found '\(cur.text)'")
    }

    private func range(from start: Token) -> SourceRange {
        SourceRange(start.range.start, toks[max(0, p - 1)].range.end)
    }

    private func lookupTypedef(_ name: String) -> CType? {
        for scope in typedefs.reversed() {
            if let t = scope[name] { return t }
        }
        return nil
    }

    /// Does the current token begin a type (so this is a declaration)?
    private func startsType(_ t: Token) -> Bool {
        if t.kind == .keyword {
            return ["int", "char", "void", "unsigned", "signed", "long", "short", "const", "static", "extern",
                    "volatile", "struct", "typedef", "float", "double", "register", "auto", "enum", "union"].contains(t.text)
        }
        return t.kind == .identifier && lookupTypedef(t.text) != nil
    }

    // MARK: Program

    private mutating func program() throws -> Program {
        let prog = Program()
        while !atEnd {
            if accept(";") { continue }
            try topLevel(prog)
        }
        prog.structs = structList
        prog.nodeCount = nextID
        return prog
    }

    private struct Specifiers {
        var type: CType
        var isTypedef = false
        var isStatic = false
        var isExtern = false
        var definedStruct: StructDef?
    }

    private mutating func topLevel(_ prog: Program) throws {
        let start = cur
        guard startsType(cur) else {
            if cur.kind == .identifier && peek().isPunct("(") {
                throw fail("'\(cur.text)' needs a return type, e.g. int \(cur.text)(...)")
            }
            throw fail("expected a declaration (a function or a global variable), found '\(cur.text)'")
        }
        let spec = try specifiers()
        if let s = spec.definedStruct, cur.isPunct(";") {
            advance()
            prog.items.append(.structDef(s, range(from: start), start.line))
            return
        }
        if let s = spec.definedStruct {
            prog.items.append(.structDef(s, range(from: start), start.line))
        }
        if spec.isTypedef {
            try typedefDeclarators(spec)
            return
        }
        var globals: [VarDecl] = []
        while true {
            let declStart = cur
            let d = try declarator(spec.type)
            if case let .function(ret, _) = d.type {
                guard globals.isEmpty else { throw fail("a function cannot be declared in a list with variables", at: declStart) }
                let fn = FunctionDecl(id: id(), name: d.name, returnType: ret, params: d.params ?? [], body: nil,
                                      range: range(from: start), nameRange: d.nameRange, line: declStart.line)
                if cur.isPunct("{") {
                    typedefs.append([:])
                    defer { typedefs.removeLast() }
                    fn.body = try block()
                } else {
                    try expect(";", "the function declaration")
                }
                prog.items.append(.function(fn))
                return
            }
            var initializer: Initializer?
            if accept("=") { initializer = try initializerList() }
            globals.append(VarDecl(id: id(), name: d.name, type: d.type, range: range(from: declStart),
                                   nameRange: d.nameRange, line: declStart.line, initializer: initializer))
            if accept(",") { continue }
            try expect(";", "the declaration")
            break
        }
        prog.items.append(.globals(globals))
    }

    private mutating func typedefDeclarators(_ spec: Specifiers) throws {
        repeat {
            let d = try declarator(spec.type)
            if typedefs[typedefs.count - 1][d.name] != nil {
                throw Diagnostic(.error, .parser, "typedef '\(d.name)' is already defined", range: d.nameRange, line: cur.line)
            }
            typedefs[typedefs.count - 1][d.name] = d.type
        } while accept(",")
        try expect(";", "the typedef")
    }

    // MARK: Types

    private mutating func specifiers() throws -> Specifiers {
        var base: CType?
        var sawUnsigned = false, sawSigned = false, sawInt = false, sawChar = false, sawLong = false
        var spec = Specifiers(type: .int)
        let start = cur
        loop: while true {
            let t = cur
            if t.kind == .identifier, base == nil, !sawUnsigned, !sawSigned, !sawInt, !sawChar, !sawLong,
               let td = lookupTypedef(t.text) {
                advance()
                base = td
                continue
            }
            guard t.kind == .keyword else { break }
            switch t.text {
            case "const", "volatile", "register", "auto": advance()
            case "static": advance(); spec.isStatic = true
            case "extern": advance(); spec.isExtern = true
            case "typedef": advance(); spec.isTypedef = true
            case "void": advance(); base = .void
            case "char": advance(); sawChar = true
            case "int": advance(); sawInt = true
            case "unsigned": advance(); sawUnsigned = true
            case "signed": advance(); sawSigned = true
            case "long": advance(); sawLong = true
            case "short": throw fail("'short' is not supported here; use int or char")
            case "float", "double": throw fail("floating point ('\(t.text)') is not supported: RV32IM has no FPU, use int")
            case "enum", "union": throw fail("'\(t.text)' is not supported")
            case "struct":
                advance()
                let s = try structSpecifier()
                base = .structure(s)
                if s.isComplete, toks[p - 1].isPunct("}") { spec.definedStruct = s }
            default: break loop
            }
        }
        if sawChar {
            if sawUnsigned { throw fail("'unsigned char' is not supported; use char or unsigned", at: start) }
            base = .char
        } else if sawUnsigned {
            base = .uint
        } else if sawInt || sawSigned || sawLong {
            if base != nil { throw fail("conflicting types in this declaration", at: start) }
            base = .int
        }
        guard let b = base else { throw fail("expected a type name, found '\(cur.text)'") }
        spec.type = b
        return spec
    }

    private mutating func structSpecifier() throws -> StructDef {
        var name = ""
        let nameTok = cur
        if cur.kind == .identifier {
            name = advance().text
        }
        if cur.isPunct("{") {
            let s: StructDef
            if !name.isEmpty, let existing = structs[name] {
                if existing.isComplete { throw fail("struct \(name) is already defined", at: nameTok) }
                s = existing
            } else {
                s = StructDef(name: name.isEmpty ? "<anonymous \(structList.count + 1)>" : name, id: structList.count + 1)
                structList.append(s)
                if !name.isEmpty { structs[name] = s }
            }
            advance()
            var offset = 0, align = 1
            while !cur.isPunct("}") {
                if atEnd { throw fail("struct \(s.name) is missing its closing '}'") }
                let fs = try specifiers()
                repeat {
                    let fieldTok = cur
                    let d = try declarator(fs.type)
                    if case .function = d.type { throw fail("a struct cannot contain a function", at: fieldTok) }
                    if d.type.isVoid { throw fail("a field cannot have type void", at: fieldTok) }
                    if case let .structure(inner) = d.type, !inner.isComplete {
                        throw fail("field '\(d.name)' has incomplete type struct \(inner.name) (use a pointer: struct \(inner.name) *\(d.name))", at: fieldTok)
                    }
                    if case let .array(_, n) = d.type, n < 0 { throw fail("a struct field needs an array size", at: fieldTok) }
                    if s.field(d.name) != nil { throw fail("duplicate field '\(d.name)'", at: fieldTok) }
                    let a = d.type.align
                    offset = (offset + a - 1) / a * a
                    s.fields.append(.init(name: d.name, type: d.type, offset: offset))
                    offset += d.type.size
                    align = max(align, a)
                } while accept(",")
                try expect(";", "the field")
            }
            advance()
            s.size = max(align, (offset + align - 1) / align * align)
            s.align = align
            s.isComplete = true
            if s.fields.isEmpty { throw fail("struct \(s.name) has no fields", at: nameTok) }
            return s
        }
        guard !name.isEmpty else { throw fail("expected a struct name or '{'") }
        if let s = structs[name] { return s }
        // Forward reference: `struct node *next;` before struct node is complete.
        let s = StructDef(name: name, id: structList.count + 1)
        structList.append(s)
        structs[name] = s
        return s
    }

    private struct Declarator {
        var name: String
        var nameRange: SourceRange
        var type: CType
        var params: [VarDecl]?
    }

    /// Parses `*name[3]`, `(*p)[4]`, `f(int a, char *b)` and so on, applied to a base type.
    /// C declarators read "inside out", so the parse builds a chain of type
    /// wrappers and applies them from the innermost one outwards.
    private mutating func declarator(_ base: CType, abstract: Bool = false) throws -> Declarator {
        var stars = 0
        while accept("*") {
            stars += 1
            while acceptKeyword("const") || acceptKeyword("volatile") {}
        }
        var name = "", nameRange = SourceRange.none
        // Pointer levels written inside parentheses: int (*p)[4] is a pointer to an array.
        var innerStars = -1
        if cur.isPunct("("), peek().isPunct("*") {
            advance()
            innerStars = 0
            while accept("*") { innerStars += 1 }
            if cur.kind == .identifier {
                let t = advance()
                name = t.text
                nameRange = t.range
            } else if !abstract {
                throw fail("expected a name, found '\(cur.text)'")
            }
            try expect(")", "to close the declarator")
        } else if cur.kind == .identifier {
            let t = advance()
            name = t.text
            nameRange = t.range
        } else if !abstract {
            if cur.kind == .keyword { throw fail("'\(cur.text)' is a keyword and cannot be used as a name") }
            throw fail("expected a name, found '\(cur.text)'")
        }
        var type = base
        for _ in 0..<stars { type = .pointer(type) }
        var params: [VarDecl]?
        // Suffixes: arrays [n] and parameter lists (...). Array dimensions
        // apply right to left: int a[2][3] is "array of 2 arrays of 3 ints".
        var dims: [Int] = []
        if cur.isPunct("(") && innerStars < 0 {
            advance()
            params = try parameterList()
            type = .function(type, params!.map { $0.type })
        }
        while cur.isPunct("[") {
            let open = advance()
            if cur.isPunct("]") {
                advance()
                if !dims.isEmpty { throw fail("only the first array dimension can be left out", at: open) }
                dims.append(-1)
                continue
            }
            let e = try assignment()
            guard let n = Parser.constantValue(e) else { throw fail("array size must be a constant", at: open) }
            if n <= 0 { throw fail("array size must be positive (got \(n))", at: open) }
            if n > 1 << 22 { throw fail("array is too large (\(n) elements)", at: open) }
            try expect("]")
            dims.append(Int(n))
        }
        for n in dims.reversed() { type = .array(type, n) }
        if innerStars > 0 { for _ in 0..<innerStars { type = .pointer(type) } }
        return Declarator(name: name, nameRange: nameRange, type: type, params: params)
    }

    private mutating func parameterList() throws -> [VarDecl] {
        var params: [VarDecl] = []
        if accept(")") { return params }
        if cur.isKeyword("void") && peek().isPunct(")") {
            advance()
            advance()
            return params
        }
        repeat {
            let start = cur
            if cur.isPunct("...") { throw fail("variadic functions (...) are not supported") }
            guard startsType(cur) else { throw fail("expected a parameter type, found '\(cur.text)'") }
            let spec = try specifiers()
            var d = try declarator(spec.type, abstract: true)
            // Array parameters are really pointers.
            if case let .array(e, _) = d.type { d.type = .pointer(e) }
            if d.type.isVoid { throw fail("a parameter cannot have type void", at: start) }
            if d.type.isStruct { throw fail("passing a struct by value is not supported; pass a pointer", at: start) }
            params.append(VarDecl(id: id(), name: d.name, type: d.type, range: range(from: start),
                                  nameRange: d.nameRange.isValid ? d.nameRange : range(from: start), line: start.line, initializer: nil))
        } while accept(",")
        try expect(")", "to close the parameter list")
        return params
    }

    /// Evaluates integer constant expressions used for array sizes and case labels.
    public static func constantValue(_ e: Expr) -> Int64? {
        switch e.kind {
        case let .number(v): return v
        case let .sizeofType(t): return Int64(t.size)
        case let .unary(op, a):
            guard let x = constantValue(a) else { return nil }
            switch op {
            case .neg: return Int64(Int32(truncatingIfNeeded: -x))
            case .plus: return x
            case .bitNot: return Int64(~Int32(truncatingIfNeeded: x))
            case .not: return x == 0 ? 1 : 0
            default: return nil
            }
        case let .binary(op, a, b):
            guard let x = constantValue(a), let y = constantValue(b) else { return nil }
            let r = RV.binary(op, Int32(truncatingIfNeeded: x), Int32(truncatingIfNeeded: y), unsigned: false)
            return Int64(r)
        case let .ternary(c, a, b):
            guard let cv = constantValue(c) else { return nil }
            return cv != 0 ? constantValue(a) : constantValue(b)
        case let .cast(t, a):
            guard let x = constantValue(a) else { return nil }
            return t == .char ? Int64(Int8(truncatingIfNeeded: x)) : x
        default: return nil
        }
    }

    // MARK: Statements

    private mutating func block() throws -> Stmt {
        let start = try expect("{")
        typedefs.append([:])
        defer { typedefs.removeLast() }
        var items: [Stmt] = []
        while !cur.isPunct("}") {
            if atEnd { throw fail("missing '}' to close the block opened on line \(start.line)", at: start) }
            items.append(try statement())
        }
        advance()
        return Stmt(id: id(), kind: .block(items), range: range(from: start), line: start.line)
    }

    private mutating func statement() throws -> Stmt {
        let start = cur
        if startsType(cur) && !(cur.kind == .identifier && peek().isPunct(":")) {
            return try declarationStatement()
        }
        if cur.isPunct("{") { return try block() }
        if accept(";") { return Stmt(id: id(), kind: .empty, range: start.range, line: start.line) }
        if cur.kind == .keyword {
            switch cur.text {
            case "if":
                advance()
                try expect("(", "after 'if'")
                let c = try expression()
                try expect(")", "to close the if condition")
                let then = try statement()
                var els: Stmt?
                if acceptKeyword("else") { els = try statement() }
                return Stmt(id: id(), kind: .ifElse(c, then, els), range: range(from: start), line: start.line)
            case "while":
                advance()
                try expect("(", "after 'while'")
                let c = try expression()
                try expect(")", "to close the while condition")
                let body = try statement()
                return Stmt(id: id(), kind: .whileLoop(c, body), range: range(from: start), line: start.line)
            case "do":
                advance()
                let body = try statement()
                guard acceptKeyword("while") else { throw fail("expected 'while' after the do-loop body") }
                try expect("(", "after 'while'")
                let c = try expression()
                try expect(")", "to close the while condition")
                try expect(";", "do { ... } while (...)")
                return Stmt(id: id(), kind: .doWhile(body, c), range: range(from: start), line: start.line)
            case "for":
                advance()
                try expect("(", "after 'for'")
                typedefs.append([:])
                defer { typedefs.removeLast() }
                var initS: Stmt?
                if startsType(cur) {
                    initS = try declarationStatement()
                } else if !accept(";") {
                    let e = try expression()
                    try expect(";", "the for-loop initialiser")
                    initS = Stmt(id: id(), kind: .expr(e), range: e.range, line: e.line)
                }
                var cond: Expr?
                if !cur.isPunct(";") { cond = try expression() }
                try expect(";", "the for-loop condition")
                var step: Expr?
                if !cur.isPunct(")") { step = try expression() }
                try expect(")", "to close the for-loop header")
                let body = try statement()
                return Stmt(id: id(), kind: .forLoop(initS, cond, step, body), range: range(from: start), line: start.line)
            case "switch":
                return try switchStatement()
            case "return":
                advance()
                var e: Expr?
                if !cur.isPunct(";") { e = try expression() }
                try expect(";", "the return statement")
                return Stmt(id: id(), kind: .returnStmt(e), range: range(from: start), line: start.line)
            case "break":
                advance()
                try expect(";", "'break'")
                return Stmt(id: id(), kind: .breakStmt, range: range(from: start), line: start.line)
            case "continue":
                advance()
                try expect(";", "'continue'")
                return Stmt(id: id(), kind: .continueStmt, range: range(from: start), line: start.line)
            case "case", "default":
                throw fail("'\(cur.text)' can only appear inside a switch")
            case "else":
                throw fail("'else' without a matching 'if'")
            case "goto":
                throw fail("'goto' is not supported")
            default: break
            }
        }
        let e = try expression()
        try expect(";", "the expression")
        return Stmt(id: id(), kind: .expr(e), range: range(from: start), line: start.line)
    }

    private mutating func switchStatement() throws -> Stmt {
        let start = advance()
        try expect("(", "after 'switch'")
        let value = try expression()
        try expect(")", "to close the switch value")
        let open = try expect("{", "to start the switch body")
        var cases: [SwitchCase] = []
        while !cur.isPunct("}") {
            if atEnd { throw fail("missing '}' to close the switch on line \(start.line)", at: open) }
            let caseTok = cur
            var label: Expr?
            if acceptKeyword("case") {
                label = try ternary()
                try expect(":", "after the case value")
            } else if acceptKeyword("default") {
                try expect(":", "after 'default'")
            } else {
                throw fail("expected 'case' or 'default' in the switch body")
            }
            var body: [Stmt] = []
            while !cur.isPunct("}") && !cur.isKeyword("case") && !cur.isKeyword("default") {
                if atEnd { break }
                body.append(try statement())
            }
            cases.append(SwitchCase(value: label, body: body, range: range(from: caseTok), line: caseTok.line))
        }
        advance()
        return Stmt(id: id(), kind: .switchCase(value, cases), range: range(from: start), line: start.line)
    }

    private mutating func declarationStatement() throws -> Stmt {
        let start = cur
        let spec = try specifiers()
        if spec.isStatic { throw fail("static local variables are not supported; use a global", at: start) }
        if spec.isTypedef {
            try typedefDeclarators(spec)
            return Stmt(id: id(), kind: .empty, range: range(from: start), line: start.line)
        }
        if spec.definedStruct != nil && cur.isPunct(";") {
            advance()
            return Stmt(id: id(), kind: .empty, range: range(from: start), line: start.line)
        }
        var decls: [VarDecl] = []
        repeat {
            let ds = cur
            let d = try declarator(spec.type)
            if case .function = d.type { throw fail("functions cannot be declared inside a function", at: ds) }
            var initializer: Initializer?
            if accept("=") { initializer = try initializerList() }
            decls.append(VarDecl(id: id(), name: d.name, type: d.type, range: range(from: ds), nameRange: d.nameRange,
                                 line: ds.line, initializer: initializer))
        } while accept(",")
        try expect(";", "the declaration")
        return Stmt(id: id(), kind: .decl(decls), range: range(from: start), line: start.line)
    }

    private mutating func initializerList() throws -> Initializer {
        if cur.isPunct("{") {
            let start = advance()
            var items: [Initializer] = []
            while !cur.isPunct("}") {
                items.append(try initializerList())
                if !accept(",") { break }
            }
            try expect("}", "to close the initialiser list")
            return .list(items, range(from: start))
        }
        return .expr(try assignment())
    }

    // MARK: Expressions

    private mutating func make(_ kind: ExprKind, _ start: Token) -> Expr {
        Expr(id: id(), kind: kind, range: range(from: start), line: start.line)
    }

    private mutating func expression() throws -> Expr {
        let start = cur
        var e = try assignment()
        while accept(",") {
            let r = try assignment()
            e = make(.comma(e, r), start)
        }
        return e
    }

    private static let assignOps: [String: BinaryOp?] = [
        "=": nil, "+=": .add, "-=": .sub, "*=": .mul, "/=": .div, "%=": .rem,
        "<<=": .shl, ">>=": .shr, "&=": .bitAnd, "|=": .bitOr, "^=": .bitXor,
    ]

    private mutating func assignment() throws -> Expr {
        let start = cur
        let lhs = try ternary()
        if cur.kind == .punct, let op = Parser.assignOps[cur.text] {
            advance()
            let rhs = try assignment()
            return make(.assign(op, lhs, rhs), start)
        }
        return lhs
    }

    private mutating func ternary() throws -> Expr {
        let start = cur
        let c = try binary(0)
        if accept("?") {
            let a = try expression()
            try expect(":", "in the ?: expression")
            let b = try ternary()
            return make(.ternary(c, a, b), start)
        }
        return c
    }

    private static let levels: [[String]] = [
        ["||"], ["&&"], ["|"], ["^"], ["&"], ["==", "!="], ["<", ">", "<=", ">="], ["<<", ">>"], ["+", "-"], ["*", "/", "%"],
    ]

    /// Precedence climbing: level 0 is `||`, the last level is `* / %`.
    private mutating func binary(_ level: Int) throws -> Expr {
        if level == Parser.levels.count { return try unary() }
        let start = cur
        var lhs = try binary(level + 1)
        while cur.kind == .punct, Parser.levels[level].contains(cur.text) {
            let op = BinaryOp(rawValue: advance().text)!
            let rhs = try binary(level + 1)
            lhs = make(.binary(op, lhs, rhs), start)
        }
        return lhs
    }

    private mutating func unary() throws -> Expr {
        let start = cur
        if cur.kind == .punct {
            switch cur.text {
            case "-", "+", "!", "~", "*", "&":
                let op = UnaryOp(rawValue: advance().text)!
                let e = try unary()
                return make(.unary(op, e), start)
            case "++", "--":
                let inc = advance().text == "++"
                let e = try unary()
                return make(.incDec(isPrefix: true, isIncrement: inc, e), start)
            case "(":
                if startsType(peek()) {
                    advance()
                    let spec = try specifiers()
                    let d = try declarator(spec.type, abstract: true)
                    try expect(")", "to close the cast")
                    let e = try unary()
                    return make(.cast(d.type, e), start)
                }
            default: break
            }
        }
        if acceptKeyword("sizeof") {
            if cur.isPunct("("), startsType(peek()) {
                advance()
                let spec = try specifiers()
                let d = try declarator(spec.type, abstract: true)
                try expect(")", "to close sizeof")
                if case let .structure(s) = d.type, !s.isComplete { throw fail("sizeof incomplete type struct \(s.name)", at: start) }
                return make(.sizeofType(d.type), start)
            }
            let e = try unary()
            return make(.sizeofExpr(e), start)
        }
        return try postfix()
    }

    private mutating func postfix() throws -> Expr {
        let start = cur
        var e = try primary()
        while true {
            if accept("[") {
                let i = try expression()
                try expect("]", "to close the index")
                e = make(.index(e, i), start)
            } else if cur.isPunct("(") {
                guard case let .ident(name) = e.kind else { throw fail("only named functions can be called (no function pointers)") }
                advance()
                var args: [Expr] = []
                if !cur.isPunct(")") {
                    repeat { args.append(try assignment()) } while accept(",")
                }
                try expect(")", "to close the argument list")
                e = make(.call(name, args), start)
            } else if accept(".") {
                let f = try expectIdentifier("a field name")
                e = make(.member(e, f.text, arrow: false), start)
            } else if accept("->") {
                let f = try expectIdentifier("a field name")
                e = make(.member(e, f.text, arrow: true), start)
            } else if cur.isPunct("++") || cur.isPunct("--") {
                let inc = advance().text == "++"
                e = make(.incDec(isPrefix: false, isIncrement: inc, e), start)
            } else {
                return e
            }
        }
    }

    private mutating func primary() throws -> Expr {
        let t = cur
        switch t.kind {
        case .number:
            advance()
            let e = make(.number(t.value), t)
            e.type = t.isUnsigned ? .uint : .int
            return e
        case .character:
            advance()
            return make(.number(t.value), t)
        case .string:
            advance()
            var bytes = t.bytes
            // Adjacent string literals are joined: "ab" "cd" == "abcd".
            while cur.kind == .string { bytes += advance().bytes }
            return make(.string(bytes), t)
        case .identifier:
            advance()
            return make(.ident(t.text), t)
        case .punct where t.text == "(":
            advance()
            let e = try expression()
            try expect(")", "to close the parenthesis")
            e.range = range(from: t)
            return e
        case .eof:
            throw fail("the file ended in the middle of an expression")
        default:
            if t.kind == .keyword { throw fail("expected an expression, found the keyword '\(t.text)'") }
            throw fail("expected an expression, found '\(t.text)'")
        }
    }
}
