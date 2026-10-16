#include "Frontend.h"

namespace sp {

// MARK: - CType

CType CType::pointer(const CType& t) {
    CType c(Pointer);
    c.sub = std::make_shared<const CType>(t);
    return c;
}

CType CType::array(const CType& t, int n) {
    CType c(Array);
    c.sub = std::make_shared<const CType>(t);
    c.count = n;
    return c;
}

CType CType::structure(StructDefP s) {
    CType c(Struct);
    c.sdef = std::move(s);
    return c;
}

CType CType::function(const CType& ret, std::vector<CType> ps) {
    CType c(Function);
    c.sub = std::make_shared<const CType>(ret);
    c.params = std::make_shared<const std::vector<CType>>(std::move(ps));
    return c;
}

bool CType::operator==(const CType& o) const {
    if (kind != o.kind) return false;
    switch (kind) {
    case Void: case Char: case Int: case UInt: return true;
    case Pointer: return *sub == *o.sub;
    case Array: return count == o.count && *sub == *o.sub;
    case Struct: return sdef == o.sdef;
    case Function: return *sub == *o.sub && *params == *o.params;
    }
    return false;
}

int CType::size() const {
    switch (kind) {
    case Void: return 1;
    case Char: return 1;
    case Int: case UInt: case Pointer: return 4;
    case Array: return sub->size() * count;
    case Struct: return sdef->size;
    case Function: return 4;
    }
    return 4;
}

int CType::align() const {
    switch (kind) {
    case Char: case Void: return 1;
    case Array: return sub->align();
    case Struct: return sdef->align;
    default: return 4;
    }
}

CType CType::decayed() const {
    if (kind == Array) return pointer(*sub);
    if (kind == Function) return pointer(*this);
    return *this;
}

static std::string joinDecl(const std::string& base, const std::string& name) {
    if (name.empty()) return base;
    if (name[0] == '[') return base + name;
    return base + " " + name;
}

std::string CType::spelled(const std::string& name) const {
    switch (kind) {
    case Void: return joinDecl("void", name);
    case Char: return joinDecl("char", name);
    case Int: return joinDecl("int", name);
    case UInt: return joinDecl("unsigned", name);
    case Struct: return joinDecl("struct " + sdef->name, name);
    case Pointer:
        if (sub->kind == Array || sub->kind == Function) return sub->spelled("(*" + name + ")");
        return sub->spelled("*" + name);
    case Array: return sub->spelled(cat(name, "[", count, "]"));
    case Function: {
        std::vector<std::string> ps;
        for (auto& p : *params) ps.push_back(p.description());
        return sub->spelled(cat(name, "(", join(ps, ", "), ")"));
    }
    }
    return "?";
}

const char* symbolKindName(Symbol::Kind k) {
    switch (k) {
    case Symbol::Kind::Global: return "global";
    case Symbol::Kind::Local: return "local";
    case Symbol::Kind::Param: return "param";
    case Symbol::Kind::Function: return "function";
    case Symbol::Kind::Library: return "library";
    }
    return "?";
}

const char* unaryOpText(UnaryOp op) {
    switch (op) {
    case UnaryOp::Neg: return "-";
    case UnaryOp::Plus: return "+";
    case UnaryOp::Not: return "!";
    case UnaryOp::BitNot: return "~";
    case UnaryOp::Deref: return "*";
    case UnaryOp::Addr: return "&";
    }
    return "?";
}

std::vector<FunctionDeclP> Program::functions() const {
    std::vector<FunctionDeclP> out;
    for (auto& it : items)
        if (it.kind == TopLevel::Kind::Function && it.function->body) out.push_back(it.function);
    return out;
}

std::vector<VarDeclP> Program::globals() const {
    std::vector<VarDeclP> out;
    for (auto& it : items)
        if (it.kind == TopLevel::Kind::Globals) out.insert(out.end(), it.globals.begin(), it.globals.end());
    return out;
}

// MARK: - Library

const std::vector<Library::Function>& Library::functions() {
    static const std::vector<Function> all = [] {
        CType i = CType::intT(), u = CType::uintT(), v = CType::voidT();
        CType cp = CType::pointer(CType::charT()), vp = CType::pointer(CType::voidT());
        return std::vector<Function>{
            {"print_int", v, {i}, "prints a signed integer (ecall 1)"},
            {"print_uint", v, {u}, "prints an unsigned integer (ecall 36)"},
            {"print_hex", v, {u}, "prints 0x… hexadecimal (ecall 34)"},
            {"print_char", v, {i}, "prints one character (ecall 11)"},
            {"print_str", v, {cp}, "prints a string (ecall 4)"},
            {"putchar", i, {i}, "prints one character"},
            {"puts", i, {cp}, "prints a string and a newline"},
            {"exit", v, {i}, "stops the program (ecall 93)"},
            {"malloc", vp, {u}, "allocates heap memory with sbrk (ecall 9); never freed"},
            {"calloc", vp, {u, u}, "allocates zeroed heap memory"},
            {"free", v, {vp}, "does nothing (bump allocator)"},
            {"abs", i, {i}, "absolute value"},
            {"rand", i, {}, "pseudo-random number 0…32767 (LCG)"},
            {"srand", v, {u}, "seeds rand()"},
            {"strlen", u, {cp}, "length of a string"},
            {"strcmp", i, {cp, cp}, "compares two strings"},
            {"strcpy", cp, {cp, cp}, "copies a string"},
            {"memset", vp, {vp, i, u}, "fills memory with a byte"},
            {"memcpy", vp, {vp, vp, u}, "copies memory"},
        };
    }();
    return all;
}

const Library::Function* Library::byName(const std::string& name) {
    static const std::map<std::string, const Function*> index = [] {
        std::map<std::string, const Function*> m;
        for (auto& f : functions()) m[f.name] = &f;
        return m;
    }();
    auto it = index.find(name);
    return it == index.end() ? nullptr : it->second;
}

std::optional<std::vector<PrintfPiece>> PrintfFormat::parse(const std::vector<uint8_t>& bytes, std::string* error) {
    auto fail = [&](const std::string& m) -> std::optional<std::vector<PrintfPiece>> {
        if (error) *error = m;
        return std::nullopt;
    };
    std::vector<PrintfPiece> pieces;
    std::vector<uint8_t> text;
    size_t i = 0;
    static const std::string skip = "-+ #0123456789.lhz";
    static const std::string conversions = "diuxXcsp";
    while (i < bytes.size()) {
        uint8_t b = bytes[i];
        if (b != '%') {
            text.push_back(b);
            i += 1;
            continue;
        }
        if (i + 1 >= bytes.size()) return fail("the format string ends with a lone %");
        size_t j = i + 1;
        // Accept and ignore flags/width/length such as %5d or %ld: we print without padding.
        while (j < bytes.size() && skip.find((char)bytes[j]) != std::string::npos) j += 1;
        if (j >= bytes.size()) return fail("the format string ends in the middle of a % conversion");
        char c = (char)bytes[j];
        if (c == '%') {
            text.push_back('%');
        } else if (bytes[j] < 128 && conversions.find(c) != std::string::npos) {
            if (!text.empty()) {
                pieces.push_back({false, text, 0});
                text.clear();
            }
            pieces.push_back({true, {}, c});
        } else if (c == 'f' || c == 'e' || c == 'g') {
            return fail(cat("%", c, " needs floating point, which is not supported"));
        } else {
            std::string shown = bytes[j] < 128 ? std::string(1, c) : toUTF8(std::u16string(1, (char16_t)bytes[j]));
            return fail(cat("unknown printf conversion %", shown, " (supported: %d %i %u %x %c %s %p %%)"));
        }
        i = j + 1;
    }
    if (!text.empty()) pieces.push_back({false, text, 0});
    return pieces;
}

}  // namespace sp
