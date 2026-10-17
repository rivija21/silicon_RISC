// Shared basics for the Silicon Path core: source ranges, diagnostics,
// RV32IM arithmetic, the memory map, and small string helpers.
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace sp {

// MARK: - Strings

/// UTF-8 to UTF-16 and back. Invalid UTF-8 becomes U+FFFD.
std::u16string toUTF16(const std::string& s);
std::string toUTF8(const std::u16string& s);
std::string toUTF8(const char16_t* p, size_t n);

namespace detail {
inline void append(std::string& out, const std::string& s) { out += s; }
inline void append(std::string& out, const char* s) { out += s; }
inline void append(std::string& out, char c) { out += c; }
inline void append(std::string& out, bool b) { out += b ? "true" : "false"; }
template <class T, std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, char> && !std::is_same_v<T, bool>, int> = 0>
inline void append(std::string& out, T v) { out += std::to_string(v); }
}  // namespace detail

/// Concatenates strings and numbers: cat("line ", 3, ": ", msg).
template <class... A>
std::string cat(const A&... a) {
    std::string out;
    (detail::append(out, a), ...);
    return out;
}

/// printf-style formatting into a std::string.
std::string format(const char* fmt, ...);

std::string trimmed(const std::string& s);                 // spaces and tabs
bool hasPrefix(const std::string& s, const std::string& p);
bool hasSuffix(const std::string& s, const std::string& p);
bool contains(const std::string& s, const std::string& part);
std::vector<std::string> split(const std::string& s, char sep);   // keeps empty pieces, like components(separatedBy:)
std::string join(const std::vector<std::string>& parts, const std::string& sep);
std::string lowercased(const std::string& s);
std::string uppercased(const std::string& s);
std::string replaceAll(std::string s, const std::string& from, const std::string& to);
std::string repeated(const std::string& s, int n);
std::string hexString(uint64_t v, bool upper = false);       // no prefix, no padding
/// Parses an integer the way Swift's Int(String, radix:) does: optional sign, digits only.
std::optional<int64_t> parseInt(const std::string& s, int radix = 10);
/// Number of Unicode scalars (approximately Swift's String.count for our texts).
int charCount(const std::string& s);
/// The first n Unicode scalars.
std::string prefixChars(const std::string& s, int n);
/// Swift's String.debugDescription-like quoting, used in test reports.
std::string debugQuoted(const std::string& s);

// MARK: - Source positions and messages

/// A span of the source text, in UTF-16 code units (so it maps straight onto the editor's text).
struct SourceRange {
    int start = -1;
    int end = -1;
    SourceRange() = default;
    SourceRange(int s, int e) : start(s), end(e) {}
    static SourceRange none() { return SourceRange(); }
    bool isValid() const { return start >= 0; }
    int length() const { return std::max(0, end - start); }
    SourceRange unite(const SourceRange& o) const {
        if (!isValid()) return o;
        if (!o.isValid()) return *this;
        return SourceRange(std::min(start, o.start), std::max(end, o.end));
    }
    bool operator==(const SourceRange&) const = default;
};

/// Which part of the tool chain produced a message.
enum class Stage { Lexer, Preprocessor, Parser, Semantic, IR, Codegen, Assembler, Linker, Runtime };
const char* stageName(Stage s);

struct Diagnostic {
    enum class Severity { Error, Warning, Note };
    Severity severity = Severity::Error;
    Stage stage = Stage::Parser;
    std::string message;
    SourceRange range;
    int line = 0;

    Diagnostic() = default;
    Diagnostic(Severity sev, Stage st, std::string msg, SourceRange r = SourceRange(), int l = 0)
        : severity(sev), stage(st), message(std::move(msg)), range(r), line(l) {}
    std::string description() const;
};

/// Maps UTF-16 offsets to 1-based line numbers and back.
struct LineTable {
    std::vector<int> lineStarts;
    int length = 0;

    LineTable() : lineStarts{0} {}
    explicit LineTable(const std::u16string& text);
    explicit LineTable(const std::string& utf8) : LineTable(toUTF16(utf8)) {}
    int lineCount() const { return (int)lineStarts.size(); }
    int line(int offset) const;
    int column(int offset) const { return offset - lineStarts[line(offset) - 1] + 1; }
    SourceRange rangeOfLine(int line) const;
};

// MARK: - Arithmetic

enum class BinaryOp { Add, Sub, Mul, Div, Rem, Shl, Shr, BitAnd, BitOr, BitXor, Lt, Gt, Le, Ge, Eq, Ne, LogAnd, LogOr };
const char* binaryOpText(BinaryOp op);
std::optional<BinaryOp> binaryOpFromText(const std::string& s);
inline bool isComparison(BinaryOp op) {
    return op == BinaryOp::Lt || op == BinaryOp::Gt || op == BinaryOp::Le || op == BinaryOp::Ge || op == BinaryOp::Eq || op == BinaryOp::Ne;
}

/// Integer arithmetic exactly as RV32IM does it. The constant folder, the
/// reference interpreter and the CPU simulators all use these, so every
/// layer agrees even on corner cases C leaves undefined:
///  - x / 0 = -1 and x % 0 = x (RISC-V does not trap on division by zero)
///  - INT_MIN / -1 = INT_MIN and INT_MIN % -1 = 0
///  - shift amounts use only their low 5 bits
///  - overflow wraps around (two's complement)
namespace RV {
inline int32_t wrap(int64_t v) { return (int32_t)(uint32_t)(uint64_t)v; }
inline int32_t add(int32_t a, int32_t b) { return (int32_t)((uint32_t)a + (uint32_t)b); }
inline int32_t sub(int32_t a, int32_t b) { return (int32_t)((uint32_t)a - (uint32_t)b); }
inline int32_t mul(int32_t a, int32_t b) { return (int32_t)((uint32_t)a * (uint32_t)b); }
inline int32_t neg(int32_t a) { return (int32_t)(0u - (uint32_t)a); }
inline int32_t div(int32_t a, int32_t b) {
    if (b == 0) return -1;
    if (a == INT32_MIN && b == -1) return INT32_MIN;
    return a / b;
}
inline int32_t rem(int32_t a, int32_t b) {
    if (b == 0) return a;
    if (a == INT32_MIN && b == -1) return 0;
    return a % b;
}
inline uint32_t divu(uint32_t a, uint32_t b) { return b == 0 ? 0xFFFFFFFFu : a / b; }
inline uint32_t remu(uint32_t a, uint32_t b) { return b == 0 ? a : a % b; }
inline int32_t sar(int32_t a, uint32_t s) { return a >> (s & 31); }
inline int32_t shl(int32_t a, uint32_t s) { return (int32_t)((uint32_t)a << (s & 31)); }
inline int32_t shr(int32_t a, uint32_t s) { return (int32_t)((uint32_t)a >> (s & 31)); }

/// A C binary operator on 32-bit values. `isUnsigned` selects unsigned division, right shift and comparison.
int32_t binary(BinaryOp op, int32_t a, int32_t b, bool isUnsigned);
}  // namespace RV

/// Where things live in the simulated machine (the same layout as the RARS simulator).
namespace MemoryMap {
constexpr uint32_t textBase = 0x00400000;
constexpr uint32_t dataBase = 0x10010000;
constexpr uint32_t heapBase = 0x10040000;
constexpr uint32_t heapLimit = 0x10440000;
constexpr uint32_t stackTop = 0x7FFFEFFC;
constexpr uint32_t stackSize = 0x00100000;
constexpr uint32_t stackBottom = (stackTop & ~0xFu) - stackSize + 16;
}  // namespace MemoryMap

namespace Format {
/// "0x0000002a"
std::string hex(uint32_t v, int digits = 8);
}

inline int trailingZeros(uint64_t v) {
    if (v == 0) return 64;
    int n = 0;
    while ((v & 1) == 0) { v >>= 1; n++; }
    return n;
}

}  // namespace sp
