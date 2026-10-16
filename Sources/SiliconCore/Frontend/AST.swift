import Foundation

// MARK: - Types

/// Layout of a `struct`, filled in once its closing brace is seen.
public final class StructDef: @unchecked Sendable {
    public struct Field: Sendable {
        public var name: String
        public var type: CType
        public var offset: Int
    }
    public let name: String
    public var fields: [Field] = []
    public var size = 0
    public var align = 1
    public var isComplete = false
    public let id: Int

    init(name: String, id: Int) {
        self.name = name
        self.id = id
    }

    public func field(_ name: String) -> Field? { fields.first { $0.name == name } }
}

/// C types on our 32-bit RISC-V target (ILP32): char is 1 byte and signed,
/// int, unsigned and pointers are 4 bytes.
public indirect enum CType: Equatable, Sendable, CustomStringConvertible {
    case void
    case char
    case int
    case uint
    case pointer(CType)
    case array(CType, Int)
    case structure(StructDef)
    case function(CType, [CType])

    public static func == (a: CType, b: CType) -> Bool {
        switch (a, b) {
        case (.void, .void), (.char, .char), (.int, .int), (.uint, .uint): return true
        case let (.pointer(x), .pointer(y)): return x == y
        case let (.array(x, n), .array(y, m)): return x == y && n == m
        case let (.structure(x), .structure(y)): return x === y
        case let (.function(r1, p1), .function(r2, p2)): return r1 == r2 && p1 == p2
        default: return false
        }
    }

    public var size: Int {
        switch self {
        case .void: return 1
        case .char: return 1
        case .int, .uint, .pointer: return 4
        case let .array(e, n): return e.size * n
        case let .structure(s): return s.size
        case .function: return 4
        }
    }

    public var align: Int {
        switch self {
        case .char, .void: return 1
        case let .array(e, _): return e.align
        case let .structure(s): return s.align
        default: return 4
        }
    }

    public var isInteger: Bool {
        switch self {
        case .char, .int, .uint: return true
        default: return false
        }
    }
    public var isPointer: Bool { if case .pointer = self { return true }; return false }
    public var isArray: Bool { if case .array = self { return true }; return false }
    public var isStruct: Bool { if case .structure = self { return true }; return false }
    public var isVoid: Bool { self == .void }
    public var isScalar: Bool { isInteger || isPointer }
    public var isUnsigned: Bool { self == .uint || isPointer }

    /// Pointed-to type of a pointer, or element type of an array.
    public var pointee: CType? {
        switch self {
        case let .pointer(t): return t
        case let .array(t, _): return t
        default: return nil
        }
    }

    /// Arrays turn into pointers to their first element when used as values.
    public var decayed: CType {
        if case let .array(e, _) = self { return .pointer(e) }
        if case .function = self { return .pointer(self) }
        return self
    }

    public var description: String { spelled("") }

    private func join(_ base: String, _ name: String) -> String {
        name.isEmpty ? base : name.hasPrefix("[") ? base + name : base + " " + name
    }

    /// C-style spelling with an optional declarator name: `int *p`, `char buf[8]`.
    public func spelled(_ name: String) -> String {
        switch self {
        case .void: return join("void", name)
        case .char: return join("char", name)
        case .int: return join("int", name)
        case .uint: return join("unsigned", name)
        case let .structure(s): return join("struct \(s.name)", name)
        case let .pointer(t):
            if case .array = t { return t.spelled("(*\(name))") }
            if case .function = t { return t.spelled("(*\(name))") }
            return t.spelled("*" + name)
        case let .array(t, n): return t.spelled("\(name)[\(n)]")
        case let .function(r, ps):
            return r.spelled("\(name)(\(ps.map { $0.description }.joined(separator: ", ")))")
        }
    }
}

// MARK: - Symbols

public final class Symbol: @unchecked Sendable {
    public enum Kind: String, Sendable { case global, local, param, function, library }
    public let id: Int
    public let name: String
    public var kind: Kind
    public var type: CType
    public let declRange: SourceRange
    public let line: Int
    /// Enclosing function, for locals and parameters.
    public var function: String?
    /// Block depth: 0 = file scope, 1 = function parameters, 2+ = nested blocks.
    public var depth: Int
    /// Set when `&x` is taken somewhere. Such a variable must live in memory.
    public var addressTaken = false
    /// Name used in assembly and IR. Unique per function (shadowed names get a suffix).
    public var uniqueName: String
    /// Functions: defined in this file (vs. only declared).
    public var isDefined = false
    /// Where the back end put it: "s0-20", "s3", "0x10010008"... Filled in after code generation.
    public var location: String = ""

    init(id: Int, name: String, kind: Kind, type: CType, range: SourceRange, line: Int, depth: Int) {
        self.id = id
        self.name = name
        self.kind = kind
        self.type = type
        declRange = range
        self.line = line
        self.depth = depth
        uniqueName = name
    }
}

// MARK: - Expressions

public enum BinaryOp: String, Sendable, CaseIterable {
    case add = "+", sub = "-", mul = "*", div = "/", rem = "%"
    case shl = "<<", shr = ">>", bitAnd = "&", bitOr = "|", bitXor = "^"
    case lt = "<", gt = ">", le = "<=", ge = ">=", eq = "==", ne = "!="
    case logAnd = "&&", logOr = "||"

    public var isComparison: Bool { [.lt, .gt, .le, .ge, .eq, .ne].contains(self) }
}

public enum UnaryOp: String, Sendable {
    case neg = "-", plus = "+", not = "!", bitNot = "~", deref = "*", addr = "&"
}

public indirect enum ExprKind {
    case number(Int64)
    case string([UInt8])
    case ident(String)
    case binary(BinaryOp, Expr, Expr)
    case unary(UnaryOp, Expr)
    /// `a = b` (op nil) or `a op= b`.
    case assign(BinaryOp?, Expr, Expr)
    /// `++x`, `x--` …
    case incDec(isPrefix: Bool, isIncrement: Bool, Expr)
    case call(String, [Expr])
    case index(Expr, Expr)
    case member(Expr, String, arrow: Bool)
    case ternary(Expr, Expr, Expr)
    case sizeofType(CType)
    case sizeofExpr(Expr)
    case cast(CType, Expr)
    case comma(Expr, Expr)
}

public final class Expr: @unchecked Sendable {
    public let id: Int
    public var kind: ExprKind
    public var range: SourceRange
    public let line: Int
    // Filled in by semantic analysis.
    public var type: CType = .int
    public var symbol: Symbol?
    public var isLValue = false
    /// For arithmetic and comparisons: the type the operation is done in (int, unsigned or pointer).
    public var opType: CType = .int
    /// For a string literal: its label in .data, e.g. ".LC0".
    public var label: String = ""
    /// For a struct member: the byte offset of the field.
    public var fieldOffset = 0

    init(id: Int, kind: ExprKind, range: SourceRange, line: Int) {
        self.id = id
        self.kind = kind
        self.range = range
        self.line = line
    }

    public var children: [Expr] {
        switch kind {
        case .number, .string, .ident, .sizeofType: return []
        case let .binary(_, a, b), let .assign(_, a, b), let .index(a, b), let .comma(a, b): return [a, b]
        case let .unary(_, a), let .incDec(_, _, a), let .member(a, _, _), let .sizeofExpr(a), let .cast(_, a): return [a]
        case let .call(_, args): return args
        case let .ternary(c, a, b): return [c, a, b]
        }
    }
}

// MARK: - Statements and declarations

public enum Initializer {
    case expr(Expr)
    case list([Initializer], SourceRange)

    public var range: SourceRange {
        switch self {
        case let .expr(e): return e.range
        case let .list(_, r): return r
        }
    }
}

public final class VarDecl: @unchecked Sendable {
    public let id: Int
    public let name: String
    public var type: CType
    public let range: SourceRange
    public let nameRange: SourceRange
    public let line: Int
    public var initializer: Initializer?
    public var symbol: Symbol?

    init(id: Int, name: String, type: CType, range: SourceRange, nameRange: SourceRange, line: Int, initializer: Initializer?) {
        self.id = id
        self.name = name
        self.type = type
        self.range = range
        self.nameRange = nameRange
        self.line = line
        self.initializer = initializer
    }
}

public final class SwitchCase: @unchecked Sendable {
    /// nil for `default:`.
    public var value: Expr?
    public var constant: Int32 = 0
    public var body: [Stmt]
    public let range: SourceRange
    public let line: Int
    init(value: Expr?, body: [Stmt], range: SourceRange, line: Int) {
        self.value = value
        self.body = body
        self.range = range
        self.line = line
    }
}

public indirect enum StmtKind {
    case block([Stmt])
    case decl([VarDecl])
    case expr(Expr)
    case ifElse(Expr, Stmt, Stmt?)
    case whileLoop(Expr, Stmt)
    case doWhile(Stmt, Expr)
    case forLoop(Stmt?, Expr?, Expr?, Stmt)
    case switchCase(Expr, [SwitchCase])
    case returnStmt(Expr?)
    case breakStmt
    case continueStmt
    case empty
}

public final class Stmt: @unchecked Sendable {
    public let id: Int
    public var kind: StmtKind
    public let range: SourceRange
    public let line: Int
    init(id: Int, kind: StmtKind, range: SourceRange, line: Int) {
        self.id = id
        self.kind = kind
        self.range = range
        self.line = line
    }
}

public final class FunctionDecl: @unchecked Sendable {
    public let id: Int
    public let name: String
    public var returnType: CType
    public var params: [VarDecl]
    public var body: Stmt?
    public let range: SourceRange
    public let nameRange: SourceRange
    public let line: Int
    public var symbol: Symbol?
    /// Every local and parameter, in declaration order (filled by semantic analysis).
    public var locals: [Symbol] = []
    public var hasCalls = false

    init(id: Int, name: String, returnType: CType, params: [VarDecl], body: Stmt?, range: SourceRange, nameRange: SourceRange, line: Int) {
        self.id = id
        self.name = name
        self.returnType = returnType
        self.params = params
        self.body = body
        self.range = range
        self.nameRange = nameRange
        self.line = line
    }
}

public enum TopLevel {
    case function(FunctionDecl)
    case globals([VarDecl])
    case structDef(StructDef, SourceRange, Int)
}

public final class Program: @unchecked Sendable {
    public var items: [TopLevel] = []
    public var structs: [StructDef] = []
    public var nodeCount = 0

    public var functions: [FunctionDecl] {
        items.compactMap { if case let .function(f) = $0, f.body != nil { return f }; return nil }
    }
    public var globals: [VarDecl] {
        items.flatMap { item -> [VarDecl] in if case let .globals(g) = item { return g }; return [] }
    }
}
