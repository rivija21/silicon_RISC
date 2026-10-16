#include "Interpreter.h"

namespace sp {

namespace {

struct ExitSignal {
    int32_t code;
};
struct Fault {
    std::string message;
};

enum class FlowKind { Normal, Break, Continue, Returned };
struct Flow {
    FlowKind kind = FlowKind::Normal;
    int32_t value = 0;
};

class Interp {
public:
    Interp(const Program& p, const SemaResult& s, int limit) : prog(p), sema(s), stepLimit(limit) {}

    ProgramOutput execute() {
        try {
            layoutData();
            for (auto& f : prog.functions()) functions[f->name] = f.get();
            auto m = functions.find("main");
            if (m == functions.end()) throw Fault{"no main"};
            const FunctionDecl& main = *m->second;
            int32_t code = call(main, std::vector<int32_t>(main.params.size(), 0));
            out.exitCode = main.returnType == CType::voidT() ? 0 : code;
        } catch (const ExitSignal& e) {
            out.exitCode = e.code;
        } catch (const Fault& f) {
            out.fault = f.message;
        } catch (const MemoryFault& m) {
            out.fault = m.message;
        }
        return out;
    }

private:
    const Program& prog;
    const SemaResult& sema;
    Memory mem;
    std::unordered_map<int, uint32_t> globalAddr;
    std::unordered_map<std::string, uint32_t> stringAddr;
    std::unordered_map<std::string, const FunctionDecl*> functions;
    std::vector<std::unordered_map<int, uint32_t>> frames;
    std::vector<CType> returnTypes;
    uint32_t sp = MemoryMap::stackTop & ~0xFu;
    uint32_t brk = MemoryMap::heapBase;
    uint32_t randNext = 1;
    ProgramOutput out;
    int steps = 0;
    int stepLimit;

    // MARK: Data

    void layoutData() {
        uint32_t addr = MemoryMap::dataBase;
        auto align = [&](int a) { addr = (addr + (uint32_t)a - 1) & ~((uint32_t)a - 1); };
        auto globals = prog.globals();
        for (auto& g : globals) {
            if (!g->symbol) continue;
            align(std::max(4, g->type.align()));
            globalAddr[g->symbol->id] = addr;
            addr += (uint32_t)g->type.size();
        }
        for (auto& s : sema.strings) {
            stringAddr[s.label] = addr;
            for (size_t i = 0; i < s.bytes.size(); i++) mem.store8(addr + (uint32_t)i, s.bytes[i]);
            mem.store8(addr + (uint32_t)s.bytes.size(), 0);
            addr += (uint32_t)s.bytes.size() + 1;
        }
        for (auto& g : globals) {
            if (!g->symbol || !g->initializer) continue;
            auto a = globalAddr.find(g->symbol->id);
            if (a == globalAddr.end()) continue;
            initialize(a->second, g->type, *g->initializer);
        }
    }

    /// Writes an initialiser into memory. Elements without an initialiser become zero.
    void initialize(uint32_t addr, const CType& t, const Initializer& ini) {
        if (ini.isList) {
            if (t.kind == CType::Array) {
                const CType& e = *t.sub;
                for (int k = 0; k < t.count; k++) {
                    uint32_t ea = addr + (uint32_t)(k * e.size());
                    if (k < (int)ini.items.size()) initialize(ea, e, ini.items[k]);
                    else zero(ea, e.size());
                }
            } else if (t.kind == CType::Struct) {
                const StructDef& s = *t.sdef;
                zero(addr, s.size);
                for (size_t k = 0; k < ini.items.size() && k < s.fields.size(); k++)
                    initialize(addr + (uint32_t)s.fields[k].offset, s.fields[k].type, ini.items[k]);
            } else {
                initialize(addr, t, ini.items[0]);
            }
            return;
        }
        const Expr& e = *ini.expr;
        if (t.kind == CType::Array && *t.sub == CType::charT()) {
            if (e.kind != ExprKind::String) return;
            for (int k = 0; k < t.count; k++) mem.store8(addr + (uint32_t)k, k < (int)e.bytes.size() ? e.bytes[k] : 0);
            return;
        }
        int32_t v = eval(e);
        store(t, addr, convert(v, t));
    }

    void zero(uint32_t addr, int n) {
        for (int k = 0; k < n; k++) mem.store8(addr + (uint32_t)k, 0);
    }

    // MARK: Memory helpers

    int32_t load(const CType& t, uint32_t addr) {
        switch (t.kind) {
        case CType::Char: return (int32_t)(int8_t)mem.load8(addr);
        case CType::Array: case CType::Struct: case CType::Function: return (int32_t)addr;
        default: return (int32_t)mem.load32(addr);
        }
    }

    void store(const CType& t, uint32_t addr, int32_t v) {
        switch (t.kind) {
        case CType::Char: mem.store8(addr, (uint8_t)v); break;
        case CType::Struct: {
            // Struct assignment copies the bytes from the source address.
            uint32_t src = (uint32_t)v;
            for (uint32_t k = 0; k < (uint32_t)t.sdef->size; k++) mem.store8(addr + k, mem.load8(src + k));
            break;
        }
        default: mem.store32(addr, (uint32_t)v); break;
        }
    }

    static int32_t convert(int32_t v, const CType& t) { return t == CType::charT() ? (int32_t)(int8_t)(uint8_t)v : v; }

    void tick() {
        steps += 1;
        if (steps > stepLimit) throw Fault{"step limit reached (infinite loop?)"};
    }

    // MARK: Calls

    int32_t call(const FunctionDecl& f, const std::vector<int32_t>& args) {
        tick();
        if (frames.size() > 2000) throw Fault{"stack overflow (recursion too deep)"};
        uint32_t savedSP = sp;
        std::unordered_map<int, uint32_t> frame;
        for (auto& s : f.locals) {
            uint32_t size = (uint32_t)std::max(4, s->type.size());
            sp = (sp - size) & ~((uint32_t)std::max(4, s->type.align()) - 1);
            frame[s->id] = sp;
        }
        if (sp < MemoryMap::stackBottom + 64) {
            sp = savedSP;
            throw Fault{"stack overflow"};
        }
        frames.push_back(std::move(frame));
        struct Pop {
            Interp& self;
            uint32_t saved;
            ~Pop() {
                self.frames.pop_back();
                self.sp = saved;
            }
        } pop{*this, savedSP};
        for (size_t k = 0; k < f.params.size() && k < args.size(); k++) {
            const auto& p = f.params[k];
            if (!p->symbol) continue;
            auto a = frames.back().find(p->symbol->id);
            if (a != frames.back().end()) store(p->symbol->type, a->second, convert(args[k], p->symbol->type));
        }
        if (!f.body) return 0;
        Flow fl = exec(*f.body);
        if (fl.kind == FlowKind::Returned) return fl.value;
        return 0;
    }

    int32_t callLibrary(const std::string& name, const std::vector<int32_t>& a) {
        auto u = [&](int i) { return (uint32_t)a[i]; };
        if (name == "print_int") out.append(std::to_string(a[0]));
        else if (name == "print_uint") out.append(std::to_string(u(0)));
        else if (name == "print_hex") out.append(Format::hex(u(0)));
        else if (name == "print_char" || name == "putchar") {
            out.append(std::vector<uint8_t>{(uint8_t)a[0]});
            return a[0];
        } else if (name == "print_str") out.append(mem.cString(u(0), 100000));
        else if (name == "puts") {
            out.append(mem.cString(u(0), 100000));
            out.append(std::vector<uint8_t>{10});
        } else if (name == "exit") throw ExitSignal{a[0]};
        else if (name == "malloc") return (int32_t)sbrk((u(0) + 7) & ~7u);
        else if (name == "calloc") {
            uint32_t n = (u(0) * u(1) + 7) & ~7u;
            uint32_t p = sbrk(n);
            if (p != 0) zero(p, (int)n);
            return (int32_t)p;
        } else if (name == "free") {
        } else if (name == "abs") return a[0] < 0 ? RV::neg(a[0]) : a[0];
        else if (name == "rand") {
            randNext = randNext * 1103515245u + 12345u;
            return (int32_t)((randNext >> 16) & 32767);
        } else if (name == "srand") randNext = u(0);
        else if (name == "strlen") return (int32_t)mem.cString(u(0), 1 << 22).size();
        else if (name == "strcmp") {
            uint32_t p = u(0), q = u(1);
            while (true) {
                uint8_t x = mem.load8(p), y = mem.load8(q);
                if (x != y || x == 0) return (int32_t)x - (int32_t)y;
                p += 1;
                q += 1;
            }
        } else if (name == "strcpy") {
            uint32_t p = u(0), q = u(1);
            while (true) {
                uint8_t b = mem.load8(q);
                mem.store8(p, b);
                if (b == 0) break;
                p += 1;
                q += 1;
            }
            return a[0];
        } else if (name == "memset") {
            for (uint32_t k = 0; k < u(2); k++) mem.store8(u(0) + k, (uint8_t)a[1]);
            return a[0];
        } else if (name == "memcpy") {
            for (uint32_t k = 0; k < u(2); k++) mem.store8(u(0) + k, mem.load8(u(1) + k));
            return a[0];
        } else {
            throw Fault{"unknown library function " + name};
        }
        return 0;
    }

    uint32_t sbrk(uint32_t n) {
        uint32_t p = brk;
        if (brk + n > MemoryMap::heapLimit || brk + n < brk) return 0;
        brk += n;
        return p;
    }

    int32_t printf(const std::vector<ExprP>& args) {
        if (args.empty() || args[0]->kind != ExprKind::String) return 0;
        auto pieces = PrintfFormat::parse(args[0]->bytes);
        if (!pieces) return 0;
        // All arguments are evaluated before anything is printed, as in C.
        std::vector<int32_t> values;
        for (size_t k = 1; k < args.size(); k++) values.push_back(eval(*args[k]));
        size_t ai = 0;
        for (auto& piece : *pieces) {
            if (!piece.isConversion) {
                out.append(piece.text);
                continue;
            }
            int32_t v = values[ai];
            ai += 1;
            switch (piece.conversion) {
            case 'd': case 'i': out.append(std::to_string(v)); break;
            case 'u': out.append(std::to_string((uint32_t)v)); break;
            case 'x': out.append(hexString((uint32_t)v)); break;
            case 'X': out.append(hexString((uint32_t)v, true)); break;
            case 'p': out.append(Format::hex((uint32_t)v)); break;
            case 'c': out.append(std::vector<uint8_t>{(uint8_t)v}); break;
            case 's': out.append(mem.cString((uint32_t)v, 100000)); break;
            default: break;
            }
        }
        return 0;
    }

    // MARK: Statements

    Flow exec(const Stmt& s) {
        tick();
        switch (s.kind) {
        case StmtKind::Block:
            for (auto& i : s.items) {
                Flow f = exec(*i);
                if (f.kind != FlowKind::Normal) return f;
            }
            return {};
        case StmtKind::Decl:
            for (auto& d : s.decls) {
                if (!d->symbol || !d->initializer || frames.empty()) continue;
                auto a = frames.back().find(d->symbol->id);
                if (a == frames.back().end()) continue;
                initialize(a->second, d->type, *d->initializer);
            }
            return {};
        case StmtKind::Expr:
            eval(*s.expr);
            return {};
        case StmtKind::IfElse:
            if (eval(*s.expr) != 0) return exec(*s.body);
            if (s.elseBody) return exec(*s.elseBody);
            return {};
        case StmtKind::While:
            while (eval(*s.expr) != 0) {
                Flow f = exec(*s.body);
                if (f.kind == FlowKind::Break) return {};
                if (f.kind == FlowKind::Returned) return f;
            }
            return {};
        case StmtKind::DoWhile:
            do {
                Flow f = exec(*s.body);
                if (f.kind == FlowKind::Break) return {};
                if (f.kind == FlowKind::Returned) return f;
            } while (eval(*s.expr) != 0);
            return {};
        case StmtKind::For:
            if (s.init) exec(*s.init);
            while (true) {
                if (s.cond && eval(*s.cond) == 0) break;
                Flow f = exec(*s.body);
                if (f.kind == FlowKind::Break) return {};
                if (f.kind == FlowKind::Returned) return f;
                if (s.step) eval(*s.step);
            }
            return {};
        case StmtKind::Switch: {
            int32_t x = eval(*s.expr);
            int start = -1;
            for (int k = 0; k < (int)s.cases.size(); k++)
                if (s.cases[k]->value && s.cases[k]->constant == x) {
                    start = k;
                    break;
                }
            if (start < 0)
                for (int k = 0; k < (int)s.cases.size(); k++)
                    if (!s.cases[k]->value) {
                        start = k;
                        break;
                    }
            if (start < 0) return {};
            for (int k = start; k < (int)s.cases.size(); k++) {
                for (auto& st : s.cases[k]->body) {
                    Flow f = exec(*st);
                    if (f.kind == FlowKind::Normal) continue;
                    if (f.kind == FlowKind::Break) return {};
                    return f;
                }
            }
            return {};
        }
        case StmtKind::Return: {
            if (!s.expr) return {FlowKind::Returned, 0};
            int32_t v = eval(*s.expr);
            if (!frames.empty() && !returnTypes.empty() && returnTypes.back() == CType::charT()) v = convert(v, CType::charT());
            return {FlowKind::Returned, v};
        }
        case StmtKind::Break: return {FlowKind::Break, 0};
        case StmtKind::Continue: return {FlowKind::Continue, 0};
        case StmtKind::Empty: return {};
        }
        return {};
    }

    // MARK: Expressions

    uint32_t address(const Expr& e) {
        switch (e.kind) {
        case ExprKind::Ident: {
            if (!e.symbol) throw Fault{"unresolved name"};
            const Symbol& s = *e.symbol;
            if (s.kind == Symbol::Kind::Global) return globalAddr.at(s.id);
            return frames.back().at(s.id);
        }
        case ExprKind::Unary:
            if (e.uop == UnaryOp::Deref) return (uint32_t)eval(*e.a());
            break;
        case ExprKind::Index: {
            uint32_t base = (uint32_t)eval(*e.a());
            int32_t idx = eval(*e.b());
            return base + (uint32_t)RV::mul(idx, e.type.size());
        }
        case ExprKind::Member: {
            uint32_t base = e.arrow ? (uint32_t)eval(*e.a()) : address(*e.a());
            return base + (uint32_t)e.fieldOffset;
        }
        case ExprKind::String: return stringAddr.at(e.label);
        case ExprKind::Cast: return address(*e.a());
        default: break;
        }
        throw Fault{"not an lvalue"};
    }

    static int32_t arith(BinaryOp op, int32_t a, int32_t b, const CType& opType) {
        if (opType.isPointer()) {
            int32_t size = opType.sub->size();
            if (op == BinaryOp::Add) return RV::add(a, RV::mul(b, size));
            if (op == BinaryOp::Sub) return RV::sub(a, RV::mul(b, size));
        }
        return RV::binary(op, a, b, opType == CType::uintT());
    }

    int32_t eval(const Expr& e) {
        switch (e.kind) {
        case ExprKind::Number: return RV::wrap(e.number);
        case ExprKind::String: return (int32_t)address(e);
        case ExprKind::Ident: return load(e.type, address(e));
        case ExprKind::Binary: {
            BinaryOp op = e.bop;
            if (op == BinaryOp::LogAnd) return (eval(*e.a()) != 0 && eval(*e.b()) != 0) ? 1 : 0;
            if (op == BinaryOp::LogOr) return (eval(*e.a()) != 0 || eval(*e.b()) != 0) ? 1 : 0;
            int32_t x = eval(*e.a());
            int32_t y = eval(*e.b());
            CType at = e.a()->type.decayed(), bt = e.b()->type.decayed();
            if (op == BinaryOp::Sub && at.isPointer() && bt.isPointer()) return RV::div(RV::sub(x, y), at.sub->size());
            if (op == BinaryOp::Add && at.isInteger() && bt.isPointer()) return arith(BinaryOp::Add, y, x, e.opType);
            return arith(op, x, y, e.opType);
        }
        case ExprKind::Unary:
            switch (e.uop) {
            case UnaryOp::Neg: return RV::neg(eval(*e.a()));
            case UnaryOp::Plus: return eval(*e.a());
            case UnaryOp::BitNot: return ~eval(*e.a());
            case UnaryOp::Not: return eval(*e.a()) == 0 ? 1 : 0;
            case UnaryOp::Deref: return load(e.type, address(e));
            case UnaryOp::Addr: return (int32_t)address(*e.a());
            }
            break;
        case ExprKind::Assign: {
            const Expr& lhs = *e.a();
            uint32_t addr = address(lhs);
            int32_t v;
            if (e.hasOp) {
                int32_t old = load(lhs.type, addr);
                int32_t r = eval(*e.b());
                v = arith(e.bop, old, r, e.opType);
            } else {
                v = eval(*e.b());
            }
            if (lhs.type.isStruct()) {
                store(lhs.type, addr, v);
                return (int32_t)addr;
            }
            v = convert(v, lhs.type);
            store(lhs.type, addr, v);
            return v;
        }
        case ExprKind::IncDec: {
            const Expr& a = *e.a();
            uint32_t addr = address(a);
            int32_t old = load(a.type, addr);
            int32_t nw = convert(arith(e.isIncrement ? BinaryOp::Add : BinaryOp::Sub, old, 1, e.opType), a.type);
            store(a.type, addr, nw);
            return e.isPrefix ? nw : old;
        }
        case ExprKind::Call: {
            if (e.name == "printf") return printf(e.kids);
            std::vector<int32_t> vals;
            for (auto& a : e.kids) vals.push_back(eval(*a));
            auto f = functions.find(e.name);
            if (f != functions.end()) {
                const FunctionDecl& fd = *f->second;
                for (size_t k = 0; k < fd.params.size() && k < vals.size(); k++) vals[k] = convert(vals[k], fd.params[k]->type);
                returnTypes.push_back(fd.returnType);
                struct Pop {
                    std::vector<CType>& v;
                    ~Pop() { v.pop_back(); }
                } pop{returnTypes};
                return call(fd, vals);
            }
            if (auto lib = Library::byName(e.name)) {
                for (size_t k = 0; k < lib->params.size() && k < vals.size(); k++) vals[k] = convert(vals[k], lib->params[k]);
                return callLibrary(e.name, vals);
            }
            throw Fault{"undefined function " + e.name};
        }
        case ExprKind::Index: case ExprKind::Member: return load(e.type, address(e));
        case ExprKind::Ternary: return eval(*e.a()) != 0 ? eval(*e.b()) : eval(*e.c());
        case ExprKind::SizeofType: return e.ctype.size();
        case ExprKind::SizeofExpr: return e.type.size();
        case ExprKind::Cast: {
            int32_t v = eval(*e.a());
            return e.ctype.isVoid() ? 0 : convert(v, e.ctype);
        }
        case ExprKind::Comma:
            eval(*e.a());
            return eval(*e.b());
        }
        return 0;
    }
};

}  // namespace

ProgramOutput Interpreter::run(const Program& prog, const SemaResult& sema, int stepLimit) {
    Interp i(prog, sema, stepLimit);
    return i.execute();
}

}  // namespace sp
