#include "Frontend.h"

namespace sp {

const char* tokenKindName(TokenKind k) {
    switch (k) {
    case TokenKind::Keyword: return "keyword";
    case TokenKind::Identifier: return "identifier";
    case TokenKind::Number: return "number";
    case TokenKind::Character: return "character";
    case TokenKind::String: return "string";
    case TokenKind::Punct: return "punct";
    case TokenKind::Eof: return "eof";
    }
    return "?";
}

bool LexResult::hasErrors() const {
    for (auto& d : diagnostics)
        if (d.severity == Diagnostic::Severity::Error) return true;
    return false;
}

const std::set<std::string> cKeywords = {
    "int", "char", "void", "unsigned", "signed", "long", "short", "const", "static", "extern", "volatile",
    "if", "else", "while", "for", "do", "return", "break", "continue", "sizeof", "struct", "typedef",
    "switch", "case", "default", "goto", "float", "double", "enum", "union", "register", "auto",
};

namespace {

class Lexer {
public:
    explicit Lexer(const std::string& text) : src(toUTF16(text)), lines(src) {}

    LexResult run() {
        while (i < (int)src.size()) {
            char16_t c = peek();
            if (c == 10) {
                i += 1;
                atLineStart = true;
                continue;
            }
            if (isSpace(c)) {
                i += 1;
                continue;
            }
            if (c == u'/' && peek(1) == u'/') {
                int s = i;
                while (i < (int)src.size() && peek() != 10) i += 1;
                trivia.push_back({Trivia::Kind::Comment, SourceRange(s, i), string(s, i)});
                continue;
            }
            if (c == u'/' && peek(1) == u'*') {
                int s = i;
                i += 2;
                while (i < (int)src.size() && !(peek() == u'*' && peek(1) == u'/')) i += 1;
                if (i >= (int)src.size()) {
                    error("this /* comment is never closed with */", s, s + 2);
                } else {
                    i += 2;
                }
                trivia.push_back({Trivia::Kind::Comment, SourceRange(s, i), string(s, i)});
                continue;
            }
            if (c == u'#' && atLineStart) {
                directive();
                continue;
            }
            atLineStart = false;
            if (auto t = token()) emit(*t, 0, {});
        }
        int end = (int)src.size();
        Token eof;
        eof.kind = TokenKind::Eof;
        eof.text = "end of file";
        eof.range = SourceRange(end, end);
        eof.line = lines.line(end);
        eof.column = 1;
        tokens.push_back(eof);
        LexResult r;
        r.tokens = std::move(tokens);
        r.trivia = std::move(trivia);
        r.macros = std::move(macros);
        r.diagnostics = std::move(diags);
        return r;
    }

private:
    std::u16string src;
    LineTable lines;
    int i = 0;
    std::vector<Token> tokens;
    std::vector<Trivia> trivia;
    std::map<std::string, Macro> macros;
    std::vector<Diagnostic> diags;
    bool atLineStart = true;

    char16_t peek(int k = 0) const { return i + k < (int)src.size() ? src[i + k] : 0; }
    std::string string(int a, int b) const { return toUTF8(src.data() + a, b - a); }

    void error(const std::string& msg, int a, int b) {
        diags.emplace_back(Diagnostic::Severity::Error, Stage::Lexer, msg, SourceRange(a, b), lines.line(a));
    }

    static bool isIdentStart(char16_t c) { return (c >= 65 && c <= 90) || (c >= 97 && c <= 122) || c == 95; }
    static bool isDigit(char16_t c) { return c >= 48 && c <= 57; }
    static bool isIdent(char16_t c) { return isIdentStart(c) || isDigit(c); }
    static bool isSpace(char16_t c) { return c == 32 || c == 9 || c == 13 || c == 11 || c == 12; }

    /// Adds a token, expanding it first if it names a macro.
    void emit(const Token& t, int depth, const std::set<std::string>& expanding) {
        if (t.kind == TokenKind::Identifier) {
            auto m = macros.find(t.text);
            if (m != macros.end() && !expanding.count(t.text)) {
                if (depth > 30) {
                    error("macro expansion is nested too deeply", t.range.start, t.range.end);
                    return;
                }
                std::set<std::string> inner = expanding;
                inner.insert(t.text);
                std::vector<Token> body = m->second.body;   // copy: expansion may redefine nothing, but stay safe
                for (Token b : body) {
                    b.range = t.range;
                    b.line = t.line;
                    b.column = t.column;
                    if (!b.fromMacro) b.fromMacro = t.text;
                    emit(b, depth + 1, inner);
                }
                return;
            }
        }
        tokens.push_back(t);
    }

    Token make(TokenKind kind, int s) const {
        Token t;
        t.kind = kind;
        t.text = string(s, i);
        t.range = SourceRange(s, i);
        t.line = lines.line(s);
        t.column = lines.column(s);
        return t;
    }

    bool matches(const char* p) const {
        int k = 0;
        for (const char* q = p; *q; q++, k++)
            if (peek(k) != (char16_t)*q) return false;
        return true;
    }

    std::optional<Token> token() {
        static const char* puncts3[] = {"<<=", ">>=", "..."};
        static const char* puncts2[] = {"->", "++", "--", "<<", ">>", "<=", ">=", "==", "!=", "&&", "||",
                                        "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^="};
        static const std::u16string puncts1 = u"+-*/%&|^~!<>=?:;,.()[]{}";
        int s = i;
        char16_t c = peek();
        if (isIdentStart(c)) {
            while (isIdent(peek())) i += 1;
            Token t = make(TokenKind::Identifier, s);
            if (cKeywords.count(t.text)) t.kind = TokenKind::Keyword;
            return t;
        }
        if (isDigit(c)) return number(s);
        if (c == u'\'') return charLiteral(s);
        if (c == u'"') return stringLiteral(s);
        for (const char* p : puncts3)
            if (matches(p)) {
                i += 3;
                return make(TokenKind::Punct, s);
            }
        for (const char* p : puncts2)
            if (matches(p)) {
                i += 2;
                return make(TokenKind::Punct, s);
            }
        if (puncts1.find(c) != std::u16string::npos) {
            i += 1;
            return make(TokenKind::Punct, s);
        }
        std::string one = string(i, i + 1);
        i += 1;
        error(cat("unexpected character '", one, "'"), s, i);
        return std::nullopt;
    }

    Token number(int s) {
        uint64_t value = 0;
        bool overflow = false;
        auto add = [&](uint64_t d, uint64_t base) {
            value = value * base + d;
            if (value > 0xFFFFFFFFull) overflow = true;
        };
        if (peek() == u'0' && (peek(1) == u'x' || peek(1) == u'X')) {
            i += 2;
            int digitsStart = i;
            while (true) {
                char16_t c = peek();
                if (isDigit(c)) add(c - 48, 16);
                else if (c >= 97 && c <= 102) add(c - 87, 16);
                else if (c >= 65 && c <= 70) add(c - 55, 16);
                else break;
                i += 1;
            }
            if (i == digitsStart) error("a hex number needs digits after 0x", s, i);
        } else if (peek() == u'0' && isDigit(peek(1))) {
            i += 1;
            while (isDigit(peek())) {
                uint64_t d = peek() - 48;
                if (d >= 8) error(cat("'", (int)d, "' is not an octal digit (numbers starting with 0 are octal)"), i, i + 1);
                add(d, 8);
                i += 1;
            }
        } else {
            while (isDigit(peek())) {
                add(peek() - 48, 10);
                i += 1;
            }
        }
        bool unsignedSuffix = false;
        while (true) {
            char16_t c = peek();
            if (c == u'u' || c == u'U') {
                unsignedSuffix = true;
                i += 1;
            } else if (c == u'l' || c == u'L') {
                i += 1;
            } else {
                break;
            }
        }
        if (isIdent(peek())) {
            while (isIdent(peek())) i += 1;
            if (peek() == u'.' || contains(string(s, i), "e")) {
                error("floating-point numbers are not supported", s, i);
            } else {
                error(cat("invalid number '", string(s, i), "'"), s, i);
            }
        } else if (peek() == u'.' && isDigit(peek(1))) {
            i += 1;
            while (isIdent(peek())) i += 1;
            error("floating-point numbers are not supported", s, i);
        }
        if (overflow) error("this number does not fit in 32 bits", s, i);
        Token t = make(TokenKind::Number, s);
        t.value = (int64_t)(value & 0xFFFFFFFFull);
        // On a 32-bit target, a literal above INT_MAX has type unsigned int
        // (for decimal it would really be long long, which we don't have).
        t.isUnsigned = unsignedSuffix || value > 0x7FFFFFFF;
        return t;
    }

    /// Reads one (possibly escaped) character inside a quote. Returns nullopt at the end of the line.
    std::optional<uint8_t> escapedChar() {
        char16_t c = peek();
        if (c == 0 || c == 10) return std::nullopt;
        if (c != u'\\') {
            i += 1;
            if (c < 128) return (uint8_t)c;
            return (uint8_t)'?';
        }
        int s = i;
        i += 1;
        char16_t e = peek();
        if (e == 10 || e == 0) {
            error("a backslash at the end of the line: strings cannot continue onto the next line (write \\n for a newline)", s, i);
            return std::nullopt;
        }
        i += 1;
        switch (e) {
        case u'n': return 10;
        case u't': return 9;
        case u'r': return 13;
        case u'\\': return 92;
        case u'\'': return 39;
        case u'"': return 34;
        case u'a': return 7;
        case u'b': return 8;
        case u'f': return 12;
        case u'v': return 11;
        case u'?': return 63;
        case u'x': {
            int v = 0, n = 0;
            while (n < 2) {
                char16_t h = peek();
                int d;
                if (isDigit(h)) d = h - 48;
                else if (h >= 97 && h <= 102) d = h - 87;
                else if (h >= 65 && h <= 70) d = h - 55;
                else break;
                v = v * 16 + d;
                n += 1;
                i += 1;
            }
            if (n == 0) error("\\x needs hex digits", s, i);
            return (uint8_t)(v & 0xFF);
        }
        default:
            if (e == u'0' && !isDigit(peek())) return 0;
            if (e >= 48 && e <= 55) {
                int v = e - 48, n = 1;
                while (n < 3 && peek() >= 48 && peek() <= 55) {
                    v = v * 8 + (peek() - 48);
                    i += 1;
                    n += 1;
                }
                return (uint8_t)(v & 0xFF);
            }
            error(cat("unknown escape sequence '\\", string(s + 1, i), "'"), s, i);
            return (uint8_t)(e & 0x7F);
        }
    }

    Token charLiteral(int s) {
        i += 1;
        uint8_t value = 0;
        if (peek() == u'\'') {
            error("empty character literal ''", s, i + 1);
            i += 1;
        } else if (auto v = escapedChar()) {
            value = *v;
            if (peek() == u'\'') {
                i += 1;
            } else {
                while (i < (int)src.size() && peek() != u'\'' && peek() != 10) i += 1;
                if (peek() == u'\'') i += 1;
                error("a character literal holds exactly one character (use \"...\" for strings)", s, i);
            }
        } else {
            error("this character literal is missing its closing '", s, i);
        }
        Token t = make(TokenKind::Character, s);
        t.value = (int64_t)(int8_t)value;
        return t;
    }

    Token stringLiteral(int s) {
        i += 1;
        std::vector<uint8_t> bytes;
        bool closed = false;
        while (true) {
            if (peek() == u'"') {
                i += 1;
                closed = true;
                break;
            }
            // Non-ASCII characters are stored as UTF-8.
            if (peek() >= 128) {
                int start = i;
                i += 1;
                if ((src[start] & 0xFC00) == 0xD800 && i < (int)src.size()) i += 1;
                std::string u8 = string(start, i);
                bytes.insert(bytes.end(), u8.begin(), u8.end());
                continue;
            }
            auto b = escapedChar();
            if (!b) break;
            bytes.push_back(*b);
        }
        if (!closed) error("this string is missing its closing \"", s, i);
        Token t = make(TokenKind::String, s);
        t.bytes = std::move(bytes);
        return t;
    }

    // MARK: Preprocessor

    void directive() {
        int s = i;
        // The directive runs to the end of the line; a backslash continues it.
        int e = i;
        while (e < (int)src.size()) {
            if (src[e] == 10) {
                if (e > s && src[e - 1] == u'\\') {
                    e += 1;
                    continue;
                }
                break;
            }
            e += 1;
        }
        std::string text = string(s, e);
        trivia.push_back({Trivia::Kind::Directive, SourceRange(s, e), text});
        i += 1;
        while (i < e && isSpace(peek())) i += 1;
        int ns = i;
        while (i < e && isIdent(peek())) i += 1;
        std::string name = string(ns, i);
        int line = lines.line(s);
        if (name == "include") {
            std::string rest = trimmed(string(i, e));
            diags.emplace_back(Diagnostic::Severity::Note, Stage::Preprocessor,
                               cat("#include ", rest, " is skipped: printf, putchar, malloc and friends are built in"),
                               SourceRange(s, e), line);
        } else if (name == "define") {
            while (i < e && isSpace(peek())) i += 1;
            int ms = i;
            while (i < e && isIdent(peek())) i += 1;
            std::string mname = string(ms, i);
            if (mname.empty() || !isIdentStart(src[ms])) {
                error("#define needs a name", s, e);
            } else if (peek() == u'(') {
                error(cat("function-like macros (#define ", mname, "(...)) are not supported; write a function instead"), s, e);
            } else {
                // Lex the replacement text with a sub-lexer limited to this line.
                std::vector<Token> saveTokens = std::move(tokens);
                tokens.clear();
                while (i < e) {
                    char16_t c = peek();
                    if (isSpace(c) || c == 10 || c == u'\\') {
                        i += 1;
                        continue;
                    }
                    if (c == u'/' && peek(1) == u'/') break;
                    if (auto t = token()) emit(*t, 0, {mname});
                }
                std::vector<Token> body = std::move(tokens);
                tokens = std::move(saveTokens);
                macros[mname] = Macro{mname, std::move(body), line};
            }
        } else if (name == "undef") {
            while (i < e && isSpace(peek())) i += 1;
            int ms = i;
            while (i < e && isIdent(peek())) i += 1;
            macros.erase(string(ms, i));
        } else if (name == "pragma" || name.empty()) {
        } else {
            error(cat("#", name, " is not supported (only #define and #include)"), s, e);
        }
        i = e;
        atLineStart = true;
    }
};

}  // namespace

LexResult lex(const std::string& source) {
    Lexer l(source);
    return l.run();
}

}  // namespace sp
