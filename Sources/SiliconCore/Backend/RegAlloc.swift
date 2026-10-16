import Foundation

public enum Location: Equatable, Sendable {
    /// A physical register x0…x31.
    case reg(Int)
    /// Spill slot number k in the stack frame.
    case spill(Int)
}

public struct LiveInterval: Sendable {
    public var vreg: Int
    public var name: String
    public var start: Int
    public var end: Int
    public var crossesCall: Bool
    public var location: Location
    /// Why it ended up where it did, for the allocation view.
    public var note: String
}

public struct Allocation: Sendable {
    public var function: String
    public var locations: [Int: Location] = [:]
    public var intervals: [LiveInterval] = []
    public var spillSlots = 0
    public var usedCalleeSaved: [Int] = []
    /// Linear position of each block's first instruction and of its terminator.
    public var blockStart: [Int: Int] = [:]
    public var blockEnd: [Int: Int] = [:]
    public var callPositions: [Int] = []
    public var maxPosition = 0
    public var registerLimit: Int?
}

/// Linear-scan register allocation (Poletto & Sarkar, 1999).
///
/// 1. Number the instructions in layout order.
/// 2. Use liveness analysis to find, for each virtual register, the interval
///    from its first definition to its last use.
/// 3. Walk the intervals in order of their start. Free the registers of
///    intervals that have ended; give the new interval a free register.
/// 4. If none is free, spill whichever interval ends last to the stack.
///
/// Values that stay alive across a call must survive it, so they only get
/// callee-saved registers (s1–s11). Others prefer the temporaries t0–t4.
public enum RegisterAllocator {
    public static func allocate(_ f: IRFunction, registerLimit: Int? = nil) -> Allocation {
        var alloc = Allocation(function: f.name)
        alloc.registerLimit = registerLimit
        let live = Liveness.analyze(f)
        var start: [Int: Int] = [:], end: [Int: Int] = [:]
        func touch(_ r: Int, _ p: Int) {
            start[r] = min(start[r] ?? p, p)
            end[r] = max(end[r] ?? p, p)
        }
        for p in f.params { touch(p, 0) }
        var pos = 2
        for b in f.blocks {
            alloc.blockStart[b.id] = pos
            for r in live.liveIn[b.id] ?? [] { touch(r, pos) }
            for i in b.insts {
                for u in i.uses { touch(u, pos) }
                if let d = i.def { touch(d, pos) }
                if case .call = i.kind { alloc.callPositions.append(pos) }
                pos += 2
            }
            for u in b.term.uses { touch(u, pos) }
            alloc.blockEnd[b.id] = pos
            for r in live.liveOut[b.id] ?? [] { touch(r, pos) }
            pos += 2
        }
        alloc.maxPosition = pos
        let calls = alloc.callPositions

        var intervals: [LiveInterval] = start.keys.sorted().map { r in
            let s = start[r]!, e = end[r]!
            let crosses = calls.contains { $0 > s && $0 < e }
            return LiveInterval(vreg: r, name: f.vregs[r].name, start: s, end: e, crossesCall: crosses, location: .spill(-1), note: "")
        }
        let params = Set(f.params)
        // a0–a7 are safe only for values that never touch a call (not even as an argument
        // or a result) and that are not incoming parameters still waiting in a0–a7.
        func argSafe(_ iv: LiveInterval) -> Bool {
            !params.contains(iv.vreg) && !calls.contains { $0 >= iv.start && $0 <= iv.end }
        }
        intervals.sort { ($0.start, $0.vreg) < ($1.start, $1.vreg) }

        var caller = Reg.callerSaved, callee = Reg.calleeSaved, argRegs = Reg.argumentTemps
        if let limit = registerLimit {
            // Teaching mode: pretend the machine has fewer registers, to see spilling happen.
            let nCaller = min(caller.count, (limit + 1) / 2)
            caller = Array(caller.prefix(nCaller))
            callee = Array(callee.prefix(max(0, limit - nCaller)))
            argRegs = []
        }
        var free = Set(caller + callee + argRegs)
        var active: [Int] = []   // indices into intervals
        var usedCallee = Set<Int>()

        func regOf(_ k: Int) -> Int? { if case let .reg(r) = intervals[k].location { return r }; return nil }

        for k in intervals.indices {
            let cur = intervals[k]
            // Expire intervals that end before this one starts. An interval ending exactly
            // here can hand its register over: the instruction reads it before writing.
            active.removeAll { a in
                if intervals[a].end <= cur.start, let r = regOf(a) {
                    free.insert(r)
                    return true
                }
                return false
            }
            var pool = cur.crossesCall ? callee : (argSafe(cur) ? caller + argRegs + callee : caller + callee)
            // A parameter that never meets a call can simply stay in the register it arrived in.
            if let k = f.params.firstIndex(of: cur.vreg), k < 8, !argRegs.isEmpty,
               !calls.contains(where: { $0 >= cur.start && $0 <= cur.end }) {
                pool = [Reg.a(k)] + pool
            }
            if let r = pool.first(where: { free.contains($0) }) {
                free.remove(r)
                intervals[k].location = .reg(r)
                intervals[k].note = cur.crossesCall ? "lives across a call, so it needs a callee-saved register" : ""
                if callee.contains(r) { usedCallee.insert(r) }
                active.append(k)
                continue
            }
            // No register free: spill the interval that ends furthest away.
            let candidates = active.filter { a in
                guard let r = regOf(a) else { return false }
                return pool.contains(r)
            }
            if let victim = candidates.max(by: { intervals[$0].end < intervals[$1].end }), intervals[victim].end > cur.end {
                let r = regOf(victim)!
                intervals[victim].location = .spill(alloc.spillSlots)
                intervals[victim].note = "spilled: evicted by \(cur.name), which ends sooner"
                alloc.spillSlots += 1
                active.removeAll { $0 == victim }
                intervals[k].location = .reg(r)
                active.append(k)
            } else {
                intervals[k].location = .spill(alloc.spillSlots)
                intervals[k].note = "spilled: no free register, and it lives the longest"
                alloc.spillSlots += 1
            }
        }
        for i in intervals { alloc.locations[i.vreg] = i.location }
        alloc.intervals = intervals
        alloc.usedCalleeSaved = usedCallee.sorted()
        return alloc
    }
}
