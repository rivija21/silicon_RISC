#include "Backend.h"

namespace sp {

const char* relocName(RelocType t) {
    switch (t) {
    case RelocType::Branch: return "R_RISCV_BRANCH";
    case RelocType::Jal: return "R_RISCV_JAL";
    case RelocType::PcrelHi20: return "R_RISCV_PCREL_HI20";
    case RelocType::PcrelLo12I: return "R_RISCV_PCREL_LO12_I";
    case RelocType::PcrelLo12S: return "R_RISCV_PCREL_LO12_S";
    case RelocType::Hi20: return "R_RISCV_HI20";
    case RelocType::Lo12I: return "R_RISCV_LO12_I";
    case RelocType::Lo12S: return "R_RISCV_LO12_S";
    case RelocType::Abs32: return "R_RISCV_32";
    }
    return "?";
}

const char* relocExplanation(RelocType t) {
    switch (t) {
    case RelocType::Branch: return "12-bit PC-relative branch offset";
    case RelocType::Jal: return "20-bit PC-relative jump offset";
    case RelocType::PcrelHi20: return "upper 20 bits of (symbol − PC), for auipc";
    case RelocType::PcrelLo12I: case RelocType::PcrelLo12S: return "lower 12 bits of (symbol − PC of the matching auipc)";
    case RelocType::Hi20: return "upper 20 bits of the absolute address, for lui";
    case RelocType::Lo12I: case RelocType::Lo12S: return "lower 12 bits of the absolute address";
    case RelocType::Abs32: return "a full 32-bit address stored as data";
    }
    return "";
}

bool ObjectFile::hasErrors() const {
    for (auto& d : diagnostics)
        if (d.severity == Diagnostic::Severity::Error) return true;
    return false;
}

std::vector<std::string> ObjectFile::globalSymbols() const {
    std::vector<std::string> out;
    for (auto& [name, s] : symbols)
        if (s.isGlobal) out.push_back(s.name);
    std::sort(out.begin(), out.end());
    return out;
}

namespace {

struct AsmError {
    std::string message;
};

bool isLetter(unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c >= 0x80; }
bool isSymbolStart(unsigned char c) { return isLetter(c) || c == '_' || c == '.' || c == '$'; }
bool isSymbolChar(unsigned char c) { return isLetter(c) || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '$'; }
bool isSymbolName(const std::string& s) {
    if (s.empty() || !isSymbolStart((unsigned char)s[0])) return false;
    for (unsigned char c : s)
        if (!isSymbolChar(c)) return false;
    return true;
}

std::string stripComment(const std::string& s) {
    bool inQuote = false, inChar = false;
    char prev = ' ';
    std::string out;
    for (char c : s) {
        if (c == '"' && !inChar && prev != '\\') inQuote = !inQuote;
        if (c == '\'' && !inQuote && prev != '\\') inChar = !inChar;
        if ((c == '#' || c == ';') && !inQuote && !inChar) break;
        out += c;
        prev = c;
    }
    return out;
}

std::vector<std::string> splitOperands(const std::string& s) {
    std::vector<std::string> parts;
    std::string cur;
    int depth = 0;
    bool inQuote = false;
    char prev = ' ';
    for (char c : s) {
        if (c == '"' && prev != '\\') inQuote = !inQuote;
        if (!inQuote) {
            if (c == '(') depth += 1;
            if (c == ')') depth -= 1;
            if (c == ',' && depth == 0) {
                parts.push_back(trimmed(cur));
                cur.clear();
                prev = c;
                continue;
            }
        }
        cur += c;
        prev = c;
    }
    std::string last = trimmed(cur);
    if (!last.empty() || !parts.empty()) parts.push_back(last);
    return parts;
}

struct Imm {
    enum Kind { Const, Symbol, Hi, Lo, PcrelHi, PcrelLo };
    Kind kind = Const;
    int32_t value = 0;     // constant or addend
    std::string symbol;
    static Imm cst(int32_t v) { return {Const, v, ""}; }
};

struct Machine {
    Op op;
    int rd = 0, rs1 = 0, rs2 = 0;
    Imm imm = Imm::cst(0);
};

struct Stmt {
    int line;
    SectionKind section;
    int offset;
    int size;
    std::string mnemonic;
    std::vector<std::string> operands;
    bool isDirective;
    std::string rawArgs;
};

/// A two-pass assembler for RV32IM, with the usual pseudo-instructions.
///
/// Pass 1 reads every line, works out how many bytes each one needs and
/// records where each label lands. Pass 2 encodes the instructions, now that
/// every label's offset is known. References that cannot be resolved inside
/// this file (another section, another file) become relocations for the linker.
class AssemblerImpl {
public:
    explicit AssemblerImpl(ObjectFileP o) : obj(std::move(o)) {}

    void run() {
        pass1();
        if (obj->hasErrors()) return;
        pass2();
        obj->undefined.clear();
        for (auto& rl : obj->relocations)
            if (!obj->symbols.count(rl.symbol)) obj->undefined.insert(rl.symbol);
        for (auto& g : globals) {
            auto it = obj->symbols.find(g);
            if (it != obj->symbols.end()) it->second.isGlobal = true;
            else obj->undefined.insert(g);
        }
    }

private:
    ObjectFileP obj;
    SectionKind section = SectionKind::Text;
    std::map<std::string, int32_t> equ;
    std::set<std::string> globals;
    std::vector<Stmt> stmts;
    int offsets[2] = {0, 0};
    int lastPcrelHi = 0;

    int& offsetOf(SectionKind s) { return offsets[s == SectionKind::Text ? 0 : 1]; }

    void error(const std::string& msg, int line) {
        obj->diagnostics.emplace_back(Diagnostic::Severity::Error, Stage::Assembler, msg, SourceRange(), line + 1);
    }

    // MARK: Pass 1

    void pass1() {
        for (int idx = 0; idx < (int)obj->lines.size(); idx++) {
            std::string s = trimmed(stripComment(obj->lines[idx].text));
            // Labels.
            while (true) {
                size_t colon = s.find(':');
                if (colon == std::string::npos) break;
                std::string name = trimmed(s.substr(0, colon));
                if (!isSymbolName(name)) break;
                if (obj->symbols.count(name)) {
                    error(cat("label '", name, "' is defined twice"), idx);
                } else {
                    obj->symbols[name] = ObjectSymbol{name, section, offsetOf(section), false, idx + 1};
                }
                s = trimmed(s.substr(colon + 1));
            }
            if (s.empty()) continue;
            size_t me = s.find_first_of(" \t");
            if (me == std::string::npos) me = s.size();
            std::string mnemonic = lowercased(s.substr(0, me));
            std::string rest = trimmed(s.substr(me));
            if (hasPrefix(mnemonic, ".")) {
                directive1(mnemonic, rest, idx);
                continue;
            }
            if (section != SectionKind::Text) {
                error("instructions belong in .text, but this line is in .data", idx);
                continue;
            }
            auto ops = splitOperands(rest);
            int n = instructionCount(mnemonic, ops);
            stmts.push_back(Stmt{idx, SectionKind::Text, offsetOf(SectionKind::Text), 4 * n, mnemonic, ops, false, rest});
            offsetOf(SectionKind::Text) += 4 * n;
        }
    }

    void align(int to) {
        int o = offsetOf(section);
        int pad = (to - o % to) % to;
        if (pad > 0) {
            stmts.push_back(Stmt{-1, section, o, pad, ".pad", {}, true, ""});
            offsetOf(section) += pad;
        }
    }

    void directive1(const std::string& d, const std::string& rest, int idx) {
        auto args = splitOperands(rest);
        auto add = [&](int size) {
            stmts.push_back(Stmt{idx, section, offsetOf(section), size, d, args, true, rest});
            offsetOf(section) += size;
        };
        std::string first = args.empty() ? "" : args[0];
        if (d == ".text") section = SectionKind::Text;
        else if (d == ".data" || d == ".rodata" || d == ".bss" || d == ".sdata") section = SectionKind::Data;
        else if (d == ".section") section = hasPrefix(first, ".text") ? SectionKind::Text : SectionKind::Data;
        else if (d == ".globl" || d == ".global") {
            for (auto& a : args) globals.insert(a);
        } else if (d == ".align" || d == ".p2align") {
            auto n = parseInt(first);
            if (!n || *n < 0 || *n > 12) {
                error(d + " needs a power of two exponent", idx);
                return;
            }
            align(1 << *n);
        } else if (d == ".balign") {
            auto n = parseInt(first);
            if (!n || *n <= 0 || (*n & (*n - 1)) != 0) {
                error(".balign needs a power of two", idx);
                return;
            }
            align((int)*n);
        } else if (d == ".word" || d == ".long") add(4 * (int)args.size());
        else if (d == ".half" || d == ".short") add(2 * (int)args.size());
        else if (d == ".byte") add((int)args.size());
        else if (d == ".space" || d == ".zero" || d == ".skip") {
            auto n = parseInt(first);
            if (!n || *n < 0) {
                error(d + " needs a size", idx);
                return;
            }
            add((int)*n);
        } else if (d == ".string" || d == ".asciz" || d == ".ascii") {
            auto bytes = Assembler::parseString(rest);
            if (!bytes) {
                error(d + " needs a \"quoted string\"", idx);
                return;
            }
            add((int)bytes->size() + (d == ".ascii" ? 0 : 1));
        } else if (d == ".equ" || d == ".set") {
            std::optional<int32_t> v;
            if (args.size() == 2) {
                try {
                    v = constant(args[1]);
                } catch (const AsmError&) {
                }
            }
            if (!v) {
                error(d + " needs a name and a constant", idx);
                return;
            }
            equ[args[0]] = *v;
        } else if (d == ".type" || d == ".size" || d == ".file" || d == ".ident" || d == ".option" || d == ".attribute" || d == ".local" ||
                   d == ".comm" || d == ".weak" || d == ".cfi_startproc" || d == ".cfi_endproc") {
        } else {
            error("unknown directive " + d, idx);
        }
    }

    /// How many machine instructions a (pseudo-)instruction expands to.
    int instructionCount(const std::string& m, const std::vector<std::string>& ops) {
        if (m == "la") return 2;
        if (m == "li") {
            if (ops.size() != 2) return 1;
            try {
                int32_t v = constant(ops[1]);
                return ((v >= -2048 && v <= 2047) || (v & 0xFFF) == 0) ? 1 : 2;
            } catch (const AsmError&) {
                return 2;
            }
        }
        return 1;
    }

    // MARK: Values

    int32_t constant(const std::string& s) {
        Imm i = immediate(s);
        if (i.kind == Imm::Const) return i.value;
        throw AsmError{cat("'", s, "' must be a constant number")};
    }

    std::optional<int64_t> number(const std::string& s) {
        std::string t = trimmed(s);
        bool neg = false;
        if (hasPrefix(t, "-")) {
            neg = true;
            t = t.substr(1);
        } else if (hasPrefix(t, "+")) {
            t = t.substr(1);
        }
        std::optional<int64_t> v;
        if (hasPrefix(t, "0x") || hasPrefix(t, "0X")) v = parseInt(t.substr(2), 16);
        else if (hasPrefix(t, "0b") || hasPrefix(t, "0B")) v = parseInt(t.substr(2), 2);
        else if (charCount(t) >= 3 && t.front() == '\'' && t.back() == '\'') {
            std::string inner = t.substr(1, t.size() - 2);
            auto b = Assembler::parseString("\"" + inner + "\"");
            if (b && b->size() == 1) v = (*b)[0];
        } else {
            v = parseInt(t);
        }
        if (!v) return std::nullopt;
        return neg ? -*v : *v;
    }

    Imm immediate(const std::string& s) {
        std::string t = trimmed(s);
        if (auto v = number(t)) {
            if (*v < -2147483648LL || *v > 0xFFFFFFFFLL) throw AsmError{t + " does not fit in 32 bits"};
            return Imm::cst((int32_t)(uint32_t)(uint64_t)*v);
        }
        static const std::pair<const char*, Imm::Kind> prefixes[] = {
            {"%hi(", Imm::Hi}, {"%lo(", Imm::Lo}, {"%pcrel_hi(", Imm::PcrelHi}, {"%pcrel_lo(", Imm::PcrelLo}};
        for (auto& [prefix, kind] : prefixes) {
            std::string p = prefix;
            if (hasPrefix(t, p) && hasSuffix(t, ")")) {
                std::string inner = t.substr(p.size(), t.size() - p.size() - 1);
                auto [sym, add] = symbolExpr(inner);
                Imm i;
                i.kind = kind;
                i.symbol = sym;
                i.value = kind == Imm::PcrelLo ? 0 : add;
                return i;
            }
        }
        auto [sym, add] = symbolExpr(t);
        auto e = equ.find(sym);
        if (e != equ.end()) return Imm::cst(RV::add(e->second, add));
        Imm i;
        i.kind = Imm::Symbol;
        i.symbol = sym;
        i.value = add;
        return i;
    }

    /// "label", "label+8", "label-4"
    std::pair<std::string, int32_t> symbolExpr(const std::string& s) {
        std::string name = trimmed(s);
        int32_t add = 0;
        size_t k = name.find_last_of("+-");
        if (k != std::string::npos && k != 0) {
            std::string tail = name.substr(k);
            if (auto v = number(tail)) {
                add = (int32_t)(uint32_t)(uint64_t)*v;
                name = trimmed(name.substr(0, k));
            }
        }
        if (!isSymbolName(name)) throw AsmError{cat("'", s, "' is not a number or a label")};
        if (Reg::parse(name)) throw AsmError{cat("expected a number or label, but '", name, "' is a register")};
        return {name, add};
    }

    int reg(const std::string& s) {
        std::string t = trimmed(s);
        auto r = Reg::parse(t);
        if (!r) {
            throw AsmError{t.empty() ? std::string("missing register")
                                     : cat("'", t, "' is not a register (use x0–x31 or names like t0, a0, sp)")};
        }
        return *r;
    }

    /// "8(sp)" → (8, sp). Also "(sp)" and "%lo(x)(t0)".
    std::pair<Imm, int> memoryOperand(const std::string& s) {
        std::string t = trimmed(s);
        size_t open = t.rfind('(');
        if (!hasSuffix(t, ")") || open == std::string::npos) {
            throw AsmError{cat("expected offset(register), like 8(sp), but found '", t, "'")};
        }
        int r = reg(t.substr(open + 1, t.size() - open - 2));
        std::string off = trimmed(t.substr(0, open));
        return {off.empty() ? Imm::cst(0) : immediate(off), r};
    }

    // MARK: Pass 2

    void pass2() {
        for (auto& st : stmts) {
            if (st.isDirective) {
                directive2(st);
                continue;
            }
            try {
                auto ms = expand(st);
                if ((int)ms.size() * 4 != st.size) {
                    error("internal: size changed between passes for " + st.mnemonic, st.line);
                    continue;
                }
                for (int k = 0; k < (int)ms.size(); k++) {
                    const Machine& m = ms[k];
                    int off = st.offset + 4 * k;
                    uint32_t word = encode(m, off);
                    obj->text.push_back(word & 0xFF);
                    obj->text.push_back((word >> 8) & 0xFF);
                    obj->text.push_back((word >> 16) & 0xFF);
                    obj->text.push_back(word >> 24);
                    auto dd = Decoder::decode(word);
                    Decoded d = dd ? *dd : Decoded{m.op, 0, 0, 0, 0, word};
                    bool isPseudo = ms.size() > 1 || opName(m.op) != st.mnemonic;
                    const AsmLine& src = obj->lines[st.line];
                    ListingEntry e;
                    e.offset = off;
                    e.word = word;
                    e.text = render(m, d);
                    if (isPseudo) e.pseudo = trimmed(st.mnemonic + " " + st.rawArgs);
                    e.part = k + 1;
                    e.parts = (int)ms.size();
                    e.asmLine = st.line;
                    e.origin = src.origin;
                    e.function = src.function;
                    obj->listing.push_back(std::move(e));
                }
            } catch (const AsmError& e) {
                error(e.message, st.line);
                obj->text.insert(obj->text.end(), st.size, 0);
            }
        }
        // Attach relocations to their listing entries.
        for (auto& rl : obj->relocations) {
            if (rl.section != SectionKind::Text) continue;
            for (int k = 0; k < (int)obj->listing.size(); k++)
                if (obj->listing[k].offset == rl.offset) {
                    rl.listing = k;
                    break;
                }
        }
    }

    std::string render(const Machine& m, const Decoded& d) {
        std::optional<std::string> sym;
        switch (m.imm.kind) {
        case Imm::Symbol: sym = m.imm.value == 0 ? m.imm.symbol : cat(m.imm.symbol, m.imm.value > 0 ? "+" : "", m.imm.value); break;
        case Imm::Hi: sym = "%hi(" + m.imm.symbol + ")"; break;
        case Imm::Lo: sym = "%lo(" + m.imm.symbol + ")"; break;
        case Imm::PcrelHi: sym = "%pcrel_hi(" + m.imm.symbol + ")"; break;
        case Imm::PcrelLo: sym = "%pcrel_lo(" + m.imm.symbol + ")"; break;
        case Imm::Const: break;
        }
        if (!sym) return Decoder::disassemble(d);
        std::string n = opName(m.op);
        auto r = [](int x) { return Reg::name(x); };
        switch (opSpec(m.op).format) {
        case InstFormat::B: return cat(n, " ", r(m.rs1), ", ", r(m.rs2), ", ", *sym);
        case InstFormat::J: return cat("jal ", r(m.rd), ", ", *sym);
        case InstFormat::U: return cat(n, " ", r(m.rd), ", ", *sym);
        case InstFormat::S: return cat(n, " ", r(m.rs2), ", ", *sym, "(", r(m.rs1), ")");
        case InstFormat::I:
            if (opIsLoad(m.op) || m.op == Op::JALR) return cat(n, " ", r(m.rd), ", ", *sym, "(", r(m.rs1), ")");
            return cat(n, " ", r(m.rd), ", ", r(m.rs1), ", ", *sym);
        default: return cat(n, " ", r(m.rd), ", ", r(m.rs1), ", ", *sym);
        }
    }

    void directive2(const Stmt& st) {
        auto emitBytes = [&](const std::vector<uint8_t>& bytes, const std::string& text) {
            if (st.section == SectionKind::Text) {
                obj->text.insert(obj->text.end(), bytes.begin(), bytes.end());
            } else {
                DataEntry e;
                e.offset = (int)obj->data.size();
                e.size = (int)bytes.size();
                e.text = text;
                e.asmLine = st.line;
                if (st.line >= 0) e.origin = obj->lines[st.line].origin;
                obj->dataListing.push_back(std::move(e));
                obj->data.insert(obj->data.end(), bytes.begin(), bytes.end());
            }
        };
        auto le = [](uint32_t v, int n) {
            std::vector<uint8_t> out;
            for (int k = 0; k < n; k++) out.push_back((v >> (8 * k)) & 0xFF);
            return out;
        };
        const std::string& m = st.mnemonic;
        if (m == ".pad") {
            emitBytes(std::vector<uint8_t>(st.size, 0), "(padding)");
        } else if (m == ".word" || m == ".long" || m == ".half" || m == ".short" || m == ".byte") {
            int size = m == ".byte" ? 1 : (m == ".half" || m == ".short") ? 2 : 4;
            std::vector<uint8_t> bytes;
            for (auto& a : st.operands) {
                std::vector<uint8_t> piece;
                try {
                    Imm i = immediate(a);
                    if (i.kind == Imm::Const) {
                        int32_t v = i.value;
                        if (size == 1 && (v < -128 || v > 255)) error(cat(".byte value ", v, " does not fit in 8 bits"), st.line);
                        if (size == 2 && (v < -32768 || v > 65535)) error(cat(".half value ", v, " does not fit in 16 bits"), st.line);
                        piece = le((uint32_t)v, size);
                    } else if (i.kind == Imm::Symbol) {
                        if (size != 4) {
                            error("only .word can hold an address", st.line);
                            piece = le(0, size);
                        } else {
                            int base = st.section == SectionKind::Text ? (int)obj->text.size() : (int)obj->data.size();
                            obj->relocations.push_back(
                                Relocation{st.section, base + (int)bytes.size(), RelocType::Abs32, i.symbol, i.value, std::nullopt, std::nullopt});
                            obj->undefined.insert(i.symbol);
                            piece = le(0, 4);
                        }
                    } else {
                        error("%hi/%lo cannot be used in data", st.line);
                        piece = le(0, size);
                    }
                } catch (const AsmError& e) {
                    error(e.message, st.line);
                    piece = le(0, size);
                }
                bytes.insert(bytes.end(), piece.begin(), piece.end());
            }
            emitBytes(bytes, m + " " + st.rawArgs);
        } else if (m == ".space" || m == ".zero" || m == ".skip") {
            emitBytes(std::vector<uint8_t>(st.size, 0), m + " " + st.rawArgs);
        } else if (m == ".string" || m == ".asciz" || m == ".ascii") {
            auto b = Assembler::parseString(st.rawArgs);
            std::vector<uint8_t> bytes = b ? *b : std::vector<uint8_t>{};
            if (m != ".ascii") bytes.push_back(0);
            emitBytes(bytes, m + " " + st.rawArgs);
        }
    }

    std::vector<Machine> expand(const Stmt& st) {
        const auto& o = st.operands;
        auto need = [&](size_t n) {
            if (o.size() != n) {
                throw AsmError{cat("'", st.mnemonic, "' takes ", (int)n, " operand", n == 1 ? "" : "s", ", but ", (int)o.size(), " ",
                                   o.size() == 1 ? "was" : "were", " given")};
            }
        };
        auto rg = [&](size_t k) { return reg(o[k]); };
        auto im = [&](size_t k) { return immediate(o[k]); };
        auto M = [](Op op, int rd = 0, int rs1 = 0, int rs2 = 0, Imm imm = Imm::cst(0)) { return Machine{op, rd, rs1, rs2, imm}; };
        const std::string& mn = st.mnemonic;
        if (mn == "nop") {
            need(0);
            return {M(Op::ADDI)};
        }
        if (mn == "li") {
            need(2);
            int rd = rg(0);
            Imm i = im(1);
            if (i.kind == Imm::Const) {
                int32_t v = i.value;
                if (v >= -2048 && v <= 2047) return {M(Op::ADDI, rd, 0, 0, Imm::cst(v))};
                int32_t lo = (int32_t)((uint32_t)v << 20) >> 20;
                int32_t hi = RV::sub(v, lo) >> 12;
                if (lo == 0) return {M(Op::LUI, rd, 0, 0, Imm::cst(hi & 0xFFFFF))};
                return {M(Op::LUI, rd, 0, 0, Imm::cst(hi & 0xFFFFF)), M(Op::ADDI, rd, rd, 0, Imm::cst(lo))};
            }
            if (i.kind == Imm::Symbol) {
                Imm h = i, l = i;
                h.kind = Imm::Hi;
                l.kind = Imm::Lo;
                return {M(Op::LUI, rd, 0, 0, h), M(Op::ADDI, rd, rd, 0, l)};
            }
            throw AsmError{"li needs a constant"};
        }
        if (mn == "la") {
            need(2);
            int rd = rg(0);
            Imm i = im(1);
            if (i.kind != Imm::Symbol) throw AsmError{"la needs a label"};
            Imm h = i;
            h.kind = Imm::PcrelHi;
            Imm l;
            l.kind = Imm::PcrelLo;
            l.symbol = ".";
            return {M(Op::AUIPC, rd, 0, 0, h), M(Op::ADDI, rd, rd, 0, l)};
        }
        if (mn == "mv") { need(2); int a = rg(0); int b = rg(1); return {M(Op::ADDI, a, b)}; }
        if (mn == "not") { need(2); int a = rg(0); int b = rg(1); return {M(Op::XORI, a, b, 0, Imm::cst(-1))}; }
        if (mn == "neg") { need(2); int a = rg(0); int b = rg(1); return {M(Op::SUB, a, 0, b)}; }
        if (mn == "seqz") { need(2); int a = rg(0); int b = rg(1); return {M(Op::SLTIU, a, b, 0, Imm::cst(1))}; }
        if (mn == "snez") { need(2); int a = rg(0); int b = rg(1); return {M(Op::SLTU, a, 0, b)}; }
        if (mn == "sltz") { need(2); int a = rg(0); int b = rg(1); return {M(Op::SLT, a, b, 0)}; }
        if (mn == "sgtz") { need(2); int a = rg(0); int b = rg(1); return {M(Op::SLT, a, 0, b)}; }
        if (mn == "beqz") { need(2); int a = rg(0); Imm i = im(1); return {M(Op::BEQ, 0, a, 0, i)}; }
        if (mn == "bnez") { need(2); int a = rg(0); Imm i = im(1); return {M(Op::BNE, 0, a, 0, i)}; }
        if (mn == "blez") { need(2); int a = rg(0); Imm i = im(1); return {M(Op::BGE, 0, 0, a, i)}; }
        if (mn == "bgez") { need(2); int a = rg(0); Imm i = im(1); return {M(Op::BGE, 0, a, 0, i)}; }
        if (mn == "bltz") { need(2); int a = rg(0); Imm i = im(1); return {M(Op::BLT, 0, a, 0, i)}; }
        if (mn == "bgtz") { need(2); int a = rg(0); Imm i = im(1); return {M(Op::BLT, 0, 0, a, i)}; }
        if (mn == "bgt") { need(3); int b = rg(1); int a = rg(0); Imm i = im(2); return {M(Op::BLT, 0, b, a, i)}; }
        if (mn == "ble") { need(3); int b = rg(1); int a = rg(0); Imm i = im(2); return {M(Op::BGE, 0, b, a, i)}; }
        if (mn == "bgtu") { need(3); int b = rg(1); int a = rg(0); Imm i = im(2); return {M(Op::BLTU, 0, b, a, i)}; }
        if (mn == "bleu") { need(3); int b = rg(1); int a = rg(0); Imm i = im(2); return {M(Op::BGEU, 0, b, a, i)}; }
        if (mn == "j") { need(1); return {M(Op::JAL, 0, 0, 0, im(0))}; }
        if (mn == "call") { need(1); return {M(Op::JAL, Reg::ra, 0, 0, im(0))}; }
        if (mn == "tail") { need(1); return {M(Op::JAL, 0, 0, 0, im(0))}; }
        if (mn == "ret") { need(0); return {M(Op::JALR, 0, Reg::ra)}; }
        if (mn == "jr") { need(1); return {M(Op::JALR, 0, rg(0))}; }
        if (mn == "jal") {
            if (o.size() == 1) return {M(Op::JAL, Reg::ra, 0, 0, im(0))};
            need(2);
            int rd = rg(0);
            return {M(Op::JAL, rd, 0, 0, im(1))};
        }
        if (mn == "jalr") {
            if (o.size() == 1) return {M(Op::JALR, Reg::ra, rg(0))};
            if (o.size() == 2) {
                auto [i, b] = memoryOperand(o[1]);
                int rd = rg(0);
                return {M(Op::JALR, rd, b, 0, i)};
            }
            need(3);
            int rd = rg(0);
            int rs = rg(1);
            return {M(Op::JALR, rd, rs, 0, im(2))};
        }
        auto opt = opFromName(mn);
        if (!opt) throw AsmError{cat("unknown instruction '", mn, "'")};
        Op op = *opt;
        switch (opSpec(op).format) {
        case InstFormat::R: {
            need(3);
            int a = rg(0), b = rg(1), c = rg(2);
            return {M(op, a, b, c)};
        }
        case InstFormat::I:
            if (op == Op::ECALL || op == Op::EBREAK || op == Op::FENCE) return {M(op)};
            if (opIsLoad(op)) {
                need(2);
                auto [i, b] = memoryOperand(o[1]);
                int rd = rg(0);
                return {M(op, rd, b, 0, i)};
            } else {
                need(3);
                int rd = rg(0), rs = rg(1);
                return {M(op, rd, rs, 0, im(2))};
            }
        case InstFormat::S: {
            need(2);
            auto [i, b] = memoryOperand(o[1]);
            int r2 = rg(0);
            return {M(op, 0, b, r2, i)};
        }
        case InstFormat::B: {
            need(3);
            int a = rg(0), b = rg(1);
            return {M(op, 0, a, b, im(2))};
        }
        case InstFormat::U: {
            need(2);
            int rd = rg(0);
            return {M(op, rd, 0, 0, im(1))};
        }
        case InstFormat::J: {
            need(2);
            int rd = rg(0);
            return {M(op, rd, 0, 0, im(1))};
        }
        }
        throw AsmError{"unknown instruction"};
    }

    void reloc(RelocType type, const std::string& sym, int32_t add, int off, std::optional<int> hi = std::nullopt) {
        obj->relocations.push_back(Relocation{SectionKind::Text, off, type, sym, add, hi, std::nullopt});
        if (!obj->symbols.count(sym)) obj->undefined.insert(sym);
    }

    uint32_t encode(const Machine& m, int off) {
        Decoded d{m.op, m.rd, m.rs1, m.rs2, 0, 0};
        InstFormat fmt = opSpec(m.op).format;
        switch (m.imm.kind) {
        case Imm::Const: {
            int32_t v = m.imm.value;
            switch (fmt) {
            case InstFormat::I:
                if (opIsShiftImm(m.op)) {
                    if (v < 0 || v > 31) throw AsmError{cat("shift amount ", v, " must be 0–31")};
                } else if (!(m.op == Op::ECALL || m.op == Op::EBREAK || m.op == Op::FENCE)) {
                    if (v < -2048 || v > 2047)
                        throw AsmError{cat("immediate ", v, " does not fit in 12 bits (−2048…2047); use li to load it into a register")};
                }
                break;
            case InstFormat::S:
                if (v < -2048 || v > 2047) throw AsmError{cat("offset ", v, " does not fit in 12 bits")};
                break;
            case InstFormat::B:
                if (v < -4096 || v > 4095 || (v & 1) != 0) throw AsmError{cat("branch offset ", v, " is out of range or odd")};
                break;
            case InstFormat::J:
                if (v < -(1 << 20) || v >= (1 << 20) || (v & 1) != 0) throw AsmError{cat("jump offset ", v, " is out of range or odd")};
                break;
            case InstFormat::U:
                if (v < -(1 << 19) || v > 0xFFFFF) throw AsmError{cat("upper immediate ", v, " does not fit in 20 bits")};
                break;
            case InstFormat::R: break;
            }
            d.imm = fmt == InstFormat::U ? (v & 0xFFFFF) : v;
            break;
        }
        case Imm::Symbol: {
            if (fmt != InstFormat::B && fmt != InstFormat::J)
                throw AsmError{"a label can only be used here with %hi/%lo; use la to load an address"};
            auto it = obj->symbols.find(m.imm.symbol);
            if (it != obj->symbols.end() && it->second.section == SectionKind::Text) {
                // Same file, same section: the assembler can resolve it right away.
                int32_t delta = RV::add((int32_t)(it->second.offset - off), m.imm.value);
                if (fmt == InstFormat::B) {
                    if (delta < -4096 || delta > 4095)
                        throw AsmError{cat("branch to '", m.imm.symbol, "' is too far (", delta, " bytes; the limit is ±4 KiB)")};
                } else {
                    if (delta < -(1 << 20) || delta >= (1 << 20)) throw AsmError{cat("jump to '", m.imm.symbol, "' is too far")};
                }
                d.imm = delta;
            } else {
                reloc(fmt == InstFormat::B ? RelocType::Branch : RelocType::Jal, m.imm.symbol, m.imm.value, off);
            }
            break;
        }
        case Imm::Hi:
            if (fmt != InstFormat::U) throw AsmError{"%hi only works with lui"};
            reloc(RelocType::Hi20, m.imm.symbol, m.imm.value, off);
            break;
        case Imm::Lo:
            if (fmt != InstFormat::I && fmt != InstFormat::S) throw AsmError{"%lo needs an I- or S-type instruction"};
            reloc(fmt == InstFormat::S ? RelocType::Lo12S : RelocType::Lo12I, m.imm.symbol, m.imm.value, off);
            break;
        case Imm::PcrelHi:
            if (fmt != InstFormat::U) throw AsmError{"%pcrel_hi only works with auipc"};
            reloc(RelocType::PcrelHi20, m.imm.symbol, m.imm.value, off);
            lastPcrelHi = off;
            break;
        case Imm::PcrelLo: {
            if (fmt != InstFormat::I && fmt != InstFormat::S) throw AsmError{"%pcrel_lo needs an I- or S-type instruction"};
            // In `la`, the low part refers to the auipc just before it.
            int hi = lastPcrelHi;
            if (m.imm.symbol != ".") {
                auto it = obj->symbols.find(m.imm.symbol);
                if (it != obj->symbols.end()) hi = it->second.offset;
            }
            const Relocation* target = nullptr;
            for (auto it = obj->relocations.rbegin(); it != obj->relocations.rend(); ++it)
                if (it->offset == hi && it->type == RelocType::PcrelHi20) {
                    target = &*it;
                    break;
                }
            std::string tsym = target ? target->symbol : m.imm.symbol;
            int32_t tadd = target ? target->addend : 0;
            reloc(fmt == InstFormat::S ? RelocType::PcrelLo12S : RelocType::PcrelLo12I, tsym, tadd, off, hi);
            break;
        }
        }
        return Encoder::encode(d);
    }
};

}  // namespace

std::optional<std::vector<uint8_t>> Assembler::parseString(const std::string& s) {
    std::string t = trimmed(s);
    if (charCount(t) < 2 || t.front() != '"' || t.back() != '"') return std::nullopt;
    std::string inner = t.substr(1, t.size() - 2);
    std::vector<uint8_t> out;
    size_t i = 0;
    auto hexDigit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    while (i < inner.size()) {
        uint8_t c = (uint8_t)inner[i];
        if (c == 92 && i + 1 < inner.size()) {
            i += 1;
            switch (inner[i]) {
            case 'n': out.push_back(10); break;
            case 't': out.push_back(9); break;
            case 'r': out.push_back(13); break;
            case '0': out.push_back(0); break;
            case '\\': out.push_back(92); break;
            case '"': out.push_back(34); break;
            case '\'': out.push_back(39); break;
            case 'x': {
                int v = 0, n = 0;
                while (n < 2 && i + 1 < inner.size() && hexDigit(inner[i + 1]) >= 0) {
                    v = v * 16 + hexDigit(inner[i + 1]);
                    i += 1;
                    n += 1;
                }
                out.push_back((uint8_t)v);
                break;
            }
            default: out.push_back((uint8_t)inner[i]); break;
            }
        } else {
            out.push_back(c);
        }
        i += 1;
    }
    return out;
}

ObjectFileP Assembler::assemble(const std::vector<AsmLine>& lines, const std::string& name) {
    auto obj = std::make_shared<ObjectFile>(name, lines);
    AssemblerImpl a(obj);
    a.run();
    return obj;
}

ObjectFileP Assembler::assembleText(const std::string& text, const std::string& name) {
    std::vector<AsmLine> lines;
    for (std::string l : split(text, '\n')) {
        if (!l.empty() && l.back() == '\r') l.pop_back();
        std::string t = trimmed(l);
        AsmLine a;
        a.kind = t.empty() ? AsmLine::Kind::Blank
                 : hasPrefix(t, "#") ? AsmLine::Kind::Comment
                 : hasSuffix(t, ":") ? AsmLine::Kind::Label
                 : hasPrefix(t, ".") ? AsmLine::Kind::Directive
                                     : AsmLine::Kind::Instruction;
        a.text = l;
        lines.push_back(std::move(a));
    }
    return assemble(lines, name);
}

}  // namespace sp
