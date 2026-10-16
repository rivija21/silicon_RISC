#include "IR.h"

namespace sp {

std::string Operand::desc() const { return isImm ? cat("imm(", value, ")") : cat("v(", value, ")"); }

const char* irOpName(IROp op) {
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
    case IROp::Shl: return "shl";
    case IROp::Shr: return "shr";
    case IROp::Sar: return "sar";
    case IROp::Eq: return "eq";
    case IROp::Ne: return "ne";
    case IROp::Lt: return "lt";
    case IROp::Le: return "le";
    case IROp::Gt: return "gt";
    case IROp::Ge: return "ge";
    case IROp::Ltu: return "ltu";
    case IROp::Leu: return "leu";
    case IROp::Gtu: return "gtu";
    case IROp::Geu: return "geu";
    }
    return "?";
}

const char* irOpSymbol(IROp op) {
    switch (op) {
    case IROp::Add: return "+";
    case IROp::Sub: return "-";
    case IROp::Mul: return "*";
    case IROp::Div: return "/";
    case IROp::Divu: return "/u";
    case IROp::Rem: return "%";
    case IROp::Remu: return "%u";
    case IROp::And: return "&";
    case IROp::Or: return "|";
    case IROp::Xor: return "^";
    case IROp::Shl: return "<<";
    case IROp::Shr: return ">>u";
    case IROp::Sar: return ">>";
    case IROp::Eq: return "==";
    case IROp::Ne: return "!=";
    case IROp::Lt: return "<";
    case IROp::Le: return "<=";
    case IROp::Gt: return ">";
    case IROp::Ge: return ">=";
    case IROp::Ltu: return "<u";
    case IROp::Leu: return "<=u";
    case IROp::Gtu: return ">u";
    case IROp::Geu: return ">=u";
    }
    return "?";
}

bool irIsComparison(IROp op) {
    switch (op) {
    case IROp::Eq: case IROp::Ne: case IROp::Lt: case IROp::Le: case IROp::Gt: case IROp::Ge:
    case IROp::Ltu: case IROp::Leu: case IROp::Gtu: case IROp::Geu: return true;
    default: return false;
    }
}

bool irIsCommutative(IROp op) {
    switch (op) {
    case IROp::Add: case IROp::Mul: case IROp::And: case IROp::Or: case IROp::Xor: case IROp::Eq: case IROp::Ne: return true;
    default: return false;
    }
}

IROp irSwapped(IROp op) {
    switch (op) {
    case IROp::Lt: return IROp::Gt;
    case IROp::Gt: return IROp::Lt;
    case IROp::Le: return IROp::Ge;
    case IROp::Ge: return IROp::Le;
    case IROp::Ltu: return IROp::Gtu;
    case IROp::Gtu: return IROp::Ltu;
    case IROp::Leu: return IROp::Geu;
    case IROp::Geu: return IROp::Leu;
    default: return op;
    }
}

IROp irNegated(IROp op) {
    switch (op) {
    case IROp::Eq: return IROp::Ne;
    case IROp::Ne: return IROp::Eq;
    case IROp::Lt: return IROp::Ge;
    case IROp::Ge: return IROp::Lt;
    case IROp::Gt: return IROp::Le;
    case IROp::Le: return IROp::Gt;
    case IROp::Ltu: return IROp::Geu;
    case IROp::Geu: return IROp::Ltu;
    case IROp::Gtu: return IROp::Leu;
    case IROp::Leu: return IROp::Gtu;
    default: return op;
    }
}

int32_t irFold(IROp op, int32_t a, int32_t b) {
    uint32_t ua = (uint32_t)a, ub = (uint32_t)b;
    switch (op) {
    case IROp::Add: return RV::add(a, b);
    case IROp::Sub: return RV::sub(a, b);
    case IROp::Mul: return RV::mul(a, b);
    case IROp::Div: return RV::div(a, b);
    case IROp::Divu: return (int32_t)RV::divu(ua, ub);
    case IROp::Rem: return RV::rem(a, b);
    case IROp::Remu: return (int32_t)RV::remu(ua, ub);
    case IROp::And: return a & b;
    case IROp::Or: return a | b;
    case IROp::Xor: return a ^ b;
    case IROp::Shl: return (int32_t)(ua << (ub & 31));
    case IROp::Shr: return (int32_t)(ua >> (ub & 31));
    case IROp::Sar: return a >> (ub & 31);
    case IROp::Eq: return a == b ? 1 : 0;
    case IROp::Ne: return a != b ? 1 : 0;
    case IROp::Lt: return a < b ? 1 : 0;
    case IROp::Le: return a <= b ? 1 : 0;
    case IROp::Gt: return a > b ? 1 : 0;
    case IROp::Ge: return a >= b ? 1 : 0;
    case IROp::Ltu: return ua < ub ? 1 : 0;
    case IROp::Leu: return ua <= ub ? 1 : 0;
    case IROp::Gtu: return ua > ub ? 1 : 0;
    case IROp::Geu: return ua >= ub ? 1 : 0;
    }
    return 0;
}

int32_t unFold(UnOp op, int32_t a) {
    switch (op) {
    case UnOp::Neg: return RV::neg(a);
    case UnOp::Not: return ~a;
    case UnOp::Sext8: return (int32_t)(int8_t)(uint8_t)(uint32_t)a;
    }
    return a;
}

// MARK: - Instructions

int Inst::def() const {
    switch (kind.kind) {
    case InstKind::Bin: case InstKind::Un: case InstKind::Mov: case InstKind::Load: case InstKind::AddrOf: case InstKind::Call:
        return kind.d;
    case InstKind::Store: return 0;
    }
    return 0;
}

std::vector<int> Inst::uses() const {
    std::vector<int> out;
    auto op = [&](const Operand& o) {
        if (o.isV()) out.push_back(o.value);
    };
    auto ad = [&](const Addr& a) {
        if (int r = a.baseVReg()) out.push_back(r);
    };
    switch (kind.kind) {
    case InstKind::Bin: op(kind.a); op(kind.b); break;
    case InstKind::Un: case InstKind::Mov: op(kind.a); break;
    case InstKind::Load: case InstKind::AddrOf: ad(kind.addr); break;
    case InstKind::Store: op(kind.a); ad(kind.addr); break;
    case InstKind::Call: for (auto& a : kind.args) op(a); break;
    }
    return out;
}

bool Inst::usesReg(int r) const {
    for (int u : uses())
        if (u == r) return true;
    return false;
}

bool Inst::isPure() const {
    switch (kind.kind) {
    case InstKind::Bin: case InstKind::Un: case InstKind::Mov: case InstKind::AddrOf: case InstKind::Load: return true;
    default: return false;
    }
}

void Inst::mapUses(const std::function<Operand(Operand)>& f) {
    auto ma = [&](Addr& a) {
        if (a.kind == Addr::Reg) a.reg = f(a.reg);
    };
    switch (kind.kind) {
    case InstKind::Bin: kind.a = f(kind.a); kind.b = f(kind.b); break;
    case InstKind::Un: case InstKind::Mov: kind.a = f(kind.a); break;
    case InstKind::Load: case InstKind::AddrOf: ma(kind.addr); break;
    case InstKind::Store: kind.a = f(kind.a); ma(kind.addr); break;
    case InstKind::Call: for (auto& a : kind.args) a = f(a); break;
    }
}

std::vector<int> Terminator::successors() const {
    switch (kind) {
    case Jump: return {t};
    case Branch: return t == f ? std::vector<int>{t} : std::vector<int>{t, f};
    case Ret: return {};
    }
    return {};
}

std::vector<int> Terminator::uses() const {
    std::vector<int> out;
    if (kind == Branch) {
        if (a.isV()) out.push_back(a.value);
        if (b.isV()) out.push_back(b.value);
    } else if (kind == Ret && hasValue && value.isV()) {
        out.push_back(value.value);
    }
    return out;
}

void Terminator::mapUses(const std::function<Operand(Operand)>& fn) {
    if (kind == Branch) {
        a = fn(a);
        b = fn(b);
    } else if (kind == Ret && hasValue) {
        value = fn(value);
    }
}

void Terminator::mapTargets(const std::function<int(int)>& fn) {
    if (kind == Jump) t = fn(t);
    else if (kind == Branch) {
        t = fn(t);
        f = fn(f);
    }
}

// MARK: - Functions and modules

int IRFunction::newVReg(const std::string& n, bool variable) {
    int k = (int)vregs.size();
    vregs.push_back({n.empty() ? cat("%t", k) : n, variable});
    return k;
}

Block* IRFunction::block(int id) const {
    for (auto& b : blocks)
        if (b->id == id) return b.get();
    return nullptr;
}

std::shared_ptr<IRFunction> IRFunction::copy() const {
    auto f = std::make_shared<IRFunction>(name, ids);
    f->params = params;
    for (auto& b : blocks) f->blocks.push_back(std::make_shared<Block>(*b));
    f->slots = slots;
    f->vregs = vregs;
    f->returnsValue = returnsValue;
    f->line = line;
    f->nextBlock = nextBlock;
    return f;
}

int IRFunction::instructionCount() const {
    int n = 0;
    for (auto& b : blocks) n += (int)b->insts.size() + 1;
    return n;
}

std::map<int, std::vector<int>> IRFunction::predecessors() const {
    std::map<int, std::vector<int>> p;
    for (auto& b : blocks) {
        p[b->id];
        for (int s : b->term.successors()) p[s].push_back(b->id);
    }
    return p;
}

std::shared_ptr<IRModule> IRModule::copy() const {
    auto m = std::make_shared<IRModule>();
    for (auto& f : functions) m->functions.push_back(f->copy());
    m->data = data;
    return m;
}

int IRModule::instructionCount() const {
    int n = 0;
    for (auto& f : functions) n += f->instructionCount();
    return n;
}

// MARK: - Printing

std::string IRPrinter::operand(const Operand& o, const IRFunction& f) {
    return o.isImm ? std::to_string(o.value) : f.vregs[o.value].name;
}

std::string IRPrinter::addr(const Addr& a, const IRFunction& f) {
    auto off = [](int32_t o) -> std::string {
        if (o == 0) return "";
        if (o > 0) return cat(" + ", o);
        return cat(" - ", -(int64_t)o);
    };
    switch (a.kind) {
    case Addr::Slot: return cat("[", f.slots[a.slot].name, off(a.off), "]");
    case Addr::Global: return cat("[@", a.global, off(a.off), "]");
    case Addr::Reg: return cat("[", operand(a.reg, f), off(a.off), "]");
    }
    return "?";
}

std::string IRPrinter::inst(const Inst& i, const IRFunction& f) {
    auto v = [&](int r) { return f.vregs[r].name; };
    auto o = [&](const Operand& x) { return operand(x, f); };
    const InstKind& k = i.kind;
    switch (k.kind) {
    case InstKind::Bin: return cat(v(k.d), " = ", o(k.a), " ", irOpSymbol(k.op), " ", o(k.b));
    case InstKind::Un:
        switch (k.uop) {
        case UnOp::Neg: return cat(v(k.d), " = -", o(k.a));
        case UnOp::Not: return cat(v(k.d), " = ~", o(k.a));
        case UnOp::Sext8: return cat(v(k.d), " = (char) ", o(k.a));
        }
        break;
    case InstKind::Mov: return cat(v(k.d), " = ", o(k.a));
    case InstKind::Load: return cat(v(k.d), " = load", k.size == 1 ? ".b" : ".w", " ", addr(k.addr, f));
    case InstKind::Store: return cat("store", k.size == 1 ? ".b" : ".w", " ", addr(k.addr, f), " ← ", o(k.a));
    case InstKind::AddrOf: return cat(v(k.d), " = &", addr(k.addr, f));
    case InstKind::Call: {
        std::vector<std::string> as;
        for (auto& a : k.args) as.push_back(o(a));
        std::string c = cat("call ", k.callee, "(", join(as, ", "), ")");
        return k.d ? cat(v(k.d), " = ", c) : c;
    }
    }
    return "?";
}

std::string IRPrinter::term(const Terminator& t, const IRFunction& f) {
    auto name = [&](int id) {
        Block* b = f.block(id);
        return b ? b->name : cat("?", id);
    };
    switch (t.kind) {
    case Terminator::Jump: return "goto " + name(t.t);
    case Terminator::Branch:
        return cat("if ", operand(t.a, f), " ", irOpSymbol(t.op), " ", operand(t.b, f), " goto ", name(t.t), " else ", name(t.f));
    case Terminator::Ret: return t.hasValue ? "return " + operand(t.value, f) : "return";
    }
    return "?";
}

std::string IRPrinter::function(const IRFunction& f) {
    std::vector<std::string> ps;
    for (int p : f.params) ps.push_back(f.vregs[p].name);
    std::string s = cat("function ", f.name, "(", join(ps, ", "), ")\n");
    if (!f.slots.empty()) {
        std::vector<std::string> sl;
        for (auto& x : f.slots) sl.push_back(cat(x.name, " (", x.size, " B)"));
        s += "  stack slots: " + join(sl, ", ") + "\n";
    }
    for (auto& b : f.blocks) {
        s += b->name + ":\n";
        for (auto& i : b->insts) s += "    " + inst(i, f) + "\n";
        s += "    " + term(b->term, f) + "\n";
    }
    return s;
}

std::string IRPrinter::module(const IRModule& m) {
    std::vector<std::string> fs;
    for (auto& f : m.functions) fs.push_back(function(*f));
    return join(fs, "\n");
}

}  // namespace sp
