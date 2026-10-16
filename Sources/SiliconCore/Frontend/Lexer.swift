import Foundation

public enum TokenKind: String, Sendable {
    case keyword, identifier, number, character, string, punct, eof
}

public struct Token: Sendable, CustomStringConvertible {
    public var kind: TokenKind
    public var text: String
    public var range: SourceRange
    public var line: Int
    public var column: Int
    /// Value of a number or character literal.
    public var value: Int64 = 0
    /// A number literal with a `u` suffix, or one too large for `int`.
    public var isUnsigned = false
    /// Bytes of a string literal, without the terminating zero.
    public var bytes: [UInt8] = []
    /// Set when this token came from expanding a `#define`.
    public var fromMacro: String?

    public var description: String { "\(kind.rawValue) '\(text)'" }
    public func isPunct(_ p: String) -> Bool { kind == .punct && text == p }
    public func isKeyword(_ k: String) -> Bool { kind == .keyword && text == k }
}

/// A comment or preprocessor line. Kept only for syntax colouring and the token view.
public struct Trivia: Sendable {
    public enum Kind: Sendable { case comment, directive }
    public var kind: Kind
    public var range: SourceRange
    public var text: String
}

public struct Macro: Sendable {
    public var name: String
    public var body: [Token]
    public var line: Int
}

public struct LexResult: Sendable {
    public var tokens: [Token]
    public var trivia: [Trivia]
    public var macros: [String: Macro]
    public var diagnostics: [Diagnostic]
    public var hasErrors: Bool { diagnostics.contains { $0.severity == .error } }
}

public let cKeywords: Set<String> = [
    "int", "char", "void", "unsigned", "signed", "long", "short", "const", "static", "extern", "volatile",
    "if", "else", "while", "for", "do", "return", "break", "continue", "sizeof", "struct", "typedef",
    "switch", "case", "default", "goto", "float", "double", "enum", "union", "register", "auto",
]

/// Turns C source text into tokens. Also acts as a tiny preprocessor:
/// `#define NAME text` macros are expanded as the identifiers are read,
/// and `#include` lines are noted and skipped (the library is built in).
public struct Lexer {
    private let src: [UInt16]
    private let lines: LineTable
    private var i = 0
    private var tokens: [Token] = []
    private var trivia: [Trivia] = []
    private var macros: [String: Macro] = [:]
    private var diags: [Diagnostic] = []
    private var atLineStart = true

    public static func lex(_ text: String) -> LexResult {
        var l = Lexer(text)
        l.run()
        return LexResult(tokens: l.tokens, trivia: l.trivia, macros: l.macros, diagnostics: l.diags)
    }

    init(_ text: String) {
        src = Array(text.utf16)
        lines = LineTable(text)
    }

    private func peek(_ k: Int = 0) -> UInt16 { i + k < src.count ? src[i + k] : 0 }
    private func ch(_ c: Character) -> UInt16 { UInt16(c.asciiValue!) }

    private func string(_ a: Int, _ b: Int) -> String { String(utf16CodeUnits: Array(src[a..<b]), count: b - a) }

    private mutating func error(_ msg: String, _ a: Int, _ b: Int) {
        diags.append(Diagnostic(.error, .lexer, msg, range: SourceRange(a, b), line: lines.line(of: a)))
    }

    private static func isIdentStart(_ c: UInt16) -> Bool {
        (c >= 65 && c <= 90) || (c >= 97 && c <= 122) || c == 95
    }
    private static func isDigit(_ c: UInt16) -> Bool { c >= 48 && c <= 57 }
    private static func isIdent(_ c: UInt16) -> Bool { isIdentStart(c) || isDigit(c) }
    private static func isSpace(_ c: UInt16) -> Bool { c == 32 || c == 9 || c == 13 || c == 11 || c == 12 }

    private mutating func run() {
        while i < src.count {
            let c = peek()
            if c == 10 {
                i += 1
                atLineStart = true
                continue
            }
            if Lexer.isSpace(c) {
                i += 1
                continue
            }
            if c == ch("/") && peek(1) == ch("/") {
                let s = i
                while i < src.count && peek() != 10 { i += 1 }
                trivia.append(Trivia(kind: .comment, range: SourceRange(s, i), text: string(s, i)))
                continue
            }
            if c == ch("/") && peek(1) == ch("*") {
                let s = i
                i += 2
                while i < src.count && !(peek() == ch("*") && peek(1) == ch("/")) { i += 1 }
                if i >= src.count {
                    error("this /* comment is never closed with */", s, s + 2)
                } else {
                    i += 2
                }
                trivia.append(Trivia(kind: .comment, range: SourceRange(s, i), text: string(s, i)))
                continue
            }
            if c == ch("#") && atLineStart {
                directive()
                continue
            }
            atLineStart = false
            if let t = token() {
                emit(t, depth: 0, expanding: [])
            }
        }
        let end = src.count
        tokens.append(Token(kind: .eof, text: "end of file", range: SourceRange(end, end), line: lines.line(of: end), column: 1))
    }

    /// Adds a token, expanding it first if it names a macro.
    private mutating func emit(_ t: Token, depth: Int, expanding: Set<String>) {
        if t.kind == .identifier, let m = macros[t.text], !expanding.contains(t.text) {
            if depth > 30 {
                error("macro expansion is nested too deeply", t.range.start, t.range.end)
                return
            }
            for var b in m.body {
                b.range = t.range
                b.line = t.line
                b.column = t.column
                b.fromMacro = b.fromMacro ?? t.text
                emit(b, depth: depth + 1, expanding: expanding.union([t.text]))
            }
            return
        }
        tokens.append(t)
    }

    // MARK: Tokens

    private static let puncts3 = ["<<=", ">>=", "..."]
    private static let puncts2 = ["->", "++", "--", "<<", ">>", "<=", ">=", "==", "!=", "&&", "||",
                                  "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^="]
    private static let puncts1 = Set("+-*/%&|^~!<>=?:;,.()[]{}".map { String($0) })

    private mutating func token() -> Token? {
        let s = i
        let c = peek()
        func make(_ kind: TokenKind) -> Token {
            Token(kind: kind, text: string(s, i), range: SourceRange(s, i), line: lines.line(of: s), column: lines.column(of: s))
        }
        if Lexer.isIdentStart(c) {
            while Lexer.isIdent(peek()) { i += 1 }
            var t = make(.identifier)
            if cKeywords.contains(t.text) { t.kind = .keyword }
            return t
        }
        if Lexer.isDigit(c) {
            return number(s)
        }
        if c == ch("'") {
            return charLiteral(s)
        }
        if c == ch("\"") {
            return stringLiteral(s)
        }
        for p in Lexer.puncts3 where matches(p) {
            i += 3
            return make(.punct)
        }
        for p in Lexer.puncts2 where matches(p) {
            i += 2
            return make(.punct)
        }
        let one = string(i, i + 1)
        if Lexer.puncts1.contains(one) {
            i += 1
            return make(.punct)
        }
        i += 1
        error("unexpected character '\(one)'", s, i)
        return nil
    }

    private func matches(_ p: String) -> Bool {
        var k = 0
        for u in p.utf16 {
            if peek(k) != u { return false }
            k += 1
        }
        return true
    }

    private mutating func number(_ s: Int) -> Token {
        var value: UInt64 = 0
        var overflow = false
        func add(_ d: UInt64, base: UInt64) {
            value = value &* base &+ d
            if value > 0xFFFF_FFFF { overflow = true }
        }
        if peek() == ch("0") && (peek(1) == ch("x") || peek(1) == ch("X")) {
            i += 2
            let digitsStart = i
            while true {
                let c = peek()
                if Lexer.isDigit(c) { add(UInt64(c - 48), base: 16) }
                else if c >= 97 && c <= 102 { add(UInt64(c - 87), base: 16) }
                else if c >= 65 && c <= 70 { add(UInt64(c - 55), base: 16) }
                else { break }
                i += 1
            }
            if i == digitsStart { error("a hex number needs digits after 0x", s, i) }
        } else if peek() == ch("0") && Lexer.isDigit(peek(1)) {
            i += 1
            while Lexer.isDigit(peek()) {
                let d = UInt64(peek() - 48)
                if d >= 8 { error("'\(d)' is not an octal digit (numbers starting with 0 are octal)", i, i + 1) }
                add(d, base: 8)
                i += 1
            }
        } else {
            while Lexer.isDigit(peek()) {
                add(UInt64(peek() - 48), base: 10)
                i += 1
            }
        }
        var unsignedSuffix = false
        while true {
            let c = peek()
            if c == ch("u") || c == ch("U") { unsignedSuffix = true; i += 1 }
            else if c == ch("l") || c == ch("L") { i += 1 }
            else { break }
        }
        if Lexer.isIdent(peek()) {
            while Lexer.isIdent(peek()) { i += 1 }
            if peek() == ch(".") || string(s, i).contains("e") {
                error("floating-point numbers are not supported", s, i)
            } else {
                error("invalid number '\(string(s, i))'", s, i)
            }
        } else if peek() == ch(".") && Lexer.isDigit(peek(1)) {
            i += 1
            while Lexer.isIdent(peek()) { i += 1 }
            error("floating-point numbers are not supported", s, i)
        }
        if overflow { error("this number does not fit in 32 bits", s, i) }
        var t = Token(kind: .number, text: string(s, i), range: SourceRange(s, i), line: lines.line(of: s), column: lines.column(of: s))
        t.value = Int64(value & 0xFFFF_FFFF)
        // On a 32-bit target, a literal above INT_MAX has type unsigned int
        // (for decimal it would really be long long, which we don't have).
        t.isUnsigned = unsignedSuffix || value > 0x7FFF_FFFF
        return t
    }

    /// Reads one (possibly escaped) character inside a quote. Returns nil at the end of the line.
    private mutating func escapedChar(quote: UInt16) -> UInt8? {
        let c = peek()
        if c == 0 || c == 10 { return nil }
        if c != ch("\\") {
            i += 1
            if c < 128 { return UInt8(c) }
            return UInt8(ascii: "?")
        }
        let s = i
        i += 1
        let e = peek()
        if e == 10 || e == 0 {
            error("a backslash at the end of the line: strings cannot continue onto the next line (write \\n for a newline)", s, i)
            return nil
        }
        i += 1
        switch e {
        case ch("n"): return 10
        case ch("t"): return 9
        case ch("r"): return 13
        case ch("0") where !Lexer.isDigit(peek()): return 0
        case ch("\\"): return 92
        case ch("'"): return 39
        case ch("\""): return 34
        case ch("a"): return 7
        case ch("b"): return 8
        case ch("f"): return 12
        case ch("v"): return 11
        case ch("?"): return 63
        case ch("x"):
            var v = 0, n = 0
            while n < 2 {
                let h = peek()
                let d: Int
                if Lexer.isDigit(h) { d = Int(h - 48) }
                else if h >= 97 && h <= 102 { d = Int(h - 87) }
                else if h >= 65 && h <= 70 { d = Int(h - 55) }
                else { break }
                v = v * 16 + d
                n += 1
                i += 1
            }
            if n == 0 { error("\\x needs hex digits", s, i) }
            return UInt8(v & 0xFF)
        default:
            if e >= 48 && e <= 55 {
                var v = Int(e - 48), n = 1
                while n < 3, peek() >= 48, peek() <= 55 {
                    v = v * 8 + Int(peek() - 48)
                    i += 1
                    n += 1
                }
                return UInt8(v & 0xFF)
            }
            error("unknown escape sequence '\\\(string(s + 1, i))'", s, i)
            return UInt8(e & 0x7F)
        }
    }

    private mutating func charLiteral(_ s: Int) -> Token {
        i += 1
        var value: UInt8 = 0
        if peek() == ch("'") {
            error("empty character literal ''", s, i + 1)
            i += 1
        } else if let v = escapedChar(quote: ch("'")) {
            value = v
            if peek() == ch("'") {
                i += 1
            } else {
                while i < src.count && peek() != ch("'") && peek() != 10 { i += 1 }
                if peek() == ch("'") { i += 1 }
                error("a character literal holds exactly one character (use \"...\" for strings)", s, i)
            }
        } else {
            error("this character literal is missing its closing '", s, i)
        }
        var t = Token(kind: .character, text: string(s, i), range: SourceRange(s, i), line: lines.line(of: s), column: lines.column(of: s))
        t.value = Int64(Int8(bitPattern: value))
        return t
    }

    private mutating func stringLiteral(_ s: Int) -> Token {
        i += 1
        var bytes: [UInt8] = []
        var closed = false
        while true {
            if peek() == ch("\"") {
                i += 1
                closed = true
                break
            }
            // Non-ASCII characters are stored as UTF-8.
            if peek() >= 128 && peek() != 0 {
                let start = i
                i += 1
                if (src[start] & 0xFC00) == 0xD800, i < src.count { i += 1 }
                bytes += Array(string(start, i).utf8)
                continue
            }
            guard let b = escapedChar(quote: ch("\"")) else { break }
            bytes.append(b)
        }
        if !closed { error("this string is missing its closing \"", s, i) }
        var t = Token(kind: .string, text: string(s, i), range: SourceRange(s, i), line: lines.line(of: s), column: lines.column(of: s))
        t.bytes = bytes
        return t
    }

    // MARK: Preprocessor

    private mutating func directive() {
        let s = i
        // The directive runs to the end of the line; a backslash continues it.
        var e = i
        while e < src.count {
            if src[e] == 10 {
                if e > s, src[e - 1] == ch("\\") { e += 1; continue }
                break
            }
            e += 1
        }
        let text = string(s, e)
        trivia.append(Trivia(kind: .directive, range: SourceRange(s, e), text: text))
        i += 1
        while i < e && Lexer.isSpace(peek()) { i += 1 }
        let ns = i
        while i < e && Lexer.isIdent(peek()) { i += 1 }
        let name = string(ns, i)
        let line = lines.line(of: s)
        switch name {
        case "include":
            let rest = string(i, e).trimmingCharacters(in: .whitespaces)
            diags.append(Diagnostic(.note, .preprocessor,
                                    "#include \(rest) is skipped: printf, putchar, malloc and friends are built in",
                                    range: SourceRange(s, e), line: line))
        case "define":
            while i < e && Lexer.isSpace(peek()) { i += 1 }
            let ms = i
            while i < e && Lexer.isIdent(peek()) { i += 1 }
            let mname = string(ms, i)
            if mname.isEmpty || !Lexer.isIdentStart(src[ms]) {
                error("#define needs a name", s, e)
                break
            }
            if peek() == ch("(") {
                error("function-like macros (#define \(mname)(...)) are not supported; write a function instead", s, e)
                break
            }
            // Lex the replacement text with a sub-lexer limited to this line.
            var body: [Token] = []
            let saveTokens = tokens
            tokens = []
            while i < e {
                let c = peek()
                if Lexer.isSpace(c) || c == 10 || c == ch("\\") { i += 1; continue }
                if c == ch("/") && peek(1) == ch("/") { break }
                if let t = token() { emit(t, depth: 0, expanding: [mname]) }
            }
            body = tokens
            tokens = saveTokens
            macros[mname] = Macro(name: mname, body: body, line: line)
        case "undef":
            while i < e && Lexer.isSpace(peek()) { i += 1 }
            let ms = i
            while i < e && Lexer.isIdent(peek()) { i += 1 }
            macros[string(ms, i)] = nil
        case "pragma", "":
            break
        default:
            error("#\(name) is not supported (only #define and #include)", s, e)
        }
        i = e
        atLineStart = true
    }
}
