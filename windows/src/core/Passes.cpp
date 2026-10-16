#include "IR.h"

namespace sp {

const std::vector<PassID>& allPasses() {
    static const std::vector<PassID> all = {PassID::Mem2Reg, PassID::ConstFold, PassID::Simplify, PassID::CopyProp,
                                            PassID::CSE, PassID::DCE, PassID::CFG};
    return all;
}

const char* passRawValue(PassID p) {
    switch (p) {
    case PassID::Mem2Reg: return "mem2reg";
    case PassID::ConstFold: return "constFold";
    case PassID::Simplify: return "simplify";
    case PassID::CopyProp: return "copyProp";
    case PassID::CSE: return "cse";
    case PassID::DCE: return "dce";
    case PassID::CFG: return "cfg";
    }
    return "?";
}

std::optional<PassID> passFromRaw(const std::string& s) {
    for (PassID p : allPasses())
        if (s == passRawValue(p)) return p;
    return std::nullopt;
}

const char* passTitle(PassID p) {
    switch (p) {
    case PassID::Mem2Reg: return "Promote locals to registers (mem2reg)";
    case PassID::ConstFold: return "Constant propagation & folding";
    case PassID::Simplify: return "Algebraic simplification & strength reduction";
    case PassID::CopyProp: return "Copy propagation";
    case PassID::CSE: return "Common subexpressions & redundant loads";
    case PassID::DCE: return "Dead code elimination";
    case PassID::CFG: return "Control-flow cleanup";
    }
    return "?";
}

const char* passShortTitle(PassID p) {
    switch (p) {
    case PassID::Mem2Reg: return "mem2reg";
    case PassID::ConstFold: return "Const fold";
    case PassID::Simplify: return "Simplify";
    case PassID::CopyProp: return "Copy prop";
    case PassID::CSE: return "CSE";
    case PassID::DCE: return "DCE";
    case PassID::CFG: return "CFG";
    }
    return "?";
}

const char* passExplanation(PassID p) {
    switch (p) {
    case PassID::Mem2Reg:
        return "A local scalar whose address is never taken (no &x) cannot be reached through a pointer, so it does not need memory at all. "
               "Its loads and stores become plain register moves. This is the biggest single win: it removes most lw/sw instructions.";
    case PassID::ConstFold:
        return "Tracks which virtual registers hold a known constant on every path (a data-flow analysis over the control-flow graph), "
               "substitutes the constants, and computes constant expressions at compile time: 3 * 4 becomes 12. Branches whose outcome "
               "is known become plain jumps.";
    case PassID::Simplify:
        return "Applies algebraic identities (x + 0 = x, x * 1 = x, x - x = 0) and replaces expensive operations by cheap ones: "
               "x * 8 → x << 3, unsigned x / 16 → x >> 4, and signed x / 4 → shifts with a rounding fix-up. It also folds "
               "constant offsets into addresses, so a[3] becomes a single lw with offset 12.";
    case PassID::CopyProp:
        return "After %a = %b, later uses of %a can read %b directly. The copy itself then becomes dead and DCE removes it.";
    case PassID::CSE:
        return "If the same expression is computed twice in a block with unchanged operands, the second one reuses the first result. "
               "Loads are reused until the next store or call, and a load right after a store to the same address gets the stored value.";
    case PassID::DCE:
        return "Removes instructions whose result is never used. Liveness analysis runs backwards over the control-flow graph to find "
               "which registers are still needed.";
    case PassID::CFG:
        return "Removes unreachable blocks, skips blocks that only jump elsewhere, and merges a block into its predecessor when it is "
               "the only way in.";
    }
    return "";
}

// MARK: - Analyses

Liveness::Result Liveness::analyze(const IRFunction& f) {
    std::unordered_map<int, std::set<int>> use, def;
    for (auto& b : f.blocks) {
        std::set<int> u, d;
        for (auto& i : b->insts) {
            for (int r : i.uses())
                if (!d.count(r)) u.insert(r);
            if (int x = i.def()) d.insert(x);
        }
        for (int r : b->term.uses())
            if (!d.count(r)) u.insert(r);
        use[b->id] = std::move(u);
        def[b->id] = std::move(d);
    }
    Result res;
    for (auto& b : f.blocks) {
        res.liveIn[b->id];
        res.liveOut[b->id];
    }
    bool changed = true;
    while (changed) {
        changed = false;
        for (auto it = f.blocks.rbegin(); it != f.blocks.rend(); ++it) {
            const Block& b = **it;
            std::set<int> out;
            for (int s : b.term.successors()) {
                auto li = res.liveIn.find(s);
                if (li != res.liveIn.end()) out.insert(li->second.begin(), li->second.end());
            }
            std::set<int> inn = use[b.id];
            const std::set<int>& d = def[b.id];
            for (int r : out)
                if (!d.count(r)) inn.insert(r);
            if (out != res.liveOut[b.id] || inn != res.liveIn[b.id]) {
                res.liveOut[b.id] = std::move(out);
                res.liveIn[b.id] = std::move(inn);
                changed = true;
            }
        }
    }
    return res;
}

namespace {

/// Blocks in reverse post-order (each block before its successors, loops aside).
std::vector<Block*> reversePostOrder(const IRFunction& f) {
    if (f.blocks.empty()) return {};
    std::set<int> seen;
    std::vector<int> order;
    std::function<void(int)> visit = [&](int id) {
        if (seen.count(id)) return;
        seen.insert(id);
        if (Block* b = f.block(id))
            for (int s : b->term.successors()) visit(s);
        order.push_back(id);
    };
    visit(f.blocks.front()->id);
    std::vector<Block*> out;
    for (auto it = order.rbegin(); it != order.rend(); ++it)
        if (Block* b = f.block(*it)) out.push_back(b);
    return out;
}

std::unordered_map<int, int> defCounts(const IRFunction& f) {
    std::unordered_map<int, int> c;
    for (auto& b : f.blocks)
        for (auto& i : b->insts)
            if (int d = i.def()) c[d] += 1;
    return c;
}

int countOf(const std::unordered_map<int, int>& m, int k) {
    auto it = m.find(k);
    return it == m.end() ? 0 : it->second;
}

// MARK: - mem2reg

bool mem2reg(IRFunction& f) {
    std::set<int> promotable;
    for (int k = 0; k < (int)f.slots.size(); k++)
        if (f.slots[k].isScalar) promotable.insert(k);
    auto check = [&](const Addr& a, std::optional<int> size) {
        if (a.kind == Addr::Slot) {
            if (a.off != 0) promotable.erase(a.slot);
            if (size && *size != f.slots[a.slot].size) promotable.erase(a.slot);
        }
    };
    for (auto& b : f.blocks) {
        for (auto& i : b->insts) {
            switch (i.kind.kind) {
            case InstKind::AddrOf:
                if (i.kind.addr.kind == Addr::Slot) promotable.erase(i.kind.addr.slot);
                break;
            case InstKind::Load: check(i.kind.addr, i.kind.size); break;
            case InstKind::Store: check(i.kind.addr, i.kind.size); break;
            default: break;
            }
        }
    }
    if (promotable.empty()) return false;

    // A parameter's slot can reuse the incoming parameter register itself.
    std::map<int, int> reg;
    std::set<int> paramStore;
    if (!f.blocks.empty()) {
        for (auto& i : f.blocks.front()->insts) {
            const InstKind& k = i.kind;
            if (k.kind != InstKind::Store || !k.a.isV() || k.addr.kind != Addr::Slot || k.addr.off != 0) continue;
            int p = k.a.value, s = k.addr.slot;
            if (std::find(f.params.begin(), f.params.end(), p) == f.params.end() || !promotable.count(s) || reg.count(s)) continue;
            reg[s] = p;
            paramStore.insert(i.id);
        }
    }
    for (int s : promotable) {
        if (reg.count(s)) continue;
        std::string n = f.slots[s].name;
        reg[s] = f.newVReg("%" + n.substr(1), true);
    }
    for (auto& b : f.blocks) {
        std::vector<Inst> out;
        for (Inst i : b->insts) {
            InstKind& k = i.kind;
            if (k.kind == InstKind::Load && k.addr.kind == Addr::Slot && k.addr.off == 0 && reg.count(k.addr.slot)) {
                k = InstKind::mov(k.d, Operand::v(reg[k.addr.slot]));
            } else if (k.kind == InstKind::Store && k.addr.kind == Addr::Slot && k.addr.off == 0 && reg.count(k.addr.slot)) {
                int r = reg[k.addr.slot];
                Operand v = k.a;
                int size = k.size;
                if (paramStore.count(i.id)) {
                    // A char parameter still needs truncating to 8 bits.
                    if (size == 1) k = InstKind::un(UnOp::Sext8, r, v);
                    else continue;
                } else {
                    k = size == 1 ? InstKind::un(UnOp::Sext8, r, v) : InstKind::mov(r, v);
                }
            }
            out.push_back(std::move(i));
        }
        b->insts = std::move(out);
    }
    // Remove the now-unused slots, renumbering the rest.
    std::map<int, int> newIndex;
    std::vector<Slot> kept;
    for (int k = 0; k < (int)f.slots.size(); k++) {
        if (promotable.count(k)) continue;
        newIndex[k] = (int)kept.size();
        kept.push_back(f.slots[k]);
    }
    f.slots = std::move(kept);
    for (auto& b : f.blocks) {
        for (auto& i : b->insts) {
            InstKind& k = i.kind;
            if ((k.kind == InstKind::Load || k.kind == InstKind::Store || k.kind == InstKind::AddrOf) && k.addr.kind == Addr::Slot) {
                k.addr.slot = newIndex[k.addr.slot];
            }
        }
    }
    return true;
}

// MARK: - Constant propagation

struct Lat {
    enum Kind : uint8_t { Top, Const, Bottom };
    Kind k = Top;
    int32_t c = 0;
    static Lat top() { return {Top, 0}; }
    static Lat cst(int32_t v) { return {Const, v}; }
    static Lat bottom() { return {Bottom, 0}; }
    bool operator==(const Lat& o) const { return k == o.k && (k != Const || c == o.c); }
    Lat meet(const Lat& o) const {
        if (k == Top) return o;
        if (o.k == Top) return *this;
        if (k == Const && o.k == Const) return c == o.c ? *this : bottom();
        return bottom();
    }
};

Lat evaluate(const Inst& i, const std::vector<Lat>& st) {
    auto val = [&](const Operand& o) { return o.isImm ? Lat::cst(o.value) : st[o.value]; };
    const InstKind& k = i.kind;
    switch (k.kind) {
    case InstKind::Bin: {
        Lat x = val(k.a), y = val(k.b);
        if (x.k == Lat::Const && y.k == Lat::Const) return Lat::cst(irFold(k.op, x.c, y.c));
        if ((k.op == IROp::Mul || k.op == IROp::And) && (x == Lat::cst(0) || y == Lat::cst(0))) return Lat::cst(0);
        if (x.k == Lat::Bottom || y.k == Lat::Bottom) return Lat::bottom();
        return Lat::top();
    }
    case InstKind::Un: {
        Lat l = val(k.a);
        if (l.k == Lat::Const) return Lat::cst(unFold(k.uop, l.c));
        return l;
    }
    case InstKind::Mov: return val(k.a);
    default: return Lat::bottom();
    }
}

std::vector<Lat> transfer(const Block& b, std::vector<Lat> st) {
    for (auto& i : b.insts)
        if (int d = i.def()) st[d] = evaluate(i, st);
    return st;
}

bool constProp(IRFunction& f) {
    size_t n = f.vregs.size();
    auto order = reversePostOrder(f);
    if (order.empty()) return false;
    int entry = order.front()->id;
    auto preds = f.predecessors();
    std::unordered_map<int, std::vector<Lat>> outs;
    auto inState = [&](const Block& b) {
        if (b.id == entry) {
            // Parameters (and anything read before it is written) are unknown on entry.
            std::vector<Lat> s(n, Lat::bottom());
            auto p = preds.find(b.id);
            if (p != preds.end()) {
                for (int q : p->second) {
                    auto o = outs.find(q);
                    if (o != outs.end())
                        for (size_t k = 0; k < n; k++) s[k] = s[k].meet(o->second[k]);
                }
            }
            return s;
        }
        std::vector<Lat> s(n, Lat::top());
        auto p = preds.find(b.id);
        if (p != preds.end()) {
            for (int q : p->second) {
                auto o = outs.find(q);
                if (o != outs.end())
                    for (size_t k = 0; k < n; k++) s[k] = s[k].meet(o->second[k]);
            }
        }
        return s;
    };
    bool changed = true;
    int rounds = 0;
    while (changed && rounds < 50) {
        changed = false;
        rounds += 1;
        for (Block* b : order) {
            auto o = transfer(*b, inState(*b));
            auto it = outs.find(b->id);
            if (it == outs.end() || it->second != o) {
                outs[b->id] = std::move(o);
                changed = true;
            }
        }
    }
    // Rewrite using the facts.
    bool modified = false;
    for (Block* b : order) {
        std::vector<Lat> st = inState(*b);
        auto sub = [&](Operand o) {
            if (o.isV() && st[o.value].k == Lat::Const) {
                modified = true;
                return Operand::imm(st[o.value].c);
            }
            return o;
        };
        for (auto& i : b->insts) {
            Lat result = evaluate(i, st);
            i.mapUses(sub);
            int d = i.def();
            if (d && result.k == Lat::Const && i.isPure()) {
                bool alreadyMov = i.kind.kind == InstKind::Mov && i.kind.a.isImm && i.kind.a.value == result.c;
                if (!alreadyMov) {
                    i.kind = InstKind::mov(d, Operand::imm(result.c));
                    modified = true;
                }
            }
            if (d) st[d] = result;
        }
        b->term.mapUses(sub);
        if (b->term.kind == Terminator::Branch && b->term.a.isImm && b->term.b.isImm) {
            const Terminator& t = b->term;
            b->term = Terminator::jump(irFold(t.op, t.a.value, t.b.value) != 0 ? t.t : t.f);
            modified = true;
        }
    }
    return modified;
}

// MARK: - Algebraic simplification and strength reduction

std::optional<int32_t> log2Exact(int32_t c) {
    if (c <= 0 || (c & (c - 1)) != 0) return std::nullopt;
    return trailingZeros((uint32_t)c);
}

/// Returns a cheaper replacement for `d = a op b`, or nullopt.
std::optional<InstKind> simplifyOp(IROp op, int d, Operand a, Operand b, IRFunction& f, const std::map<int, InstKind>& avail,
                                   std::vector<Inst>& extra) {
    if (a.isImm && !b.isImm && irIsCommutative(op)) std::swap(a, b);
    if (a.isV() && b.isV() && a.value == b.value) {
        switch (op) {
        case IROp::Sub: case IROp::Xor: case IROp::Ne: case IROp::Lt: case IROp::Gt: case IROp::Ltu: case IROp::Gtu:
            return InstKind::mov(d, Operand::imm(0));
        case IROp::And: case IROp::Or: return InstKind::mov(d, a);
        case IROp::Eq: case IROp::Le: case IROp::Ge: case IROp::Leu: case IROp::Geu: return InstKind::mov(d, Operand::imm(1));
        default: break;
        }
    }
    if (!b.isImm) {
        if (a.isImm && irIsComparison(op) && b.isV()) return InstKind::bin(irSwapped(op), d, b, a);
        return std::nullopt;
    }
    int32_t c = b.value;
    switch (op) {
    case IROp::Add: case IROp::Or: case IROp::Xor: case IROp::Shl: case IROp::Shr: case IROp::Sar:
        if (c == 0) return InstKind::mov(d, a);
        if (op == IROp::Add && a.isV()) {
            auto it = avail.find(a.value);
            if (it != avail.end() && it->second.kind == InstKind::Bin && it->second.op == IROp::Add && it->second.b.isImm) {
                // (y + k) + c → y + (k + c)
                return InstKind::bin(IROp::Add, d, it->second.a, Operand::imm(RV::add(it->second.b.value, c)));
            }
        }
        break;
    case IROp::Sub:
        if (c == 0) return InstKind::mov(d, a);
        if (c != INT32_MIN) return InstKind::bin(IROp::Add, d, a, Operand::imm(-c));
        break;
    case IROp::Mul:
        if (c == 0) return InstKind::mov(d, Operand::imm(0));
        if (c == 1) return InstKind::mov(d, a);
        if (c == -1) return InstKind::un(UnOp::Neg, d, a);
        if (auto k = log2Exact(c)) return InstKind::bin(IROp::Shl, d, a, Operand::imm(*k));
        break;
    case IROp::Div:
        if (c == 1) return InstKind::mov(d, a);
        if (c == -1) return InstKind::un(UnOp::Neg, d, a);
        if (auto k = log2Exact(c); k && *k > 0 && a.isV()) {
            // Signed division rounds toward zero, but an arithmetic shift rounds down.
            // Add (2^k − 1) first when a is negative: t = (a >> 31) >>> (32 − k).
            int s = f.newVReg(), t = f.newVReg(), u = f.newVReg();
            extra.push_back(Inst{InstKind::bin(IROp::Sar, s, a, Operand::imm(31)), 0, 0, 0});
            extra.push_back(Inst{InstKind::bin(IROp::Shr, t, Operand::v(s), Operand::imm(32 - *k)), 0, 0, 0});
            extra.push_back(Inst{InstKind::bin(IROp::Add, u, a, Operand::v(t)), 0, 0, 0});
            return InstKind::bin(IROp::Sar, d, Operand::v(u), Operand::imm(*k));
        }
        break;
    case IROp::Divu:
        if (c == 1) return InstKind::mov(d, a);
        if (auto k = log2Exact(c)) return InstKind::bin(IROp::Shr, d, a, Operand::imm(*k));
        if (c < 0 && (uint32_t)c == 0x80000000u) return InstKind::bin(IROp::Shr, d, a, Operand::imm(31));
        break;
    case IROp::Remu:
        if (c == 1) return InstKind::mov(d, Operand::imm(0));
        if (log2Exact(c)) return InstKind::bin(IROp::And, d, a, Operand::imm(c - 1));
        break;
    case IROp::Rem:
        if (c == 1 || c == -1) return InstKind::mov(d, Operand::imm(0));
        break;
    case IROp::And:
        if (c == 0) return InstKind::mov(d, Operand::imm(0));
        if (c == -1) return InstKind::mov(d, a);
        break;
    case IROp::Ltu:
        if (c == 0) return InstKind::mov(d, Operand::imm(0));
        break;
    case IROp::Geu:
        if (c == 0) return InstKind::mov(d, Operand::imm(1));
        break;
    default: break;
    }
    return std::nullopt;
}

bool simplify(IRFunction& f) {
    bool changed = false;
    for (auto& bp : f.blocks) {
        Block& b = *bp;
        // Definitions made earlier in this block whose operands are still unchanged.
        std::map<int, InstKind> avail;
        auto invalidate = [&](int r) {
            avail.erase(r);
            for (auto it = avail.begin(); it != avail.end();) {
                Inst tmp{it->second, 0, 0, 0};
                if (tmp.usesReg(r)) it = avail.erase(it);
                else ++it;
            }
        };
        auto fits = [](int64_t v) { return (v < 0 ? -v : v) < 2048; };
        auto fold = [&](const Addr& a) -> Addr {
            if (a.kind != Addr::Reg || !a.reg.isV()) return a;
            auto it = avail.find(a.reg.value);
            if (it == avail.end()) return a;
            const InstKind& def = it->second;
            int32_t off = a.off;
            // Only fold while the offset still fits a 12-bit load/store immediate.
            if (def.kind == InstKind::Bin && def.op == IROp::Add && def.b.isImm && fits((int64_t)off + def.b.value))
                return Addr::ofReg(def.a, RV::add(off, def.b.value));
            if (def.kind == InstKind::AddrOf) {
                const Addr& x = def.addr;
                if (fits((int64_t)x.off + off)) {
                    Addr r = x;
                    r.off = RV::add(x.off, off);
                    return r;
                }
            }
            return a;
        };
        std::vector<Inst> out;
        for (Inst i : b.insts) {
            InstKind before = i.kind;
            InstKind& k = i.kind;
            switch (k.kind) {
            case InstKind::Load: k.addr = fold(k.addr); break;
            case InstKind::Store: k.addr = fold(k.addr); break;
            case InstKind::AddrOf: {
                Addr fa = fold(k.addr);
                if (fa.kind == Addr::Reg && fa.off == 0) k = InstKind::mov(k.d, fa.reg);
                else if (fa.kind == Addr::Reg) k = InstKind::bin(IROp::Add, k.d, fa.reg, Operand::imm(fa.off));
                else k = InstKind::addrOf(k.d, fa);
                break;
            }
            default: break;
            }
            std::vector<Inst> extra;
            if (k.kind == InstKind::Bin) {
                if (auto r = simplifyOp(k.op, k.d, k.a, k.b, f, avail, extra)) k = *r;
            }
            if (k.kind == InstKind::Un && k.uop == UnOp::Sext8 && k.a.isV()) {
                auto it = avail.find(k.a.value);
                if (it != avail.end()) {
                    // Already a sign-extended byte: the conversion does nothing.
                    const InstKind& def = it->second;
                    if ((def.kind == InstKind::Un && def.uop == UnOp::Sext8) || (def.kind == InstKind::Load && def.size == 1)) {
                        k = InstKind::mov(k.d, k.a);
                    }
                }
            }
            if (!(i.kind == before)) changed = true;
            for (Inst e : extra) {
                e.id = f.ids->next();
                e.origin = i.origin;
                e.line = i.line;
                out.push_back(e);
                if (int d = e.def()) {
                    invalidate(d);
                    avail[d] = e.kind;
                }
            }
            out.push_back(i);
            if (int d = i.def()) {
                invalidate(d);
                if (!i.usesReg(d)) avail[d] = i.kind;
            }
        }
        b.insts = std::move(out);
    }
    return changed;
}

// MARK: - Copy propagation

bool copyProp(IRFunction& f) {
    bool changed = false;
    // Global: a register written exactly once by a copy of another single-assignment
    // register (or of an unmodified parameter) can be replaced everywhere.
    auto counts = defCounts(f);
    // A parameter already holds its incoming value, so it is single-valued only if never assigned.
    std::set<int> params(f.params.begin(), f.params.end());
    auto stable = [&](int r) { return params.count(r) ? countOf(counts, r) == 0 : countOf(counts, r) == 1; };
    std::map<int, Operand> global;
    for (auto& b : f.blocks) {
        for (auto& i : b->insts) {
            const InstKind& k = i.kind;
            if (k.kind == InstKind::Mov && countOf(counts, k.d) == 1 && !params.count(k.d)) {
                if (k.a.isV() && k.a.value != k.d && stable(k.a.value)) global[k.d] = k.a;
            }
        }
    }
    auto resolve = [&](Operand o) {
        int hops = 0;
        while (o.isV() && hops < 50) {
            auto it = global.find(o.value);
            if (it == global.end()) break;
            o = it->second;
            hops += 1;
        }
        return o;
    };
    if (!global.empty()) {
        for (auto& b : f.blocks) {
            for (auto& i : b->insts) {
                InstKind before = i.kind;
                i.mapUses(resolve);
                if (!(i.kind == before)) changed = true;
            }
            Terminator t = b->term;
            b->term.mapUses(resolve);
            if (!(b->term == t)) changed = true;
        }
    }
    // Local: within a block, follow copies until either side is overwritten.
    for (auto& b : f.blocks) {
        std::map<int, Operand> copies;
        auto sub = [&](Operand o) {
            if (o.isV()) {
                auto it = copies.find(o.value);
                if (it != copies.end()) return it->second;
            }
            return o;
        };
        for (auto& i : b->insts) {
            InstKind before = i.kind;
            i.mapUses(sub);
            if (!(i.kind == before)) changed = true;
            if (int d = i.def()) {
                copies.erase(d);
                for (auto it = copies.begin(); it != copies.end();) {
                    if (it->second == Operand::v(d)) it = copies.erase(it);
                    else ++it;
                }
                if (i.kind.kind == InstKind::Mov && !(i.kind.a == Operand::v(d))) copies[d] = i.kind.a;
            }
        }
        Terminator t = b->term;
        b->term.mapUses(sub);
        if (!(b->term == t)) changed = true;
    }
    // Coalesce "%t = a + b; %x = %t" into "%x = a + b" when %t is used nowhere else.
    std::unordered_map<int, int> useCount;
    for (auto& b : f.blocks) {
        for (auto& i : b->insts)
            for (int u : i.uses()) useCount[u] += 1;
        for (int u : b->term.uses()) useCount[u] += 1;
    }
    auto defs = defCounts(f);
    for (auto& bp : f.blocks) {
        Block& b = *bp;
        if (b.insts.size() < 2) continue;
        size_t k = 1;
        while (k < b.insts.size()) {
            const InstKind& mk = b.insts[k].kind;
            if (mk.kind == InstKind::Mov && mk.a.isV()) {
                int x = mk.d, t = mk.a.value;
                if (x != t && countOf(useCount, t) == 1 && countOf(defs, t) == 1 && !f.vregs[t].isVariable && b.insts[k - 1].def() == t &&
                    std::find(f.params.begin(), f.params.end(), t) == f.params.end()) {
                    Inst prev = b.insts[k - 1];
                    if (prev.kind.kind == InstKind::Store) {
                        k += 1;
                        continue;
                    }
                    prev.kind.d = x;
                    b.insts[k - 1] = prev;
                    b.insts.erase(b.insts.begin() + k);
                    changed = true;
                    continue;
                }
            }
            k += 1;
        }
    }
    // Remove copies to self.
    for (auto& b : f.blocks) {
        size_t n = b->insts.size();
        b->insts.erase(std::remove_if(b->insts.begin(), b->insts.end(),
                                      [](const Inst& i) { return i.kind.kind == InstKind::Mov && i.kind.a.isV() && i.kind.a.value == i.kind.d; }),
                       b->insts.end());
        if (b->insts.size() != n) changed = true;
    }
    return changed;
}

// MARK: - Common subexpression elimination

std::optional<InstKind> cseKey(const InstKind& k) {
    switch (k.kind) {
    case InstKind::Bin:
        if (irIsCommutative(k.op) && k.a.desc() > k.b.desc()) return InstKind::bin(k.op, 0, k.b, k.a);
        return InstKind::bin(k.op, 0, k.a, k.b);
    case InstKind::Un: return InstKind::un(k.uop, 0, k.a);
    case InstKind::AddrOf: return InstKind::addrOf(0, k.addr);
    case InstKind::Load: return InstKind::load(0, k.addr, k.size);
    default: return std::nullopt;
    }
}

bool cse(IRFunction& f) {
    bool changed = false;
    for (auto& bp : f.blocks) {
        Block& b = *bp;
        std::map<InstKind, Operand> table;
        auto invalidate = [&](int r) {
            for (auto it = table.begin(); it != table.end();) {
                Inst tmp{it->first, 0, 0, 0};
                if (tmp.usesReg(r) || it->second == Operand::v(r)) it = table.erase(it);
                else ++it;
            }
        };
        auto dropLoads = [&] {
            for (auto it = table.begin(); it != table.end();) {
                if (it->first.kind == InstKind::Load) it = table.erase(it);
                else ++it;
            }
        };
        for (size_t idx = 0; idx < b.insts.size(); idx++) {
            const Inst i = b.insts[idx];
            if (i.kind.kind == InstKind::Store) {
                dropLoads();
                if (i.kind.size == 4) table[InstKind::load(0, i.kind.addr, 4)] = i.kind.a;
                continue;
            }
            if (i.kind.kind == InstKind::Call) {
                dropLoads();
                if (i.kind.d) invalidate(i.kind.d);
                continue;
            }
            int d = i.def();
            if (!d) continue;
            auto key = cseKey(i.kind);
            if (key) {
                auto prev = table.find(*key);
                if (prev != table.end() && !(prev->second == Operand::v(d))) {
                    b.insts[idx].kind = InstKind::mov(d, prev->second);
                    changed = true;
                    invalidate(d);
                    continue;
                }
            }
            invalidate(d);
            if (key && !i.usesReg(d)) table[*key] = Operand::v(d);
        }
    }
    return changed;
}

// MARK: - Dead code elimination

bool dce(IRFunction& f) {
    bool changed = false;
    bool again = true;
    while (again) {
        again = false;
        auto live = Liveness::analyze(f);
        for (auto& b : f.blocks) {
            std::set<int> alive;
            auto lo = live.liveOut.find(b->id);
            if (lo != live.liveOut.end()) alive = lo->second;
            for (int r : b->term.uses()) alive.insert(r);
            std::vector<Inst> keep;
            for (auto it = b->insts.rbegin(); it != b->insts.rend(); ++it) {
                const Inst& i = *it;
                int d = i.def();
                if (d && i.isPure() && !alive.count(d)) {
                    again = true;
                    changed = true;
                    continue;
                }
                if (d) alive.erase(d);
                for (int u : i.uses()) alive.insert(u);
                keep.push_back(i);
            }
            std::reverse(keep.begin(), keep.end());
            b->insts = std::move(keep);
        }
    }
    return changed;
}

// MARK: - Control-flow cleanup

bool cfgSimplify(IRFunction& f) {
    bool changed = false;
    bool again = true;
    int rounds = 0;
    while (again && rounds < 100) {
        again = false;
        rounds += 1;
        for (auto& b : f.blocks) {
            Terminator& t = b->term;
            if (t.kind == Terminator::Branch) {
                if (t.t == t.f) {
                    t = Terminator::jump(t.t);
                    again = true;
                } else if (t.a.isImm && t.b.isImm) {
                    t = Terminator::jump(irFold(t.op, t.a.value, t.b.value) != 0 ? t.t : t.f);
                    again = true;
                }
            }
        }
        // Jump threading through empty blocks.
        if (f.blocks.empty()) return changed;
        int entry = f.blocks.front()->id;
        std::map<int, int> forward;
        for (auto& b : f.blocks) {
            if (b->id != entry && b->insts.empty() && b->term.kind == Terminator::Jump && b->term.t != b->id) forward[b->id] = b->term.t;
        }
        auto finalTarget = [&](int id) {
            int x = id, hops = 0;
            while (hops < 64) {
                auto it = forward.find(x);
                if (it == forward.end()) break;
                x = it->second;
                hops += 1;
            }
            return x;
        };
        if (!forward.empty()) {
            for (auto& b : f.blocks) {
                Terminator t = b->term;
                b->term.mapTargets(finalTarget);
                if (!(b->term == t)) again = true;
            }
        }
        // Unreachable blocks.
        std::set<int> seen{entry};
        std::vector<int> work{entry};
        while (!work.empty()) {
            int id = work.back();
            work.pop_back();
            if (Block* b = f.block(id)) {
                for (int s : b->term.successors()) {
                    if (!seen.count(s)) {
                        seen.insert(s);
                        work.push_back(s);
                    }
                }
            }
        }
        size_t n = f.blocks.size();
        f.blocks.erase(std::remove_if(f.blocks.begin(), f.blocks.end(), [&](const BlockP& b) { return !seen.count(b->id); }), f.blocks.end());
        if (f.blocks.size() != n) again = true;
        // Merge a block into its only predecessor.
        auto preds = f.predecessors();
        for (auto& a : f.blocks) {
            if (a->term.kind != Terminator::Jump) continue;
            int t = a->term.t;
            if (t == a->id || t == entry) continue;
            auto p = preds.find(t);
            if (p == preds.end() || p->second.size() != 1) continue;
            Block* bb = f.block(t);
            if (!bb) continue;
            a->insts.insert(a->insts.end(), bb->insts.begin(), bb->insts.end());
            a->term = bb->term;
            a->termID = bb->termID;
            a->termOrigin = bb->termOrigin;
            a->termLine = bb->termLine;
            f.blocks.erase(std::remove_if(f.blocks.begin(), f.blocks.end(), [&](const BlockP& x) { return x->id == t; }), f.blocks.end());
            again = true;
            break;
        }
        changed = changed || again;
    }
    return changed;
}

}  // namespace

bool Optimizer::apply(PassID p, IRFunction& f) {
    switch (p) {
    case PassID::Mem2Reg: return mem2reg(f);
    case PassID::ConstFold: return constProp(f);
    case PassID::Simplify: return simplify(f);
    case PassID::CopyProp: return copyProp(f);
    case PassID::CSE: return cse(f);
    case PassID::DCE: return dce(f);
    case PassID::CFG: return cfgSimplify(f);
    }
    return false;
}

std::vector<PassSnapshot> Optimizer::run(const IRModule& input, const std::set<PassID>& enabled) {
    std::vector<PassSnapshot> snaps;
    snaps.push_back({std::nullopt, "Unoptimised IR (straight from the syntax tree)", input.copy(), false, input.instructionCount()});
    auto m = input.copy();
    for (PassID p : allPasses()) {
        if (!enabled.count(p)) continue;
        bool changed = false;
        for (auto& f : m->functions) changed = apply(p, *f) || changed;
        snaps.push_back({p, passTitle(p), m->copy(), changed, m->instructionCount()});
    }
    // One pass often creates work for another (folding a constant makes code dead,
    // removing code makes blocks empty…), so the cheap passes repeat until nothing changes.
    std::vector<PassID> cleanup;
    for (PassID p : {PassID::ConstFold, PassID::Simplify, PassID::CopyProp, PassID::CSE, PassID::DCE, PassID::CFG})
        if (enabled.count(p)) cleanup.push_back(p);
    if (!cleanup.empty()) {
        bool any = false;
        for (int round = 0; round < 6; round++) {
            bool changed = false;
            for (PassID p : cleanup)
                for (auto& f : m->functions) changed = apply(p, *f) || changed;
            any = any || changed;
            if (!changed) break;
        }
        snaps.push_back({std::nullopt, "Repeat until nothing changes", m->copy(), any, m->instructionCount()});
    }
    return snaps;
}

}  // namespace sp
