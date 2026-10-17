#include "Base.h"

#include <cstdarg>

namespace sp {

// MARK: - UTF conversion

std::u16string toUTF16(const std::string& s) {
    std::u16string out;
    out.reserve(s.size());
    size_t i = 0, n = s.size();
    auto bad = [&] { out.push_back(0xFFFD); };
    while (i < n) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x80) {
            out.push_back(c);
            i++;
            continue;
        }
        int len = 0;
        uint32_t cp = 0;
        if ((c & 0xE0) == 0xC0) { len = 2; cp = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { len = 3; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { len = 4; cp = c & 0x07; }
        else { bad(); i++; continue; }
        if (i + len > n) { bad(); i++; continue; }
        bool ok = true;
        for (int k = 1; k < len; k++) {
            unsigned char d = (unsigned char)s[i + k];
            if ((d & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (d & 0x3F);
        }
        if (!ok || (len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && (cp < 0x10000 || cp > 0x10FFFF)) ||
            (cp >= 0xD800 && cp <= 0xDFFF)) {
            bad();
            i++;
            continue;
        }
        i += len;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out.push_back((char16_t)(0xD800 + (cp >> 10)));
            out.push_back((char16_t)(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back((char16_t)cp);
        }
    }
    return out;
}

std::string toUTF8(const char16_t* p, size_t n) {
    std::string out;
    out.reserve(n);
    for (size_t i = 0; i < n; i++) {
        uint32_t cp = p[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < n && p[i + 1] >= 0xDC00 && p[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (p[i + 1] - 0xDC00);
            i++;
        } else if (cp >= 0xD800 && cp <= 0xDFFF) {
            cp = 0xFFFD;
        }
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) {
            out += (char)(0xC0 | (cp >> 6));
            out += (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += (char)(0xE0 | (cp >> 12));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        } else {
            out += (char)(0xF0 | (cp >> 18));
            out += (char)(0x80 | ((cp >> 12) & 0x3F));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        }
    }
    return out;
}

std::string toUTF8(const std::u16string& s) { return toUTF8(s.data(), s.size()); }

// MARK: - String helpers

std::string format(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return "";
    if ((size_t)n < sizeof buf) return std::string(buf, n);
    std::string out(n, '\0');
    va_start(ap, fmt);
    vsnprintf(out.data(), n + 1, fmt, ap);
    va_end(ap);
    return out;
}

std::string trimmed(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t')) a++;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t')) b--;
    return s.substr(a, b - a);
}

bool hasPrefix(const std::string& s, const std::string& p) { return s.size() >= p.size() && s.compare(0, p.size(), p) == 0; }
bool hasSuffix(const std::string& s, const std::string& p) {
    return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0;
}
bool contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t start = 0;
    while (true) {
        size_t k = s.find(sep, start);
        if (k == std::string::npos) {
            out.push_back(s.substr(start));
            break;
        }
        out.push_back(s.substr(start, k - start));
        start = k + 1;
    }
    return out;
}

std::string join(const std::vector<std::string>& parts, const std::string& sep) {
    std::string out;
    for (size_t i = 0; i < parts.size(); i++) {
        if (i) out += sep;
        out += parts[i];
    }
    return out;
}

std::string lowercased(const std::string& s) {
    std::string out = s;
    for (char& c : out)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return out;
}

std::string uppercased(const std::string& s) {
    std::string out = s;
    for (char& c : out)
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    return out;
}

std::string replaceAll(std::string s, const std::string& from, const std::string& to) {
    if (from.empty()) return s;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
    return s;
}

std::string repeated(const std::string& s, int n) {
    std::string out;
    for (int i = 0; i < n; i++) out += s;
    return out;
}

std::string hexString(uint64_t v, bool upper) {
    const char* digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    if (v == 0) return "0";
    std::string out;
    while (v) {
        out += digits[v & 15];
        v >>= 4;
    }
    std::reverse(out.begin(), out.end());
    return out;
}

std::optional<int64_t> parseInt(const std::string& s, int radix) {
    if (s.empty()) return std::nullopt;
    size_t i = 0;
    bool neg = false;
    if (s[0] == '-' || s[0] == '+') {
        neg = s[0] == '-';
        i = 1;
    }
    if (i >= s.size()) return std::nullopt;
    uint64_t v = 0;
    const uint64_t limit = neg ? (uint64_t)INT64_MAX + 1 : (uint64_t)INT64_MAX;
    for (; i < s.size(); i++) {
        char c = s[i];
        int d;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'z') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'Z') d = c - 'A' + 10;
        else return std::nullopt;
        if (d >= radix) return std::nullopt;
        if (v > (limit - d) / radix) return std::nullopt;
        v = v * radix + d;
    }
    if (neg) return v == (uint64_t)INT64_MAX + 1 ? INT64_MIN : -(int64_t)v;
    return (int64_t)v;
}

int charCount(const std::string& s) {
    int n = 0;
    for (unsigned char c : s)
        if ((c & 0xC0) != 0x80) n++;
    return n;
}

std::string prefixChars(const std::string& s, int n) {
    int count = 0;
    for (size_t i = 0; i < s.size(); i++) {
        unsigned char c = (unsigned char)s[i];
        if ((c & 0xC0) != 0x80) {
            if (count == n) return s.substr(0, i);
            count++;
        }
    }
    return s;
}

std::string debugQuoted(const std::string& s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        switch (c) {
        case '\n': out += "\\n"; break;
        case '\t': out += "\\t"; break;
        case '\r': out += "\\r"; break;
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case 0: out += "\\0"; break;
        default:
            if (c < 32 || c == 127) out += format("\\u{%x}", c);
            else out += (char)c;
        }
    }
    return out + "\"";
}

// MARK: - Diagnostics

const char* stageName(Stage s) {
    switch (s) {
    case Stage::Lexer: return "Lexer";
    case Stage::Preprocessor: return "Preprocessor";
    case Stage::Parser: return "Parser";
    case Stage::Semantic: return "Semantic analysis";
    case Stage::IR: return "IR";
    case Stage::Codegen: return "Code generation";
    case Stage::Assembler: return "Assembler";
    case Stage::Linker: return "Linker";
    case Stage::Runtime: return "Runtime";
    }
    return "?";
}

std::string Diagnostic::description() const {
    const char* kind = severity == Severity::Error ? "error" : severity == Severity::Warning ? "warning" : "note";
    return line > 0 ? cat("line ", line, ": ", kind, ": ", message) : cat(kind, ": ", message);
}

LineTable::LineTable(const std::u16string& text) {
    lineStarts.push_back(0);
    int i = 0;
    for (char16_t u : text) {
        i++;
        if (u == 10) lineStarts.push_back(i);
    }
    length = i;
}

int LineTable::line(int offset) const {
    int lo = 0, hi = (int)lineStarts.size() - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (lineStarts[mid] <= offset) lo = mid;
        else hi = mid - 1;
    }
    return lo + 1;
}

SourceRange LineTable::rangeOfLine(int ln) const {
    if (ln < 1 || ln > (int)lineStarts.size()) return SourceRange::none();
    int s = lineStarts[ln - 1];
    int e = ln < (int)lineStarts.size() ? lineStarts[ln] - 1 : length;
    return SourceRange(s, std::max(s, e));
}

// MARK: - Operators

const char* binaryOpText(BinaryOp op) {
    switch (op) {
    case BinaryOp::Add: return "+";
    case BinaryOp::Sub: return "-";
    case BinaryOp::Mul: return "*";
    case BinaryOp::Div: return "/";
    case BinaryOp::Rem: return "%";
    case BinaryOp::Shl: return "<<";
    case BinaryOp::Shr: return ">>";
    case BinaryOp::BitAnd: return "&";
    case BinaryOp::BitOr: return "|";
    case BinaryOp::BitXor: return "^";
    case BinaryOp::Lt: return "<";
    case BinaryOp::Gt: return ">";
    case BinaryOp::Le: return "<=";
    case BinaryOp::Ge: return ">=";
    case BinaryOp::Eq: return "==";
    case BinaryOp::Ne: return "!=";
    case BinaryOp::LogAnd: return "&&";
    case BinaryOp::LogOr: return "||";
    }
    return "?";
}

std::optional<BinaryOp> binaryOpFromText(const std::string& s) {
    static const BinaryOp all[] = {BinaryOp::Add, BinaryOp::Sub, BinaryOp::Mul, BinaryOp::Div, BinaryOp::Rem, BinaryOp::Shl,
                                   BinaryOp::Shr, BinaryOp::BitAnd, BinaryOp::BitOr, BinaryOp::BitXor, BinaryOp::Lt, BinaryOp::Gt,
                                   BinaryOp::Le, BinaryOp::Ge, BinaryOp::Eq, BinaryOp::Ne, BinaryOp::LogAnd, BinaryOp::LogOr};
    for (BinaryOp op : all)
        if (s == binaryOpText(op)) return op;
    return std::nullopt;
}

int32_t RV::binary(BinaryOp op, int32_t a, int32_t b, bool isUnsigned) {
    uint32_t ua = (uint32_t)a, ub = (uint32_t)b;
    switch (op) {
    case BinaryOp::Add: return add(a, b);
    case BinaryOp::Sub: return sub(a, b);
    case BinaryOp::Mul: return mul(a, b);
    case BinaryOp::Div: return isUnsigned ? (int32_t)divu(ua, ub) : div(a, b);
    case BinaryOp::Rem: return isUnsigned ? (int32_t)remu(ua, ub) : rem(a, b);
    case BinaryOp::Shl: return (int32_t)(ua << (ub & 31));
    case BinaryOp::Shr: return isUnsigned ? (int32_t)(ua >> (ub & 31)) : a >> (ub & 31);
    case BinaryOp::BitAnd: return a & b;
    case BinaryOp::BitOr: return a | b;
    case BinaryOp::BitXor: return a ^ b;
    case BinaryOp::Lt: return (isUnsigned ? ua < ub : a < b) ? 1 : 0;
    case BinaryOp::Gt: return (isUnsigned ? ua > ub : a > b) ? 1 : 0;
    case BinaryOp::Le: return (isUnsigned ? ua <= ub : a <= b) ? 1 : 0;
    case BinaryOp::Ge: return (isUnsigned ? ua >= ub : a >= b) ? 1 : 0;
    case BinaryOp::Eq: return a == b ? 1 : 0;
    case BinaryOp::Ne: return a != b ? 1 : 0;
    case BinaryOp::LogAnd: return (a != 0 && b != 0) ? 1 : 0;
    case BinaryOp::LogOr: return (a != 0 || b != 0) ? 1 : 0;
    }
    return 0;
}

std::string Format::hex(uint32_t v, int digits) {
    std::string s = hexString(v);
    if ((int)s.size() < digits) s = std::string(digits - s.size(), '0') + s;
    return "0x" + s;
}

}  // namespace sp
