// The back end: registers, linear-scan register allocation, the RV32IM
// instruction set, code generation, the assembler and the linker.
#pragma once

#include "IR.h"

namespace sp {

// MARK: - Registers

/// The 32 RISC-V integer registers and their ABI roles.
namespace Reg {
extern const char* const names[32];
constexpr int zero = 0, ra = 1, sp = 2, gp = 3, tp = 4;
constexpr int s0 = 8, fp = 8;
constexpr int a0 = 10, a7 = 17;
constexpr int t4 = 29, t5 = 30, t6 = 31;
inline int a(int k) { return 10 + k; }
/// Temporaries the allocator may use (caller-saved: a call may destroy them).
extern const std::vector<int> callerSaved;
/// Argument registers double as temporaries for values that never meet a call.
extern const std::vector<int> argumentTemps;
/// Saved registers the allocator may use (callee-saved: a function must restore them).
extern const std::vector<int> calleeSaved;
/// t4, t5 and t6 are kept free for reloading spilled values, big constants and addresses.
extern const std::vector<int> scratch;
inline std::string name(int r) { return names[r & 31]; }
/// Accepts ABI names (a0, sp, fp…) and numeric names (x10).
std::optional<int> parse(const std::string& s);
const char* role(int r);
}  // namespace Reg

// MARK: - Register allocation

struct Location {
    enum Kind : uint8_t { Register, Spill };
    Kind kind = Spill;
    int n = -1;   // register number or spill slot
    static Location reg(int r) { return {Register, r}; }
    static Location spill(int k) { return {Spill, k}; }
    bool operator==(const Location&) const = default;
};

struct LiveInterval {
    int vreg = 0;
    std::string name;
    int start = 0;
    int end = 0;
    bool crossesCall = false;
    Location location;
    /// Why it ended up where it did, for the allocation view.
    std::string note;
};

struct Allocation {
    std::string function;
    std::map<int, Location> locations;
    std::vector<LiveInterval> intervals;
    int spillSlots = 0;
    std::vector<int> usedCalleeSaved;
    /// Linear position of each block's first instruction and of its terminator.
    std::map<int, int> blockStart;
    std::map<int, int> blockEnd;
    std::vector<int> callPositions;
    int maxPosition = 0;
    std::optional<int> registerLimit;

    const Location* locationOf(int v) const {
        auto it = locations.find(v);
        return it == locations.end() ? nullptr : &it->second;
    }
};

/// Linear-scan register allocation (Poletto & Sarkar, 1999).
Allocation allocateRegisters(const IRFunction& f, std::optional<int> registerLimit = std::nullopt);

// MARK: - Instruction set

/// The six RISC-V instruction formats.
enum class InstFormat : uint8_t { R, I, S, B, U, J };
const char* formatName(InstFormat f);

/// Every RV32I base instruction plus the M (multiply/divide) extension.
enum class Op : uint8_t {
    LUI, AUIPC, JAL, JALR,
    BEQ, BNE, BLT, BGE, BLTU, BGEU,
    LB, LH, LW, LBU, LHU, SB, SH, SW,
    ADDI, SLTI, SLTIU, XORI, ORI, ANDI, SLLI, SRLI, SRAI,
    ADD, SUB, SLL, SLT, SLTU, XOR, SRL, SRA, OR, AND,
    FENCE, ECALL, EBREAK,
    MUL, MULH, MULHSU, MULHU, DIV, DIVU, REM, REMU,
    COUNT
};

struct OpSpec {
    InstFormat format;
    uint32_t opcode;
    uint32_t funct3;
    uint32_t funct7;
};

const OpSpec& opSpec(Op op);
/// "addi", "and"…
const char* opName(Op op);
std::optional<Op> opFromName(const std::string& s);
const char* opSummary(Op op);
inline bool opIsLoad(Op o) { return o == Op::LB || o == Op::LH || o == Op::LW || o == Op::LBU || o == Op::LHU; }
inline bool opIsStore(Op o) { return o == Op::SB || o == Op::SH || o == Op::SW; }
inline bool opIsBranch(Op o) { return opSpec(o).format == InstFormat::B; }
inline bool opIsJump(Op o) { return o == Op::JAL || o == Op::JALR; }
inline bool opIsShiftImm(Op o) { return o == Op::SLLI || o == Op::SRLI || o == Op::SRAI; }
inline bool opIsMulDiv(Op o) { return opSpec(o).opcode == 0b0110011 && opSpec(o).funct7 == 1; }
bool opWritesRd(Op o);
bool opReadsRs1(Op o);
bool opReadsRs2(Op o);

/// A decoded instruction word.
struct Decoded {
    Op op = Op::ADDI;
    int rd = 0, rs1 = 0, rs2 = 0;
    int32_t imm = 0;
    uint32_t raw = 0;

    /// Registers actually read (x0 excluded).
    std::vector<int> sources() const;
    /// Register written (nullopt if none).
    std::optional<int> dest() const {
        if (op == Op::ECALL) return Reg::a0;
        return opWritesRd(op) && rd != 0 ? std::optional<int>(rd) : std::nullopt;
    }
    bool operator==(const Decoded&) const = default;
};

namespace Encoder {
uint32_t encode(const Decoded& d);
/// Replaces just the immediate of an already-encoded instruction (used by the linker).
std::optional<uint32_t> patch(uint32_t word, int32_t imm);
}  // namespace Encoder

struct FieldInfo {
    std::string name;
    int hi = 0, lo = 0;
    uint32_t value = 0;
};

namespace Decoder {
std::optional<Decoded> decode(uint32_t w);
using Symbolizer = std::function<std::optional<std::string>(uint32_t)>;
/// Assembly text for a decoded instruction. With `pc`, branch targets are shown as addresses.
std::string disassemble(const Decoded& d, std::optional<uint32_t> pc = std::nullopt, const Symbolizer& symbol = nullptr);
/// Bit fields of an instruction word for the machine-code view.
std::vector<FieldInfo> fields(const Decoded& d);
}  // namespace Decoder

// MARK: - Code generation

/// Where an assembly line came from.
struct AsmOrigin {
    /// IR instruction id (0 for prologue/epilogue and other glue).
    int inst = 0;
    /// Syntax-tree node id.
    int node = 0;
    /// C source line (0 = none).
    int line = 0;
    bool operator==(const AsmOrigin&) const = default;
};

struct AsmLine {
    enum class Kind { Label, Instruction, Directive, Comment, Blank };
    Kind kind = Kind::Blank;
    std::string text;
    std::optional<AsmOrigin> origin;
    /// The function this line belongs to, if any.
    std::optional<std::string> function;
    /// A short note shown to the right of the instruction.
    std::string note;
};

/// The stack frame of one function. Offsets are from s0 (the frame pointer).
struct FrameLayout {
    struct Entry {
        int offset = 0;   // negative: s0 + offset
        int size = 0;
        std::string what;
        std::string kind; // "ra", "fp", "saved", "spill", "local"
    };
    std::string function;
    int size = 0;
    std::vector<Entry> entries;
    bool hasFrame = true;
    std::vector<int> slotOffset;
    std::vector<int> spillOffset;
    std::map<int, int> savedOffset;
};

/// Turns optimised IR plus a register allocation into RISC-V assembly text.
class CodeGen {
public:
    explicit CodeGen(const std::string& source);
    std::vector<AsmLine> generate(const IRModule& m, const std::map<std::string, Allocation>& allocations);
    const std::vector<FrameLayout>& frames() const { return frames_; }
    static std::string label(const std::string& fn, const Block& block);
    static std::string escape(const std::vector<uint8_t>& bytes);

private:
    struct Dst;
    std::vector<AsmLine> out;
    const IRFunction* f = nullptr;
    const Allocation* alloc = nullptr;
    FrameLayout frame;
    std::optional<AsmOrigin> origin;
    std::vector<std::string> sourceLines;
    int lastLine = -1;
    std::vector<FrameLayout> frames_;

    void emit(const std::string& text, const std::string& note = "");
    void directive(const std::string& text);
    void comment(const std::string& text);
    void data(const GlobalData& g);
    void layoutFrame();
    void li(int rd, int32_t v);
    int src(const Operand& o, int scratch);
    Dst dst(int v);
    void memOp(const std::string& op, int rd, int base, int offset, const std::string& note = "");
    std::pair<int, int> address(const Addr& a);
    void function(const IRFunction& fn, const Allocation& a);
    bool predecessorsOfEntry() const;
    void sourceComment(int line);
    void prologue();
    void epilogue();
    void inst(const Inst& i);
    void addImm(int rd, int rs, int32_t c);
    void binary(IROp op, int d, Operand a, Operand b);
    void terminator(const Block& b, const Block* next);
};

// MARK: - Assembler

enum class SectionKind { Text, Data };
inline const char* sectionName(SectionKind s) { return s == SectionKind::Text ? ".text" : ".data"; }

enum class RelocType { Branch, Jal, PcrelHi20, PcrelLo12I, PcrelLo12S, Hi20, Lo12I, Lo12S, Abs32 };
const char* relocName(RelocType t);
const char* relocExplanation(RelocType t);

struct Relocation {
    SectionKind section = SectionKind::Text;
    int offset = 0;
    RelocType type = RelocType::Abs32;
    std::string symbol;
    int32_t addend = 0;
    /// For a %pcrel_lo: offset of the auipc it pairs with.
    std::optional<int> hiOffset;
    /// Index of the listing entry (text) it patches.
    std::optional<int> listing;
};

struct ObjectSymbol {
    std::string name;
    SectionKind section = SectionKind::Text;
    int offset = 0;
    bool isGlobal = false;
    int line = 0;
};

/// One machine instruction in an object file.
struct ListingEntry {
    int offset = 0;
    uint32_t word = 0;
    /// Assembly text of this one machine instruction.
    std::string text;
    /// The pseudo-instruction it was expanded from ("li t0, 100000"), if any.
    std::optional<std::string> pseudo;
    /// Which part of the expansion, e.g. 1 of 2.
    int part = 1;
    int parts = 1;
    /// Index into the object's assembly lines.
    int asmLine = 0;
    std::optional<AsmOrigin> origin;
    std::optional<std::string> function;
};

struct DataEntry {
    int offset = 0;
    int size = 0;
    std::string text;
    int asmLine = 0;
    std::optional<AsmOrigin> origin;
};

struct ObjectFile {
    std::string name;
    std::vector<uint8_t> text;
    std::vector<uint8_t> data;
    std::map<std::string, ObjectSymbol> symbols;
    std::vector<Relocation> relocations;
    std::vector<ListingEntry> listing;
    std::vector<DataEntry> dataListing;
    std::vector<AsmLine> lines;
    std::vector<Diagnostic> diagnostics;
    /// Symbols this object uses but does not define.
    std::set<std::string> undefined;
    /// A library member: only linked in when something needs one of its symbols.
    bool isLibrary = false;
    std::string summary;

    ObjectFile(std::string n, std::vector<AsmLine> l) : name(std::move(n)), lines(std::move(l)) {}
    bool hasErrors() const;
    std::vector<std::string> globalSymbols() const;
};
using ObjectFileP = std::shared_ptr<ObjectFile>;

/// A two-pass assembler for RV32IM, with the usual pseudo-instructions.
namespace Assembler {
ObjectFileP assemble(const std::vector<AsmLine>& lines, const std::string& name);
/// Assembles hand-written assembly text.
ObjectFileP assembleText(const std::string& text, const std::string& name);
std::optional<std::vector<uint8_t>> parseString(const std::string& s);
}  // namespace Assembler

// MARK: - Linker

struct LinkedInstruction {
    uint32_t address = 0;
    uint32_t word = 0;
    std::optional<Decoded> decoded;
    /// Assembly text with symbols (as written by the compiler or assembler).
    std::string text;
    std::optional<std::string> pseudo;
    int part = 1;
    int parts = 1;
    std::string object;
    int asmLine = 0;
    std::optional<AsmOrigin> origin;
    std::optional<std::string> function;
    /// The word before the linker patched it.
    uint32_t unlinkedWord = 0;
};

struct RelocationRecord {
    std::string object;
    RelocType type;
    std::string symbol;
    uint32_t address = 0;
    uint32_t target = 0;
    uint32_t before = 0;
    uint32_t after = 0;
    int32_t value = 0;
};

struct LinkedObject {
    std::string name;
    uint32_t textStart = 0;
    int textSize = 0;
    uint32_t dataStart = 0;
    int dataSize = 0;
    bool isLibrary = false;
    std::string reason;
    std::string summary;
};

struct LinkedSymbol {
    std::string name;
    uint32_t address = 0;
    SectionKind section = SectionKind::Text;
    std::string object;
    bool isGlobal = false;
};

struct LinkedData {
    uint32_t address = 0;
    int size = 0;
    std::string text;
    std::string object;
    std::optional<AsmOrigin> origin;
};

/// A linked program, ready to load: code at 0x00400000, data at 0x10010000.
class Executable {
public:
    std::vector<uint8_t> text;
    std::vector<uint8_t> data;
    const uint32_t textBase = MemoryMap::textBase;
    const uint32_t dataBase = MemoryMap::dataBase;
    uint32_t entry = MemoryMap::textBase;
    std::vector<LinkedSymbol> symbols;
    std::vector<LinkedInstruction> instructions;
    std::vector<RelocationRecord> relocations;
    std::vector<LinkedObject> objects;
    std::vector<LinkedData> dataEntries;

    uint32_t textEnd() const { return textBase + (uint32_t)text.size(); }
    uint32_t dataEnd() const { return dataBase + (uint32_t)data.size(); }
    const LinkedInstruction* instruction(uint32_t pc) const;
    void finish();
    /// "main+12" for an address inside main.
    std::optional<std::string> symbolize(uint32_t addr) const;
    /// The label at exactly this code address, local labels (.L…) included. For branch targets.
    std::optional<std::string> label(uint32_t addr) const;
    std::optional<std::string> function(uint32_t pc) const;
    std::optional<uint32_t> address(const std::string& name) const;

private:
    std::vector<std::pair<uint32_t, std::string>> functionStarts;
};
using ExecutableP = std::shared_ptr<Executable>;

namespace Linker {
std::pair<ExecutableP, std::vector<Diagnostic>> link(const std::vector<ObjectFileP>& inputs, const std::vector<ObjectFileP>& archive,
                                                     const std::string& entry = "_start");
}

}  // namespace sp
