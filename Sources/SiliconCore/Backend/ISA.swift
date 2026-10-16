import Foundation

/// The six RISC-V instruction formats. They differ only in where the
/// immediate bits go; rd, rs1, rs2 and the opcode are always in the same place.
public enum InstFormat: String, Sendable {
    case R, I, S, B, U, J
}

/// Every RV32I base instruction plus the M (multiply/divide) extension.
public enum Op: String, CaseIterable, Sendable {
    case lui, auipc, jal, jalr
    case beq, bne, blt, bge, bltu, bgeu
    case lb, lh, lw, lbu, lhu, sb, sh, sw
    case addi, slti, sltiu, xori, ori, andi, slli, srli, srai
    case add, sub, sll, slt, sltu, xor, srl, sra, or, and
    case fence, ecall, ebreak
    case mul, mulh, mulhsu, mulhu, div, divu, rem, remu

    public struct Spec: Sendable {
        public var format: InstFormat
        public var opcode: UInt32
        public var funct3: UInt32
        public var funct7: UInt32
    }

    public var spec: Spec {
        switch self {
        case .lui: return .init(format: .U, opcode: 0b0110111, funct3: 0, funct7: 0)
        case .auipc: return .init(format: .U, opcode: 0b0010111, funct3: 0, funct7: 0)
        case .jal: return .init(format: .J, opcode: 0b1101111, funct3: 0, funct7: 0)
        case .jalr: return .init(format: .I, opcode: 0b1100111, funct3: 0, funct7: 0)
        case .beq: return .init(format: .B, opcode: 0b1100011, funct3: 0, funct7: 0)
        case .bne: return .init(format: .B, opcode: 0b1100011, funct3: 1, funct7: 0)
        case .blt: return .init(format: .B, opcode: 0b1100011, funct3: 4, funct7: 0)
        case .bge: return .init(format: .B, opcode: 0b1100011, funct3: 5, funct7: 0)
        case .bltu: return .init(format: .B, opcode: 0b1100011, funct3: 6, funct7: 0)
        case .bgeu: return .init(format: .B, opcode: 0b1100011, funct3: 7, funct7: 0)
        case .lb: return .init(format: .I, opcode: 0b0000011, funct3: 0, funct7: 0)
        case .lh: return .init(format: .I, opcode: 0b0000011, funct3: 1, funct7: 0)
        case .lw: return .init(format: .I, opcode: 0b0000011, funct3: 2, funct7: 0)
        case .lbu: return .init(format: .I, opcode: 0b0000011, funct3: 4, funct7: 0)
        case .lhu: return .init(format: .I, opcode: 0b0000011, funct3: 5, funct7: 0)
        case .sb: return .init(format: .S, opcode: 0b0100011, funct3: 0, funct7: 0)
        case .sh: return .init(format: .S, opcode: 0b0100011, funct3: 1, funct7: 0)
        case .sw: return .init(format: .S, opcode: 0b0100011, funct3: 2, funct7: 0)
        case .addi: return .init(format: .I, opcode: 0b0010011, funct3: 0, funct7: 0)
        case .slti: return .init(format: .I, opcode: 0b0010011, funct3: 2, funct7: 0)
        case .sltiu: return .init(format: .I, opcode: 0b0010011, funct3: 3, funct7: 0)
        case .xori: return .init(format: .I, opcode: 0b0010011, funct3: 4, funct7: 0)
        case .ori: return .init(format: .I, opcode: 0b0010011, funct3: 6, funct7: 0)
        case .andi: return .init(format: .I, opcode: 0b0010011, funct3: 7, funct7: 0)
        case .slli: return .init(format: .I, opcode: 0b0010011, funct3: 1, funct7: 0)
        case .srli: return .init(format: .I, opcode: 0b0010011, funct3: 5, funct7: 0)
        case .srai: return .init(format: .I, opcode: 0b0010011, funct3: 5, funct7: 0b0100000)
        case .add: return .init(format: .R, opcode: 0b0110011, funct3: 0, funct7: 0)
        case .sub: return .init(format: .R, opcode: 0b0110011, funct3: 0, funct7: 0b0100000)
        case .sll: return .init(format: .R, opcode: 0b0110011, funct3: 1, funct7: 0)
        case .slt: return .init(format: .R, opcode: 0b0110011, funct3: 2, funct7: 0)
        case .sltu: return .init(format: .R, opcode: 0b0110011, funct3: 3, funct7: 0)
        case .xor: return .init(format: .R, opcode: 0b0110011, funct3: 4, funct7: 0)
        case .srl: return .init(format: .R, opcode: 0b0110011, funct3: 5, funct7: 0)
        case .sra: return .init(format: .R, opcode: 0b0110011, funct3: 5, funct7: 0b0100000)
        case .or: return .init(format: .R, opcode: 0b0110011, funct3: 6, funct7: 0)
        case .and: return .init(format: .R, opcode: 0b0110011, funct3: 7, funct7: 0)
        case .fence: return .init(format: .I, opcode: 0b0001111, funct3: 0, funct7: 0)
        case .ecall: return .init(format: .I, opcode: 0b1110011, funct3: 0, funct7: 0)
        case .ebreak: return .init(format: .I, opcode: 0b1110011, funct3: 0, funct7: 0)
        case .mul: return .init(format: .R, opcode: 0b0110011, funct3: 0, funct7: 1)
        case .mulh: return .init(format: .R, opcode: 0b0110011, funct3: 1, funct7: 1)
        case .mulhsu: return .init(format: .R, opcode: 0b0110011, funct3: 2, funct7: 1)
        case .mulhu: return .init(format: .R, opcode: 0b0110011, funct3: 3, funct7: 1)
        case .div: return .init(format: .R, opcode: 0b0110011, funct3: 4, funct7: 1)
        case .divu: return .init(format: .R, opcode: 0b0110011, funct3: 5, funct7: 1)
        case .rem: return .init(format: .R, opcode: 0b0110011, funct3: 6, funct7: 1)
        case .remu: return .init(format: .R, opcode: 0b0110011, funct3: 7, funct7: 1)
        }
    }

    public var isLoad: Bool { [.lb, .lh, .lw, .lbu, .lhu].contains(self) }
    public var isStore: Bool { [.sb, .sh, .sw].contains(self) }
    public var isBranch: Bool { spec.format == .B }
    public var isJump: Bool { self == .jal || self == .jalr }
    public var isShiftImm: Bool { self == .slli || self == .srli || self == .srai }
    public var isMulDiv: Bool { spec.opcode == 0b0110011 && spec.funct7 == 1 }
    public var accessSize: Int {
        switch self {
        case .lb, .lbu, .sb: return 1
        case .lh, .lhu, .sh: return 2
        default: return 4
        }
    }

    /// Does this instruction write rd?
    public var writesRd: Bool {
        switch spec.format {
        case .R, .U, .J: return true
        case .I: return self != .fence && self != .ecall && self != .ebreak
        case .S, .B: return false
        }
    }
    public var readsRs1: Bool {
        switch spec.format {
        case .R, .S, .B: return true
        case .I: return self != .fence && self != .ecall && self != .ebreak
        case .U, .J: return false
        }
    }
    public var readsRs2: Bool { [.R, .S, .B].contains(spec.format) }

    public var summary: String {
        switch self {
        case .lui: return "rd = imm << 12"
        case .auipc: return "rd = pc + (imm << 12)"
        case .jal: return "rd = pc + 4; pc += imm"
        case .jalr: return "rd = pc + 4; pc = (rs1 + imm) & ~1"
        case .beq: return "if (rs1 == rs2) pc += imm"
        case .bne: return "if (rs1 != rs2) pc += imm"
        case .blt: return "if (rs1 < rs2) pc += imm (signed)"
        case .bge: return "if (rs1 >= rs2) pc += imm (signed)"
        case .bltu: return "if (rs1 < rs2) pc += imm (unsigned)"
        case .bgeu: return "if (rs1 >= rs2) pc += imm (unsigned)"
        case .lb: return "rd = sign-extended byte M[rs1 + imm]"
        case .lh: return "rd = sign-extended half M[rs1 + imm]"
        case .lw: return "rd = M[rs1 + imm] (word)"
        case .lbu: return "rd = zero-extended byte M[rs1 + imm]"
        case .lhu: return "rd = zero-extended half M[rs1 + imm]"
        case .sb: return "M[rs1 + imm] = rs2 (low byte)"
        case .sh: return "M[rs1 + imm] = rs2 (low half)"
        case .sw: return "M[rs1 + imm] = rs2 (word)"
        case .addi: return "rd = rs1 + imm"
        case .slti: return "rd = (rs1 < imm) ? 1 : 0"
        case .sltiu: return "rd = (rs1 < imm) ? 1 : 0 (unsigned)"
        case .xori: return "rd = rs1 ^ imm"
        case .ori: return "rd = rs1 | imm"
        case .andi: return "rd = rs1 & imm"
        case .slli: return "rd = rs1 << shamt"
        case .srli: return "rd = rs1 >> shamt (logical)"
        case .srai: return "rd = rs1 >> shamt (arithmetic)"
        case .add: return "rd = rs1 + rs2"
        case .sub: return "rd = rs1 - rs2"
        case .sll: return "rd = rs1 << rs2[4:0]"
        case .slt: return "rd = (rs1 < rs2) ? 1 : 0"
        case .sltu: return "rd = (rs1 < rs2) ? 1 : 0 (unsigned)"
        case .xor: return "rd = rs1 ^ rs2"
        case .srl: return "rd = rs1 >> rs2[4:0] (logical)"
        case .sra: return "rd = rs1 >> rs2[4:0] (arithmetic)"
        case .or: return "rd = rs1 | rs2"
        case .and: return "rd = rs1 & rs2"
        case .fence: return "memory ordering (no-op here)"
        case .ecall: return "system call: service number in a7"
        case .ebreak: return "breakpoint (stops the program)"
        case .mul: return "rd = (rs1 * rs2)[31:0]"
        case .mulh: return "rd = (rs1 * rs2)[63:32] signed"
        case .mulhsu: return "rd = (rs1 * rs2)[63:32] signed × unsigned"
        case .mulhu: return "rd = (rs1 * rs2)[63:32] unsigned"
        case .div: return "rd = rs1 / rs2 (signed)"
        case .divu: return "rd = rs1 / rs2 (unsigned)"
        case .rem: return "rd = rs1 % rs2 (signed)"
        case .remu: return "rd = rs1 % rs2 (unsigned)"
        }
    }
}

/// A decoded instruction word.
public struct Decoded: Sendable, Equatable {
    public var op: Op
    public var rd: Int
    public var rs1: Int
    public var rs2: Int
    public var imm: Int32
    public var raw: UInt32

    public init(op: Op, rd: Int = 0, rs1: Int = 0, rs2: Int = 0, imm: Int32 = 0, raw: UInt32 = 0) {
        self.op = op
        self.rd = rd
        self.rs1 = rs1
        self.rs2 = rs2
        self.imm = imm
        self.raw = raw
    }

    /// Registers actually read (x0 excluded).
    public var sources: [Int] {
        var s: [Int] = []
        if op.readsRs1 && rs1 != 0 { s.append(rs1) }
        if op.readsRs2 && rs2 != 0 { s.append(rs2) }
        if op == .ecall { s += [Reg.a0, Reg.a7] }
        return s
    }
    public var dest: Int? {
        if op == .ecall { return Reg.a0 }
        return op.writesRd && rd != 0 ? rd : nil
    }
}

public enum Encoder {
    private static func bits(_ v: Int32, _ hi: Int, _ lo: Int) -> UInt32 {
        (UInt32(bitPattern: v) >> UInt32(lo)) & ((1 << UInt32(hi - lo + 1)) - 1)
    }

    public static func encode(_ d: Decoded) -> UInt32 {
        let s = d.op.spec
        let rd = UInt32(d.rd & 31), rs1 = UInt32(d.rs1 & 31), rs2 = UInt32(d.rs2 & 31)
        switch d.op {
        case .ecall: return 0x0000_0073
        case .ebreak: return 0x0010_0073
        case .fence: return 0x0FF0_000F
        default: break
        }
        switch s.format {
        case .R:
            return s.funct7 << 25 | rs2 << 20 | rs1 << 15 | s.funct3 << 12 | rd << 7 | s.opcode
        case .I:
            var imm = UInt32(bitPattern: d.imm) & 0xFFF
            if d.op.isShiftImm { imm = (UInt32(bitPattern: d.imm) & 31) | s.funct7 << 5 }
            return imm << 20 | rs1 << 15 | s.funct3 << 12 | rd << 7 | s.opcode
        case .S:
            return bits(d.imm, 11, 5) << 25 | rs2 << 20 | rs1 << 15 | s.funct3 << 12 | bits(d.imm, 4, 0) << 7 | s.opcode
        case .B:
            return bits(d.imm, 12, 12) << 31 | bits(d.imm, 10, 5) << 25 | rs2 << 20 | rs1 << 15 | s.funct3 << 12
                | bits(d.imm, 4, 1) << 8 | bits(d.imm, 11, 11) << 7 | s.opcode
        case .U:
            return (UInt32(bitPattern: d.imm) & 0xFFFFF) << 12 | rd << 7 | s.opcode
        case .J:
            return bits(d.imm, 20, 20) << 31 | bits(d.imm, 10, 1) << 21 | bits(d.imm, 11, 11) << 20
                | bits(d.imm, 19, 12) << 12 | rd << 7 | s.opcode
        }
    }

    /// Replaces just the immediate of an already-encoded instruction (used by the linker).
    public static func patch(_ word: UInt32, imm: Int32) -> UInt32? {
        guard var d = Decoder.decode(word) else { return nil }
        d.imm = imm
        return encode(d)
    }
}

public enum Decoder {
    private static func sx(_ v: UInt32, _ bitsN: Int) -> Int32 {
        let shift = UInt32(32 - bitsN)
        return Int32(bitPattern: v << shift) >> Int32(shift)
    }

    public static func decode(_ w: UInt32) -> Decoded? {
        let opcode = w & 0x7F
        let rd = Int((w >> 7) & 31), rs1 = Int((w >> 15) & 31), rs2 = Int((w >> 20) & 31)
        let f3 = (w >> 12) & 7, f7 = w >> 25
        let immI = sx(w >> 20, 12)
        func d(_ op: Op, _ imm: Int32 = 0) -> Decoded { Decoded(op: op, rd: rd, rs1: rs1, rs2: rs2, imm: imm, raw: w) }
        switch opcode {
        case 0b0110111: return d(.lui, Int32(bitPattern: w >> 12))
        case 0b0010111: return d(.auipc, Int32(bitPattern: w >> 12))
        case 0b1101111:
            let imm = (w >> 31) << 20 | ((w >> 12) & 0xFF) << 12 | ((w >> 20) & 1) << 11 | ((w >> 21) & 0x3FF) << 1
            return d(.jal, sx(imm, 21))
        case 0b1100111:
            return f3 == 0 ? d(.jalr, immI) : nil
        case 0b1100011:
            let imm = (w >> 31) << 12 | ((w >> 7) & 1) << 11 | ((w >> 25) & 0x3F) << 5 | ((w >> 8) & 0xF) << 1
            let ops: [UInt32: Op] = [0: .beq, 1: .bne, 4: .blt, 5: .bge, 6: .bltu, 7: .bgeu]
            guard let op = ops[f3] else { return nil }
            return d(op, sx(imm, 13))
        case 0b0000011:
            let ops: [UInt32: Op] = [0: .lb, 1: .lh, 2: .lw, 4: .lbu, 5: .lhu]
            guard let op = ops[f3] else { return nil }
            return d(op, immI)
        case 0b0100011:
            let ops: [UInt32: Op] = [0: .sb, 1: .sh, 2: .sw]
            guard let op = ops[f3] else { return nil }
            return d(op, sx((w >> 25) << 5 | (w >> 7) & 31, 12))
        case 0b0010011:
            switch f3 {
            case 0: return d(.addi, immI)
            case 2: return d(.slti, immI)
            case 3: return d(.sltiu, immI)
            case 4: return d(.xori, immI)
            case 6: return d(.ori, immI)
            case 7: return d(.andi, immI)
            case 1: return f7 == 0 ? d(.slli, Int32(rs2)) : nil
            case 5:
                if f7 == 0 { return d(.srli, Int32(rs2)) }
                if f7 == 0b0100000 { return d(.srai, Int32(rs2)) }
                return nil
            default: return nil
            }
        case 0b0110011:
            switch (f7, f3) {
            case (0, 0): return d(.add)
            case (0b0100000, 0): return d(.sub)
            case (0, 1): return d(.sll)
            case (0, 2): return d(.slt)
            case (0, 3): return d(.sltu)
            case (0, 4): return d(.xor)
            case (0, 5): return d(.srl)
            case (0b0100000, 5): return d(.sra)
            case (0, 6): return d(.or)
            case (0, 7): return d(.and)
            case (1, 0): return d(.mul)
            case (1, 1): return d(.mulh)
            case (1, 2): return d(.mulhsu)
            case (1, 3): return d(.mulhu)
            case (1, 4): return d(.div)
            case (1, 5): return d(.divu)
            case (1, 6): return d(.rem)
            case (1, 7): return d(.remu)
            default: return nil
            }
        case 0b0001111: return d(.fence)
        case 0b1110011:
            if w == 0x0000_0073 { return Decoded(op: .ecall, raw: w) }
            if w == 0x0010_0073 { return Decoded(op: .ebreak, raw: w) }
            return nil
        default: return nil
        }
    }

    /// Assembly text for a decoded instruction. With `pc`, branch targets are shown as addresses.
    public static func disassemble(_ d: Decoded, pc: UInt32? = nil, symbol: ((UInt32) -> String?)? = nil) -> String {
        let n = d.op.rawValue
        func r(_ x: Int) -> String { Reg.name(x) }
        func target() -> String {
            guard let pc else { return String(d.imm) }
            let t = pc &+ UInt32(bitPattern: d.imm)
            return symbol?(t) ?? Format.hex(t)
        }
        switch d.op.spec.format {
        case .R: return "\(n) \(r(d.rd)), \(r(d.rs1)), \(r(d.rs2))"
        case .I:
            switch d.op {
            case .ecall, .ebreak, .fence: return n
            case .lb, .lh, .lw, .lbu, .lhu: return "\(n) \(r(d.rd)), \(d.imm)(\(r(d.rs1)))"
            case .jalr: return "jalr \(r(d.rd)), \(d.imm)(\(r(d.rs1)))"
            default: return "\(n) \(r(d.rd)), \(r(d.rs1)), \(d.imm)"
            }
        case .S: return "\(n) \(r(d.rs2)), \(d.imm)(\(r(d.rs1)))"
        case .B: return "\(n) \(r(d.rs1)), \(r(d.rs2)), \(target())"
        case .U: return "\(n) \(r(d.rd)), \(Format.hex(UInt32(bitPattern: d.imm) & 0xFFFFF, digits: 5))"
        case .J: return "jal \(r(d.rd)), \(target())"
        }
    }

    /// Bit fields of an instruction word for the machine-code view: (name, high bit, low bit, value).
    public static func fields(_ d: Decoded) -> [(String, Int, Int, UInt32)] {
        let w = d.raw
        func f(_ name: String, _ hi: Int, _ lo: Int) -> (String, Int, Int, UInt32) {
            (name, hi, lo, (w >> UInt32(lo)) & ((1 << UInt32(hi - lo + 1)) - 1))
        }
        switch d.op.spec.format {
        case .R: return [f("funct7", 31, 25), f("rs2", 24, 20), f("rs1", 19, 15), f("funct3", 14, 12), f("rd", 11, 7), f("opcode", 6, 0)]
        case .I:
            if d.op.isShiftImm {
                return [f("funct7", 31, 25), f("shamt", 24, 20), f("rs1", 19, 15), f("funct3", 14, 12), f("rd", 11, 7), f("opcode", 6, 0)]
            }
            return [f("imm[11:0]", 31, 20), f("rs1", 19, 15), f("funct3", 14, 12), f("rd", 11, 7), f("opcode", 6, 0)]
        case .S: return [f("imm[11:5]", 31, 25), f("rs2", 24, 20), f("rs1", 19, 15), f("funct3", 14, 12), f("imm[4:0]", 11, 7), f("opcode", 6, 0)]
        case .B: return [f("imm[12|10:5]", 31, 25), f("rs2", 24, 20), f("rs1", 19, 15), f("funct3", 14, 12), f("imm[4:1|11]", 11, 7), f("opcode", 6, 0)]
        case .U: return [f("imm[31:12]", 31, 12), f("rd", 11, 7), f("opcode", 6, 0)]
        case .J: return [f("imm[20|10:1|11|19:12]", 31, 12), f("rd", 11, 7), f("opcode", 6, 0)]
        }
    }
}
