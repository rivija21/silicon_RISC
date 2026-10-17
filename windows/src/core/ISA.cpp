#include "Backend.h"

namespace sp {

const char* formatName(InstFormat f) {
    switch (f) {
    case InstFormat::R: return "R";
    case InstFormat::I: return "I";
    case InstFormat::S: return "S";
    case InstFormat::B: return "B";
    case InstFormat::U: return "U";
    case InstFormat::J: return "J";
    }
    return "?";
}

namespace {

struct OpInfo {
    const char* name;
    OpSpec spec;
    const char* summary;
};

using F = InstFormat;

const OpInfo opTable[(int)Op::COUNT] = {
    {"lui", {F::U, 0b0110111, 0, 0}, "rd = imm << 12"},
    {"auipc", {F::U, 0b0010111, 0, 0}, "rd = pc + (imm << 12)"},
    {"jal", {F::J, 0b1101111, 0, 0}, "rd = pc + 4; pc += imm"},
    {"jalr", {F::I, 0b1100111, 0, 0}, "rd = pc + 4; pc = (rs1 + imm) & ~1"},
    {"beq", {F::B, 0b1100011, 0, 0}, "if (rs1 == rs2) pc += imm"},
    {"bne", {F::B, 0b1100011, 1, 0}, "if (rs1 != rs2) pc += imm"},
    {"blt", {F::B, 0b1100011, 4, 0}, "if (rs1 < rs2) pc += imm (signed)"},
    {"bge", {F::B, 0b1100011, 5, 0}, "if (rs1 >= rs2) pc += imm (signed)"},
    {"bltu", {F::B, 0b1100011, 6, 0}, "if (rs1 < rs2) pc += imm (unsigned)"},
    {"bgeu", {F::B, 0b1100011, 7, 0}, "if (rs1 >= rs2) pc += imm (unsigned)"},
    {"lb", {F::I, 0b0000011, 0, 0}, "rd = sign-extended byte M[rs1 + imm]"},
    {"lh", {F::I, 0b0000011, 1, 0}, "rd = sign-extended half M[rs1 + imm]"},
    {"lw", {F::I, 0b0000011, 2, 0}, "rd = M[rs1 + imm] (word)"},
    {"lbu", {F::I, 0b0000011, 4, 0}, "rd = zero-extended byte M[rs1 + imm]"},
    {"lhu", {F::I, 0b0000011, 5, 0}, "rd = zero-extended half M[rs1 + imm]"},
    {"sb", {F::S, 0b0100011, 0, 0}, "M[rs1 + imm] = rs2 (low byte)"},
    {"sh", {F::S, 0b0100011, 1, 0}, "M[rs1 + imm] = rs2 (low half)"},
    {"sw", {F::S, 0b0100011, 2, 0}, "M[rs1 + imm] = rs2 (word)"},
    {"addi", {F::I, 0b0010011, 0, 0}, "rd = rs1 + imm"},
    {"slti", {F::I, 0b0010011, 2, 0}, "rd = (rs1 < imm) ? 1 : 0"},
    {"sltiu", {F::I, 0b0010011, 3, 0}, "rd = (rs1 < imm) ? 1 : 0 (unsigned)"},
    {"xori", {F::I, 0b0010011, 4, 0}, "rd = rs1 ^ imm"},
    {"ori", {F::I, 0b0010011, 6, 0}, "rd = rs1 | imm"},
    {"andi", {F::I, 0b0010011, 7, 0}, "rd = rs1 & imm"},
    {"slli", {F::I, 0b0010011, 1, 0}, "rd = rs1 << shamt"},
    {"srli", {F::I, 0b0010011, 5, 0}, "rd = rs1 >> shamt (logical)"},
    {"srai", {F::I, 0b0010011, 5, 0b0100000}, "rd = rs1 >> shamt (arithmetic)"},
    {"add", {F::R, 0b0110011, 0, 0}, "rd = rs1 + rs2"},
    {"sub", {F::R, 0b0110011, 0, 0b0100000}, "rd = rs1 - rs2"},
    {"sll", {F::R, 0b0110011, 1, 0}, "rd = rs1 << rs2[4:0]"},
    {"slt", {F::R, 0b0110011, 2, 0}, "rd = (rs1 < rs2) ? 1 : 0"},
    {"sltu", {F::R, 0b0110011, 3, 0}, "rd = (rs1 < rs2) ? 1 : 0 (unsigned)"},
    {"xor", {F::R, 0b0110011, 4, 0}, "rd = rs1 ^ rs2"},
    {"srl", {F::R, 0b0110011, 5, 0}, "rd = rs1 >> rs2[4:0] (logical)"},
    {"sra", {F::R, 0b0110011, 5, 0b0100000}, "rd = rs1 >> rs2[4:0] (arithmetic)"},
    {"or", {F::R, 0b0110011, 6, 0}, "rd = rs1 | rs2"},
    {"and", {F::R, 0b0110011, 7, 0}, "rd = rs1 & rs2"},
    {"fence", {F::I, 0b0001111, 0, 0}, "memory ordering (no-op here)"},
    {"ecall", {F::I, 0b1110011, 0, 0}, "system call: service number in a7"},
    {"ebreak", {F::I, 0b1110011, 0, 0}, "breakpoint (stops the program)"},
    {"mul", {F::R, 0b0110011, 0, 1}, "rd = (rs1 * rs2)[31:0]"},
    {"mulh", {F::R, 0b0110011, 1, 1}, "rd = (rs1 * rs2)[63:32] signed"},
    {"mulhsu", {F::R, 0b0110011, 2, 1}, "rd = (rs1 * rs2)[63:32] signed × unsigned"},
    {"mulhu", {F::R, 0b0110011, 3, 1}, "rd = (rs1 * rs2)[63:32] unsigned"},
    {"div", {F::R, 0b0110011, 4, 1}, "rd = rs1 / rs2 (signed)"},
    {"divu", {F::R, 0b0110011, 5, 1}, "rd = rs1 / rs2 (unsigned)"},
    {"rem", {F::R, 0b0110011, 6, 1}, "rd = rs1 % rs2 (signed)"},
    {"remu", {F::R, 0b0110011, 7, 1}, "rd = rs1 % rs2 (unsigned)"},
};

}  // namespace

const OpSpec& opSpec(Op op) { return opTable[(int)op].spec; }
const char* opName(Op op) { return opTable[(int)op].name; }
const char* opSummary(Op op) { return opTable[(int)op].summary; }

std::optional<Op> opFromName(const std::string& s) {
    for (int k = 0; k < (int)Op::COUNT; k++)
        if (s == opTable[k].name) return (Op)k;
    return std::nullopt;
}

bool opWritesRd(Op o) {
    switch (opSpec(o).format) {
    case F::R: case F::U: case F::J: return true;
    case F::I: return o != Op::FENCE && o != Op::ECALL && o != Op::EBREAK;
    case F::S: case F::B: return false;
    }
    return false;
}

bool opReadsRs1(Op o) {
    switch (opSpec(o).format) {
    case F::R: case F::S: case F::B: return true;
    case F::I: return o != Op::FENCE && o != Op::ECALL && o != Op::EBREAK;
    case F::U: case F::J: return false;
    }
    return false;
}

bool opReadsRs2(Op o) {
    F f = opSpec(o).format;
    return f == F::R || f == F::S || f == F::B;
}

std::vector<int> Decoded::sources() const {
    std::vector<int> s;
    if (opReadsRs1(op) && rs1 != 0) s.push_back(rs1);
    if (opReadsRs2(op) && rs2 != 0) s.push_back(rs2);
    if (op == Op::ECALL) {
        s.push_back(Reg::a0);
        s.push_back(Reg::a7);
    }
    return s;
}

// MARK: - Encoder

static uint32_t bits(int32_t v, int hi, int lo) { return ((uint32_t)v >> lo) & ((1u << (hi - lo + 1)) - 1); }

uint32_t Encoder::encode(const Decoded& d) {
    const OpSpec& s = opSpec(d.op);
    uint32_t rd = d.rd & 31, rs1 = d.rs1 & 31, rs2 = d.rs2 & 31;
    switch (d.op) {
    case Op::ECALL: return 0x00000073;
    case Op::EBREAK: return 0x00100073;
    case Op::FENCE: return 0x0FF0000F;
    default: break;
    }
    switch (s.format) {
    case F::R: return s.funct7 << 25 | rs2 << 20 | rs1 << 15 | s.funct3 << 12 | rd << 7 | s.opcode;
    case F::I: {
        uint32_t imm = (uint32_t)d.imm & 0xFFF;
        if (opIsShiftImm(d.op)) imm = ((uint32_t)d.imm & 31) | s.funct7 << 5;
        return imm << 20 | rs1 << 15 | s.funct3 << 12 | rd << 7 | s.opcode;
    }
    case F::S: return bits(d.imm, 11, 5) << 25 | rs2 << 20 | rs1 << 15 | s.funct3 << 12 | bits(d.imm, 4, 0) << 7 | s.opcode;
    case F::B:
        return bits(d.imm, 12, 12) << 31 | bits(d.imm, 10, 5) << 25 | rs2 << 20 | rs1 << 15 | s.funct3 << 12 | bits(d.imm, 4, 1) << 8 |
               bits(d.imm, 11, 11) << 7 | s.opcode;
    case F::U: return ((uint32_t)d.imm & 0xFFFFF) << 12 | rd << 7 | s.opcode;
    case F::J:
        return bits(d.imm, 20, 20) << 31 | bits(d.imm, 10, 1) << 21 | bits(d.imm, 11, 11) << 20 | bits(d.imm, 19, 12) << 12 | rd << 7 |
               s.opcode;
    }
    return 0;
}

std::optional<uint32_t> Encoder::patch(uint32_t word, int32_t imm) {
    auto d = Decoder::decode(word);
    if (!d) return std::nullopt;
    d->imm = imm;
    return encode(*d);
}

// MARK: - Decoder

static int32_t sx(uint32_t v, int bitsN) {
    uint32_t shift = 32 - bitsN;
    return (int32_t)(v << shift) >> shift;
}

std::optional<Decoded> Decoder::decode(uint32_t w) {
    uint32_t opcode = w & 0x7F;
    int rd = (w >> 7) & 31, rs1 = (w >> 15) & 31, rs2 = (w >> 20) & 31;
    uint32_t f3 = (w >> 12) & 7, f7 = w >> 25;
    int32_t immI = sx(w >> 20, 12);
    auto d = [&](Op op, int32_t imm = 0) { return Decoded{op, rd, rs1, rs2, imm, w}; };
    switch (opcode) {
    case 0b0110111: return d(Op::LUI, (int32_t)(w >> 12));
    case 0b0010111: return d(Op::AUIPC, (int32_t)(w >> 12));
    case 0b1101111: {
        uint32_t imm = (w >> 31) << 20 | ((w >> 12) & 0xFF) << 12 | ((w >> 20) & 1) << 11 | ((w >> 21) & 0x3FF) << 1;
        return d(Op::JAL, sx(imm, 21));
    }
    case 0b1100111:
        if (f3 == 0) return d(Op::JALR, immI);
        return std::nullopt;
    case 0b1100011: {
        uint32_t imm = (w >> 31) << 12 | ((w >> 7) & 1) << 11 | ((w >> 25) & 0x3F) << 5 | ((w >> 8) & 0xF) << 1;
        Op op;
        switch (f3) {
        case 0: op = Op::BEQ; break;
        case 1: op = Op::BNE; break;
        case 4: op = Op::BLT; break;
        case 5: op = Op::BGE; break;
        case 6: op = Op::BLTU; break;
        case 7: op = Op::BGEU; break;
        default: return std::nullopt;
        }
        return d(op, sx(imm, 13));
    }
    case 0b0000011: {
        Op op;
        switch (f3) {
        case 0: op = Op::LB; break;
        case 1: op = Op::LH; break;
        case 2: op = Op::LW; break;
        case 4: op = Op::LBU; break;
        case 5: op = Op::LHU; break;
        default: return std::nullopt;
        }
        return d(op, immI);
    }
    case 0b0100011: {
        Op op;
        switch (f3) {
        case 0: op = Op::SB; break;
        case 1: op = Op::SH; break;
        case 2: op = Op::SW; break;
        default: return std::nullopt;
        }
        return d(op, sx((w >> 25) << 5 | ((w >> 7) & 31), 12));
    }
    case 0b0010011:
        switch (f3) {
        case 0: return d(Op::ADDI, immI);
        case 2: return d(Op::SLTI, immI);
        case 3: return d(Op::SLTIU, immI);
        case 4: return d(Op::XORI, immI);
        case 6: return d(Op::ORI, immI);
        case 7: return d(Op::ANDI, immI);
        case 1:
            if (f7 == 0) return d(Op::SLLI, rs2);
            return std::nullopt;
        case 5:
            if (f7 == 0) return d(Op::SRLI, rs2);
            if (f7 == 0b0100000) return d(Op::SRAI, rs2);
            return std::nullopt;
        default: return std::nullopt;
        }
    case 0b0110011:
        if (f7 == 0) {
            static const Op ops[8] = {Op::ADD, Op::SLL, Op::SLT, Op::SLTU, Op::XOR, Op::SRL, Op::OR, Op::AND};
            return d(ops[f3]);
        }
        if (f7 == 0b0100000) {
            if (f3 == 0) return d(Op::SUB);
            if (f3 == 5) return d(Op::SRA);
            return std::nullopt;
        }
        if (f7 == 1) {
            static const Op ops[8] = {Op::MUL, Op::MULH, Op::MULHSU, Op::MULHU, Op::DIV, Op::DIVU, Op::REM, Op::REMU};
            return d(ops[f3]);
        }
        return std::nullopt;
    case 0b0001111: return d(Op::FENCE);
    case 0b1110011:
        if (w == 0x00000073) return Decoded{Op::ECALL, 0, 0, 0, 0, w};
        if (w == 0x00100073) return Decoded{Op::EBREAK, 0, 0, 0, 0, w};
        return std::nullopt;
    default: return std::nullopt;
    }
}

std::string Decoder::disassemble(const Decoded& d, std::optional<uint32_t> pc, const Symbolizer& symbol) {
    std::string n = opName(d.op);
    auto r = [](int x) { return Reg::name(x); };
    auto target = [&]() -> std::string {
        if (!pc) return std::to_string(d.imm);
        uint32_t t = *pc + (uint32_t)d.imm;
        if (symbol)
            if (auto s = symbol(t)) return *s;
        return Format::hex(t);
    };
    switch (opSpec(d.op).format) {
    case F::R: return cat(n, " ", r(d.rd), ", ", r(d.rs1), ", ", r(d.rs2));
    case F::I:
        switch (d.op) {
        case Op::ECALL: case Op::EBREAK: case Op::FENCE: return n;
        case Op::LB: case Op::LH: case Op::LW: case Op::LBU: case Op::LHU: return cat(n, " ", r(d.rd), ", ", d.imm, "(", r(d.rs1), ")");
        case Op::JALR: return cat("jalr ", r(d.rd), ", ", d.imm, "(", r(d.rs1), ")");
        default: return cat(n, " ", r(d.rd), ", ", r(d.rs1), ", ", d.imm);
        }
    case F::S: return cat(n, " ", r(d.rs2), ", ", d.imm, "(", r(d.rs1), ")");
    case F::B: return cat(n, " ", r(d.rs1), ", ", r(d.rs2), ", ", target());
    case F::U: return cat(n, " ", r(d.rd), ", ", Format::hex((uint32_t)d.imm & 0xFFFFF, 5));
    case F::J: return cat("jal ", r(d.rd), ", ", target());
    }
    return n;
}

std::vector<FieldInfo> Decoder::fields(const Decoded& d) {
    uint32_t w = d.raw;
    auto f = [&](const char* name, int hi, int lo) { return FieldInfo{name, hi, lo, (w >> lo) & ((1u << (hi - lo + 1)) - 1)}; };
    switch (opSpec(d.op).format) {
    case F::R: return {f("funct7", 31, 25), f("rs2", 24, 20), f("rs1", 19, 15), f("funct3", 14, 12), f("rd", 11, 7), f("opcode", 6, 0)};
    case F::I:
        if (opIsShiftImm(d.op))
            return {f("funct7", 31, 25), f("shamt", 24, 20), f("rs1", 19, 15), f("funct3", 14, 12), f("rd", 11, 7), f("opcode", 6, 0)};
        return {f("imm[11:0]", 31, 20), f("rs1", 19, 15), f("funct3", 14, 12), f("rd", 11, 7), f("opcode", 6, 0)};
    case F::S: return {f("imm[11:5]", 31, 25), f("rs2", 24, 20), f("rs1", 19, 15), f("funct3", 14, 12), f("imm[4:0]", 11, 7), f("opcode", 6, 0)};
    case F::B:
        return {f("imm[12|10:5]", 31, 25), f("rs2", 24, 20), f("rs1", 19, 15), f("funct3", 14, 12), f("imm[4:1|11]", 11, 7), f("opcode", 6, 0)};
    case F::U: return {f("imm[31:12]", 31, 12), f("rd", 11, 7), f("opcode", 6, 0)};
    case F::J: return {f("imm[20|10:1|11|19:12]", 31, 12), f("rd", 11, 7), f("opcode", 6, 0)};
    }
    return {};
}

}  // namespace sp
