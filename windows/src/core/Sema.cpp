#include "Frontend.h"

namespace sp {

bool SemaResult::hasErrors() const {
    for (auto& d : diagnostics)
        if (d.severity == Diagnostic::Severity::Error) return true;
    return false;
}

namespace {

using Sev = Diagnostic::Severity;

struct Failure {
    Diagnostic d;
};

class Sema {
public:
    Sema(Program& p, bool library) : prog(p), isLibrary(library), result(std::make_shared<SemaResult>()) {}

    SemaResultP run() {
        try {
            analyzeProgram();
        } catch (const Failure& f) {
            result->diagnostics.push_back(f.d);
        }
        return result;
    }

private:
    Program& prog;
    bool isLibrary;
    SemaResultP result;
    std::vector<std::map<std::string, SymbolP>> scopes{{}};
    std::vector<ScopeInfo> scopeInfo;
    int nextSymbol = 0;
    FunctionDecl* currentFunction = nullptr;
    int loopDepth = 0;
    int switchDepth = 0;
    std::map<std::string, int> nameCounts;
    int stringCount = 0;
    std::map<std::vector<uint8_t>, std::string> stringLabels;
    std::set<std::string> referencedFunctions;

    Failure fail(const std::string& msg, SourceRange range, int line) const {
        return Failure{Diagnostic(Sev::Error, Stage::Semantic, msg, range, line)};
    }
    Failure fail(const std::string& msg, const Expr& e) const { return fail(msg, e.range, e.line); }

    void warn(const std::string& msg, SourceRange range, int line) {
        result->diagnostics.emplace_back(Sev::Warning, Stage::Semantic, msg, range, line);
    }

    SymbolP newSymbol(const std::string& name, Symbol::Kind kind, const CType& type, SourceRange range, int line) {
        nextSymbol += 1;
        auto s = std::make_shared<Symbol>(nextSymbol, name, kind, type, range, line, (int)scopes.size() - 1);
        result->symbols.push_back(s);
        return s;
    }

    SymbolP lookup(const std::string& name) const {
        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
            auto f = it->find(name);
            if (f != it->end()) return f->second;
        }
        return nullptr;
    }

    void pushScope(int line) {
        scopes.emplace_back();
        ScopeInfo si;
        if (currentFunction) si.function = currentFunction->name;
        si.depth = (int)scopes.size() - 1;
        si.line = line;
        scopeInfo.push_back(si);
    }

    void popScope() {
        scopes.pop_back();
        // Find the most recent open record at this depth and close it.
        for (int i = (int)scopeInfo.size() - 1; i >= 0; i--) {
            if (scopeInfo[i].depth == (int)scopes.size()) {
                result->scopes.push_back(scopeInfo[i]);
                scopeInfo.erase(scopeInfo.begin() + i);
                break;
            }
        }
    }

    void declare(const SymbolP& sym) {
        scopes.back()[sym->name] = sym;
        for (int i = (int)scopeInfo.size() - 1; i >= 0; i--) {
            if (scopeInfo[i].depth == (int)scopes.size() - 1) {
                scopeInfo[i].symbols.push_back(sym);
                break;
            }
        }
    }

    void analyzeProgram() {
        ScopeInfo fileScope;
        fileScope.depth = 0;
        fileScope.line = 1;
        for (auto& item : prog.items) {
            switch (item.kind) {
            case TopLevel::Kind::StructDef: continue;
            case TopLevel::Kind::Function: {
                FunctionDecl& f = *item.function;
                declareFunction(f, fileScope);
                if (f.body) analyzeFunction(f);
                break;
            }
            case TopLevel::Kind::Globals:
                for (auto& d : item.globals) {
                    checkObjectType(d->type, d->nameRange, d->line, d->name);
                    auto ex = scopes[0].find(d->name);
                    if (ex != scopes[0].end()) {
                        const SymbolP& existing = ex->second;
                        throw fail(existing->kind == Symbol::Kind::Function ? cat("'", d->name, "' is already a function")
                                                                            : cat("global '", d->name, "' is already defined on line ", existing->line),
                                   d->nameRange, d->line);
                    }
                    if (Library::byName(d->name)) throw fail(cat("'", d->name, "' is a library function name"), d->nameRange, d->line);
                    resolveArraySize(*d);
                    auto sym = newSymbol(d->name, Symbol::Kind::Global, d->type, d->nameRange, d->line);
                    d->symbol = sym;
                    scopes[0][d->name] = sym;
                    fileScope.symbols.push_back(sym);
                    if (d->initializer) checkGlobalInitializer(*d->initializer, d->type, *d);
                }
                break;
            }
        }
        if (isLibrary) {
            result->scopes.insert(result->scopes.begin(), fileScope);
            return;
        }
        auto m = scopes[0].find("main");
        if (m == scopes[0].end() || m->second->kind != Symbol::Kind::Function || !m->second->isDefined) {
            result->diagnostics.emplace_back(Sev::Error, Stage::Semantic, "the program needs a main function: int main() { ... }");
            return;
        }
        SymbolP main = m->second;
        if (main->type.kind == CType::Function) {
            if (!main->type.params->empty()) {
                warn("main's parameters are ignored (there is no command line); they will be 0", main->declRange, main->line);
            }
            const CType& r = *main->type.sub;
            if (r != CType::intT() && r != CType::voidT()) throw fail("main should return int", main->declRange, main->line);
        }
        for (auto& [name, sym] : scopes[0]) {
            if (sym->kind == Symbol::Kind::Function && !sym->isDefined && referencedFunctions.count(sym->name)) {
                result->diagnostics.emplace_back(
                    Sev::Error, Stage::Semantic,
                    cat("function '", sym->name, "' is declared but never defined (the linker would report: undefined reference to '", sym->name, "')"),
                    sym->declRange, sym->line);
            }
        }
        result->scopes.insert(result->scopes.begin(), fileScope);
    }

    void checkObjectType(const CType& t, SourceRange range, int line, const std::string& name) {
        switch (t.kind) {
        case CType::Void: throw fail(cat("variable '", name, "' cannot have type void"), range, line);
        case CType::Struct:
            if (!t.sdef->isComplete)
                throw fail(cat("struct ", t.sdef->name, " is not defined (only declared), so '", name, "' has no known size"), range, line);
            break;
        case CType::Array: checkObjectType(*t.sub, range, line, name); break;
        default: break;
        }
    }

    void declareFunction(FunctionDecl& f, ScopeInfo& fileScope) {
        std::vector<CType> pts;
        for (auto& p : f.params) pts.push_back(p->type);
        CType type = CType::function(f.returnType, pts);
        if (f.returnType.isArray()) throw fail("a function cannot return an array", f.nameRange, f.line);
        if (f.returnType.isStruct()) throw fail("returning a struct by value is not supported; return a pointer", f.nameRange, f.line);
        if (Library::byName(f.name) && !isLibrary)
            throw fail(cat("'", f.name, "' is a built-in library function and cannot be redefined"), f.nameRange, f.line);
        if (f.name == "printf") throw fail("'printf' is built in and cannot be redefined", f.nameRange, f.line);
        auto ex = scopes[0].find(f.name);
        if (ex != scopes[0].end()) {
            SymbolP existing = ex->second;
            if (existing->kind != Symbol::Kind::Function)
                throw fail(cat("'", f.name, "' is already a global variable"), f.nameRange, f.line);
            if (existing->type != type) {
                throw fail(cat("'", f.name, "' was declared on line ", existing->line, " as ", existing->type.spelled(f.name), ", which does not match"),
                           f.nameRange, f.line);
            }
            if (existing->isDefined && f.body)
                throw fail(cat("function '", f.name, "' is already defined on line ", existing->line), f.nameRange, f.line);
            if (f.body) existing->isDefined = true;
            f.symbol = existing;
            return;
        }
        auto sym = newSymbol(f.name, Symbol::Kind::Function, type, f.nameRange, f.line);
        sym->isDefined = f.body != nullptr;
        f.symbol = sym;
        scopes[0][f.name] = sym;
        fileScope.symbols.push_back(sym);
    }

    void resolveArraySize(VarDecl& d) {
        if (d.type.kind != CType::Array || d.type.count >= 0) return;
        const CType& e = *d.type.sub;
        if (d.initializer && d.initializer->isList) {
            d.type = CType::array(e, std::max(1, (int)d.initializer->items.size()));
        } else if (d.initializer) {
            const Expr& x = *d.initializer->expr;
            if (x.kind == ExprKind::String && e == CType::charT()) {
                d.type = CType::array(CType::charT(), (int)x.bytes.size() + 1);
            } else {
                throw fail(cat("cannot work out the size of array '", d.name, "' from this initialiser"), d.nameRange, d.line);
            }
        } else {
            throw fail(cat("array '", d.name, "' needs a size, e.g. ", d.name, "[10]"), d.nameRange, d.line);
        }
    }

    // MARK: Globals

    bool isConstantAddress(const Expr& e) {
        switch (e.kind) {
        case ExprKind::String: return true;
        case ExprKind::Unary:
            if (e.uop == UnaryOp::Addr) {
                const Expr& x = *e.a();
                if (x.kind == ExprKind::Ident) {
                    auto s = lookup(x.name);
                    if (s && s->kind == Symbol::Kind::Global) return true;
                }
            }
            return false;
        case ExprKind::Ident: {
            auto s = lookup(e.name);
            return s && s->kind == Symbol::Kind::Global && s->type.isArray();
        }
        case ExprKind::Cast: return isConstantAddress(*e.a());
        default: return false;
        }
    }

    void checkGlobalInitializer(const Initializer& ini, const CType& t, VarDecl& d) {
        if (ini.isList) {
            if (t.kind == CType::Array) {
                if ((int)ini.items.size() > t.count)
                    throw fail(cat("too many initialisers for '", d.name, "' (it holds ", t.count, ")"), ini.listRange, d.line);
                for (auto& i : ini.items) checkGlobalInitializer(i, *t.sub, d);
            } else if (t.kind == CType::Struct) {
                const StructDef& s = *t.sdef;
                if (ini.items.size() > s.fields.size()) throw fail(cat("too many initialisers for struct ", s.name), ini.listRange, d.line);
                for (size_t k = 0; k < ini.items.size() && k < s.fields.size(); k++) checkGlobalInitializer(ini.items[k], s.fields[k].type, d);
            } else {
                if (ini.items.size() != 1) throw fail("a scalar needs exactly one initialiser", ini.listRange, d.line);
                checkGlobalInitializer(ini.items[0], t, d);
            }
            return;
        }
        Expr& e = *ini.expr;
        if (t.kind == CType::Array && *t.sub == CType::charT()) {
            if (e.kind != ExprKind::String) throw fail("a char array needs a string or {...} initialiser", e);
            if ((int)e.bytes.size() > t.count)
                throw fail(cat("the string is longer than '", d.name, "' (", (int)e.bytes.size(), " > ", t.count, ")"), e);
            return;
        }
        if (t.kind == CType::Array) throw fail("an array needs a {...} initialiser", e);
        if (t.kind == CType::Struct) throw fail("a struct needs a {...} initialiser", e);
        check(e);
        if (t.isPointer() && isConstantAddress(e)) {
            if (e.kind == ExprKind::String && t != CType::pointer(CType::charT())) throw fail("a string can only initialise a char *", e);
            checkAssignable(t, e, cat("initialise '", d.name, "'"));
            return;
        }
        if (!constantValue(e)) throw fail("a global's initial value must be a constant (globals are filled in before main runs)", e);
        checkAssignable(t, e, cat("initialise '", d.name, "'"));
    }

    // MARK: Functions

    void analyzeFunction(FunctionDecl& f) {
        currentFunction = &f;
        nameCounts.clear();
        f.locals.clear();
        f.hasCalls = false;
        struct Reset {
            FunctionDecl*& cf;
            ~Reset() { cf = nullptr; }
        } reset{currentFunction};
        pushScope(f.line);
        for (auto& p : f.params) {
            if (p->name.empty()) throw fail("parameter needs a name in a function definition", p->range, p->line);
            if (scopes.back().count(p->name)) throw fail(cat("duplicate parameter '", p->name, "'"), p->nameRange, p->line);
            checkObjectType(p->type, p->nameRange, p->line, p->name);
            auto s = newSymbol(p->name, Symbol::Kind::Param, p->type, p->nameRange, p->line);
            s->function = f.name;
            s->uniqueName = uniqueName(p->name);
            p->symbol = s;
            declare(s);
            f.locals.push_back(s);
        }
        if (f.params.size() > 8) {
            throw fail("more than 8 parameters: our calling convention passes arguments only in a0–a7", f.nameRange, f.line);
        }
        if (!f.body || f.body->kind != StmtKind::Block) return;
        // The body shares the parameters' scope, as in C.
        for (auto& s : f.body->items) stmt(*s);
        popScope();
        if (f.returnType != CType::voidT() && f.name != "main" && !alwaysReturns(*f.body)) {
            warn(cat("'", f.name, "' may reach the end without returning a value"), f.nameRange, f.line);
        }
    }

    std::string uniqueName(const std::string& n) {
        int k = nameCounts[n];
        nameCounts[n] = k + 1;
        return k == 0 ? n : cat(n, ".", k);
    }

    bool alwaysReturns(const Stmt& s) {
        switch (s.kind) {
        case StmtKind::Return: return true;
        case StmtKind::Block: return !s.items.empty() && alwaysReturns(*s.items.back());
        case StmtKind::IfElse: return s.elseBody && alwaysReturns(*s.body) && alwaysReturns(*s.elseBody);
        case StmtKind::While: {
            auto v = constantValue(*s.expr);
            return v && *v != 0;
        }
        case StmtKind::For: return s.cond == nullptr;
        case StmtKind::Expr: return s.expr->kind == ExprKind::Call && s.expr->name == "exit";
        default: return false;
        }
    }

    // MARK: Statements

    void stmt(Stmt& s) {
        switch (s.kind) {
        case StmtKind::Block:
            pushScope(s.line);
            for (auto& i : s.items) stmt(*i);
            popScope();
            break;
        case StmtKind::Decl:
            for (auto& d : s.decls) localDecl(*d);
            break;
        case StmtKind::Expr: check(*s.expr); break;
        case StmtKind::IfElse:
            condition(*s.expr);
            stmt(*s.body);
            if (s.elseBody) stmt(*s.elseBody);
            break;
        case StmtKind::While:
            condition(*s.expr);
            loopDepth += 1;
            stmt(*s.body);
            loopDepth -= 1;
            break;
        case StmtKind::DoWhile:
            loopDepth += 1;
            stmt(*s.body);
            loopDepth -= 1;
            condition(*s.expr);
            break;
        case StmtKind::For:
            pushScope(s.line);
            if (s.init) stmt(*s.init);
            if (s.cond) condition(*s.cond);
            if (s.step) check(*s.step);
            loopDepth += 1;
            stmt(*s.body);
            loopDepth -= 1;
            popScope();
            break;
        case StmtKind::Switch: {
            Expr& v = *s.expr;
            check(v);
            if (!v.type.decayed().isInteger()) throw fail("switch needs an integer value", v);
            std::map<int32_t, int> seen;
            bool hasDefault = false;
            switchDepth += 1;
            for (auto& c : s.cases) {
                if (c->value) {
                    check(*c->value);
                    auto k = constantValue(*c->value);
                    if (!k) throw fail("a case label must be a constant", *c->value);
                    c->constant = RV::wrap(*k);
                    auto prev = seen.find(c->constant);
                    if (prev != seen.end()) throw fail(cat("case ", c->constant, " already appears on line ", prev->second), *c->value);
                    seen[c->constant] = c->line;
                } else {
                    if (hasDefault) throw fail("a switch can only have one default", c->range, c->line);
                    hasDefault = true;
                }
                pushScope(c->line);
                for (auto& b : c->body) stmt(*b);
                popScope();
            }
            switchDepth -= 1;
            break;
        }
        case StmtKind::Return: {
            FunctionDecl* f = currentFunction;
            if (!f) return;
            if (s.expr) {
                Expr& e = *s.expr;
                check(e);
                if (f->returnType == CType::voidT()) {
                    if (e.type != CType::voidT()) throw fail(cat("'", f->name, "' returns void, so it cannot return a value"), e);
                } else {
                    checkAssignable(f->returnType, e, cat("return from '", f->name, "'"));
                }
            } else if (f->returnType != CType::voidT() && f->name != "main") {
                throw fail(cat("'", f->name, "' must return a ", f->returnType.description()), s.range, s.line);
            }
            break;
        }
        case StmtKind::Break:
            if (loopDepth == 0 && switchDepth == 0) throw fail("'break' is only allowed inside a loop or switch", s.range, s.line);
            break;
        case StmtKind::Continue:
            if (loopDepth == 0) throw fail("'continue' is only allowed inside a loop", s.range, s.line);
            break;
        case StmtKind::Empty: break;
        }
    }

    void condition(Expr& c) {
        check(c);
        if (!c.type.decayed().isScalar()) throw fail(cat("a condition must be a number or a pointer, not ", c.type.description()), c);
        if (c.kind == ExprKind::Assign && !c.hasOp) warn("'=' in a condition assigns; did you mean '=='?", c.range, c.line);
    }

    void localDecl(VarDecl& d) {
        FunctionDecl* f = currentFunction;
        if (!f) return;
        resolveArraySize(d);
        checkObjectType(d.type, d.nameRange, d.line, d.name);
        auto prev = scopes.back().find(d.name);
        if (prev != scopes.back().end()) {
            throw fail(cat("'", d.name, "' is already declared in this scope (line ", prev->second->line, ")"), d.nameRange, d.line);
        }
        if (d.type.size() > 256 * 1024) {
            throw fail(cat("local '", d.name, "' is ", d.type.size(), " bytes — too big for the stack; make it a global"), d.nameRange, d.line);
        }
        // Check the initialiser before declaring the name: in `int x = x;` the right x is the outer one.
        if (d.initializer) checkLocalInitializer(*d.initializer, d.type, d);
        auto s = newSymbol(d.name, Symbol::Kind::Local, d.type, d.nameRange, d.line);
        s->function = f->name;
        s->uniqueName = uniqueName(d.name);
        d.symbol = s;
        declare(s);
        f->locals.push_back(s);
    }

    void checkLocalInitializer(const Initializer& ini, const CType& t, VarDecl& d) {
        if (ini.isList) {
            if (t.kind == CType::Array) {
                if ((int)ini.items.size() > t.count)
                    throw fail(cat("too many initialisers for '", d.name, "' (it holds ", t.count, ")"), ini.listRange, d.line);
                for (auto& i : ini.items) checkLocalInitializer(i, *t.sub, d);
            } else if (t.kind == CType::Struct) {
                const StructDef& s = *t.sdef;
                if (ini.items.size() > s.fields.size()) throw fail(cat("too many initialisers for struct ", s.name), ini.listRange, d.line);
                for (size_t k = 0; k < ini.items.size() && k < s.fields.size(); k++) checkLocalInitializer(ini.items[k], s.fields[k].type, d);
            } else {
                if (ini.items.size() != 1) throw fail("a scalar needs exactly one initialiser", ini.listRange, d.line);
                checkLocalInitializer(ini.items[0], t, d);
            }
            return;
        }
        Expr& e = *ini.expr;
        if (t.kind == CType::Array && *t.sub == CType::charT()) {
            if (e.kind == ExprKind::String) {
                if ((int)e.bytes.size() > t.count)
                    throw fail(cat("the string is longer than '", d.name, "' (", (int)e.bytes.size(), " > ", t.count, ")"), e);
                check(e);
                return;
            }
            throw fail("a char array needs a string or {...} initialiser", e);
        }
        if (t.kind == CType::Array) throw fail("an array needs a {...} initialiser", e);
        check(e);
        checkAssignable(t, e, cat("initialise '", d.name, "'"));
    }

    // MARK: Expressions

    bool isNullConstant(const Expr& e) {
        switch (e.kind) {
        case ExprKind::Number: return e.number == 0;
        case ExprKind::Cast: return (e.ctype.isPointer() || e.ctype.isInteger()) && isNullConstant(*e.a());
        default: return false;
        }
    }

    /// Can a value of e's type be stored in something of type `t`?
    void checkAssignable(const CType& t, const Expr& e, const std::string& what) {
        CType s = e.type.decayed();
        if (t.isInteger() && s.isInteger()) return;
        if (t.kind == CType::Struct && s.kind == CType::Struct && t.sdef == s.sdef) return;
        if (t.isPointer()) {
            const CType& tp = *t.sub;
            if (isNullConstant(e)) return;
            if (s.isPointer()) {
                const CType& spt = *s.sub;
                if (tp == spt || tp.isVoid() || spt.isVoid()) return;
                warn(cat(what, ": converting ", s.description(), " to ", t.description(), " (different pointer types)"), e.range, e.line);
                return;
            }
            if (s.isInteger())
                throw fail(cat("cannot ", what, " with an integer: ", t.description(), " needs a pointer (cast with (", t.description(),
                               ") if you really mean it)"),
                           e);
        }
        if (t.isInteger() && s.isPointer())
            throw fail(cat("cannot ", what, " with a pointer: ", t.description(), " is an integer (cast with (", t.description(),
                           ") if you really mean it)"),
                       e);
        if (s.isVoid()) throw fail(cat("cannot ", what, " with a void value"), e);
        throw fail(cat("cannot ", what, ": ", s.description(), " is not compatible with ", t.description()), e);
    }

    /// The "usual arithmetic conversions": char promotes to int; if either side is unsigned, both become unsigned.
    static CType arith(const CType& a, const CType& b) { return (a == CType::uintT() || b == CType::uintT()) ? CType::uintT() : CType::intT(); }
    static CType promote(const CType& a) { return a == CType::uintT() ? CType::uintT() : CType::intT(); }

    void requireLValue(const Expr& e, const std::string& what) {
        if (!e.isLValue) throw fail(cat(what, " needs a variable or memory location on the left, not an expression"), e);
        if (e.type.isArray()) throw fail(cat(what, ": an array cannot be assigned as a whole; assign its elements"), e);
    }

    void markAddressTaken(const Expr& e) {
        switch (e.kind) {
        case ExprKind::Ident:
            if (e.symbol) e.symbol->addressTaken = true;
            break;
        case ExprKind::Member:
            if (!e.arrow) markAddressTaken(*e.a());
            break;
        case ExprKind::Index:
            if (e.a()->type.isArray()) markAddressTaken(*e.a());
            break;
        default: break;
        }
    }

    std::string internString(const std::vector<uint8_t>& bytes) {
        auto f = stringLabels.find(bytes);
        if (f != stringLabels.end()) return f->second;
        std::string l = cat(".LC", stringCount);
        stringCount += 1;
        stringLabels[bytes] = l;
        result->strings.push_back({l, bytes});
        return l;
    }

    void check(Expr& e) {
        switch (e.kind) {
        case ExprKind::Number:
            // The parser set int or unsigned already.
            e.isLValue = false;
            break;
        case ExprKind::String:
            e.type = CType::array(CType::charT(), (int)e.bytes.size() + 1);
            e.label = internString(e.bytes);
            break;
        case ExprKind::Ident: {
            SymbolP s = lookup(e.name);
            if (!s) {
                if (e.name == "NULL") {
                    auto zero = std::make_shared<Expr>(-e.id, ExprKind::Number, e.range, e.line);
                    zero->number = 0;
                    e.kind = ExprKind::Cast;
                    e.ctype = CType::pointer(CType::voidT());
                    e.kids = {zero};
                    e.name.clear();
                    check(e);
                    return;
                }
                if (Library::byName(e.name) || e.name == "printf") throw fail(cat("'", e.name, "' is a function: call it with ", e.name, "(...)"), e);
                throw fail(cat("'", e.name, "' is not declared"), e);
            }
            if (s->kind == Symbol::Kind::Function) throw fail(cat("'", e.name, "' is a function; function pointers are not supported"), e);
            e.symbol = s;
            e.type = s->type;
            e.isLValue = true;
            break;
        }
        case ExprKind::Binary:
            check(*e.a());
            check(*e.b());
            binary(e, e.bop, *e.a(), *e.b());
            break;
        case ExprKind::Unary: {
            Expr& a = *e.a();
            check(a);
            CType t = a.type.decayed();
            switch (e.uop) {
            case UnaryOp::Neg: case UnaryOp::Plus: case UnaryOp::BitNot:
                if (!t.isInteger()) throw fail(cat("unary '", unaryOpText(e.uop), "' needs a number, not ", t.description()), e);
                e.type = promote(t);
                e.opType = e.type;
                break;
            case UnaryOp::Not:
                if (!t.isScalar()) throw fail(cat("'!' needs a number or pointer, not ", t.description()), e);
                e.type = CType::intT();
                break;
            case UnaryOp::Deref:
                if (!t.isPointer()) throw fail(cat("'*' needs a pointer, but this is ", t.description()), e);
                if (t.sub->isVoid()) throw fail("cannot dereference a void * (cast it to a real pointer type first)", e);
                e.type = *t.sub;
                e.isLValue = true;
                break;
            case UnaryOp::Addr:
                if (!(a.isLValue || a.type.isArray())) throw fail("'&' needs a variable or memory location", e);
                markAddressTaken(a);
                e.type = CType::pointer(a.type);
                break;
            }
            break;
        }
        case ExprKind::Assign: {
            Expr& lhs = *e.a();
            Expr& rhs = *e.b();
            check(lhs);
            check(rhs);
            requireLValue(lhs, "assignment");
            if (e.hasOp) {
                CType lt = lhs.type, rt = rhs.type.decayed();
                BinaryOp op = e.bop;
                if ((op == BinaryOp::Add || op == BinaryOp::Sub) && lt.isPointer()) {
                    if (!rt.isInteger()) throw fail(cat("pointer ", binaryOpText(op), "= needs an integer on the right"), e);
                    e.opType = lt;
                } else {
                    if (!(lt.isInteger() && rt.isInteger())) throw fail(cat("'", binaryOpText(op), "=' needs numbers on both sides"), e);
                    e.opType = (op == BinaryOp::Shl || op == BinaryOp::Shr) ? promote(lt) : arith(lt, rt);
                }
            } else {
                checkAssignable(lhs.type, rhs, cat("assign to '", describe(lhs), "'"));
            }
            e.type = lhs.type;
            break;
        }
        case ExprKind::IncDec: {
            Expr& a = *e.a();
            check(a);
            requireLValue(a, e.isIncrement ? "'++'" : "'--'");
            if (!a.type.isScalar()) throw fail(cat("'", e.isIncrement ? "++" : "--", "' needs a number or pointer"), e);
            if (a.type.isPointer() && a.type.sub->isVoid()) throw fail("cannot do arithmetic on a void *", e);
            e.type = a.type;
            e.opType = a.type.isPointer() ? a.type : promote(a.type);
            break;
        }
        case ExprKind::Call: call(e); break;
        case ExprKind::Index: {
            check(*e.a());
            check(*e.b());
            CType b = e.a()->type.decayed(), i = e.b()->type.decayed();
            if (b.isInteger() && i.isPointer()) {
                // C allows 3[a]. Swap so the pointer is first.
                std::swap(e.kids[0], e.kids[1]);
                std::swap(b, i);
            }
            const Expr& base = *e.a();
            if (!b.isPointer()) throw fail(cat("only arrays and pointers can be indexed with [ ]; this is ", base.type.description()), e);
            if (!i.isInteger()) throw fail("an array index must be an integer", *e.b());
            if (b.sub->isVoid()) throw fail("cannot index a void *", e);
            e.type = *b.sub;
            e.isLValue = true;
            break;
        }
        case ExprKind::Member: {
            Expr& base = *e.a();
            check(base);
            StructDefP st;
            if (e.arrow) {
                CType bt = base.type.decayed();
                if (!(bt.isPointer() && bt.sub->kind == CType::Struct))
                    throw fail(cat("'->' needs a pointer to a struct, but this is ", base.type.description()), e);
                st = bt.sub->sdef;
            } else {
                if (base.type.kind != CType::Struct) {
                    if (base.type.isPointer() && base.type.sub->kind == CType::Struct) throw fail("this is a pointer: use '->' instead of '.'", e);
                    throw fail(cat("'.' needs a struct, but this is ", base.type.description()), e);
                }
                st = base.type.sdef;
            }
            const StructDef::Field* f = st->field(e.name);
            if (!f) throw fail(cat("struct ", st->name, " has no field '", e.name, "'"), e);
            e.type = f->type;
            e.fieldOffset = f->offset;
            e.isLValue = true;
            break;
        }
        case ExprKind::Ternary: {
            Expr& c = *e.a();
            Expr& a = *e.b();
            Expr& b = *e.c();
            check(c);
            check(a);
            check(b);
            if (!c.type.decayed().isScalar()) throw fail("the ?: condition must be a number or pointer", c);
            CType at = a.type.decayed(), bt = b.type.decayed();
            if (at.isInteger() && bt.isInteger()) {
                e.type = arith(at, bt);
            } else if (at.isPointer() && bt.isPointer()) {
                e.type = at.sub->isVoid() ? bt : at;
            } else if (at.isPointer() && isNullConstant(b)) {
                e.type = at;
            } else if (bt.isPointer() && isNullConstant(a)) {
                e.type = bt;
            } else if (at.isVoid() && bt.isVoid()) {
                e.type = CType::voidT();
            } else if (at.kind == CType::Struct && bt.kind == CType::Struct && at.sdef == bt.sdef) {
                throw fail("?: on structs is not supported", e);
            } else {
                throw fail(cat("the two sides of ?: have incompatible types (", at.description(), " and ", bt.description(), ")"), e);
            }
            break;
        }
        case ExprKind::SizeofType:
            if (e.ctype.isVoid()) throw fail("sizeof(void) is not allowed", e);
            e.type = CType::uintT();
            break;
        case ExprKind::SizeofExpr: {
            Expr& a = *e.a();
            check(a);
            e.type = CType::uintT();
            e.kind = ExprKind::SizeofType;
            e.ctype = a.type;
            e.kids.clear();
            break;
        }
        case ExprKind::Cast: {
            Expr& a = *e.a();
            check(a);
            CType s = a.type.decayed();
            const CType& t = e.ctype;
            if (t.isVoid()) {
                e.type = CType::voidT();
                return;
            }
            if (!t.isScalar()) throw fail(cat("can only cast to a number or pointer type, not ", t.description()), e);
            if (!s.isScalar()) throw fail(cat("cannot cast ", a.type.description(), " to ", t.description()), e);
            e.type = t;
            break;
        }
        case ExprKind::Comma:
            check(*e.a());
            check(*e.b());
            e.type = e.b()->type.decayed();
            break;
        }
    }

    std::string describe(const Expr& e) {
        switch (e.kind) {
        case ExprKind::Ident: return e.name;
        case ExprKind::Index: return describe(*e.a()) + "[…]";
        case ExprKind::Member: return describe(*e.a()) + (e.arrow ? "->" : ".") + e.name;
        case ExprKind::Unary:
            if (e.uop == UnaryOp::Deref) return "*" + describe(*e.a());
            return "expression";
        default: return "expression";
        }
    }

    void binary(Expr& e, BinaryOp op, const Expr& a, const Expr& b) {
        CType at = a.type.decayed(), bt = b.type.decayed();
        switch (op) {
        case BinaryOp::Add:
            if (at.isPointer() && bt.isInteger()) {
                requireComplete(at, e);
                e.type = at;
                e.opType = at;
            } else if (at.isInteger() && bt.isPointer()) {
                requireComplete(bt, e);
                e.type = bt;
                e.opType = bt;
            } else if (at.isInteger() && bt.isInteger()) {
                e.type = arith(at, bt);
                e.opType = e.type;
            } else {
                throw fail(cat("cannot add ", at.description(), " and ", bt.description()), e);
            }
            break;
        case BinaryOp::Sub:
            if (at.isPointer() && bt.isInteger()) {
                requireComplete(at, e);
                e.type = at;
                e.opType = at;
            } else if (at.isPointer() && bt.isPointer()) {
                if (*at.sub != *bt.sub)
                    throw fail(cat("cannot subtract pointers of different types (", at.description(), " and ", bt.description(), ")"), e);
                requireComplete(at, e);
                e.type = CType::intT();
                e.opType = at;
            } else if (at.isInteger() && bt.isInteger()) {
                e.type = arith(at, bt);
                e.opType = e.type;
            } else {
                throw fail(cat("cannot subtract ", bt.description(), " from ", at.description()), e);
            }
            break;
        case BinaryOp::Mul: case BinaryOp::Div: case BinaryOp::Rem: case BinaryOp::BitAnd: case BinaryOp::BitOr: case BinaryOp::BitXor: {
            if (!(at.isInteger() && bt.isInteger()))
                throw fail(cat("'", binaryOpText(op), "' needs numbers on both sides (got ", at.description(), " and ", bt.description(), ")"), e);
            e.type = arith(at, bt);
            e.opType = e.type;
            if (op == BinaryOp::Div || op == BinaryOp::Rem) {
                auto v = constantValue(b);
                if (v && *v == 0)
                    warn(cat("division by zero (RISC-V gives ", op == BinaryOp::Div ? "-1" : "the dividend", " instead of crashing)"), e.range, e.line);
            }
            break;
        }
        case BinaryOp::Shl: case BinaryOp::Shr: {
            if (!(at.isInteger() && bt.isInteger())) throw fail(cat("'", binaryOpText(op), "' needs numbers on both sides"), e);
            e.type = promote(at);
            e.opType = e.type;
            auto v = constantValue(b);
            if (v && (*v < 0 || *v > 31)) warn(cat("shifting by ", *v, ": RISC-V only uses the low 5 bits of the shift amount"), e.range, e.line);
            break;
        }
        case BinaryOp::Lt: case BinaryOp::Gt: case BinaryOp::Le: case BinaryOp::Ge: case BinaryOp::Eq: case BinaryOp::Ne:
            if (at.isInteger() && bt.isInteger()) {
                e.opType = arith(at, bt);
                if (e.opType == CType::uintT() && at != bt && op != BinaryOp::Eq && op != BinaryOp::Ne) {
                    if ((at == CType::intT() && !isNonNegativeConstant(a)) || (bt == CType::intT() && !isNonNegativeConstant(b))) {
                        warn("comparing a signed and an unsigned value: the signed one is treated as unsigned", e.range, e.line);
                    }
                }
            } else if (at.isPointer() && bt.isPointer()) {
                if (*at.sub != *bt.sub && !(at.sub->isVoid() || bt.sub->isVoid())) {
                    warn(cat("comparing different pointer types (", at.description(), " and ", bt.description(), ")"), e.range, e.line);
                }
                e.opType = CType::uintT();
            } else if ((at.isPointer() && isNullConstant(b)) || (bt.isPointer() && isNullConstant(a))) {
                e.opType = CType::uintT();
            } else {
                throw fail(cat("cannot compare ", at.description(), " with ", bt.description()), e);
            }
            e.type = CType::intT();
            break;
        case BinaryOp::LogAnd: case BinaryOp::LogOr:
            if (!(at.isScalar() && bt.isScalar())) throw fail(cat("'", binaryOpText(op), "' needs numbers or pointers"), e);
            e.type = CType::intT();
            break;
        }
    }

    bool isNonNegativeConstant(const Expr& e) {
        auto v = constantValue(e);
        return v && *v >= 0;
    }

    void requireComplete(const CType& p, const Expr& e) {
        const CType* t = p.pointee();
        if (!t) return;
        if (t->isVoid()) throw fail("cannot do arithmetic on a void * (cast it to char * first)", e);
        if (t->kind == CType::Struct && !t->sdef->isComplete) throw fail(cat("struct ", t->sdef->name, " is incomplete"), e);
    }

    void call(Expr& e) {
        const std::string& name = e.name;
        auto& args = e.kids;
        for (auto& a : args) check(*a);
        if (currentFunction) currentFunction->hasCalls = true;
        if (name == "printf") {
            printf(e);
            return;
        }
        std::vector<CType> params;
        CType ret;
        SymbolP s = lookup(name);
        if (s && s->kind == Symbol::Kind::Function && s->type.kind == CType::Function) {
            params = *s->type.params;
            ret = *s->type.sub;
            referencedFunctions.insert(name);
        } else if (s && s->kind != Symbol::Kind::Function) {
            throw fail(cat("'", name, "' is a variable, not a function"), e);
        } else if (auto lib = Library::byName(name)) {
            params = lib->params;
            ret = lib->returnType;
            result->libraryUsed.insert(name);
        } else {
            throw fail(cat("function '", name, "' is not declared (declare or define it above this line)"), e);
        }
        if (args.size() != params.size()) {
            int pc = (int)params.size(), ac = (int)args.size();
            throw fail(cat("'", name, "' takes ", pc, " argument", pc == 1 ? "" : "s", " but ", ac, " ", ac == 1 ? "was" : "were", " given"), e);
        }
        for (size_t i = 0; i < args.size(); i++) {
            checkAssignable(params[i], *args[i], cat("pass argument ", (int)i + 1, " of '", name, "'"));
        }
        e.type = ret;
    }

    void printf(Expr& e) {
        auto& args = e.kids;
        if (args.empty() || args[0]->kind != ExprKind::String) {
            throw fail("printf needs a string literal as its format, e.g. printf(\"%d\\n\", x)", e);
        }
        const Expr& fmt = *args[0];
        std::string err;
        auto parsed = PrintfFormat::parse(fmt.bytes, &err);
        if (!parsed) throw fail(err, fmt);
        size_t ai = 1;
        for (auto& piece : *parsed) {
            if (!piece.isConversion) {
                if (piece.text.size() == 1) {
                    result->libraryUsed.insert("print_char");
                } else {
                    internString(piece.text);
                    result->libraryUsed.insert("print_str");
                }
                continue;
            }
            char c = piece.conversion;
            if (ai >= args.size()) throw fail("printf: the format has more % conversions than arguments", e);
            const Expr& a = *args[ai];
            CType t = a.type.decayed();
            ai += 1;
            switch (c) {
            case 'd': case 'i':
                if (!t.isInteger()) throw fail(cat("printf %", c, " needs an integer, but argument ", (int)ai - 1, " is ", t.description()), a);
                result->libraryUsed.insert("print_int");
                break;
            case 'u':
                if (!t.isInteger()) throw fail("printf %u needs an integer", a);
                result->libraryUsed.insert("print_uint");
                break;
            case 'x': case 'X': case 'p':
                if (!t.isScalar()) throw fail(cat("printf %", c, " needs an integer or pointer"), a);
                result->libraryUsed.insert("print_hex");
                break;
            case 'c':
                if (!t.isInteger()) throw fail("printf %c needs a character", a);
                result->libraryUsed.insert("print_char");
                break;
            case 's':
                if (t != CType::pointer(CType::charT()))
                    throw fail(cat("printf %s needs a char * string, but argument ", (int)ai - 1, " is ", t.description()), a);
                result->libraryUsed.insert("print_str");
                break;
            default: break;
            }
        }
        if (ai < args.size()) {
            int extra = (int)(args.size() - ai);
            throw fail(cat("printf has ", extra, " more argument", extra == 1 ? "" : "s", " than % conversions"), *args[ai]);
        }
        e.type = CType::intT();
    }
};

}  // namespace

SemaResultP analyze(Program& prog, bool library) {
    Sema s(prog, library);
    return s.run();
}

}  // namespace sp
