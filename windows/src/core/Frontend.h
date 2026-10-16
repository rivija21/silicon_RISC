// The front end: lexer and tiny preprocessor, the syntax tree, the parser,
// semantic analysis, and the built-in C library's signatures.
#pragma once

#include "Base.h"

namespace sp {

// MARK: - Tokens

enum class TokenKind { Keyword, Identifier, Number, Character, String, Punct, Eof };
const char* tokenKindName(TokenKind k);

struct Token {
    TokenKind kind = TokenKind::Eof;
    std::string text;
    SourceRange range;
    int line = 0;
    int column = 0;
    /// Value of a number or character literal.
    int64_t value = 0;
    /// A number literal with a `u` suffix, or one too large for `int`.
    bool isUnsigned = false;
    /// Bytes of a string literal, without the terminating zero.
    std::vector<uint8_t> bytes;
    /// Set when this token came from expanding a `#define`.
    std::optional<std::string> fromMacro;

    bool isPunct(const char* p) const { return kind == TokenKind::Punct && text == p; }
    bool isKeyword(const char* k) const { return kind == TokenKind::Keyword && text == k; }
};

/// A comment or preprocessor line. Kept only for syntax colouring and the token view.
struct Trivia {
    enum class Kind { Comment, Directive };
    Kind kind;
    SourceRange range;
    std::string text;
};

struct Macro {
    std::string name;
    std::vector<Token> body;
    int line = 0;
};

struct LexResult {
    std::vector<Token> tokens;
    std::vector<Trivia> trivia;
    std::map<std::string, Macro> macros;
    std::vector<Diagnostic> diagnostics;
    bool hasErrors() const;
};

extern const std::set<std::string> cKeywords;

/// Turns C source text into tokens. Also acts as a tiny preprocessor:
/// `#define NAME text` macros are expanded as the identifiers are read,
/// and `#include` lines are noted and skipped (the library is built in).
LexResult lex(const std::string& source);

// MARK: - Types

struct StructDef;
using StructDefP = std::shared_ptr<StructDef>;

/// C types on our 32-bit RISC-V target (ILP32): char is 1 byte and signed,
/// int, unsigned and pointers are 4 bytes.
struct CType {
    enum Kind : uint8_t { Void, Char, Int, UInt, Pointer, Array, Struct, Function };
    Kind kind = Int;
    /// Pointee (pointer), element (array) or return type (function).
    std::shared_ptr<const CType> sub;
    /// Element count of an array (-1 = not given yet).
    int count = 0;
    StructDefP sdef;
    std::shared_ptr<const std::vector<CType>> params;

    CType() = default;
    explicit CType(Kind k) : kind(k) {}
    static CType voidT() { return CType(Void); }
    static CType charT() { return CType(Char); }
    static CType intT() { return CType(Int); }
    static CType uintT() { return CType(UInt); }
    static CType pointer(const CType& t);
    static CType array(const CType& t, int n);
    static CType structure(StructDefP s);
    static CType function(const CType& ret, std::vector<CType> ps);

    bool operator==(const CType& o) const;
    bool operator!=(const CType& o) const { return !(*this == o); }

    int size() const;
    int align() const;
    bool isInteger() const { return kind == Char || kind == Int || kind == UInt; }
    bool isPointer() const { return kind == Pointer; }
    bool isArray() const { return kind == Array; }
    bool isStruct() const { return kind == Struct; }
    bool isFunction() const { return kind == Function; }
    bool isVoid() const { return kind == Void; }
    bool isScalar() const { return isInteger() || isPointer(); }
    bool isUnsigned() const { return kind == UInt || kind == Pointer; }
    /// Pointed-to type of a pointer, or element type of an array (nullptr otherwise).
    const CType* pointee() const { return (kind == Pointer || kind == Array) ? sub.get() : nullptr; }
    const CType& element() const { return *sub; }
    /// Arrays turn into pointers to their first element when used as values.
    CType decayed() const;
    std::string description() const { return spelled(""); }
    /// C-style spelling with an optional declarator name: `int *p`, `char buf[8]`.
    std::string spelled(const std::string& name) const;
};

/// Layout of a `struct`, filled in once its closing brace is seen.
struct StructDef {
    struct Field {
        std::string name;
        CType type;
        int offset = 0;
    };
    std::string name;
    std::vector<Field> fields;
    int size = 0;
    int align = 1;
    bool isComplete = false;
    int id = 0;

    StructDef(std::string n, int i) : name(std::move(n)), id(i) {}
    const Field* field(const std::string& n) const {
        for (auto& f : fields)
            if (f.name == n) return &f;
        return nullptr;
    }
};

// MARK: - Symbols

struct Symbol {
    enum class Kind { Global, Local, Param, Function, Library };
    int id;
    std::string name;
    Kind kind;
    CType type;
    SourceRange declRange;
    int line;
    /// Enclosing function, for locals and parameters.
    std::optional<std::string> function;
    /// Block depth: 0 = file scope, 1 = function parameters, 2+ = nested blocks.
    int depth;
    /// Set when `&x` is taken somewhere. Such a variable must live in memory.
    bool addressTaken = false;
    /// Name used in assembly and IR. Unique per function (shadowed names get a suffix).
    std::string uniqueName;
    /// Functions: defined in this file (vs. only declared).
    bool isDefined = false;
    /// Where the back end put it. Filled in after code generation.
    std::string location;

    Symbol(int i, std::string n, Kind k, CType t, SourceRange r, int l, int d)
        : id(i), name(n), kind(k), type(std::move(t)), declRange(r), line(l), depth(d), uniqueName(n) {}
};
using SymbolP = std::shared_ptr<Symbol>;
const char* symbolKindName(Symbol::Kind k);

// MARK: - Expressions

enum class UnaryOp { Neg, Plus, Not, BitNot, Deref, Addr };
const char* unaryOpText(UnaryOp op);

enum class ExprKind { Number, String, Ident, Binary, Unary, Assign, IncDec, Call, Index, Member, Ternary, SizeofType, SizeofExpr, Cast, Comma };

struct Expr;
using ExprP = std::shared_ptr<Expr>;

struct Expr {
    int id;
    ExprKind kind;
    SourceRange range;
    int line;

    // Payload, depending on the kind.
    int64_t number = 0;              // Number
    std::vector<uint8_t> bytes;      // String
    std::string name;                // Ident name, Call function name, Member field name
    BinaryOp bop = BinaryOp::Add;    // Binary; Assign when hasOp
    bool hasOp = false;              // Assign: `a op= b` (vs plain `a = b`)
    UnaryOp uop = UnaryOp::Neg;      // Unary
    bool isPrefix = false;           // IncDec
    bool isIncrement = false;        // IncDec
    bool arrow = false;              // Member
    CType ctype;                     // SizeofType, Cast
    /// Operands in source order: binary/assign/index/comma [a, b], unary/incDec/member/sizeofExpr/cast [a],
    /// call [args…], ternary [cond, a, b].
    std::vector<ExprP> kids;

    // Filled in by semantic analysis.
    CType type = CType::intT();
    SymbolP symbol;
    bool isLValue = false;
    /// For arithmetic and comparisons: the type the operation is done in (int, unsigned or pointer).
    CType opType = CType::intT();
    /// For a string literal: its label in .data, e.g. ".LC0".
    std::string label;
    /// For a struct member: the byte offset of the field.
    int fieldOffset = 0;

    Expr(int i, ExprKind k, SourceRange r, int l) : id(i), kind(k), range(r), line(l) {}
    const ExprP& a() const { return kids[0]; }
    const ExprP& b() const { return kids[1]; }
    const ExprP& c() const { return kids[2]; }
    const std::vector<ExprP>& children() const { return kids; }
};

// MARK: - Statements and declarations

struct Initializer {
    bool isList = false;
    ExprP expr;
    std::vector<Initializer> items;
    SourceRange listRange;
    SourceRange range() const { return isList ? listRange : expr->range; }
};

struct VarDecl {
    int id;
    std::string name;
    CType type;
    SourceRange range;
    SourceRange nameRange;
    int line;
    std::optional<Initializer> initializer;
    SymbolP symbol;

    VarDecl(int i, std::string n, CType t, SourceRange r, SourceRange nr, int l, std::optional<Initializer> ini)
        : id(i), name(std::move(n)), type(std::move(t)), range(r), nameRange(nr), line(l), initializer(std::move(ini)) {}
};
using VarDeclP = std::shared_ptr<VarDecl>;

struct Stmt;
using StmtP = std::shared_ptr<Stmt>;

struct SwitchCase {
    /// nullptr for `default:`.
    ExprP value;
    int32_t constant = 0;
    std::vector<StmtP> body;
    SourceRange range;
    int line;
    SwitchCase(ExprP v, std::vector<StmtP> b, SourceRange r, int l) : value(std::move(v)), body(std::move(b)), range(r), line(l) {}
};
using SwitchCaseP = std::shared_ptr<SwitchCase>;

enum class StmtKind { Block, Decl, Expr, IfElse, While, DoWhile, For, Switch, Return, Break, Continue, Empty };

struct Stmt {
    int id;
    StmtKind kind;
    SourceRange range;
    int line;

    std::vector<StmtP> items;          // Block
    std::vector<VarDeclP> decls;       // Decl
    ExprP expr;                        // Expr statement, if/while/do condition, switch value, return value (may be null)
    StmtP body;                        // then (if), loop body
    StmtP elseBody;                    // else (may be null)
    StmtP init;                        // for: initialiser statement (may be null)
    ExprP cond, step;                  // for: condition and step (may be null)
    std::vector<SwitchCaseP> cases;    // Switch

    Stmt(int i, StmtKind k, SourceRange r, int l) : id(i), kind(k), range(r), line(l) {}
};

struct FunctionDecl {
    int id;
    std::string name;
    CType returnType;
    std::vector<VarDeclP> params;
    StmtP body;
    SourceRange range;
    SourceRange nameRange;
    int line;
    SymbolP symbol;
    /// Every local and parameter, in declaration order (filled by semantic analysis).
    std::vector<SymbolP> locals;
    bool hasCalls = false;

    FunctionDecl(int i, std::string n, CType r, std::vector<VarDeclP> ps, StmtP b, SourceRange rg, SourceRange nr, int l)
        : id(i), name(std::move(n)), returnType(std::move(r)), params(std::move(ps)), body(std::move(b)), range(rg), nameRange(nr), line(l) {}
};
using FunctionDeclP = std::shared_ptr<FunctionDecl>;

struct TopLevel {
    enum class Kind { Function, Globals, StructDef };
    Kind kind;
    FunctionDeclP function;
    std::vector<VarDeclP> globals;
    StructDefP structDef;
    SourceRange range;
    int line = 0;
};

struct Program {
    std::vector<TopLevel> items;
    std::vector<StructDefP> structs;
    int nodeCount = 0;

    /// Functions with a body.
    std::vector<FunctionDeclP> functions() const;
    std::vector<VarDeclP> globals() const;
};
using ProgramP = std::shared_ptr<Program>;

// MARK: - Library

/// The built-in C library. Its functions are known to the type checker
/// without any #include, and the linker pulls in their code on demand.
namespace Library {
struct Function {
    std::string name;
    CType returnType;
    std::vector<CType> params;
    std::string summary;
};
const std::vector<Function>& functions();
const Function* byName(const std::string& name);
}  // namespace Library

/// A printf format string, split into literal text and conversions.
/// The compiler expands printf at compile time into calls to
/// print_str / print_int / print_char / print_hex / print_uint.
struct PrintfPiece {
    bool isConversion = false;
    std::vector<uint8_t> text;
    char conversion = 0;
};

namespace PrintfFormat {
/// Returns the pieces, or sets `error`.
std::optional<std::vector<PrintfPiece>> parse(const std::vector<uint8_t>& bytes, std::string* error = nullptr);
}

// MARK: - Parser and semantic analysis

/// Recursive-descent parser: tokens in, abstract syntax tree out.
/// Returns the program (nullptr on a syntax error) and the diagnostics.
std::pair<ProgramP, std::vector<Diagnostic>> parse(const LexResult& lex);

/// Evaluates integer constant expressions used for array sizes and case labels.
std::optional<int64_t> constantValue(const Expr& e);

/// One lexical scope, kept for the symbol-table view.
struct ScopeInfo {
    std::optional<std::string> function;
    int depth = 0;
    int line = 0;
    std::vector<SymbolP> symbols;
};

struct StringLiteral {
    std::string label;
    std::vector<uint8_t> bytes;
};

struct SemaResult {
    std::vector<SymbolP> symbols;
    std::vector<ScopeInfo> scopes;
    std::vector<StringLiteral> strings;
    /// Library functions the program calls (directly or through printf).
    std::set<std::string> libraryUsed;
    std::vector<Diagnostic> diagnostics;
    bool hasErrors() const;
};
using SemaResultP = std::shared_ptr<SemaResult>;

/// Semantic analysis: resolves every name to its declaration, works out the
/// type of every expression, inserts the implicit C conversions (array → pointer,
/// char → int, int → unsigned) as type annotations, and rejects programs that
/// are syntactically fine but meaningless.
SemaResultP analyze(Program& prog, bool library = false);

}  // namespace sp
