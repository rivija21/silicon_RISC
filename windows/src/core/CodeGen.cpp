#include "Backend.h"

namespace sp {

static bool fits12(int64_t v) { return v >= -2048 && v <= 2047; }
static std::string r(int x) { return Reg::name(x); }

/// The register to compute a result into, and how to store it if the value lives in memory.
struct CodeGen::Dst {
    int reg = 0;
    int spillSlot = -1;   // >= 0: store t6 here afterwards
    int vreg = 0;
};

CodeGen::CodeGen(const std::string& source) { sourceLines = split(source, '\n'); }

std::string CodeGen::label(const std::string& fn, const Block& block) { return ".L" + fn + "_" + replaceAll(block.name, ".", "_"); }

// MARK: Output helpers

void CodeGen::emit(const std::string& text, const std::string& note) {
    AsmLine l;
    l.kind = AsmLine::Kind::Instruction;
    l.text = "    " + text;
    l.origin = origin;
    if (f) l.function = f->name;
    l.note = note;
    out.push_back(std::move(l));
}

void CodeGen::directive(const std::string& text) {
    AsmLine l;
    l.kind = AsmLine::Kind::Directive;
    l.text = "    " + text;
    out.push_back(std::move(l));
}

void CodeGen::comment(const std::string& text) {
    AsmLine l;
    l.kind = AsmLine::Kind::Comment;
    l.text = "    # " + text;
    l.origin = origin;
    if (f) l.function = f->name;
    out.push_back(std::move(l));
}

// MARK: Module

std::vector<AsmLine> CodeGen::generate(const IRModule& m, const std::map<std::string, Allocation>& allocations) {
    out.clear();
    if (!m.data.empty()) {
        directive(".data");
        for (auto& g : m.data) data(g);
        out.push_back(AsmLine{AsmLine::Kind::Blank, "", std::nullopt, std::nullopt, ""});
    }
    directive(".text");
    for (auto& fn : m.functions) {
        function(*fn, allocations.at(fn->name));
        out.push_back(AsmLine{AsmLine::Kind::Blank, "", std::nullopt, std::nullopt, ""});
    }
    return out;
}

void CodeGen::data(const GlobalData& g) {
    if (g.align > 1) directive(cat(".align ", trailingZeros((uint64_t)g.align)));
    if (!hasPrefix(g.label, ".")) directive(".globl " + g.label);
    AsmLine lab;
    lab.kind = AsmLine::Kind::Label;
    lab.text = g.label + ":";
    lab.note = g.comment;
    if (g.line > 0) {
        origin = AsmOrigin{0, 0, g.line};
        lab.origin = origin;
    }
    out.push_back(lab);
    if (g.comment == "string") {
        std::vector<uint8_t> bytes;
        for (auto& it : g.items)
            if (it.kind == DataItem::Byte) bytes.push_back((uint8_t)it.value);
        if (!bytes.empty()) bytes.pop_back();
        directive(".string \"" + escape(bytes) + "\"");
        origin.reset();
        return;
    }
    std::vector<std::string> pendingWords, pendingBytes;
    auto flush = [&] {
        if (!pendingWords.empty()) directive(".word " + join(pendingWords, ", "));
        if (!pendingBytes.empty()) directive(".byte " + join(pendingBytes, ", "));
        pendingWords.clear();
        pendingBytes.clear();
    };
    for (auto& it : g.items) {
        switch (it.kind) {
        case DataItem::Word:
            if (!pendingBytes.empty()) flush();
            pendingWords.push_back(std::to_string(it.value));
            break;
        case DataItem::Address:
            if (!pendingBytes.empty()) flush();
            pendingWords.push_back(it.value == 0 ? it.label : cat(it.label, "+", it.value));
            break;
        case DataItem::Byte:
            if (!pendingWords.empty()) flush();
            pendingBytes.push_back(std::to_string((int)(int8_t)(uint8_t)it.value));
            break;
        case DataItem::Zero:
            flush();
            directive(cat(".space ", it.value));
            break;
        }
        if (pendingWords.size() >= 8 || pendingBytes.size() >= 16) flush();
    }
    flush();
    origin.reset();
}

std::string CodeGen::escape(const std::vector<uint8_t>& bytes) {
    std::string s;
    for (uint8_t b : bytes) {
        switch (b) {
        case 10: s += "\\n"; break;
        case 9: s += "\\t"; break;
        case 13: s += "\\r"; break;
        case 0: s += "\\0"; break;
        case 34: s += "\\\""; break;
        case 92: s += "\\\\"; break;
        default:
            if (b >= 32 && b < 127) s += (char)b;
            else s += format("\\x%02x", b);
        }
    }
    return s;
}

// MARK: Frame

void CodeGen::layoutFrame() {
    FrameLayout fr;
    fr.function = f->name;
    bool hasCalls = false;
    for (auto& b : f->blocks)
        for (auto& i : b->insts)
            if (i.kind.kind == InstKind::Call) hasCalls = true;
    bool leaf = !hasCalls && f->slots.empty() && alloc->spillSlots == 0 && alloc->usedCalleeSaved.empty();
    fr.hasFrame = !leaf;
    if (leaf) {
        frame = fr;
        return;
    }
    int off = 0;
    auto place = [&](int size, int align, const std::string& what, const std::string& kind) {
        off += size;
        off = (off + align - 1) / align * align;
        fr.entries.push_back({-off, size, what, kind});
        return -off;
    };
    place(4, 4, "return address (ra)", "ra");
    place(4, 4, "caller's frame pointer (old s0)", "fp");
    for (int s : alloc->usedCalleeSaved) fr.savedOffset[s] = place(4, 4, cat("saved ", r(s), " (callee-saved)"), "saved");
    for (int k = 0; k < alloc->spillSlots; k++) {
        std::string name = "?";
        for (auto& iv : alloc->intervals)
            if (iv.location == Location::spill(k)) {
                name = iv.name;
                break;
            }
        fr.spillOffset.push_back(place(4, 4, "spill slot for " + name, "spill"));
    }
    for (auto& s : f->slots) {
        fr.slotOffset.push_back(place(s.size, std::max(s.align, s.size >= 4 ? 4 : 1), cat(s.name.substr(1), " (", s.size, " bytes)"), "local"));
    }
    fr.size = (off + 15) / 16 * 16;
    frame = fr;
}

// MARK: Operands

/// Loads a constant into a register (`li` is a pseudo-instruction: addi, or lui + addi).
void CodeGen::li(int rd, int32_t v) { emit(cat("li ", r(rd), ", ", v)); }

/// The register holding an operand, loading it into `scratch` if needed.
int CodeGen::src(const Operand& o, int scratch) {
    if (o.isImm) {
        if (o.value == 0) return Reg::zero;
        li(scratch, o.value);
        return scratch;
    }
    const Location* loc = alloc->locationOf(o.value);
    if (!loc) return Reg::zero;   // Never defined (read of an uninitialised variable): any value will do.
    if (loc->kind == Location::Register) return loc->n;
    memOp("lw", scratch, Reg::s0, frame.spillOffset[loc->n], "reload " + f->vregs[o.value].name);
    return scratch;
}

CodeGen::Dst CodeGen::dst(int v) {
    const Location* loc = alloc->locationOf(v);
    if (!loc) return Dst{Reg::t6, -1, v};
    if (loc->kind == Location::Register) return Dst{loc->n, -1, v};
    return Dst{Reg::t6, loc->n, v};
}

/// `op rd, offset(base)`, using t4 to build the address if the offset does not fit in 12 bits.
void CodeGen::memOp(const std::string& op, int rd, int base, int offset, const std::string& note) {
    if (fits12(offset)) {
        emit(cat(op, " ", r(rd), ", ", offset, "(", r(base), ")"), note);
    } else {
        li(Reg::t4, offset);
        emit(cat("add t4, ", r(base), ", t4"));
        emit(cat(op, " ", r(rd), ", 0(t4)"), note);
    }
}

/// Resolves an IR address into base register + 12-bit offset. Uses t6 when needed.
std::pair<int, int> CodeGen::address(const Addr& a) {
    switch (a.kind) {
    case Addr::Slot: {
        int off = frame.slotOffset[a.slot] + a.off;
        if (fits12(off)) return {Reg::s0, off};
        li(Reg::t6, off);
        emit("add t6, s0, t6");
        return {Reg::t6, 0};
    }
    case Addr::Global:
        emit("la t6, " + a.global);
        if (fits12(a.off)) return {Reg::t6, a.off};
        li(Reg::t4, a.off);
        emit("add t6, t6, t4");
        return {Reg::t6, 0};
    case Addr::Reg: {
        int b = src(a.reg, Reg::t6);
        if (fits12(a.off)) return {b, a.off};
        li(Reg::t4, a.off);
        emit(cat("add t6, ", r(b), ", t4"));
        return {Reg::t6, 0};
    }
    }
    return {Reg::zero, 0};
}

// MARK: Functions

void CodeGen::function(const IRFunction& fn, const Allocation& a) {
    f = &fn;
    alloc = &a;
    lastLine = -1;
    layoutFrame();
    frames_.push_back(frame);
    origin = AsmOrigin{0, 0, fn.line};
    directive(".globl " + fn.name);
    AsmLine lab;
    lab.kind = AsmLine::Kind::Label;
    lab.text = fn.name + ":";
    lab.origin = origin;
    lab.function = fn.name;
    out.push_back(lab);
    prologue();
    // Copy incoming arguments out of a0–a7.
    for (int k = 0; k < (int)fn.params.size(); k++) {
        int p = fn.params[k];
        const Location* loc = a.locationOf(p);
        if (!loc) continue;
        if (loc->kind == Location::Register) {
            if (loc->n != Reg::a(k)) emit(cat("mv ", r(loc->n), ", ", r(Reg::a(k))), "parameter " + fn.vregs[p].name);
        } else {
            memOp("sw", Reg::a(k), Reg::s0, frame.spillOffset[loc->n], "parameter " + fn.vregs[p].name);
        }
    }
    for (size_t bi = 0; bi < fn.blocks.size(); bi++) {
        const Block& b = *fn.blocks[bi];
        if (bi > 0 || predecessorsOfEntry()) {
            AsmLine l;
            l.kind = AsmLine::Kind::Label;
            l.text = label(fn.name, b) + ":";
            l.function = fn.name;
            out.push_back(l);
        }
        for (auto& i : b.insts) {
            origin = AsmOrigin{i.id, i.origin, i.line};
            sourceComment(i.line);
            inst(i);
        }
        origin = AsmOrigin{b.termID, b.termOrigin, b.termLine};
        sourceComment(b.termLine);
        terminator(b, bi + 1 < fn.blocks.size() ? fn.blocks[bi + 1].get() : nullptr);
    }
    AsmLine ret;
    ret.kind = AsmLine::Kind::Label;
    ret.text = ".L" + fn.name + "_return:";
    ret.function = fn.name;
    out.push_back(ret);
    origin = AsmOrigin{0, 0, 0};
    epilogue();
    origin.reset();
}

bool CodeGen::predecessorsOfEntry() const {
    if (f->blocks.empty()) return false;
    int e = f->blocks.front()->id;
    for (auto& b : f->blocks)
        for (int s : b->term.successors())
            if (s == e) return true;
    return false;
}

void CodeGen::sourceComment(int line) {
    if (line <= 0 || line == lastLine || line > (int)sourceLines.size()) return;
    lastLine = line;
    std::string text = trimmed(sourceLines[line - 1]);
    if (!text.empty()) comment(cat(line, ": ", text));
}

void CodeGen::prologue() {
    if (!frame.hasFrame) {
        comment("leaf function: no calls, nothing on the stack, so no frame is needed");
        return;
    }
    int size = frame.size;
    if (size <= 2047) {
        emit(cat("addi sp, sp, -", size), cat("allocate the ", size, "-byte frame"));
        emit(cat("sw ra, ", size - 4, "(sp)"), "save return address");
        emit(cat("sw s0, ", size - 8, "(sp)"), "save caller's frame pointer");
        emit(cat("addi s0, sp, ", size), "s0 = frame pointer");
    } else {
        emit("sw ra, -4(sp)", "save return address");
        emit("sw s0, -8(sp)", "save caller's frame pointer");
        emit("mv s0, sp", "s0 = frame pointer");
        li(Reg::t6, size);
        emit("sub sp, sp, t6", cat("allocate the ", size, "-byte frame"));
    }
    for (int s : alloc->usedCalleeSaved) memOp("sw", s, Reg::s0, frame.savedOffset.at(s), r(s) + " is callee-saved");
}

void CodeGen::epilogue() {
    if (!frame.hasFrame) {
        emit("ret");
        return;
    }
    for (int s : alloc->usedCalleeSaved) memOp("lw", s, Reg::s0, frame.savedOffset.at(s), "restore " + r(s));
    int size = frame.size;
    if (size <= 2047) {
        emit(cat("lw ra, ", size - 4, "(sp)"), "restore return address");
        emit(cat("lw s0, ", size - 8, "(sp)"), "restore caller's frame pointer");
        emit(cat("addi sp, sp, ", size), "free the frame");
    } else {
        emit("lw ra, -4(s0)", "restore return address");
        emit("mv sp, s0", "free the frame");
        emit("lw s0, -8(sp)", "restore caller's frame pointer");
    }
    emit("ret");
}

// MARK: Instructions

static const char* rTypeName(IROp op) {
    switch (op) {
    case IROp::Add: return "add";
    case IROp::Sub: return "sub";
    case IROp::Mul: return "mul";
    case IROp::Div: return "div";
    case IROp::Divu: return "divu";
    case IROp::Rem: return "rem";
    case IROp::Remu: return "remu";
    case IROp::And: return "and";
    case IROp::Or: return "or";
    case IROp::Xor: return "xor";
    case IROp::Shl: return "sll";
    case IROp::Shr: return "srl";
    case IROp::Sar: return "sra";
    case IROp::Lt: return "slt";
    case IROp::Ltu: return "sltu";
    default: return nullptr;
    }
}

static const char* iTypeName(IROp op) {
    switch (op) {
    case IROp::Add: return "addi";
    case IROp::And: return "andi";
    case IROp::Or: return "ori";
    case IROp::Xor: return "xori";
    case IROp::Lt: return "slti";
    case IROp::Ltu: return "sltiu";
    default: return nullptr;
    }
}

static const char* shiftImmName(IROp op) {
    switch (op) {
    case IROp::Shl: return "slli";
    case IROp::Shr: return "srli";
    case IROp::Sar: return "srai";
    default: return nullptr;
    }
}

void CodeGen::inst(const Inst& i) {
    const InstKind& k = i.kind;
    auto finish = [&](const Dst& d) {
        if (d.spillSlot >= 0) memOp("sw", Reg::t6, Reg::s0, frame.spillOffset[d.spillSlot], "spill " + f->vregs[d.vreg].name);
    };
    switch (k.kind) {
    case InstKind::Bin: binary(k.op, k.d, k.a, k.b); break;
    case InstKind::Un: {
        int x = src(k.a, Reg::t5);
        Dst d = dst(k.d);
        switch (k.uop) {
        case UnOp::Neg: emit(cat("neg ", r(d.reg), ", ", r(x))); break;
        case UnOp::Not: emit(cat("not ", r(d.reg), ", ", r(x))); break;
        case UnOp::Sext8:
            emit(cat("slli ", r(d.reg), ", ", r(x), ", 24"), "sign-extend");
            emit(cat("srai ", r(d.reg), ", ", r(d.reg), ", 24"), "the low byte");
            break;
        }
        finish(d);
        break;
    }
    case InstKind::Mov: {
        Dst d = dst(k.d);
        if (k.a.isImm) {
            li(d.reg, k.a.value);
        } else {
            int x = src(k.a, d.reg);
            if (x != d.reg) emit(cat("mv ", r(d.reg), ", ", r(x)));
        }
        finish(d);
        break;
    }
    case InstKind::Load: {
        auto [base, off] = address(k.addr);
        Dst d = dst(k.d);
        emit(cat(k.size == 1 ? "lb" : "lw", " ", r(d.reg), ", ", off, "(", r(base), ")"));
        finish(d);
        break;
    }
    case InstKind::Store: {
        int x = src(k.a, Reg::t5);
        auto [base, off] = address(k.addr);
        emit(cat(k.size == 1 ? "sb" : "sw", " ", r(x), ", ", off, "(", r(base), ")"));
        break;
    }
    case InstKind::AddrOf: {
        Dst d = dst(k.d);
        const Addr& a = k.addr;
        switch (a.kind) {
        case Addr::Slot: {
            int off = frame.slotOffset[a.slot] + a.off;
            std::string nm = "&" + f->slots[a.slot].name.substr(1);
            if (fits12(off)) {
                emit(cat("addi ", r(d.reg), ", s0, ", off), nm);
            } else {
                li(d.reg, off);
                emit(cat("add ", r(d.reg), ", s0, ", r(d.reg)), nm);
            }
            break;
        }
        case Addr::Global:
            emit(cat("la ", r(d.reg), ", ", a.global), "&" + a.global);
            if (a.off != 0) addImm(d.reg, d.reg, a.off);
            break;
        case Addr::Reg: {
            int x = src(a.reg, Reg::t5);
            addImm(d.reg, x, a.off);
            break;
        }
        }
        finish(d);
        break;
    }
    case InstKind::Call: {
        for (int ak = 0; ak < (int)k.args.size(); ak++) {
            int target = Reg::a(ak);
            const Operand& a = k.args[ak];
            if (a.isImm) {
                li(target, a.value);
            } else if (const Location* loc = alloc->locationOf(a.value)) {
                if (loc->kind == Location::Register) emit(cat("mv ", r(target), ", ", r(loc->n)));
                else memOp("lw", target, Reg::s0, frame.spillOffset[loc->n]);
            }
        }
        emit("call " + k.callee);
        if (k.d) {
            Dst d = dst(k.d);
            if (d.reg != Reg::a0) emit(cat("mv ", r(d.reg), ", a0"), "result");
            finish(d);
        }
        break;
    }
    }
}

void CodeGen::addImm(int rd, int rs, int32_t c) {
    if (c == 0) {
        if (rd != rs) emit(cat("mv ", r(rd), ", ", r(rs)));
    } else if (fits12(c)) {
        emit(cat("addi ", r(rd), ", ", r(rs), ", ", c));
    } else {
        li(Reg::t4, c);
        emit(cat("add ", r(rd), ", ", r(rs), ", t4"));
    }
}

void CodeGen::binary(IROp op, int d, Operand a, Operand b) {
    auto finish = [&](const Dst& x) {
        if (x.spillSlot >= 0) memOp("sw", Reg::t6, Reg::s0, frame.spillOffset[x.spillSlot], "spill " + f->vregs[x.vreg].name);
    };
    if (a.isImm && !b.isImm && irIsCommutative(op)) std::swap(a, b);
    // Immediate forms: addi, andi, slti, slli…
    if (b.isImm) {
        int32_t c = b.value;
        if (const char* name = shiftImmName(op)) {
            int x = src(a, Reg::t5);
            Dst rd = dst(d);
            emit(cat(name, " ", r(rd.reg), ", ", r(x), ", ", c & 31));
            finish(rd);
            return;
        }
        IROp opI = op;
        int32_t cI = c;
        if (op == IROp::Sub && c != INT32_MIN) {
            opI = IROp::Add;
            cI = -c;
        }
        if (const char* name = iTypeName(opI); name && fits12(cI)) {
            int x = src(a, Reg::t5);
            Dst rd = dst(d);
            emit(cat(name, " ", r(rd.reg), ", ", r(x), ", ", cI));
            finish(rd);
            return;
        }
        if ((op == IROp::Eq || op == IROp::Ne) && fits12(c)) {
            int x = src(a, Reg::t5);
            Dst rd = dst(d);
            const char* set = op == IROp::Eq ? "seqz" : "snez";
            if (c == 0) {
                emit(cat(set, " ", r(rd.reg), ", ", r(x)));
            } else {
                emit(cat("xori ", r(rd.reg), ", ", r(x), ", ", c), "zero if equal");
                emit(cat(set, " ", r(rd.reg), ", ", r(rd.reg)));
            }
            finish(rd);
            return;
        }
        if ((op == IROp::Le || op == IROp::Leu) && c != INT32_MAX && fits12((int64_t)c + 1)) {
            // x <= c  ⟺  x < c + 1
            if (op == IROp::Le || c != -1) {
                int x = src(a, Reg::t5);
                Dst rd = dst(d);
                emit(cat(op == IROp::Le ? "slti" : "sltiu", " ", r(rd.reg), ", ", r(x), ", ", c + 1));
                finish(rd);
                return;
            }
        }
    }
    int x = src(a, Reg::t5);
    int y = src(b, Reg::t6);
    Dst rd = dst(d);
    std::string R = r(rd.reg), X = r(x), Y = r(y);
    switch (op) {
    case IROp::Gt: emit(cat("slt ", R, ", ", Y, ", ", X)); break;
    case IROp::Gtu: emit(cat("sltu ", R, ", ", Y, ", ", X)); break;
    case IROp::Le:
        emit(cat("slt ", R, ", ", Y, ", ", X), "b < a …");
        emit(cat("xori ", R, ", ", R, ", 1"), "… a <= b is the opposite");
        break;
    case IROp::Leu:
        emit(cat("sltu ", R, ", ", Y, ", ", X));
        emit(cat("xori ", R, ", ", R, ", 1"));
        break;
    case IROp::Ge:
        emit(cat("slt ", R, ", ", X, ", ", Y), "a < b …");
        emit(cat("xori ", R, ", ", R, ", 1"), "… a >= b is the opposite");
        break;
    case IROp::Geu:
        emit(cat("sltu ", R, ", ", X, ", ", Y));
        emit(cat("xori ", R, ", ", R, ", 1"));
        break;
    case IROp::Eq:
        emit(cat("sub ", R, ", ", X, ", ", Y));
        emit(cat("seqz ", R, ", ", R));
        break;
    case IROp::Ne:
        emit(cat("sub ", R, ", ", X, ", ", Y));
        emit(cat("snez ", R, ", ", R));
        break;
    default: emit(cat(rTypeName(op), " ", R, ", ", X, ", ", Y)); break;
    }
    finish(rd);
}

void CodeGen::terminator(const Block& b, const Block* next) {
    const Terminator& t = b.term;
    switch (t.kind) {
    case Terminator::Jump:
        if (!next || next->id != t.t) emit("j " + label(f->name, *f->block(t.t)));
        break;
    case Terminator::Branch: {
        int x = src(t.a, Reg::t5);
        int y = src(t.b, Reg::t6);
        IROp op = t.op;
        int tt = t.t, ee = t.f;
        // Branch to the far block; fall through to the one laid out next.
        if (next && next->id == tt) {
            op = irNegated(op);
            std::swap(tt, ee);
        }
        std::string tl = label(f->name, *f->block(tt));
        std::string X = r(x), Y = r(y);
        switch (op) {
        case IROp::Eq: emit(y == 0 ? cat("beqz ", X, ", ", tl) : cat("beq ", X, ", ", Y, ", ", tl)); break;
        case IROp::Ne: emit(y == 0 ? cat("bnez ", X, ", ", tl) : cat("bne ", X, ", ", Y, ", ", tl)); break;
        case IROp::Lt: emit(cat("blt ", X, ", ", Y, ", ", tl)); break;
        case IROp::Ge: emit(cat("bge ", X, ", ", Y, ", ", tl)); break;
        case IROp::Gt: emit(cat("bgt ", X, ", ", Y, ", ", tl)); break;
        case IROp::Le: emit(cat("ble ", X, ", ", Y, ", ", tl)); break;
        case IROp::Ltu: emit(cat("bltu ", X, ", ", Y, ", ", tl)); break;
        case IROp::Geu: emit(cat("bgeu ", X, ", ", Y, ", ", tl)); break;
        case IROp::Gtu: emit(cat("bgtu ", X, ", ", Y, ", ", tl)); break;
        case IROp::Leu: emit(cat("bleu ", X, ", ", Y, ", ", tl)); break;
        default: break;
        }
        if (!next || next->id != ee) emit("j " + label(f->name, *f->block(ee)));
        break;
    }
    case Terminator::Ret:
        if (t.hasValue) {
            if (t.value.isImm) {
                li(Reg::a0, t.value.value);
            } else {
                int x = src(t.value, Reg::a0);
                if (x != Reg::a0) emit("mv a0, " + r(x), "return value");
            }
        }
        if (next) emit("j .L" + f->name + "_return");
        break;
    }
}

}  // namespace sp
