#include "IR.h"

namespace sp {

namespace {

/// Translates the checked syntax tree into IR, the way an unoptimising
/// compiler (`-O0`) does: every local variable and parameter gets a stack
/// slot, every read is a `load` and every write a `store`. The optimiser
/// passes (starting with mem2reg) then clean this up.
///
/// Control flow becomes basic blocks: `if`, loops, `&&`/`||`, `?:` and
/// `switch` all turn into conditional branches between blocks.
class Lowering {
public:
    Lowering(const Program& p, const SemaResult& s, IDGenP g) : prog(p), sema(s), ids(std::move(g)), module(std::make_shared<IRModule>()) {}

    IRModuleP run() {
        for (auto& g : prog.globals()) lowerGlobal(*g);
        for (auto& s : sema.strings) {
            GlobalData gd;
            gd.label = s.label;
            gd.align = 1;
            for (uint8_t b : s.bytes) gd.items.push_back(DataItem::byte(b));
            gd.items.push_back(DataItem::byte(0));
            gd.size = (int)s.bytes.size() + 1;
            gd.line = 0;
            gd.comment = "string";
            module->data.push_back(gd);
        }
        for (auto& fd : prog.functions()) lowerFunction(*fd);
        return module;
    }

private:
    const Program& prog;
    const SemaResult& sema;
    IDGenP ids;
    IRModuleP module;

    IRFunctionP f;
    const FunctionDecl* fn = nullptr;
    BlockP cur;
    std::map<int, BlockP> pending;
    std::map<int, int> slotOf;
    std::vector<int> breakTargets;
    std::vector<int> continueTargets;
    std::map<std::string, int> nameCounts;
    int origin = 0;
    int line = 0;

    struct Restore {
        int& o;
        int& l;
        int so, sl;
        Restore(int& oo, int& ll) : o(oo), l(ll), so(oo), sl(ll) {}
        ~Restore() {
            o = so;
            l = sl;
        }
    };

    // MARK: Blocks and emission

    int nextID() { return ids->next(); }

    int newBlock(const std::string& name) {
        int k = nameCounts[name];
        nameCounts[name] = k + 1;
        auto b = std::make_shared<Block>(f->nextBlock, k == 0 ? name : cat(name, ".", k));
        f->nextBlock += 1;
        pending[b->id] = b;
        return b->id;
    }

    /// Makes `id` the current block. If the previous block is still open, it falls through with a jump.
    void start(int id) {
        if (cur) terminate(Terminator::jump(id));
        auto it = pending.find(id);
        if (it == pending.end()) return;
        BlockP b = it->second;
        pending.erase(it);
        f->blocks.push_back(b);
        cur = b;
    }

    Block& current() {
        if (cur) return *cur;
        // Code after return/break/continue: unreachable, but it still needs a home.
        int id = newBlock("dead");
        start(id);
        return *cur;
    }

    void emit(const InstKind& k) {
        Block& b = current();
        Inst i;
        i.kind = k;
        i.id = nextID();
        i.origin = origin;
        i.line = line;
        b.insts.push_back(std::move(i));
    }

    void terminate(const Terminator& t) {
        Block& b = current();
        b.term = t;
        b.termID = nextID();
        b.termOrigin = origin;
        b.termLine = line;
        cur = nullptr;
    }

    void jump(int id) {
        if (cur) terminate(Terminator::jump(id));
    }

    int temp() { return f->newVReg(); }

    Operand bin(IROp op, Operand a, Operand b) {
        int d = temp();
        emit(InstKind::bin(op, d, a, b));
        return Operand::v(d);
    }

    Operand un(UnOp op, Operand a) {
        int d = temp();
        emit(InstKind::un(op, d, a));
        return Operand::v(d);
    }

    // MARK: Functions

    void lowerFunction(const FunctionDecl& d) {
        f = std::make_shared<IRFunction>(d.name, ids);
        fn = &d;
        f->returnsValue = d.returnType != CType::voidT();
        f->line = d.line;
        pending.clear();
        slotOf.clear();
        nameCounts.clear();
        cur = nullptr;
        origin = d.id;
        line = d.line;
        start(newBlock("entry"));

        // Every local and parameter starts life in its own stack slot.
        for (auto& s : d.locals) {
            slotOf[s->id] = (int)f->slots.size();
            f->slots.push_back(Slot{"$" + s->uniqueName, s->type.size(), std::max(1, s->type.align()), s->id, s->type.isScalar()});
        }
        for (auto& p : d.params) {
            if (!p->symbol) continue;
            const Symbol& s = *p->symbol;
            int v = f->newVReg("%" + s.uniqueName, true);
            f->params.push_back(v);
            line = p->line;
            emit(InstKind::store(Operand::v(v), Addr::ofSlot(slotOf[s.id], 0), s.type == CType::charT() ? 1 : 4));
        }
        line = d.line;
        if (d.body) stmt(*d.body);
        if (cur) {
            // Falling off the end: void functions return; main returns 0 (as C99 says).
            if (d.body && d.body->kind == StmtKind::Block) line = lastLine(*d.body);
            terminate(f->returnsValue ? Terminator::ret(Operand::imm(0)) : Terminator::ret());
        }
        removeUnreachable();
        module->functions.push_back(f);
    }

    int lastLine(const Stmt& s) {
        if (s.kind == StmtKind::Block && !s.items.empty()) {
            const Stmt& l = *s.items.back();
            return std::max(l.line, lastLine(l));
        }
        return s.line;
    }

    void removeUnreachable() {
        if (f->blocks.empty()) return;
        int entry = f->blocks.front()->id;
        std::set<int> seen{entry};
        std::vector<int> work{entry};
        while (!work.empty()) {
            int id = work.back();
            work.pop_back();
            Block* b = f->block(id);
            if (!b) continue;
            for (int s : b->term.successors()) {
                if (!seen.count(s)) {
                    seen.insert(s);
                    work.push_back(s);
                }
            }
        }
        f->blocks.erase(std::remove_if(f->blocks.begin(), f->blocks.end(), [&](const BlockP& b) { return !seen.count(b->id); }),
                        f->blocks.end());
    }

    // MARK: Globals

    void lowerGlobal(const VarDecl& g) {
        if (!g.symbol) return;
        const Symbol& s = *g.symbol;
        std::vector<DataItem> items;
        dataItems(g.type, g.initializer ? &*g.initializer : nullptr, items);
        // Merge runs of zeros.
        std::vector<DataItem> merged;
        for (auto& it : items) {
            if (it.kind == DataItem::Zero && !merged.empty() && merged.back().kind == DataItem::Zero) {
                merged.back().value += it.value;
            } else if (it.kind == DataItem::Zero && it.value == 0) {
                continue;
            } else {
                merged.push_back(it);
            }
        }
        GlobalData gd;
        gd.label = s.uniqueName;
        gd.align = std::max(4, g.type.align());
        gd.items = std::move(merged);
        gd.size = g.type.size();
        gd.line = g.line;
        gd.comment = g.type.spelled(g.name);
        module->data.push_back(gd);
    }

    void dataItems(const CType& t, const Initializer* ini, std::vector<DataItem>& out) {
        if (!ini) {
            out.push_back(DataItem::zero(t.size()));
            return;
        }
        if (ini->isList) {
            if (t.kind == CType::Array) {
                int n = t.count;
                const CType& e = *t.sub;
                for (int k = 0; k < (int)ini->items.size() && k < n; k++) dataItems(e, &ini->items[k], out);
                if ((int)ini->items.size() < n) out.push_back(DataItem::zero(e.size() * (n - (int)ini->items.size())));
            } else if (t.kind == CType::Struct) {
                const StructDef& s = *t.sdef;
                int offset = 0;
                for (size_t k = 0; k < s.fields.size(); k++) {
                    const auto& fld = s.fields[k];
                    if (fld.offset > offset) out.push_back(DataItem::zero(fld.offset - offset));
                    dataItems(fld.type, k < ini->items.size() ? &ini->items[k] : nullptr, out);
                    offset = fld.offset + fld.type.size();
                }
                if (s.size > offset) out.push_back(DataItem::zero(s.size - offset));
            } else {
                dataItems(t, ini->items.empty() ? nullptr : &ini->items[0], out);
            }
            return;
        }
        const Expr& e = *ini->expr;
        if (t.kind == CType::Array && *t.sub == CType::charT()) {
            if (e.kind == ExprKind::String) {
                int n = t.count;
                for (int k = 0; k < (int)e.bytes.size() && k < n; k++) out.push_back(DataItem::byte(e.bytes[k]));
                if ((int)e.bytes.size() < n) out.push_back(DataItem::zero(n - (int)e.bytes.size()));
            }
            return;
        }
        if (t.isPointer()) {
            if (auto target = constantAddress(e)) {
                out.push_back(DataItem::address(*target, 0));
                return;
            }
        }
        auto cv = constantValue(e);
        int32_t v = RV::wrap(cv ? *cv : 0);
        if (t == CType::charT()) out.push_back(DataItem::byte((uint8_t)v));
        else out.push_back(DataItem::word(v));
    }

    std::optional<std::string> constantAddress(const Expr& e) {
        switch (e.kind) {
        case ExprKind::String: return e.label;
        case ExprKind::Unary:
            if (e.uop == UnaryOp::Addr) {
                const Expr& x = *e.a();
                if (x.kind == ExprKind::Ident && x.symbol) return x.symbol->uniqueName;
            }
            return std::nullopt;
        case ExprKind::Ident:
            if (e.symbol && e.symbol->type.isArray()) return e.symbol->uniqueName;
            return std::nullopt;
        case ExprKind::Cast: return constantAddress(*e.a());
        default: return std::nullopt;
        }
    }

    // MARK: Statements

    void stmt(const Stmt& s) {
        Restore r(origin, line);
        origin = s.id;
        line = s.line;
        switch (s.kind) {
        case StmtKind::Block:
            for (auto& i : s.items) stmt(*i);
            break;
        case StmtKind::Decl:
            for (auto& d : s.decls) {
                if (!d->symbol || !d->initializer) continue;
                auto sl = slotOf.find(d->symbol->id);
                if (sl == slotOf.end()) continue;
                line = d->line;
                initLocal(Addr::ofSlot(sl->second, 0), d->type, *d->initializer);
            }
            break;
        case StmtKind::Expr: value(*s.expr, false); break;
        case StmtKind::IfElse: {
            int then = newBlock("if.then");
            int end = newBlock("if.end");
            int els = s.elseBody ? newBlock("if.else") : end;
            cond(*s.expr, then, els);
            start(then);
            stmt(*s.body);
            jump(end);
            if (s.elseBody) {
                start(els);
                stmt(*s.elseBody);
                jump(end);
            }
            start(end);
            break;
        }
        case StmtKind::While: {
            int head = newBlock("while.cond");
            int bodyB = newBlock("while.body");
            int end = newBlock("while.end");
            start(head);
            cond(*s.expr, bodyB, end);
            start(bodyB);
            loop(end, head, [&] { stmt(*s.body); });
            jump(head);
            start(end);
            break;
        }
        case StmtKind::DoWhile: {
            int bodyB = newBlock("do.body");
            int test = newBlock("do.cond");
            int end = newBlock("do.end");
            start(bodyB);
            loop(end, test, [&] { stmt(*s.body); });
            start(test);
            cond(*s.expr, bodyB, end);
            start(end);
            break;
        }
        case StmtKind::For: {
            if (s.init) stmt(*s.init);
            int head = newBlock("for.cond");
            int bodyB = newBlock("for.body");
            int stepB = newBlock("for.step");
            int end = newBlock("for.end");
            start(head);
            if (s.cond) cond(*s.cond, bodyB, end);
            else jump(bodyB);
            start(bodyB);
            loop(end, stepB, [&] { stmt(*s.body); });
            start(stepB);
            if (s.step) value(*s.step, false);
            jump(head);
            start(end);
            break;
        }
        case StmtKind::Switch: lowerSwitch(*s.expr, s.cases); break;
        case StmtKind::Return:
            if (s.expr) {
                Operand v = value(*s.expr);
                if (fn->returnType == CType::charT()) v = un(UnOp::Sext8, v);
                terminate(f->returnsValue ? Terminator::ret(v) : Terminator::ret());
            } else {
                terminate(f->returnsValue ? Terminator::ret(Operand::imm(0)) : Terminator::ret());
            }
            break;
        case StmtKind::Break: terminate(Terminator::jump(breakTargets.back())); break;
        case StmtKind::Continue: terminate(Terminator::jump(continueTargets.back())); break;
        case StmtKind::Empty: break;
        }
    }

    void loop(int b, int c, const std::function<void()>& body) {
        breakTargets.push_back(b);
        continueTargets.push_back(c);
        body();
        breakTargets.pop_back();
        continueTargets.pop_back();
    }

    /// A switch becomes a chain of compare-and-branch tests, then the case
    /// bodies laid out in order so that a missing `break` falls through.
    void lowerSwitch(const Expr& x, const std::vector<SwitchCaseP>& cases) {
        Operand v = value(x);
        if (v.isImm) {
            int r = temp();
            emit(InstKind::mov(r, v));
            v = Operand::v(r);
        }
        int end = newBlock("switch.end");
        std::vector<int> bodies;
        for (auto& c : cases) bodies.push_back(newBlock(c->value ? "switch.case" : "switch.default"));
        for (size_t k = 0; k < cases.size(); k++) {
            if (!cases[k]->value) continue;
            int next = newBlock("switch.test");
            line = cases[k]->line;
            terminate(Terminator::branch(IROp::Eq, v, Operand::imm(cases[k]->constant), bodies[k], next));
            start(next);
        }
        int target = end;
        for (size_t k = 0; k < cases.size(); k++)
            if (!cases[k]->value) {
                target = bodies[k];
                break;
            }
        jump(target);
        breakTargets.push_back(end);
        for (size_t k = 0; k < cases.size(); k++) {
            start(bodies[k]);
            for (auto& s : cases[k]->body) stmt(*s);
        }
        breakTargets.pop_back();
        start(end);
    }

    // MARK: Local initialisers

    void initLocal(const Addr& a, const CType& t, const Initializer& ini) {
        if (ini.isList) {
            if (t.kind == CType::Array) {
                int n = t.count;
                const CType& e = *t.sub;
                for (int k = 0; k < std::min(n, (int)ini.items.size()); k++) initLocal(a.adding(k * e.size()), e, ini.items[k]);
                if ((int)ini.items.size() < n) zero(a.adding((int)ini.items.size() * e.size()), (n - (int)ini.items.size()) * e.size(), e.align());
            } else if (t.kind == CType::Struct) {
                const StructDef& s = *t.sdef;
                zero(a, s.size, s.align);
                for (size_t k = 0; k < s.fields.size() && k < ini.items.size(); k++) {
                    initLocal(a.adding(s.fields[k].offset), s.fields[k].type, ini.items[k]);
                }
            } else {
                initLocal(a, t, ini.items[0]);
            }
            return;
        }
        const Expr& e = *ini.expr;
        Restore r(origin, line);
        origin = e.id;
        line = e.line;
        if (t.kind == CType::Array && *t.sub == CType::charT()) {
            if (e.kind != ExprKind::String) return;
            int n = t.count;
            if (n <= 64) {
                for (int k = 0; k < n; k++) {
                    int32_t b = k < (int)e.bytes.size() ? (int32_t)(int8_t)e.bytes[k] : 0;
                    emit(InstKind::store(Operand::imm(b), a.adding(k), 1));
                }
            } else {
                int copy = std::min(n, (int)e.bytes.size() + 1);
                Operand d = addrValue(a);
                Operand s = addrValue(Addr::ofGlobal(e.label, 0));
                emit(InstKind::call(0, "memcpy", {d, s, Operand::imm(copy)}));
                if (n > copy) zero(a.adding(copy), n - copy, 1);
            }
            return;
        }
        if (t.isStruct()) {
            Operand v = value(e);
            copyStruct(a, v, t.size(), t.align());
        } else {
            Operand v = value(e);
            storeValue(t, a, v);
        }
    }

    void zero(const Addr& a, int size, int align) {
        if (size > 64) {
            Operand d = addrValue(a);
            emit(InstKind::call(0, "memset", {d, Operand::imm(0), Operand::imm(size)}));
            return;
        }
        int k = 0;
        if (align >= 4) {
            while (k + 4 <= size) {
                emit(InstKind::store(Operand::imm(0), a.adding(k), 4));
                k += 4;
            }
        }
        while (k < size) {
            emit(InstKind::store(Operand::imm(0), a.adding(k), 1));
            k += 1;
        }
    }

    void copyStruct(const Addr& dst, Operand src, int size, int align) {
        int step = align >= 4 ? 4 : 1;
        for (int k = 0; k < size; k += step) {
            int t = temp();
            emit(InstKind::load(t, Addr::ofReg(src, k), step));
            emit(InstKind::store(Operand::v(t), dst.adding(k), step));
        }
    }

    // MARK: Values

    Operand addrValue(const Addr& a) {
        if (a.kind == Addr::Reg) {
            if (a.off == 0) return a.reg;
            return bin(IROp::Add, a.reg, Operand::imm(a.off));
        }
        int d = temp();
        emit(InstKind::addrOf(d, a));
        return Operand::v(d);
    }

    Operand loadValue(const CType& t, const Addr& a) {
        switch (t.kind) {
        case CType::Array: case CType::Struct: case CType::Function: return addrValue(a);
        default: {
            int d = temp();
            emit(InstKind::load(d, a, t == CType::charT() ? 1 : 4));
            return Operand::v(d);
        }
        }
    }

    void storeValue(const CType& t, const Addr& a, Operand v) {
        if (t.isStruct()) {
            copyStruct(a, v, t.size(), t.align());
            return;
        }
        emit(InstKind::store(v, a, t == CType::charT() ? 1 : 4));
    }

    Operand scale(Operand idx, int size) { return size == 1 ? idx : bin(IROp::Mul, idx, Operand::imm(size)); }

    static IROp arithOp(BinaryOp op, bool isUnsigned) {
        switch (op) {
        case BinaryOp::Add: return IROp::Add;
        case BinaryOp::Sub: return IROp::Sub;
        case BinaryOp::Mul: return IROp::Mul;
        case BinaryOp::Div: return isUnsigned ? IROp::Divu : IROp::Div;
        case BinaryOp::Rem: return isUnsigned ? IROp::Remu : IROp::Rem;
        case BinaryOp::Shl: return IROp::Shl;
        case BinaryOp::Shr: return isUnsigned ? IROp::Shr : IROp::Sar;
        case BinaryOp::BitAnd: return IROp::And;
        case BinaryOp::BitOr: return IROp::Or;
        case BinaryOp::BitXor: return IROp::Xor;
        case BinaryOp::Lt: return isUnsigned ? IROp::Ltu : IROp::Lt;
        case BinaryOp::Gt: return isUnsigned ? IROp::Gtu : IROp::Gt;
        case BinaryOp::Le: return isUnsigned ? IROp::Leu : IROp::Le;
        case BinaryOp::Ge: return isUnsigned ? IROp::Geu : IROp::Ge;
        case BinaryOp::Eq: return IROp::Eq;
        case BinaryOp::Ne: return IROp::Ne;
        case BinaryOp::LogAnd: case BinaryOp::LogOr: return IROp::And;
        }
        return IROp::Add;
    }

    /// `a op b` where the operation happens in `opType` (int, unsigned, or pointer arithmetic).
    Operand arith(BinaryOp op, Operand a, Operand b, const CType& opType, bool bIsPointer = false) {
        if (opType.isPointer() && (op == BinaryOp::Add || op == BinaryOp::Sub)) {
            int size = opType.sub->size();
            if (bIsPointer) {
                Operand s = scale(a, size);
                return bin(IROp::Add, b, s);
            }
            Operand s = scale(b, size);
            return bin(op == BinaryOp::Add ? IROp::Add : IROp::Sub, a, s);
        }
        return bin(arithOp(op, opType == CType::uintT()), a, b);
    }

    Operand value(const Expr& e, bool want = true) {
        Restore r(origin, line);
        origin = e.id;
        line = e.line;
        return valueAt(e, want);
    }

    Operand valueAt(const Expr& e, bool want) {
        switch (e.kind) {
        case ExprKind::Number: return Operand::imm(RV::wrap(e.number));
        case ExprKind::String: return addrValue(Addr::ofGlobal(e.label, 0));
        case ExprKind::Ident: case ExprKind::Index: case ExprKind::Member: {
            Addr a = address(e);
            return loadValue(e.type, a);
        }
        case ExprKind::Binary: {
            BinaryOp op = e.bop;
            if (op == BinaryOp::LogAnd || op == BinaryOp::LogOr) return boolValue(e);
            CType at = e.a()->type.decayed(), bt = e.b()->type.decayed();
            Operand x = value(*e.a());
            Operand y = value(*e.b());
            if (op == BinaryOp::Sub && at.isPointer() && bt.isPointer()) {
                Operand diff = bin(IROp::Sub, x, y);
                int size = at.sub->size();
                if (size == 1) return diff;
                // Pointer differences are exact multiples, so a shift is enough for powers of two.
                if ((size & (size - 1)) == 0) return bin(IROp::Sar, diff, Operand::imm(trailingZeros(size)));
                return bin(IROp::Div, diff, Operand::imm(size));
            }
            return arith(op, x, y, e.opType, at.isInteger() && bt.isPointer());
        }
        case ExprKind::Unary:
            switch (e.uop) {
            case UnaryOp::Neg: {
                Operand v = value(*e.a());
                return un(UnOp::Neg, v);
            }
            case UnaryOp::Plus: return value(*e.a());
            case UnaryOp::BitNot: {
                Operand v = value(*e.a());
                return un(UnOp::Not, v);
            }
            case UnaryOp::Not: {
                Operand v = value(*e.a());
                return bin(IROp::Eq, v, Operand::imm(0));
            }
            case UnaryOp::Addr: {
                Addr a = address(*e.a());
                return addrValue(a);
            }
            case UnaryOp::Deref: {
                Addr a = address(e);
                return loadValue(e.type, a);
            }
            }
            break;
        case ExprKind::Assign: {
            const Expr& lhs = *e.a();
            const Expr& rhs = *e.b();
            Addr a = address(lhs);
            if (lhs.type.isStruct()) {
                Operand v = value(rhs);
                copyStruct(a, v, lhs.type.size(), lhs.type.align());
                return want ? addrValue(a) : Operand::imm(0);
            }
            Operand v;
            if (e.hasOp) {
                Operand old = loadValue(lhs.type, a);
                Operand rv = value(rhs);
                v = arith(e.bop, old, rv, e.opType);
            } else {
                v = value(rhs);
            }
            storeValue(lhs.type, a, v);
            if (want && lhs.type == CType::charT()) v = un(UnOp::Sext8, v);
            return v;
        }
        case ExprKind::IncDec: {
            const Expr& x = *e.a();
            Addr a = address(x);
            Operand old = loadValue(x.type, a);
            int32_t step = x.type.pointee() ? x.type.pointee()->size() : 1;
            Operand nw = bin(e.isIncrement ? IROp::Add : IROp::Sub, old, Operand::imm(step));
            storeValue(x.type, a, nw);
            if (e.isPrefix) return (want && x.type == CType::charT()) ? un(UnOp::Sext8, nw) : nw;
            return old;
        }
        case ExprKind::Call: {
            if (e.name == "printf") {
                printf(e.kids);
                return Operand::imm(0);
            }
            std::vector<Operand> vals;
            for (auto& a : e.kids) vals.push_back(value(*a));
            if (e.type == CType::voidT()) {
                emit(InstKind::call(0, e.name, vals));
                return Operand::imm(0);
            }
            int d = temp();
            emit(InstKind::call(d, e.name, vals));
            return Operand::v(d);
        }
        case ExprKind::Ternary: {
            int t = newBlock("cond.true");
            int fl = newBlock("cond.false");
            int end = newBlock("cond.end");
            int r = e.type.isVoid() ? 0 : temp();
            cond(*e.a(), t, fl);
            start(t);
            Operand va = value(*e.b());
            if (r != 0) emit(InstKind::mov(r, va));
            jump(end);
            start(fl);
            Operand vb = value(*e.c());
            if (r != 0) emit(InstKind::mov(r, vb));
            jump(end);
            start(end);
            return r != 0 ? Operand::v(r) : Operand::imm(0);
        }
        case ExprKind::SizeofType: return Operand::imm(e.ctype.size());
        case ExprKind::SizeofExpr: return Operand::imm(e.type.size());
        case ExprKind::Cast: {
            Operand v = value(*e.a(), !e.ctype.isVoid());
            return e.ctype == CType::charT() ? un(UnOp::Sext8, v) : v;
        }
        case ExprKind::Comma:
            value(*e.a(), false);
            return value(*e.b(), want);
        }
        return Operand::imm(0);
    }

    /// `a && b` or `a || b` used as a value: branch, then put 1 or 0 in a register.
    Operand boolValue(const Expr& e) {
        int t = newBlock("bool.true");
        int fl = newBlock("bool.false");
        int end = newBlock("bool.end");
        int r = temp();
        cond(e, t, fl);
        start(t);
        emit(InstKind::mov(r, Operand::imm(1)));
        jump(end);
        start(fl);
        emit(InstKind::mov(r, Operand::imm(0)));
        jump(end);
        start(end);
        return Operand::v(r);
    }

    /// printf with a constant format is expanded right here into simple print calls.
    void printf(const std::vector<ExprP>& args) {
        if (args.empty() || args[0]->kind != ExprKind::String) return;
        auto pieces = PrintfFormat::parse(args[0]->bytes);
        if (!pieces) return;
        // C evaluates every argument before the call, so do that first (left to right),
        // then print the pieces. A function called in an argument prints before printf does.
        std::vector<Operand> values;
        for (size_t k = 1; k < args.size(); k++) values.push_back(value(*args[k]));
        size_t ai = 0;
        for (auto& piece : *pieces) {
            if (!piece.isConversion) {
                if (piece.text.size() == 1) {
                    emit(InstKind::call(0, "print_char", {Operand::imm(piece.text[0])}));
                } else {
                    std::string label = ".LC?";
                    for (auto& s : sema.strings)
                        if (s.bytes == piece.text) {
                            label = s.label;
                            break;
                        }
                    Operand a = addrValue(Addr::ofGlobal(label, 0));
                    emit(InstKind::call(0, "print_str", {a}));
                }
                continue;
            }
            Operand v = values[ai];
            ai += 1;
            switch (piece.conversion) {
            case 'd': case 'i': emit(InstKind::call(0, "print_int", {v})); break;
            case 'u': emit(InstKind::call(0, "print_uint", {v})); break;
            case 'x': emit(InstKind::call(0, "__printx", {v, Operand::imm(0)})); break;
            case 'X': emit(InstKind::call(0, "__printx", {v, Operand::imm(1)})); break;
            case 'p': emit(InstKind::call(0, "print_hex", {v})); break;
            case 'c': emit(InstKind::call(0, "print_char", {v})); break;
            case 's': emit(InstKind::call(0, "print_str", {v})); break;
            default: break;
            }
        }
    }

    // MARK: Addresses

    Addr address(const Expr& e) {
        Restore r(origin, line);
        origin = e.id;
        line = e.line;
        return addressAt(e);
    }

    Addr addressAt(const Expr& e) {
        switch (e.kind) {
        case ExprKind::Ident: {
            if (!e.symbol) return Addr::ofGlobal("?", 0);
            const Symbol& s = *e.symbol;
            if (s.kind == Symbol::Kind::Global) return Addr::ofGlobal(s.uniqueName, 0);
            return Addr::ofSlot(slotOf[s.id], 0);
        }
        case ExprKind::Unary:
            if (e.uop == UnaryOp::Deref) {
                Operand v = value(*e.a());
                return Addr::ofReg(v, 0);
            }
            break;
        case ExprKind::Index: {
            Operand base = value(*e.a());
            Operand iv = value(*e.b());
            Operand off = scale(iv, e.type.size());
            return Addr::ofReg(bin(IROp::Add, base, off), 0);
        }
        case ExprKind::Member:
            if (e.arrow) {
                Operand v = value(*e.a());
                return Addr::ofReg(v, e.fieldOffset);
            }
            return address(*e.a()).adding(e.fieldOffset);
        case ExprKind::String: return Addr::ofGlobal(e.label, 0);
        case ExprKind::Cast: return address(*e.a());
        default: break;
        }
        Operand v = value(e);
        return Addr::ofReg(v, 0);
    }

    // MARK: Conditions

    /// Lowers a condition straight into branches. `&&` and `||` short-circuit:
    /// the right side is only evaluated when it can still change the answer.
    void cond(const Expr& e, int t, int fl) {
        Restore r(origin, line);
        origin = e.id;
        line = e.line;
        if (e.kind == ExprKind::Binary && e.bop == BinaryOp::LogAnd) {
            int mid = newBlock("and.rhs");
            cond(*e.a(), mid, fl);
            start(mid);
            cond(*e.b(), t, fl);
        } else if (e.kind == ExprKind::Binary && e.bop == BinaryOp::LogOr) {
            int mid = newBlock("or.rhs");
            cond(*e.a(), t, mid);
            start(mid);
            cond(*e.b(), t, fl);
        } else if (e.kind == ExprKind::Unary && e.uop == UnaryOp::Not) {
            cond(*e.a(), fl, t);
        } else if (e.kind == ExprKind::Binary && isComparison(e.bop)) {
            Operand x = value(*e.a());
            Operand y = value(*e.b());
            terminate(Terminator::branch(arithOp(e.bop, e.opType == CType::uintT()), x, y, t, fl));
        } else if (e.kind == ExprKind::Number) {
            terminate(Terminator::jump(e.number != 0 ? t : fl));
        } else {
            Operand v = value(e);
            terminate(Terminator::branch(IROp::Ne, v, Operand::imm(0), t, fl));
        }
    }
};

}  // namespace

IRModuleP lower(const Program& prog, const SemaResult& sema, const IDGenP& ids) {
    Lowering l(prog, sema, ids);
    return l.run();
}

}  // namespace sp
