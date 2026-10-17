// The intermediate representation: three-address code in basic blocks.
//
// Each instruction does one small thing (`%t3 = %i * 4`). Values live in an
// unlimited supply of virtual registers (%t1, %x …); the register allocator
// later maps them onto the 32 real RISC-V registers. Instructions are grouped
// into basic blocks: straight-line code that is entered only at the top and
// left only at the bottom, by a jump, a conditional branch or a return.
#pragma once

#include "Frontend.h"

namespace sp {

/// An instruction operand: a virtual register or a constant.
struct Operand {
    bool isImm = false;
    int32_t value = 0;   // vreg number or constant

    static Operand v(int r) { return Operand{false, r}; }
    static Operand imm(int32_t c) { return Operand{true, c}; }
    bool isV() const { return !isImm; }
    /// The virtual register, or 0 (vreg 0 is never a real register).
    int vreg() const { return isImm ? 0 : value; }
    std::optional<int32_t> constant() const { return isImm ? std::optional<int32_t>(value) : std::nullopt; }
    /// "v(3)" or "imm(5)", as the original's string description (used to order commutative operands).
    std::string desc() const;
    auto operator<=>(const Operand&) const = default;
};

/// A memory address: a stack slot, a global symbol or a register, plus a byte offset.
struct Addr {
    enum Kind : uint8_t { Slot, Global, Reg };
    Kind kind = Slot;
    int slot = 0;
    std::string global;
    Operand reg;
    int32_t off = 0;

    static Addr ofSlot(int s, int32_t o) { Addr a; a.kind = Slot; a.slot = s; a.off = o; return a; }
    static Addr ofGlobal(std::string g, int32_t o) { Addr a; a.kind = Global; a.global = std::move(g); a.off = o; return a; }
    static Addr ofReg(Operand r, int32_t o) { Addr a; a.kind = Reg; a.reg = r; a.off = o; return a; }
    Addr adding(int32_t k) const { Addr a = *this; a.off = RV::add(a.off, k); return a; }
    int baseVReg() const { return kind == Reg ? reg.vreg() : 0; }
    auto operator<=>(const Addr&) const = default;
};

enum class IROp : uint8_t { Add, Sub, Mul, Div, Divu, Rem, Remu, And, Or, Xor, Shl, Shr, Sar, Eq, Ne, Lt, Le, Gt, Ge, Ltu, Leu, Gtu, Geu };
const char* irOpName(IROp op);
const char* irOpSymbol(IROp op);
bool irIsComparison(IROp op);
bool irIsCommutative(IROp op);
/// a op b == b swapped(op) a
IROp irSwapped(IROp op);
/// !(a op b) == a negated(op) b
IROp irNegated(IROp op);
/// Evaluates the operation on constants, with RISC-V semantics.
int32_t irFold(IROp op, int32_t a, int32_t b);

enum class UnOp : uint8_t { Neg, Not, Sext8 };
int32_t unFold(UnOp op, int32_t a);

struct InstKind {
    enum Kind : uint8_t { Bin, Un, Mov, Load, Store, AddrOf, Call };
    Kind kind = Mov;
    IROp op = IROp::Add;
    UnOp uop = UnOp::Neg;
    /// Destination vreg (0 = none, for a call without a result).
    int d = 0;
    Operand a, b;
    Addr addr;
    /// Load/store size: 1 (char, sign-extended) or 4.
    int size = 0;
    std::string callee;
    std::vector<Operand> args;

    static InstKind bin(IROp op, int d, Operand a, Operand b) { InstKind k; k.kind = Bin; k.op = op; k.d = d; k.a = a; k.b = b; return k; }
    static InstKind un(UnOp op, int d, Operand a) { InstKind k; k.kind = Un; k.uop = op; k.d = d; k.a = a; return k; }
    static InstKind mov(int d, Operand a) { InstKind k; k.kind = Mov; k.d = d; k.a = a; return k; }
    static InstKind load(int d, Addr a, int size) { InstKind k; k.kind = Load; k.d = d; k.addr = std::move(a); k.size = size; return k; }
    static InstKind store(Operand v, Addr a, int size) { InstKind k; k.kind = Store; k.a = v; k.addr = std::move(a); k.size = size; return k; }
    static InstKind addrOf(int d, Addr a) { InstKind k; k.kind = AddrOf; k.d = d; k.addr = std::move(a); return k; }
    static InstKind call(int d, std::string name, std::vector<Operand> args) {
        InstKind k; k.kind = Call; k.d = d; k.callee = std::move(name); k.args = std::move(args); return k;
    }
    auto operator<=>(const InstKind&) const = default;
};

struct Inst {
    InstKind kind;
    /// Stable identity, kept when a pass rewrites the instruction, so views can link IR ↔ assembly.
    int id = 0;
    /// The syntax-tree node it came from, and its source line.
    int origin = 0;
    int line = 0;

    /// The register written (0 = none).
    int def() const;
    std::vector<int> uses() const;
    bool usesReg(int r) const;
    /// Instructions with no effect other than writing their destination.
    bool isPure() const;
    /// Rewrites every operand that reads a virtual register.
    void mapUses(const std::function<Operand(Operand)>& f);
};

struct Terminator {
    enum Kind : uint8_t { Jump, Branch, Ret };
    Kind kind = Ret;
    IROp op = IROp::Eq;
    Operand a, b;
    int t = 0, f = 0;
    bool hasValue = false;   // Ret
    Operand value;           // Ret

    static Terminator jump(int t) { Terminator x; x.kind = Jump; x.t = t; return x; }
    static Terminator branch(IROp op, Operand a, Operand b, int t, int f) {
        Terminator x; x.kind = Branch; x.op = op; x.a = a; x.b = b; x.t = t; x.f = f; return x;
    }
    static Terminator ret() { return Terminator(); }
    static Terminator ret(Operand v) { Terminator x; x.hasValue = true; x.value = v; return x; }

    std::vector<int> successors() const;
    std::vector<int> uses() const;
    void mapUses(const std::function<Operand(Operand)>& fn);
    void mapTargets(const std::function<int(int)>& fn);
    bool operator==(const Terminator&) const = default;
};

struct Block {
    int id;
    std::string name;
    std::vector<Inst> insts;
    Terminator term;
    int termID = 0;
    int termOrigin = 0;
    int termLine = 0;
    Block(int i, std::string n) : id(i), name(std::move(n)) {}
};
using BlockP = std::shared_ptr<Block>;

/// A stack slot: memory for a local variable, an array, or a struct.
struct Slot {
    std::string name;
    int size = 0;
    int align = 1;
    std::optional<int> symbolID;
    /// Scalars may be promoted to registers by mem2reg; arrays and structs may not.
    bool isScalar = false;
};

struct VRegInfo {
    std::string name;
    /// Named after a C variable (vs. a compiler temporary).
    bool isVariable = false;
};

/// Hands out unique instruction ids for a whole compilation.
class IDGen {
public:
    explicit IDGen(int start = 0) : n(start) {}
    int next() { return ++n; }
private:
    int n;
};
using IDGenP = std::shared_ptr<IDGen>;

struct IRFunction {
    std::string name;
    std::vector<int> params;
    std::vector<BlockP> blocks;
    std::vector<Slot> slots;
    std::vector<VRegInfo> vregs{{"%invalid", false}};
    bool returnsValue = true;
    int line = 0;
    int nextBlock = 0;
    IDGenP ids;

    IRFunction(std::string n, IDGenP g) : name(std::move(n)), ids(std::move(g)) {}
    int newVReg(const std::string& name = "", bool variable = false);
    Block* block(int id) const;
    std::shared_ptr<IRFunction> copy() const;
    int instructionCount() const;
    /// Predecessors of every block (every block has an entry, possibly empty).
    std::map<int, std::vector<int>> predecessors() const;
};
using IRFunctionP = std::shared_ptr<IRFunction>;

/// Initial contents of a global in the data section.
struct DataItem {
    enum Kind : uint8_t { Word, Byte, Zero, Address };
    Kind kind = Word;
    int32_t value = 0;      // Word value, Byte value (0…255), Zero count, Address offset
    std::string label;      // Address
    static DataItem word(int32_t v) { return {Word, v, ""}; }
    static DataItem byte(uint8_t b) { return {Byte, b, ""}; }
    static DataItem zero(int n) { return {Zero, n, ""}; }
    static DataItem address(std::string l, int32_t o) { return {Address, o, std::move(l)}; }
};

struct GlobalData {
    std::string label;
    int align = 1;
    std::vector<DataItem> items;
    int size = 0;
    int line = 0;
    std::string comment;
};

struct IRModule {
    std::vector<IRFunctionP> functions;
    std::vector<GlobalData> data;
    std::shared_ptr<IRModule> copy() const;
    int instructionCount() const;
};
using IRModuleP = std::shared_ptr<IRModule>;

namespace IRPrinter {
std::string operand(const Operand& o, const IRFunction& f);
std::string addr(const Addr& a, const IRFunction& f);
std::string inst(const Inst& i, const IRFunction& f);
std::string term(const Terminator& t, const IRFunction& f);
std::string function(const IRFunction& f);
std::string module(const IRModule& m);
}  // namespace IRPrinter

// MARK: - Lowering

/// Translates the checked syntax tree into IR, the way an unoptimising compiler (-O0) does.
IRModuleP lower(const Program& prog, const SemaResult& sema, const IDGenP& ids);

// MARK: - Optimiser

/// The optimiser passes, in the order they run.
enum class PassID : uint8_t { Mem2Reg, ConstFold, Simplify, CopyProp, CSE, DCE, CFG };
const std::vector<PassID>& allPasses();
const char* passRawValue(PassID p);
std::optional<PassID> passFromRaw(const std::string& s);
const char* passTitle(PassID p);
const char* passShortTitle(PassID p);
const char* passExplanation(PassID p);

struct PassSnapshot {
    /// nullopt for the unoptimised IR and for the final cleanup round.
    std::optional<PassID> pass;
    std::string title;
    IRModuleP module;
    bool changed = false;
    int instructions = 0;
};

namespace Optimizer {
/// Runs the enabled passes and returns the IR after each one.
std::vector<PassSnapshot> run(const IRModule& input, const std::set<PassID>& enabled);
bool apply(PassID p, IRFunction& f);
}  // namespace Optimizer

namespace Liveness {
struct Result {
    std::unordered_map<int, std::set<int>> liveIn;
    std::unordered_map<int, std::set<int>> liveOut;
};
/// Classic backward data-flow: in[b] = use[b] ∪ (out[b] − def[b]), out[b] = ∪ in[successors].
Result analyze(const IRFunction& f);
}  // namespace Liveness

}  // namespace sp
