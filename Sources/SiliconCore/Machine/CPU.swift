import Foundation

/// The parts of an instruction's behaviour that both CPU models share.
/// The single-cycle simulator calls them back to back; the pipeline calls
/// them in different stages (ALU in EX, memory in MEM, write-back in WB).
public enum Exec {
    /// The ALU result: arithmetic, the address for loads/stores, or the link value pc+4 for jumps.
    @inline(__always)
    public static func alu(_ d: Decoded, _ a: UInt32, _ b: UInt32, pc: UInt32) -> UInt32 {
        let imm = UInt32(bitPattern: d.imm)
        let sa = Int32(bitPattern: a), sb = Int32(bitPattern: b)
        switch d.op {
        case .lui: return imm << 12
        case .auipc: return pc &+ (imm << 12)
        case .jal, .jalr: return pc &+ 4
        case .lb, .lh, .lw, .lbu, .lhu, .sb, .sh, .sw: return a &+ imm
        case .addi: return a &+ imm
        case .slti: return sa < d.imm ? 1 : 0
        case .sltiu: return a < imm ? 1 : 0
        case .xori: return a ^ imm
        case .ori: return a | imm
        case .andi: return a & imm
        case .slli: return a << (imm & 31)
        case .srli: return a >> (imm & 31)
        case .srai: return UInt32(bitPattern: sa >> Int32(imm & 31))
        case .add: return a &+ b
        case .sub: return a &- b
        case .sll: return a << (b & 31)
        case .slt: return sa < sb ? 1 : 0
        case .sltu: return a < b ? 1 : 0
        case .xor: return a ^ b
        case .srl: return a >> (b & 31)
        case .sra: return UInt32(bitPattern: sa >> Int32(b & 31))
        case .or: return a | b
        case .and: return a & b
        case .mul: return a &* b
        case .mulh: return UInt32(truncatingIfNeeded: (Int64(sa) * Int64(sb)) >> 32)
        case .mulhsu: return UInt32(truncatingIfNeeded: (Int64(sa) * Int64(b)) >> 32)
        case .mulhu: return UInt32(truncatingIfNeeded: (UInt64(a) * UInt64(b)) >> 32)
        case .div: return UInt32(bitPattern: RV.div(sa, sb))
        case .divu: return RV.divu(a, b)
        case .rem: return UInt32(bitPattern: RV.rem(sa, sb))
        case .remu: return RV.remu(a, b)
        case .beq, .bne, .blt, .bge, .bltu, .bgeu, .fence, .ecall, .ebreak: return 0
        }
    }

    @inline(__always)
    public static func taken(_ d: Decoded, _ a: UInt32, _ b: UInt32) -> Bool {
        switch d.op {
        case .beq: return a == b
        case .bne: return a != b
        case .blt: return Int32(bitPattern: a) < Int32(bitPattern: b)
        case .bge: return Int32(bitPattern: a) >= Int32(bitPattern: b)
        case .bltu: return a < b
        case .bgeu: return a >= b
        case .jal, .jalr: return true
        default: return false
        }
    }

    /// Where control goes if the branch/jump is taken.
    @inline(__always)
    public static func target(_ d: Decoded, _ a: UInt32, pc: UInt32) -> UInt32 {
        if d.op == .jalr { return (a &+ UInt32(bitPattern: d.imm)) & ~1 }
        return pc &+ UInt32(bitPattern: d.imm)
    }

    /// Performs a load or store. Returns the loaded value for loads.
    @inline(__always)
    public static func memory(_ d: Decoded, _ addr: UInt32, _ storeValue: UInt32, _ mem: Memory) throws -> UInt32 {
        switch d.op {
        case .lb: return UInt32(bitPattern: Int32(Int8(bitPattern: try mem.load8(addr))))
        case .lbu: return UInt32(try mem.load8(addr))
        case .lh: return UInt32(bitPattern: Int32(Int16(bitPattern: try mem.load16(addr))))
        case .lhu: return UInt32(try mem.load16(addr))
        case .lw: return try mem.load32(addr)
        case .sb: try mem.store8(addr, UInt8(truncatingIfNeeded: storeValue)); return 0
        case .sh: try mem.store16(addr, UInt16(truncatingIfNeeded: storeValue)); return 0
        case .sw: try mem.store32(addr, storeValue); return 0
        default: return 0
        }
    }
}

/// What the "operating system" does on ecall. Shared by both CPU models.
public final class SyscallHandler {
    public var output = ProgramOutput()
    public var brk = MemoryMap.heapBase
    public var halted = false
    public var log: [(UInt32, String)] = []

    public init() {}

    /// Returns the new value of a0, or nil if a0 is unchanged.
    public func handle(a0: UInt32, a7: UInt32, mem: Memory, pc: UInt32) throws -> UInt32? {
        guard let call = Syscall(rawValue: a7) else {
            throw MemoryFault(address: pc, message: "ecall with unknown service number a7 = \(a7)")
        }
        switch call {
        case .printInt: output.append(String(Int32(bitPattern: a0)))
        case .printUnsigned: output.append(String(a0))
        case .printHex: output.append(Format.hex(a0))
        case .printChar: output.append([UInt8(truncatingIfNeeded: a0)])
        case .printString: output.append(try mem.cString(a0, limit: 100_000))
        case .sbrk:
            let old = brk
            if brk &+ a0 > MemoryMap.heapLimit || brk &+ a0 < brk { return 0 }
            brk &+= a0
            return old
        case .exit0:
            output.exitCode = 0
            halted = true
        case .exit:
            output.exitCode = Int32(bitPattern: a0)
            halted = true
        }
        if log.count < 10_000 { log.append((pc, call.name)) }
        return nil
    }
}

/// The instruction-set simulator: one instruction per step, no timing.
/// This is the "architectural" model — what the program means. The pipeline
/// must always end up in exactly the same state.
public final class ISASimulator {
    public let exe: Executable
    public private(set) var regs = [UInt32](repeating: 0, count: 32)
    public private(set) var pc: UInt32
    public let mem: Memory
    public let sys = SyscallHandler()
    public private(set) var instret = 0
    private var decoded: [Decoded?]
    public var hierarchy: MemoryHierarchy?

    public init(_ exe: Executable) {
        self.exe = exe
        mem = Memory(text: exe.text, data: exe.data)
        pc = exe.entry
        regs[Reg.sp] = MemoryMap.stackTop
        regs[Reg.gp] = 0x1000_8000
        decoded = stride(from: 0, to: exe.text.count, by: 4).map { k in
            let w = UInt32(exe.text[k]) | UInt32(exe.text[k + 1]) << 8 | UInt32(exe.text[k + 2]) << 16 | UInt32(exe.text[k + 3]) << 24
            return Decoder.decode(w)
        }
    }

    public var output: ProgramOutput { sys.output }
    public var halted: Bool { sys.halted || sys.output.fault != nil }

    private func fault(_ m: String) {
        sys.output.fault = m
    }

    /// Executes one instruction. Returns false once the program has stopped.
    @discardableResult
    public func step() -> Bool {
        guard !halted else { return false }
        let off = pc &- exe.textBase
        guard pc >= exe.textBase, off / 4 < UInt32(decoded.count), pc & 3 == 0 else {
            fault("jumped to \(Format.hex(pc)), which is not code (pc out of the text segment)")
            return false
        }
        hierarchy?.access(.fetch, pc)
        guard let d = decoded[Int(off / 4)] else {
            fault("illegal instruction at \(Format.hex(pc))")
            return false
        }
        let a = regs[d.rs1], b = regs[d.rs2]
        var next = pc &+ 4
        var result: UInt32?
        do {
            switch d.op {
            case .ecall:
                if let v = try sys.handle(a0: regs[Reg.a0], a7: regs[Reg.a7], mem: mem, pc: pc) { regs[Reg.a0] = v }
            case .ebreak:
                fault("ebreak at \(Format.hex(pc))")
                return false
            case .fence:
                break
            case .beq, .bne, .blt, .bge, .bltu, .bgeu:
                if Exec.taken(d, a, b) { next = Exec.target(d, a, pc: pc) }
            case .jal, .jalr:
                result = pc &+ 4
                next = Exec.target(d, a, pc: pc)
            default:
                let v = Exec.alu(d, a, b, pc: pc)
                if d.op.isLoad || d.op.isStore {
                    hierarchy?.access(d.op.isStore ? .store : .load, v)
                    let m = try Exec.memory(d, v, b, mem)
                    if d.op.isLoad { result = m }
                } else {
                    result = v
                }
            }
        } catch let f as MemoryFault {
            fault("\(f.message) — at \(Format.hex(pc)) (\(exe.symbolize(pc) ?? "?"))")
            return false
        } catch {
            return false
        }
        if let r = result, d.rd != 0 { regs[d.rd] = r }
        pc = next
        instret += 1
        return !halted
    }

    /// Runs until the program exits, faults, or `limit` instructions have run.
    public func run(limit: Int = 50_000_000) {
        while instret < limit && step() {}
        if !halted && instret >= limit {
            fault("stopped after \(limit) instructions (infinite loop?)")
        }
    }

    public func register(_ r: Int) -> UInt32 { regs[r] }
}
