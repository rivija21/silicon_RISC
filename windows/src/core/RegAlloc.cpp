#include "Backend.h"

namespace sp {

// MARK: - Registers

const char* const Reg::names[32] = {
    "zero", "ra", "sp", "gp", "tp", "t0", "t1", "t2",
    "s0", "s1", "a0", "a1", "a2", "a3", "a4", "a5",
    "a6", "a7", "s2", "s3", "s4", "s5", "s6", "s7",
    "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6",
};

const std::vector<int> Reg::callerSaved = {5, 6, 7, 28};
const std::vector<int> Reg::argumentTemps = {15, 14, 13, 12, 16, 17, 11, 10};
const std::vector<int> Reg::calleeSaved = {9, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27};
const std::vector<int> Reg::scratch = {29, 30, 31};

std::optional<int> Reg::parse(const std::string& s) {
    std::string l = lowercased(s);
    for (int i = 0; i < 32; i++)
        if (l == names[i]) return i;
    if (l == "fp") return 8;
    if (hasPrefix(l, "x")) {
        std::string rest = l.substr(1);
        auto n = parseInt(rest);
        if (n && *n >= 0 && *n < 32 && std::to_string(*n) == rest) return (int)*n;
    }
    return std::nullopt;
}

const char* Reg::role(int r) {
    switch (r) {
    case 0: return "hard-wired zero";
    case 1: return "return address";
    case 2: return "stack pointer";
    case 3: return "global pointer";
    case 4: return "thread pointer";
    case 5: case 6: case 7: case 28: case 29: case 30: case 31: return "temporary (caller-saved)";
    case 8: return "saved / frame pointer";
    case 9: case 18: case 19: case 20: case 21: case 22: case 23: case 24: case 25: case 26: case 27: return "saved (callee-saved)";
    case 10: case 11: return "argument / return value";
    default: return "argument";
    }
}

// MARK: - Linear scan

/// 1. Number the instructions in layout order.
/// 2. Use liveness analysis to find, for each virtual register, the interval
///    from its first definition to its last use.
/// 3. Walk the intervals in order of their start. Free the registers of
///    intervals that have ended; give the new interval a free register.
/// 4. If none is free, spill whichever interval ends last to the stack.
///
/// Values that stay alive across a call must survive it, so they only get
/// callee-saved registers (s1–s11). Others prefer the temporaries.
Allocation allocateRegisters(const IRFunction& f, std::optional<int> registerLimit) {
    Allocation alloc;
    alloc.function = f.name;
    alloc.registerLimit = registerLimit;
    auto live = Liveness::analyze(f);
    std::map<int, int> start, end;
    auto touch = [&](int r, int p) {
        auto s = start.find(r);
        if (s == start.end()) start[r] = p;
        else s->second = std::min(s->second, p);
        auto e = end.find(r);
        if (e == end.end()) end[r] = p;
        else e->second = std::max(e->second, p);
    };
    for (int p : f.params) touch(p, 0);
    int pos = 2;
    for (auto& b : f.blocks) {
        alloc.blockStart[b->id] = pos;
        for (int r : live.liveIn[b->id]) touch(r, pos);
        for (auto& i : b->insts) {
            for (int u : i.uses()) touch(u, pos);
            if (int d = i.def()) touch(d, pos);
            if (i.kind.kind == InstKind::Call) alloc.callPositions.push_back(pos);
            pos += 2;
        }
        for (int u : b->term.uses()) touch(u, pos);
        alloc.blockEnd[b->id] = pos;
        for (int r : live.liveOut[b->id]) touch(r, pos);
        pos += 2;
    }
    alloc.maxPosition = pos;
    const std::vector<int>& calls = alloc.callPositions;

    std::vector<LiveInterval> intervals;
    for (auto& [r, s] : start) {
        int e = end[r];
        bool crosses = std::any_of(calls.begin(), calls.end(), [&](int c) { return c > s && c < e; });
        intervals.push_back(LiveInterval{r, f.vregs[r].name, s, e, crosses, Location::spill(-1), ""});
    }
    std::set<int> params(f.params.begin(), f.params.end());
    // a0–a7 are safe only for values that never touch a call (not even as an argument
    // or a result) and that are not incoming parameters still waiting in a0–a7.
    auto touchesCall = [&](const LiveInterval& iv) {
        return std::any_of(calls.begin(), calls.end(), [&](int c) { return c >= iv.start && c <= iv.end; });
    };
    auto argSafe = [&](const LiveInterval& iv) { return !params.count(iv.vreg) && !touchesCall(iv); };
    std::stable_sort(intervals.begin(), intervals.end(), [](const LiveInterval& a, const LiveInterval& b) {
        return a.start != b.start ? a.start < b.start : a.vreg < b.vreg;
    });

    std::vector<int> caller = Reg::callerSaved, callee = Reg::calleeSaved, argRegs = Reg::argumentTemps;
    if (registerLimit) {
        // Teaching mode: pretend the machine has fewer registers, to see spilling happen.
        int limit = *registerLimit;
        int nCaller = std::min((int)caller.size(), (limit + 1) / 2);
        caller.resize(nCaller);
        callee.resize(std::min((int)callee.size(), std::max(0, limit - nCaller)));
        argRegs.clear();
    }
    std::set<int> freeRegs;
    for (int r : caller) freeRegs.insert(r);
    for (int r : callee) freeRegs.insert(r);
    for (int r : argRegs) freeRegs.insert(r);
    std::vector<int> active;   // indices into intervals
    std::set<int> usedCallee;
    auto regOf = [&](int k) -> std::optional<int> {
        if (intervals[k].location.kind == Location::Register) return intervals[k].location.n;
        return std::nullopt;
    };
    auto contains = [](const std::vector<int>& v, int x) { return std::find(v.begin(), v.end(), x) != v.end(); };

    for (int k = 0; k < (int)intervals.size(); k++) {
        const LiveInterval cur = intervals[k];
        // Expire intervals that end before this one starts. An interval ending exactly
        // here can hand its register over: the instruction reads it before writing.
        active.erase(std::remove_if(active.begin(), active.end(),
                                    [&](int a) {
                                        if (intervals[a].end <= cur.start) {
                                            if (auto r = regOf(a)) {
                                                freeRegs.insert(*r);
                                                return true;
                                            }
                                        }
                                        return false;
                                    }),
                     active.end());
        std::vector<int> pool;
        if (cur.crossesCall) {
            pool = callee;
        } else if (argSafe(cur)) {
            pool = caller;
            pool.insert(pool.end(), argRegs.begin(), argRegs.end());
            pool.insert(pool.end(), callee.begin(), callee.end());
        } else {
            pool = caller;
            pool.insert(pool.end(), callee.begin(), callee.end());
        }
        // A parameter that never meets a call can simply stay in the register it arrived in.
        auto pit = std::find(f.params.begin(), f.params.end(), cur.vreg);
        if (pit != f.params.end()) {
            int pk = (int)(pit - f.params.begin());
            if (pk < 8 && !argRegs.empty() && !touchesCall(cur)) pool.insert(pool.begin(), Reg::a(pk));
        }
        std::optional<int> chosen;
        for (int r : pool)
            if (freeRegs.count(r)) {
                chosen = r;
                break;
            }
        if (chosen) {
            freeRegs.erase(*chosen);
            intervals[k].location = Location::reg(*chosen);
            intervals[k].note = cur.crossesCall ? "lives across a call, so it needs a callee-saved register" : "";
            if (contains(callee, *chosen)) usedCallee.insert(*chosen);
            active.push_back(k);
            continue;
        }
        // No register free: spill the interval that ends furthest away.
        std::optional<int> victim;
        for (int a : active) {
            auto r = regOf(a);
            if (!r || !contains(pool, *r)) continue;
            if (!victim || intervals[*victim].end < intervals[a].end) victim = a;
        }
        if (victim && intervals[*victim].end > cur.end) {
            int r = *regOf(*victim);
            intervals[*victim].location = Location::spill(alloc.spillSlots);
            intervals[*victim].note = cat("spilled: evicted by ", cur.name, ", which ends sooner");
            alloc.spillSlots += 1;
            active.erase(std::remove(active.begin(), active.end(), *victim), active.end());
            intervals[k].location = Location::reg(r);
            active.push_back(k);
        } else {
            intervals[k].location = Location::spill(alloc.spillSlots);
            intervals[k].note = "spilled: no free register, and it lives the longest";
            alloc.spillSlots += 1;
        }
    }
    for (auto& i : intervals) alloc.locations[i.vreg] = i.location;
    alloc.intervals = intervals;
    alloc.usedCalleeSaved.assign(usedCallee.begin(), usedCallee.end());
    return alloc;
}

}  // namespace sp
