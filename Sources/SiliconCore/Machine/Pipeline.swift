import Foundation

public enum BranchStage: String, CaseIterable, Sendable {
    case id = "ID", ex = "EX", mem = "MEM"
    /// Instructions thrown away on a misprediction.
    public var penalty: Int {
        switch self {
        case .id: return 1
        case .ex: return 2
        case .mem: return 3
        }
    }
}

public enum PredictorKind: String, CaseIterable, Sendable {
    case notTaken = "Static: predict not taken"
    case oneBit = "Dynamic: 1-bit history"
    case twoBit = "Dynamic: 2-bit saturating counter"
}

public struct PipelineConfig: Equatable, Sendable {
    public var forwarding = true
    public var branchStage: BranchStage = .ex
    public var predictor: PredictorKind = .notTaken
    public var hierarchy: HierarchyConfig = .teaching
    public var btbEntries = 64

    public init(forwarding: Bool = true, branchStage: BranchStage = .ex, predictor: PredictorKind = .notTaken,
                hierarchy: HierarchyConfig = .teaching) {
        self.forwarding = forwarding
        self.branchStage = branchStage
        self.predictor = predictor
        self.hierarchy = hierarchy
    }
}

public enum PipeStage: Int, CaseIterable, Sendable {
    case IF = 0, ID, EX, MEM, WB
    public var name: String { ["IF", "ID", "EX", "MEM", "WB"][rawValue] }
    public var longName: String {
        ["Instruction fetch", "Instruction decode & register read", "Execute (ALU)", "Memory access", "Write back"][rawValue]
    }
}

public enum StallReason: String, Sendable {
    case loadUse = "load-use hazard"
    case dataHazard = "data hazard (no forwarding)"
    case branchOperand = "branch operand not ready"
    case ecall = "system call waits for the pipeline to drain"
    case dcache = "D-cache miss"
    case icache = "I-cache miss"
}

public enum ForwardSource: String, Sendable {
    case none, exmem = "EX/MEM", memwb = "MEM/WB"
}

/// What one stage held during one cycle.
public struct StageSlot: Sendable {
    public enum Status: Sendable { case normal, bubble, stalled, flushed, waiting }
    public var seq: Int
    public var pc: UInt32
    public var status: Status
    public static let bubble = StageSlot(seq: -1, pc: 0, status: .bubble)
}

public struct CycleRecord: Sendable {
    public var cycle: Int
    public var stages: [StageSlot]
    public var forwardA: ForwardSource = .none
    public var forwardB: ForwardSource = .none
    /// Forwarding into ID for a branch resolved there.
    public var forwardID: Bool = false
    public var stall: StallReason?
    public var flushed: Int = 0
    public var flushReason: String = ""
    public var retired: Int?
    public var fetch: MemoryAccess?
    public var data: MemoryAccess?
    public var redirect: UInt32?
    public var registerWrite: (Int, UInt32)?
}

/// One instruction travelling down the pipeline.
public struct InFlight: Sendable {
    public var seq: Int
    public var pc: UInt32
    public var d: Decoded?
    public var fault: String?
    public var predictedTaken = false
    public var predictedTarget: UInt32 = 0
    public var a: UInt32 = 0
    public var b: UInt32 = 0
    public var alu: UInt32 = 0
    public var result: UInt32 = 0
    public var taken = false
    public var target: UInt32 = 0
    public var resolved = false
    public var memDone = false
    public var memWait = 0
    public var exits = false

    var dest: Int? { d?.dest }
    var isLoad: Bool { d?.op.isLoad ?? false }
    var sources: [Int] { d?.sources ?? [] }
}

public struct PipelineStats: Sendable {
    public var cycles = 0
    public var retired = 0
    public var stalls: [StallReason: Int] = [:]
    public var flushes = 0
    public var flushedInstructions = 0
    public var branches = 0
    public var mispredictions = 0
    public var forwards = 0
    public var cpi: Double { retired == 0 ? 0 : Double(cycles) / Double(retired) }
}

/// The classic five-stage RISC-V pipeline of Patterson & Hennessy, chapter 4.
///
/// Every call to `step()` is one clock cycle. The stages are evaluated from
/// WB back to IF, each reading the pipeline register in front of it as it was
/// at the start of the cycle — just like flip-flops that all update on the
/// clock edge. The register file is written in the first half of the cycle
/// and read in the second half, so WB → ID needs no forwarding.
public final class Pipeline {
    public let exe: Executable
    public let config: PipelineConfig
    public private(set) var regs = [UInt32](repeating: 0, count: 32)
    public let mem: Memory
    public let sys = SyscallHandler()
    public let hierarchy: MemoryHierarchy
    public private(set) var stats = PipelineStats()

    // Pipeline registers.
    public private(set) var ifid: InFlight?
    public private(set) var idex: InFlight?
    public private(set) var exmem: InFlight?
    public private(set) var memwb: InFlight?
    public private(set) var pc: UInt32
    private var fetchPending: (seq: Int, pc: UInt32, wait: Int)?
    private var draining = false
    public private(set) var halted = false
    private var nextSeq = 0
    private var decoded: [Decoded?]

    // Branch prediction.
    private var counters: [UInt8]
    private var btbTag: [UInt32]
    private var btbTarget: [UInt32]

    // History for the pipeline diagram.
    public var historyLimit = 4096
    public private(set) var history: [CycleRecord] = []
    /// pc of every dynamic instruction still visible in the history.
    public private(set) var seqPC: [Int: UInt32] = [:]
    public var recordHistory = true

    public init(_ exe: Executable, config: PipelineConfig = PipelineConfig()) {
        self.exe = exe
        self.config = config
        mem = Memory(text: exe.text, data: exe.data)
        hierarchy = MemoryHierarchy(config.hierarchy)
        pc = exe.entry
        regs[Reg.sp] = MemoryMap.stackTop
        regs[Reg.gp] = 0x1000_8000
        decoded = stride(from: 0, to: exe.text.count, by: 4).map { k in
            let w = UInt32(exe.text[k]) | UInt32(exe.text[k + 1]) << 8 | UInt32(exe.text[k + 2]) << 16 | UInt32(exe.text[k + 3]) << 24
            return Decoder.decode(w)
        }
        counters = [UInt8](repeating: 1, count: config.btbEntries)
        btbTag = [UInt32](repeating: 0xFFFF_FFFF, count: config.btbEntries)
        btbTarget = [UInt32](repeating: 0, count: config.btbEntries)
    }

    public var output: ProgramOutput { sys.output }
    public var cycle: Int { stats.cycles }
    public func register(_ r: Int) -> UInt32 { regs[r] }
    public var lastRecord: CycleRecord? { history.last }

    // MARK: Branch predictor

    private func btbIndex(_ pc: UInt32) -> Int { Int((pc >> 2) % UInt32(config.btbEntries)) }

    private func predict(_ pc: UInt32) -> (Bool, UInt32) {
        guard config.predictor != .notTaken else { return (false, pc &+ 4) }
        let k = btbIndex(pc)
        guard btbTag[k] == pc else { return (false, pc &+ 4) }
        let taken = config.predictor == .oneBit ? counters[k] >= 1 : counters[k] >= 2
        return taken ? (true, btbTarget[k]) : (false, pc &+ 4)
    }

    private func train(_ pc: UInt32, taken: Bool, target: UInt32) {
        guard config.predictor != .notTaken else { return }
        let k = btbIndex(pc)
        if btbTag[k] != pc {
            guard taken else { return }
            btbTag[k] = pc
            counters[k] = config.predictor == .oneBit ? 1 : 2
            btbTarget[k] = target
            return
        }
        btbTarget[k] = target
        if config.predictor == .oneBit {
            counters[k] = taken ? 1 : 0
        } else if taken {
            counters[k] = min(3, counters[k] + 1)
        } else {
            counters[k] = counters[k] == 0 ? 0 : counters[k] - 1
        }
    }

    public func predictorState(_ pc: UInt32) -> (inBTB: Bool, counter: Int, target: UInt32) {
        let k = btbIndex(pc)
        return (btbTag[k] == pc, Int(counters[k]), btbTarget[k])
    }

    // MARK: Helpers

    private func writes(_ x: InFlight?, _ r: Int) -> Bool {
        guard let x, let d = x.dest, x.fault == nil else { return false }
        return d == r
    }

    private func slot(_ x: InFlight?, _ status: StageSlot.Status = .normal) -> StageSlot {
        guard let x else { return .bubble }
        return StageSlot(seq: x.seq, pc: x.pc, status: status)
    }

    /// Resolves a branch or jump. Returns the redirect target if the prediction was wrong.
    private func resolve(_ x: inout InFlight) -> UInt32? {
        guard let d = x.d, d.op.isBranch || d.op.isJump, !x.resolved else { return nil }
        x.resolved = true
        x.taken = Exec.taken(d, x.a, x.b)
        x.target = Exec.target(d, x.a, pc: x.pc)
        if d.op.isBranch { stats.branches += 1 }
        let actualNext = x.taken ? x.target : x.pc &+ 4
        let predictedNext = x.predictedTaken ? x.predictedTarget : x.pc &+ 4
        train(x.pc, taken: x.taken, target: x.target)
        if actualNext != predictedNext {
            if d.op.isBranch { stats.mispredictions += 1 }
            return actualNext
        }
        return nil
    }

    // MARK: The clock

    /// Advances one clock cycle. Returns false once the program has finished.
    @discardableResult
    public func step() -> Bool {
        guard !halted else { return false }
        stats.cycles += 1
        hierarchy.time = stats.cycles
        var rec = CycleRecord(cycle: stats.cycles, stages: [.bubble, slot(ifid), slot(idex), slot(exmem), slot(memwb)])
        if let f = fetchPending { rec.stages[0] = StageSlot(seq: f.seq, pc: f.pc, status: .normal) }

        // ---- WB: write the result into the register file (first half of the cycle).
        if let w = memwb {
            rec.retired = w.seq
            if let f = w.fault {
                sys.output.fault = f
                halted = true
                finish(rec)
                return false
            }
            if let r = w.dest {
                regs[r] = w.result
                rec.registerWrite = (r, w.result)
            }
            stats.retired += 1
            if w.exits {
                halted = true
                finish(rec)
                return false
            }
        }

        var redirect: UInt32?
        var flushStages = 0
        var reason = ""

        // ---- MEM: loads and stores go to the data cache.
        var toWB: InFlight?
        if var m = exmem {
            if !m.memDone {
                m.memDone = true
                if let d = m.d, m.fault == nil, d.op.isLoad || d.op.isStore {
                    let acc = hierarchy.access(d.op.isStore ? .store : .load, m.alu)
                    rec.data = acc
                    m.memWait = acc.penalty
                    do {
                        let v = try Exec.memory(d, m.alu, m.b, mem)
                        m.result = d.op.isLoad ? v : m.alu
                    } catch let f as MemoryFault {
                        m.fault = "\(f.message) — at \(Format.hex(m.pc)) (\(exe.symbolize(m.pc) ?? "?"))"
                    } catch {}
                } else {
                    m.result = m.alu
                }
                if config.branchStage == .mem, m.fault == nil, let target = resolve(&m) {
                    redirect = target
                    flushStages = 3
                    reason = "branch resolved in MEM was mispredicted"
                }
            }
            if m.memWait > 0 {
                // Data-cache miss: the whole pipeline behind MEM freezes; WB gets a bubble.
                m.memWait -= 1
                exmem = m
                // The instruction frozen in EX must not lose a value MEM/WB was about to forward.
                if config.forwarding, var e = idex, let d = e.d, let w = memwb, let r = w.dest, w.fault == nil {
                    let r1 = d.op == .ecall ? Reg.a0 : d.rs1, r2 = d.op == .ecall ? Reg.a7 : d.rs2
                    if r1 == r { e.a = w.result }
                    if r2 == r { e.b = w.result }
                    idex = e
                }
                memwb = nil
                rec.stall = .dcache
                rec.stages[PipeStage.MEM.rawValue].status = .waiting
                for k in 0...2 where rec.stages[k].seq >= 0 { rec.stages[k].status = .stalled }
                stats.stalls[.dcache, default: 0] += 1
                finish(rec)
                return true
            }
            toWB = m
        }

        // ---- EX: the ALU, with operands forwarded from later stages.
        var toMEM: InFlight?
        if var e = idex {
            if let d = e.d, e.fault == nil, redirect == nil || flushStages < 3 {
                let r1 = d.op == .ecall ? Reg.a0 : d.rs1
                let r2 = d.op == .ecall ? Reg.a7 : d.rs2
                if config.forwarding {
                    // toWB is what EX/MEM held at the start of the cycle; memwb is the old MEM/WB.
                    func forward(_ r: Int, _ v: UInt32) -> (UInt32, ForwardSource) {
                        guard r != 0 else { return (0, .none) }
                        if let x = toWB, writes(x, r), !x.isLoad { return (x.alu, .exmem) }
                        if let x = memwb, writes(x, r) { return (x.result, .memwb) }
                        return (v, .none)
                    }
                    let uses1 = d.op.readsRs1 || d.op == .ecall, uses2 = d.op.readsRs2 || d.op == .ecall
                    let (a, fa) = uses1 ? forward(r1, e.a) : (e.a, .none)
                    let (b, fb) = uses2 ? forward(r2, e.b) : (e.b, .none)
                    e.a = a
                    e.b = b
                    rec.forwardA = fa
                    rec.forwardB = fb
                    stats.forwards += (fa != .none ? 1 : 0) + (fb != .none ? 1 : 0)
                }
                if d.op == .ecall {
                    do {
                        let newA0 = try sys.handle(a0: e.a, a7: e.b, mem: mem, pc: e.pc)
                        e.alu = newA0 ?? e.a
                        if sys.halted {
                            e.exits = true
                            draining = true
                        }
                    } catch let f as MemoryFault {
                        e.fault = f.message
                    } catch {}
                } else if d.op == .ebreak {
                    e.fault = "ebreak at \(Format.hex(e.pc))"
                } else {
                    e.alu = Exec.alu(d, e.a, e.b, pc: e.pc)
                }
                if config.branchStage == .ex, e.fault == nil, let target = resolve(&e) {
                    redirect = target
                    flushStages = 2
                    reason = "branch resolved in EX was mispredicted"
                }
            }
            toMEM = e
        }

        // ---- ID: decode, read registers, detect hazards.
        var stallID = false
        var toEX: InFlight?
        if var i = ifid, redirect == nil {
            let srcs = i.sources
            let op = i.d?.op
            let comparesHere = config.branchStage == .id && (op?.isBranch == true || op == .jalr)
            if i.fault == nil {
                if config.forwarding {
                    if let x = idex, x.isLoad, let r = x.dest, srcs.contains(r) {
                        stallID = true
                        rec.stall = .loadUse
                    }
                } else if srcs.contains(where: { writes(idex, $0) || writes(exmem, $0) }) {
                    stallID = true
                    rec.stall = .dataHazard
                }
                if !stallID && comparesHere {
                    // The comparator sits in ID, so its operands must already exist.
                    let waitingALU = srcs.contains { writes(idex, $0) }
                    let waitingLoad = srcs.contains { r in exmem.map { $0.isLoad && writes($0, r) } ?? false }
                    if waitingALU || waitingLoad {
                        stallID = true
                        rec.stall = .branchOperand
                    }
                }
                if !stallID, op == .ecall, idex != nil || exmem != nil {
                    stallID = true
                    rec.stall = .ecall
                }
            }
            if stallID {
                stats.stalls[rec.stall!, default: 0] += 1
                rec.stages[PipeStage.ID.rawValue].status = .stalled
            } else {
                if let d = i.d {
                    let r1 = op == .ecall ? Reg.a0 : d.rs1
                    let r2 = op == .ecall ? Reg.a7 : d.rs2
                    i.a = regs[r1]
                    i.b = regs[r2]
                    if comparesHere {
                        // Forward an ALU result sitting in EX/MEM straight into the comparator.
                        if config.forwarding, let x = exmem, !x.isLoad {
                            if d.rs1 != 0 && writes(x, d.rs1) { i.a = x.alu; rec.forwardID = true }
                            if d.rs2 != 0 && writes(x, d.rs2) { i.b = x.alu; rec.forwardID = true }
                        }
                        if let target = resolve(&i) {
                            redirect = target
                            flushStages = 1
                            reason = "branch resolved in ID was mispredicted"
                        }
                    } else if d.op == .jal, let target = resolve(&i) {
                        // jal's target (pc + offset) is known as soon as it is decoded.
                        redirect = target
                        flushStages = 1
                        reason = "jump: target known in ID"
                    }
                }
                toEX = i
            }
        }

        // ---- IF: fetch from the I-cache. A miss keeps IF busy for several cycles.
        var toID: InFlight?
        if !draining {
            if fetchPending == nil {
                let fpc = pc
                var acc: MemoryAccess?
                if fpc >= exe.textBase && fpc < exe.textEnd && fpc & 3 == 0 {
                    acc = hierarchy.access(.fetch, fpc)
                    rec.fetch = acc
                }
                fetchPending = (nextSeq, fpc, acc?.penalty ?? 0)
                nextSeq += 1
                if recordHistory { seqPC[nextSeq - 1] = fpc }
            }
            var f = fetchPending!
            rec.stages[0] = StageSlot(seq: f.seq, pc: f.pc, status: .normal)
            if f.wait > 0 {
                f.wait -= 1
                fetchPending = f
                rec.stages[0].status = .waiting
                if rec.stall == nil { rec.stall = .icache }
                stats.stalls[.icache, default: 0] += 1
            } else if stallID {
                rec.stages[0].status = .stalled
            } else {
                fetchPending = nil
                var x = InFlight(seq: f.seq, pc: f.pc)
                let off = f.pc &- exe.textBase
                if f.pc >= exe.textBase, f.pc & 3 == 0, Int(off / 4) < decoded.count {
                    x.d = decoded[Int(off / 4)]
                    if x.d == nil { x.fault = "illegal instruction at \(Format.hex(f.pc))" }
                } else {
                    x.fault = "jumped to \(Format.hex(f.pc)), which is not code (pc out of the text segment)"
                }
                let (t, target) = x.fault == nil ? predict(f.pc) : (false, f.pc &+ 4)
                x.predictedTaken = t
                x.predictedTarget = target
                pc = t ? target : f.pc &+ 4
                toID = x
            }
        }

        // ---- Clock edge: every pipeline register updates at once.
        memwb = toWB
        exmem = toMEM
        if stallID {
            idex = nil          // a bubble (nop) goes down the pipe
        } else {
            idex = toEX
            ifid = toID
        }
        if draining {
            // An exit ecall is on its way out: cancel everything younger.
            if exmem?.exits == true || memwb?.exits == true {
                if idex != nil, idex?.exits != true { idex = nil }
                ifid = nil
                fetchPending = nil
                for k in 0...1 where rec.stages[k].seq >= 0 { rec.stages[k].status = .flushed }
            }
        }
        if let r = redirect {
            applyRedirect(r, stages: flushStages, &rec, reason: reason)
        }
        finish(rec)
        return true
    }

    /// Throws away the instructions fetched after a mispredicted branch and restarts fetch at `target`.
    private func applyRedirect(_ target: UInt32, stages: Int, _ rec: inout CycleRecord, reason: String) {
        // The stages in front of the one that resolved the branch hold wrong-path instructions.
        var n = 0
        for k in 0..<stages where rec.stages[k].seq >= 0 {
            rec.stages[k].status = .flushed
            n += 1
        }
        ifid = nil
        if stages >= 2 { idex = nil }
        if stages >= 3 { exmem = nil }
        fetchPending = nil
        pc = target
        rec.redirect = target
        rec.flushed = n
        rec.flushReason = reason
        if n > 0 {
            stats.flushes += 1
            stats.flushedInstructions += n
        }
    }

    private func finish(_ rec: CycleRecord) {
        guard recordHistory else { return }
        history.append(rec)
        if history.count > historyLimit + 512 {
            history.removeFirst(history.count - historyLimit)
            let minSeq = history.first?.stages.map { $0.seq }.filter { $0 >= 0 }.min() ?? 0
            seqPC = seqPC.filter { $0.key >= minSeq - 8 }
        }
    }

    /// Runs until the program stops or `maxCycles` cycles have passed.
    public func run(maxCycles: Int = 100_000_000) {
        while stats.cycles < maxCycles && step() {}
        if !halted && stats.cycles >= maxCycles {
            sys.output.fault = "stopped after \(maxCycles) cycles (infinite loop?)"
            halted = true
        }
    }

    /// What each stage holds right now (between cycles), for the datapath view.
    public var stageContents: [InFlight?] { [nil, ifid, idex, exmem, memwb] }
    public var fetchPC: UInt32 { fetchPending?.pc ?? pc }
}
