#include "Frontend.h"

namespace sp {

namespace {

/// Recursive-descent parser: tokens in, abstract syntax tree out.
///
/// Each grammar rule is one function. Binary operators use one function
/// per precedence level, from `comma` (loosest) down to `primary` (tightest).
/// The parser stops at the first syntax error (thrown as a Diagnostic).
class Parser {
public:
    explicit Parser(const std::vector<Token>& t) : toks(t) {}

    ProgramP program() {
        auto prog = std::make_shared<Program>();
        while (!atEnd()) {
            if (accept(";")) continue;
            topLevel(*prog);
        }
        prog->structs = structList;
        prog->nodeCount = nextID;
        return prog;
    }

    std::vector<Diagnostic> warnings;

private:
    const std::vector<Token>& toks;
    size_t p = 0;
    int nextID = 0;
    std::map<std::string, StructDefP> structs;
    std::vector<StructDefP> structList;
    /// typedef scopes: names that mean a type. Innermost last.
    std::vector<std::map<std::string, CType>> typedefs{{}};

    struct ScopeGuard {
        std::vector<std::map<std::string, CType>>& t;
        explicit ScopeGuard(std::vector<std::map<std::string, CType>>& v) : t(v) { t.emplace_back(); }
        ~ScopeGuard() { t.pop_back(); }
    };

    // MARK: Helpers

    const Token& cur() const { return toks[p]; }
    const Token& peek(size_t k = 1) const { return toks[std::min(p + k, toks.size() - 1)]; }
    bool atEnd() const { return cur().kind == TokenKind::Eof; }

    int id() { return ++nextID; }

    Diagnostic fail(const std::string& msg, const Token* at = nullptr) const {
        const Token& t = at ? *at : cur();
        return Diagnostic(Diagnostic::Severity::Error, Stage::Parser, msg, t.range, t.line);
    }

    Token advance() {
        Token t = cur();
        if (p < toks.size() - 1) p += 1;
        return t;
    }

    bool accept(const char* punct) {
        if (cur().isPunct(punct)) {
            advance();
            return true;
        }
        return false;
    }

    bool acceptKeyword(const char* k) {
        if (cur().isKeyword(k)) {
            advance();
            return true;
        }
        return false;
    }

    Token expect(const char* punct, const char* what = nullptr) {
        if (cur().isPunct(punct)) return advance();
        std::string found = cur().kind == TokenKind::Eof ? "the end of the file" : cat("'", cur().text, "'");
        // Point a missing ';' at the end of the previous token, which is where it belongs.
        if (std::string(punct) == ";" && p > 0) {
            const Token& prev = toks[p - 1];
            throw Diagnostic(Diagnostic::Severity::Error, Stage::Parser, cat("expected ';' after ", what ? what : "this", ", found ", found),
                             SourceRange(prev.range.end, prev.range.end + 1), prev.line);
        }
        throw fail(cat("expected '", punct, "'", what ? cat(" ", what) : "", ", found ", found));
    }

    Token expectIdentifier(const char* what) {
        if (cur().kind == TokenKind::Identifier) return advance();
        if (cur().kind == TokenKind::Keyword) throw fail(cat("'", cur().text, "' is a keyword and cannot be used as ", what));
        throw fail(cat("expected ", what, ", found '", cur().text, "'"));
    }

    SourceRange rangeFrom(const Token& start) const { return SourceRange(start.range.start, toks[p > 0 ? p - 1 : 0].range.end); }

    const CType* lookupTypedef(const std::string& name) const {
        for (auto it = typedefs.rbegin(); it != typedefs.rend(); ++it) {
            auto f = it->find(name);
            if (f != it->end()) return &f->second;
        }
        return nullptr;
    }

    /// Does the token begin a type (so this is a declaration)?
    bool startsType(const Token& t) const {
        if (t.kind == TokenKind::Keyword) {
            static const std::set<std::string> kw = {"int", "char", "void", "unsigned", "signed", "long", "short", "const", "static",
                                                     "extern", "volatile", "struct", "typedef", "float", "double", "register", "auto",
                                                     "enum", "union"};
            return kw.count(t.text) > 0;
        }
        return t.kind == TokenKind::Identifier && lookupTypedef(t.text) != nullptr;
    }

    // MARK: Program

    struct Specifiers {
        CType type;
        bool isTypedef = false;
        bool isStatic = false;
        bool isExtern = false;
        StructDefP definedStruct;
    };

    void topLevel(Program& prog) {
        Token start = cur();
        if (!startsType(cur())) {
            if (cur().kind == TokenKind::Identifier && peek().isPunct("(")) {
                throw fail(cat("'", cur().text, "' needs a return type, e.g. int ", cur().text, "(...)"));
            }
            throw fail(cat("expected a declaration (a function or a global variable), found '", cur().text, "'"));
        }
        Specifiers spec = specifiers();
        if (spec.definedStruct && cur().isPunct(";")) {
            advance();
            prog.items.push_back({TopLevel::Kind::StructDef, nullptr, {}, spec.definedStruct, rangeFrom(start), start.line});
            return;
        }
        if (spec.definedStruct) {
            prog.items.push_back({TopLevel::Kind::StructDef, nullptr, {}, spec.definedStruct, rangeFrom(start), start.line});
        }
        if (spec.isTypedef) {
            typedefDeclarators(spec);
            return;
        }
        std::vector<VarDeclP> globals;
        while (true) {
            Token declStart = cur();
            Declarator d = declarator(spec.type);
            if (d.type.kind == CType::Function) {
                if (!globals.empty()) throw fail("a function cannot be declared in a list with variables", &declStart);
                int fid = id();
                auto fn = std::make_shared<FunctionDecl>(fid, d.name, *d.type.sub, d.params ? *d.params : std::vector<VarDeclP>{}, nullptr,
                                                         rangeFrom(start), d.nameRange, declStart.line);
                if (cur().isPunct("{")) {
                    ScopeGuard g(typedefs);
                    fn->body = block();
                } else {
                    expect(";", "the function declaration");
                }
                prog.items.push_back({TopLevel::Kind::Function, fn, {}, nullptr, SourceRange(), 0});
                return;
            }
            std::optional<Initializer> initializer;
            if (accept("=")) initializer = initializerList();
            int vid = id();
            globals.push_back(std::make_shared<VarDecl>(vid, d.name, d.type, rangeFrom(declStart), d.nameRange, declStart.line, initializer));
            if (accept(",")) continue;
            expect(";", "the declaration");
            break;
        }
        prog.items.push_back({TopLevel::Kind::Globals, nullptr, globals, nullptr, SourceRange(), 0});
    }

    void typedefDeclarators(const Specifiers& spec) {
        do {
            Declarator d = declarator(spec.type);
            auto& scope = typedefs.back();
            if (scope.count(d.name)) {
                throw Diagnostic(Diagnostic::Severity::Error, Stage::Parser, cat("typedef '", d.name, "' is already defined"), d.nameRange,
                                 cur().line);
            }
            scope[d.name] = d.type;
        } while (accept(","));
        expect(";", "the typedef");
    }

    // MARK: Types

    Specifiers specifiers() {
        std::optional<CType> base;
        bool sawUnsigned = false, sawSigned = false, sawInt = false, sawChar = false, sawLong = false;
        Specifiers spec;
        spec.type = CType::intT();
        Token start = cur();
        while (true) {
            const Token& t = cur();
            if (t.kind == TokenKind::Identifier && !base && !sawUnsigned && !sawSigned && !sawInt && !sawChar && !sawLong) {
                if (const CType* td = lookupTypedef(t.text)) {
                    CType copy = *td;
                    advance();
                    base = copy;
                    continue;
                }
            }
            if (t.kind != TokenKind::Keyword) break;
            const std::string& k = t.text;
            if (k == "const" || k == "volatile" || k == "register" || k == "auto") advance();
            else if (k == "static") { advance(); spec.isStatic = true; }
            else if (k == "extern") { advance(); spec.isExtern = true; }
            else if (k == "typedef") { advance(); spec.isTypedef = true; }
            else if (k == "void") { advance(); base = CType::voidT(); }
            else if (k == "char") { advance(); sawChar = true; }
            else if (k == "int") { advance(); sawInt = true; }
            else if (k == "unsigned") { advance(); sawUnsigned = true; }
            else if (k == "signed") { advance(); sawSigned = true; }
            else if (k == "long") { advance(); sawLong = true; }
            else if (k == "short") throw fail("'short' is not supported here; use int or char");
            else if (k == "float" || k == "double") throw fail(cat("floating point ('", k, "') is not supported: RV32IM has no FPU, use int"));
            else if (k == "enum" || k == "union") throw fail(cat("'", k, "' is not supported"));
            else if (k == "struct") {
                advance();
                StructDefP s = structSpecifier();
                base = CType::structure(s);
                if (s->isComplete && toks[p - 1].isPunct("}")) spec.definedStruct = s;
            } else break;
        }
        if (sawChar) {
            if (sawUnsigned) throw fail("'unsigned char' is not supported; use char or unsigned", &start);
            base = CType::charT();
        } else if (sawUnsigned) {
            base = CType::uintT();
        } else if (sawInt || sawSigned || sawLong) {
            if (base) throw fail("conflicting types in this declaration", &start);
            base = CType::intT();
        }
        if (!base) throw fail(cat("expected a type name, found '", cur().text, "'"));
        spec.type = *base;
        return spec;
    }

    StructDefP structSpecifier() {
        std::string name;
        Token nameTok = cur();
        if (cur().kind == TokenKind::Identifier) name = advance().text;
        if (cur().isPunct("{")) {
            StructDefP s;
            auto existing = name.empty() ? structs.end() : structs.find(name);
            if (!name.empty() && existing != structs.end()) {
                if (existing->second->isComplete) throw fail(cat("struct ", name, " is already defined"), &nameTok);
                s = existing->second;
            } else {
                s = std::make_shared<StructDef>(name.empty() ? cat("<anonymous ", (int)structList.size() + 1, ">") : name,
                                                (int)structList.size() + 1);
                structList.push_back(s);
                if (!name.empty()) structs[name] = s;
            }
            advance();
            int offset = 0, align = 1;
            while (!cur().isPunct("}")) {
                if (atEnd()) throw fail(cat("struct ", s->name, " is missing its closing '}'"));
                Specifiers fs = specifiers();
                do {
                    Token fieldTok = cur();
                    Declarator d = declarator(fs.type);
                    if (d.type.kind == CType::Function) throw fail("a struct cannot contain a function", &fieldTok);
                    if (d.type.isVoid()) throw fail("a field cannot have type void", &fieldTok);
                    if (d.type.kind == CType::Struct && !d.type.sdef->isComplete) {
                        const std::string& in = d.type.sdef->name;
                        throw fail(cat("field '", d.name, "' has incomplete type struct ", in, " (use a pointer: struct ", in, " *", d.name, ")"),
                                   &fieldTok);
                    }
                    if (d.type.kind == CType::Array && d.type.count < 0) throw fail("a struct field needs an array size", &fieldTok);
                    if (s->field(d.name)) throw fail(cat("duplicate field '", d.name, "'"), &fieldTok);
                    int a = d.type.align();
                    offset = (offset + a - 1) / a * a;
                    s->fields.push_back({d.name, d.type, offset});
                    offset += d.type.size();
                    align = std::max(align, a);
                } while (accept(","));
                expect(";", "the field");
            }
            advance();
            s->size = std::max(align, (offset + align - 1) / align * align);
            s->align = align;
            s->isComplete = true;
            if (s->fields.empty()) throw fail(cat("struct ", s->name, " has no fields"), &nameTok);
            return s;
        }
        if (name.empty()) throw fail("expected a struct name or '{'");
        auto f = structs.find(name);
        if (f != structs.end()) return f->second;
        // Forward reference: `struct node *next;` before struct node is complete.
        auto s = std::make_shared<StructDef>(name, (int)structList.size() + 1);
        structList.push_back(s);
        structs[name] = s;
        return s;
    }

    struct Declarator {
        std::string name;
        SourceRange nameRange;
        CType type;
        std::optional<std::vector<VarDeclP>> params;
    };

    /// Parses `*name[3]`, `(*p)[4]`, `f(int a, char *b)` and so on, applied to a base type.
    Declarator declarator(const CType& base, bool abstract = false) {
        int stars = 0;
        while (accept("*")) {
            stars += 1;
            while (acceptKeyword("const") || acceptKeyword("volatile")) {}
        }
        std::string name;
        SourceRange nameRange;
        // Pointer levels written inside parentheses: int (*p)[4] is a pointer to an array.
        int innerStars = -1;
        if (cur().isPunct("(") && peek().isPunct("*")) {
            advance();
            innerStars = 0;
            while (accept("*")) innerStars += 1;
            if (cur().kind == TokenKind::Identifier) {
                Token t = advance();
                name = t.text;
                nameRange = t.range;
            } else if (!abstract) {
                throw fail(cat("expected a name, found '", cur().text, "'"));
            }
            expect(")", "to close the declarator");
        } else if (cur().kind == TokenKind::Identifier) {
            Token t = advance();
            name = t.text;
            nameRange = t.range;
        } else if (!abstract) {
            if (cur().kind == TokenKind::Keyword) throw fail(cat("'", cur().text, "' is a keyword and cannot be used as a name"));
            throw fail(cat("expected a name, found '", cur().text, "'"));
        }
        CType type = base;
        for (int k = 0; k < stars; k++) type = CType::pointer(type);
        std::optional<std::vector<VarDeclP>> params;
        // Suffixes: arrays [n] and parameter lists (...). Array dimensions
        // apply right to left: int a[2][3] is "array of 2 arrays of 3 ints".
        std::vector<int> dims;
        if (cur().isPunct("(") && innerStars < 0) {
            advance();
            params = parameterList();
            std::vector<CType> pts;
            for (auto& v : *params) pts.push_back(v->type);
            type = CType::function(type, pts);
        }
        while (cur().isPunct("[")) {
            Token open = advance();
            if (cur().isPunct("]")) {
                advance();
                if (!dims.empty()) throw fail("only the first array dimension can be left out", &open);
                dims.push_back(-1);
                continue;
            }
            ExprP e = assignment();
            auto n = constantValue(*e);
            if (!n) throw fail("array size must be a constant", &open);
            if (*n <= 0) throw fail(cat("array size must be positive (got ", *n, ")"), &open);
            if (*n > (1 << 22)) throw fail(cat("array is too large (", *n, " elements)"), &open);
            expect("]");
            dims.push_back((int)*n);
        }
        for (auto it = dims.rbegin(); it != dims.rend(); ++it) type = CType::array(type, *it);
        for (int k = 0; k < innerStars; k++) type = CType::pointer(type);
        return Declarator{name, nameRange, type, params};
    }

    std::vector<VarDeclP> parameterList() {
        std::vector<VarDeclP> params;
        if (accept(")")) return params;
        if (cur().isKeyword("void") && peek().isPunct(")")) {
            advance();
            advance();
            return params;
        }
        do {
            Token start = cur();
            if (cur().isPunct("...")) throw fail("variadic functions (...) are not supported");
            if (!startsType(cur())) throw fail(cat("expected a parameter type, found '", cur().text, "'"));
            Specifiers spec = specifiers();
            Declarator d = declarator(spec.type, true);
            // Array parameters are really pointers.
            if (d.type.kind == CType::Array) d.type = CType::pointer(*d.type.sub);
            if (d.type.isVoid()) throw fail("a parameter cannot have type void", &start);
            if (d.type.isStruct()) throw fail("passing a struct by value is not supported; pass a pointer", &start);
            int pid = id();
            SourceRange r = rangeFrom(start);
            params.push_back(std::make_shared<VarDecl>(pid, d.name, d.type, r, d.nameRange.isValid() ? d.nameRange : r, start.line,
                                                       std::nullopt));
        } while (accept(","));
        expect(")", "to close the parameter list");
        return params;
    }

    // MARK: Statements

    StmtP makeStmt(StmtKind k, const Token& start) {
        int sid = id();
        return std::make_shared<Stmt>(sid, k, rangeFrom(start), start.line);
    }

    StmtP block() {
        Token start = expect("{");
        ScopeGuard g(typedefs);
        std::vector<StmtP> items;
        while (!cur().isPunct("}")) {
            if (atEnd()) throw fail(cat("missing '}' to close the block opened on line ", start.line), &start);
            items.push_back(statement());
        }
        advance();
        auto s = makeStmt(StmtKind::Block, start);
        s->items = std::move(items);
        return s;
    }

    StmtP statement() {
        Token start = cur();
        if (startsType(cur()) && !(cur().kind == TokenKind::Identifier && peek().isPunct(":"))) return declarationStatement();
        if (cur().isPunct("{")) return block();
        if (accept(";")) {
            int sid = id();
            return std::make_shared<Stmt>(sid, StmtKind::Empty, start.range, start.line);
        }
        if (cur().kind == TokenKind::Keyword) {
            const std::string k = cur().text;
            if (k == "if") {
                advance();
                expect("(", "after 'if'");
                ExprP c = expression();
                expect(")", "to close the if condition");
                StmtP then = statement();
                StmtP els;
                if (acceptKeyword("else")) els = statement();
                auto s = makeStmt(StmtKind::IfElse, start);
                s->expr = c;
                s->body = then;
                s->elseBody = els;
                return s;
            }
            if (k == "while") {
                advance();
                expect("(", "after 'while'");
                ExprP c = expression();
                expect(")", "to close the while condition");
                StmtP body = statement();
                auto s = makeStmt(StmtKind::While, start);
                s->expr = c;
                s->body = body;
                return s;
            }
            if (k == "do") {
                advance();
                StmtP body = statement();
                if (!acceptKeyword("while")) throw fail("expected 'while' after the do-loop body");
                expect("(", "after 'while'");
                ExprP c = expression();
                expect(")", "to close the while condition");
                expect(";", "do { ... } while (...)");
                auto s = makeStmt(StmtKind::DoWhile, start);
                s->body = body;
                s->expr = c;
                return s;
            }
            if (k == "for") {
                advance();
                expect("(", "after 'for'");
                ScopeGuard g(typedefs);
                StmtP initS;
                if (startsType(cur())) {
                    initS = declarationStatement();
                } else if (!accept(";")) {
                    ExprP e = expression();
                    expect(";", "the for-loop initialiser");
                    int sid = id();
                    initS = std::make_shared<Stmt>(sid, StmtKind::Expr, e->range, e->line);
                    initS->expr = e;
                }
                ExprP cond;
                if (!cur().isPunct(";")) cond = expression();
                expect(";", "the for-loop condition");
                ExprP step;
                if (!cur().isPunct(")")) step = expression();
                expect(")", "to close the for-loop header");
                StmtP body = statement();
                auto s = makeStmt(StmtKind::For, start);
                s->init = initS;
                s->cond = cond;
                s->step = step;
                s->body = body;
                return s;
            }
            if (k == "switch") return switchStatement();
            if (k == "return") {
                advance();
                ExprP e;
                if (!cur().isPunct(";")) e = expression();
                expect(";", "the return statement");
                auto s = makeStmt(StmtKind::Return, start);
                s->expr = e;
                return s;
            }
            if (k == "break") {
                advance();
                expect(";", "'break'");
                return makeStmt(StmtKind::Break, start);
            }
            if (k == "continue") {
                advance();
                expect(";", "'continue'");
                return makeStmt(StmtKind::Continue, start);
            }
            if (k == "case" || k == "default") throw fail(cat("'", k, "' can only appear inside a switch"));
            if (k == "else") throw fail("'else' without a matching 'if'");
            if (k == "goto") throw fail("'goto' is not supported");
        }
        ExprP e = expression();
        expect(";", "the expression");
        auto s = makeStmt(StmtKind::Expr, start);
        s->expr = e;
        return s;
    }

    StmtP switchStatement() {
        Token start = advance();
        expect("(", "after 'switch'");
        ExprP value = expression();
        expect(")", "to close the switch value");
        Token open = expect("{", "to start the switch body");
        std::vector<SwitchCaseP> cases;
        while (!cur().isPunct("}")) {
            if (atEnd()) throw fail(cat("missing '}' to close the switch on line ", start.line), &open);
            Token caseTok = cur();
            ExprP label;
            if (acceptKeyword("case")) {
                label = ternary();
                expect(":", "after the case value");
            } else if (acceptKeyword("default")) {
                expect(":", "after 'default'");
            } else {
                throw fail("expected 'case' or 'default' in the switch body");
            }
            std::vector<StmtP> body;
            while (!cur().isPunct("}") && !cur().isKeyword("case") && !cur().isKeyword("default")) {
                if (atEnd()) break;
                body.push_back(statement());
            }
            cases.push_back(std::make_shared<SwitchCase>(label, body, rangeFrom(caseTok), caseTok.line));
        }
        advance();
        auto s = makeStmt(StmtKind::Switch, start);
        s->expr = value;
        s->cases = std::move(cases);
        return s;
    }

    StmtP declarationStatement() {
        Token start = cur();
        Specifiers spec = specifiers();
        if (spec.isStatic) throw fail("static local variables are not supported; use a global", &start);
        if (spec.isTypedef) {
            typedefDeclarators(spec);
            return makeStmt(StmtKind::Empty, start);
        }
        if (spec.definedStruct && cur().isPunct(";")) {
            advance();
            return makeStmt(StmtKind::Empty, start);
        }
        std::vector<VarDeclP> decls;
        do {
            Token ds = cur();
            Declarator d = declarator(spec.type);
            if (d.type.kind == CType::Function) throw fail("functions cannot be declared inside a function", &ds);
            std::optional<Initializer> initializer;
            if (accept("=")) initializer = initializerList();
            int vid = id();
            decls.push_back(std::make_shared<VarDecl>(vid, d.name, d.type, rangeFrom(ds), d.nameRange, ds.line, initializer));
        } while (accept(","));
        expect(";", "the declaration");
        auto s = makeStmt(StmtKind::Decl, start);
        s->decls = std::move(decls);
        return s;
    }

    Initializer initializerList() {
        if (cur().isPunct("{")) {
            Token start = advance();
            std::vector<Initializer> items;
            while (!cur().isPunct("}")) {
                items.push_back(initializerList());
                if (!accept(",")) break;
            }
            expect("}", "to close the initialiser list");
            Initializer ini;
            ini.isList = true;
            ini.items = std::move(items);
            ini.listRange = rangeFrom(start);
            return ini;
        }
        Initializer ini;
        ini.expr = assignment();
        return ini;
    }

    // MARK: Expressions

    ExprP make(ExprKind kind, const Token& start) {
        int eid = id();
        return std::make_shared<Expr>(eid, kind, rangeFrom(start), start.line);
    }

    ExprP make2(ExprKind kind, const Token& start, ExprP a, ExprP b) {
        auto e = make(kind, start);
        e->kids = {std::move(a), std::move(b)};
        return e;
    }

    ExprP expression() {
        Token start = cur();
        ExprP e = assignment();
        while (accept(",")) {
            ExprP r = assignment();
            e = make2(ExprKind::Comma, start, e, r);
        }
        return e;
    }

    ExprP assignment() {
        Token start = cur();
        ExprP lhs = ternary();
        if (cur().kind == TokenKind::Punct) {
            static const std::map<std::string, std::optional<BinaryOp>> assignOps = {
                {"=", std::nullopt}, {"+=", BinaryOp::Add}, {"-=", BinaryOp::Sub}, {"*=", BinaryOp::Mul}, {"/=", BinaryOp::Div},
                {"%=", BinaryOp::Rem}, {"<<=", BinaryOp::Shl}, {">>=", BinaryOp::Shr}, {"&=", BinaryOp::BitAnd},
                {"|=", BinaryOp::BitOr}, {"^=", BinaryOp::BitXor},
            };
            auto it = assignOps.find(cur().text);
            if (it != assignOps.end()) {
                advance();
                ExprP rhs = assignment();
                ExprP e = make2(ExprKind::Assign, start, lhs, rhs);
                if (it->second) {
                    e->hasOp = true;
                    e->bop = *it->second;
                }
                return e;
            }
        }
        return lhs;
    }

    ExprP ternary() {
        Token start = cur();
        ExprP c = binary(0);
        if (accept("?")) {
            ExprP a = expression();
            expect(":", "in the ?: expression");
            ExprP b = ternary();
            auto e = make(ExprKind::Ternary, start);
            e->kids = {c, a, b};
            return e;
        }
        return c;
    }

    /// Precedence climbing: level 0 is `||`, the last level is `* / %`.
    ExprP binary(size_t level) {
        static const std::vector<std::vector<std::string>> levels = {
            {"||"}, {"&&"}, {"|"}, {"^"}, {"&"}, {"==", "!="}, {"<", ">", "<=", ">="}, {"<<", ">>"}, {"+", "-"}, {"*", "/", "%"},
        };
        if (level == levels.size()) return unary();
        Token start = cur();
        ExprP lhs = binary(level + 1);
        while (cur().kind == TokenKind::Punct &&
               std::find(levels[level].begin(), levels[level].end(), cur().text) != levels[level].end()) {
            BinaryOp op = *binaryOpFromText(advance().text);
            ExprP rhs = binary(level + 1);
            lhs = make2(ExprKind::Binary, start, lhs, rhs);
            lhs->bop = op;
        }
        return lhs;
    }

    ExprP unary() {
        Token start = cur();
        if (cur().kind == TokenKind::Punct) {
            const std::string& t = cur().text;
            if (t == "-" || t == "+" || t == "!" || t == "~" || t == "*" || t == "&") {
                std::string s = advance().text;
                UnaryOp op = s == "-" ? UnaryOp::Neg : s == "+" ? UnaryOp::Plus : s == "!" ? UnaryOp::Not
                           : s == "~" ? UnaryOp::BitNot : s == "*" ? UnaryOp::Deref : UnaryOp::Addr;
                ExprP a = unary();
                auto e = make(ExprKind::Unary, start);
                e->uop = op;
                e->kids = {a};
                return e;
            }
            if (t == "++" || t == "--") {
                bool inc = advance().text == "++";
                ExprP a = unary();
                auto e = make(ExprKind::IncDec, start);
                e->isPrefix = true;
                e->isIncrement = inc;
                e->kids = {a};
                return e;
            }
            if (t == "(" && startsType(peek())) {
                advance();
                Specifiers spec = specifiers();
                Declarator d = declarator(spec.type, true);
                expect(")", "to close the cast");
                ExprP a = unary();
                auto e = make(ExprKind::Cast, start);
                e->ctype = d.type;
                e->kids = {a};
                return e;
            }
        }
        if (acceptKeyword("sizeof")) {
            if (cur().isPunct("(") && startsType(peek())) {
                advance();
                Specifiers spec = specifiers();
                Declarator d = declarator(spec.type, true);
                expect(")", "to close sizeof");
                if (d.type.kind == CType::Struct && !d.type.sdef->isComplete)
                    throw fail(cat("sizeof incomplete type struct ", d.type.sdef->name), &start);
                auto e = make(ExprKind::SizeofType, start);
                e->ctype = d.type;
                return e;
            }
            ExprP a = unary();
            auto e = make(ExprKind::SizeofExpr, start);
            e->kids = {a};
            return e;
        }
        return postfix();
    }

    ExprP postfix() {
        Token start = cur();
        ExprP e = primary();
        while (true) {
            if (accept("[")) {
                ExprP i = expression();
                expect("]", "to close the index");
                e = make2(ExprKind::Index, start, e, i);
            } else if (cur().isPunct("(")) {
                if (e->kind != ExprKind::Ident) throw fail("only named functions can be called (no function pointers)");
                std::string name = e->name;
                advance();
                std::vector<ExprP> args;
                if (!cur().isPunct(")")) {
                    do { args.push_back(assignment()); } while (accept(","));
                }
                expect(")", "to close the argument list");
                e = make(ExprKind::Call, start);
                e->name = name;
                e->kids = std::move(args);
            } else if (accept(".")) {
                Token f = expectIdentifier("a field name");
                ExprP base = e;
                e = make(ExprKind::Member, start);
                e->name = f.text;
                e->kids = {base};
            } else if (accept("->")) {
                Token f = expectIdentifier("a field name");
                ExprP base = e;
                e = make(ExprKind::Member, start);
                e->name = f.text;
                e->arrow = true;
                e->kids = {base};
            } else if (cur().isPunct("++") || cur().isPunct("--")) {
                bool inc = advance().text == "++";
                ExprP base = e;
                e = make(ExprKind::IncDec, start);
                e->isPrefix = false;
                e->isIncrement = inc;
                e->kids = {base};
            } else {
                return e;
            }
        }
    }

    ExprP primary() {
        Token t = cur();
        switch (t.kind) {
        case TokenKind::Number: {
            advance();
            auto e = make(ExprKind::Number, t);
            e->number = t.value;
            e->type = t.isUnsigned ? CType::uintT() : CType::intT();
            return e;
        }
        case TokenKind::Character: {
            advance();
            auto e = make(ExprKind::Number, t);
            e->number = t.value;
            return e;
        }
        case TokenKind::String: {
            advance();
            std::vector<uint8_t> bytes = t.bytes;
            // Adjacent string literals are joined: "ab" "cd" == "abcd".
            while (cur().kind == TokenKind::String) {
                Token n = advance();
                bytes.insert(bytes.end(), n.bytes.begin(), n.bytes.end());
            }
            auto e = make(ExprKind::String, t);
            e->bytes = std::move(bytes);
            return e;
        }
        case TokenKind::Identifier: {
            advance();
            auto e = make(ExprKind::Ident, t);
            e->name = t.text;
            return e;
        }
        case TokenKind::Eof:
            throw fail("the file ended in the middle of an expression");
        default:
            if (t.kind == TokenKind::Punct && t.text == "(") {
                advance();
                ExprP e = expression();
                expect(")", "to close the parenthesis");
                e->range = rangeFrom(t);
                return e;
            }
            if (t.kind == TokenKind::Keyword) throw fail(cat("expected an expression, found the keyword '", t.text, "'"));
            throw fail(cat("expected an expression, found '", t.text, "'"));
        }
    }
};

}  // namespace

std::pair<ProgramP, std::vector<Diagnostic>> parse(const LexResult& lex) {
    Parser ps(lex.tokens);
    try {
        ProgramP prog = ps.program();
        return {prog, ps.warnings};
    } catch (const Diagnostic& d) {
        auto w = ps.warnings;
        w.push_back(d);
        return {nullptr, w};
    }
}

std::optional<int64_t> constantValue(const Expr& e) {
    switch (e.kind) {
    case ExprKind::Number: return e.number;
    case ExprKind::SizeofType: return (int64_t)e.ctype.size();
    case ExprKind::Unary: {
        auto x = constantValue(*e.a());
        if (!x) return std::nullopt;
        switch (e.uop) {
        case UnaryOp::Neg: return (int64_t)(int32_t)(uint32_t)(uint64_t)(-*x);
        case UnaryOp::Plus: return *x;
        case UnaryOp::BitNot: return (int64_t)(~(int32_t)(uint32_t)(uint64_t)*x);
        case UnaryOp::Not: return *x == 0 ? 1 : 0;
        default: return std::nullopt;
        }
    }
    case ExprKind::Binary: {
        auto x = constantValue(*e.a());
        if (!x) return std::nullopt;
        auto y = constantValue(*e.b());
        if (!y) return std::nullopt;
        int32_t r = RV::binary(e.bop, RV::wrap(*x), RV::wrap(*y), false);
        return (int64_t)r;
    }
    case ExprKind::Ternary: {
        auto cv = constantValue(*e.a());
        if (!cv) return std::nullopt;
        return *cv != 0 ? constantValue(*e.b()) : constantValue(*e.c());
    }
    case ExprKind::Cast: {
        auto x = constantValue(*e.a());
        if (!x) return std::nullopt;
        return e.ctype == CType::charT() ? (int64_t)(int8_t)(uint8_t)(uint64_t)*x : *x;
    }
    default: return std::nullopt;
    }
}

}  // namespace sp
