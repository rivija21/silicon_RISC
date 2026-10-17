#include "Machine.h"

namespace sp {

// MARK: - Memory

Memory::Memory(const std::vector<uint8_t>& text, const std::vector<uint8_t>& data)
    : textBase(MemoryMap::textBase), dataBase(MemoryMap::dataBase), stackBase(MemoryMap::stackBottom), text_(text) {
    data_.assign(MemoryMap::heapLimit - MemoryMap::dataBase, 0);
    std::copy(data.begin(), data.end(), data_.begin());
    stack_.assign((MemoryMap::stackTop + 4 - MemoryMap::stackBottom) + 16, 0);
    dataHigh_ = dataBase + (uint32_t)data.size();
    stackLow_ = MemoryMap::stackTop;
}

std::string Memory::describe(uint32_t addr) const {
    if (addr < 4096) return cat("address ", Format::hex(addr), " (looks like a NULL pointer)");
    return cat("address ", Format::hex(addr));
}

size_t Memory::locateSlow(uint32_t addr, uint32_t size, bool write, int& region) const {
    if (addr >= textBase && text_.size() >= size && addr - textBase <= (uint32_t)text_.size() - size) {
        if (write) throw MemoryFault{addr, cat("write to the code (text) segment at ", Format::hex(addr))};
        region = 0;
        return addr - textBase;
    }
    throw MemoryFault{addr, cat(write ? "store to" : "load from", " unmapped ", describe(addr), " — segmentation fault")};
}

bool Memory::isMapped(uint32_t addr) const {
    try {
        int r;
        locate(addr, 1, false, r);
        return true;
    } catch (const MemoryFault&) {
        return false;
    }
}

uint16_t Memory::load16(uint32_t addr) const {
    if (addr & 1) throw MemoryFault{addr, cat("misaligned halfword load at ", Format::hex(addr))};
    return (uint16_t)(load8(addr) | load8(addr + 1) << 8);
}

uint32_t Memory::load32(uint32_t addr) const {
    if (addr & 3) throw MemoryFault{addr, cat("misaligned word load at ", Format::hex(addr), " (words must be at multiples of 4)")};
    int r;
    size_t o = locate(addr, 4, false, r);
    const uint8_t* p = r == 1 ? &data_[o] : r == 2 ? &stack_[o] : &text_[o];
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

void Memory::store16(uint32_t addr, uint16_t v) {
    if (addr & 1) throw MemoryFault{addr, cat("misaligned halfword store at ", Format::hex(addr))};
    store8(addr, v & 0xFF);
    store8(addr + 1, v >> 8);
}

void Memory::store32(uint32_t addr, uint32_t v) {
    if (addr & 3) throw MemoryFault{addr, cat("misaligned word store at ", Format::hex(addr), " (words must be at multiples of 4)")};
    int r;
    size_t o = locate(addr, 4, true, r);
    uint8_t* p = r == 1 ? &data_[o] : &stack_[o];
    p[0] = v & 0xFF;
    p[1] = (v >> 8) & 0xFF;
    p[2] = (v >> 16) & 0xFF;
    p[3] = v >> 24;
    touched(r, addr);
}

std::vector<uint8_t> Memory::cString(uint32_t addr, int limit) const {
    std::vector<uint8_t> out;
    uint32_t a = addr;
    while ((int)out.size() < limit) {
        uint8_t b = load8(a);
        if (b == 0) break;
        out.push_back(b);
        a += 1;
    }
    return out;
}

std::vector<std::optional<uint8_t>> Memory::bytes(uint32_t addr, int count) const {
    std::vector<std::optional<uint8_t>> out;
    for (int k = 0; k < count; k++) {
        try {
            out.push_back(load8(addr + k));
        } catch (const MemoryFault&) {
            out.push_back(std::nullopt);
        }
    }
    return out;
}

const char* syscallName(Syscall s) {
    switch (s) {
    case Syscall::PrintInt: return "print_int";
    case Syscall::PrintString: return "print_string";
    case Syscall::Sbrk: return "sbrk";
    case Syscall::Exit0: return "exit";
    case Syscall::PrintChar: return "print_char";
    case Syscall::PrintHex: return "print_hex";
    case Syscall::PrintUnsigned: return "print_unsigned";
    case Syscall::Exit: return "exit";
    }
    return "?";
}

// MARK: - Shared execution

uint32_t Exec::alu(const Decoded& d, uint32_t a, uint32_t b, uint32_t pc) {
    uint32_t imm = (uint32_t)d.imm;
    int32_t sa = (int32_t)a, sb = (int32_t)b;
    switch (d.op) {
    case Op::LUI: return imm << 12;
    case Op::AUIPC: return pc + (imm << 12);
    case Op::JAL: case Op::JALR: return pc + 4;
    case Op::LB: case Op::LH: case Op::LW: case Op::LBU: case Op::LHU: case Op::SB: case Op::SH: case Op::SW: return a + imm;
    case Op::ADDI: return a + imm;
    case Op::SLTI: return sa < d.imm ? 1 : 0;
    case Op::SLTIU: return a < imm ? 1 : 0;
    case Op::XORI: return a ^ imm;
    case Op::ORI: return a | imm;
    case Op::ANDI: return a & imm;
    case Op::SLLI: return a << (imm & 31);
    case Op::SRLI: return a >> (imm & 31);
    case Op::SRAI: return (uint32_t)(sa >> (imm & 31));
    case Op::ADD: return a + b;
    case Op::SUB: return a - b;
    case Op::SLL: return a << (b & 31);
    case Op::SLT: return sa < sb ? 1 : 0;
    case Op::SLTU: return a < b ? 1 : 0;
    case Op::XOR: return a ^ b;
    case Op::SRL: return a >> (b & 31);
    case Op::SRA: return (uint32_t)(sa >> (b & 31));
    case Op::OR: return a | b;
    case Op::AND: return a & b;
    case Op::MUL: return a * b;
    case Op::MULH: return (uint32_t)(((int64_t)sa * (int64_t)sb) >> 32);
    case Op::MULHSU: return (uint32_t)(((int64_t)sa * (int64_t)(uint64_t)b) >> 32);
    case Op::MULHU: return (uint32_t)(((uint64_t)a * (uint64_t)b) >> 32);
    case Op::DIV: return (uint32_t)RV::div(sa, sb);
    case Op::DIVU: return RV::divu(a, b);
    case Op::REM: return (uint32_t)RV::rem(sa, sb);
    case Op::REMU: return RV::remu(a, b);
    default: return 0;
    }
}

bool Exec::taken(const Decoded& d, uint32_t a, uint32_t b) {
    switch (d.op) {
    case Op::BEQ: return a == b;
    case Op::BNE: return a != b;
    case Op::BLT: return (int32_t)a < (int32_t)b;
    case Op::BGE: return (int32_t)a >= (int32_t)b;
    case Op::BLTU: return a < b;
    case Op::BGEU: return a >= b;
    case Op::JAL: case Op::JALR: return true;
    default: return false;
    }
}

uint32_t Exec::memory(const Decoded& d, uint32_t addr, uint32_t storeValue, Memory& mem) {
    switch (d.op) {
    case Op::LB: return (uint32_t)(int32_t)(int8_t)mem.load8(addr);
    case Op::LBU: return mem.load8(addr);
    case Op::LH: return (uint32_t)(int32_t)(int16_t)mem.load16(addr);
    case Op::LHU: return mem.load16(addr);
    case Op::LW: return mem.load32(addr);
    case Op::SB: mem.store8(addr, (uint8_t)storeValue); return 0;
    case Op::SH: mem.store16(addr, (uint16_t)storeValue); return 0;
    case Op::SW: mem.store32(addr, storeValue); return 0;
    default: return 0;
    }
}

std::optional<uint32_t> SyscallHandler::handle(uint32_t a0, uint32_t a7, Memory& mem, uint32_t pc) {
    Syscall call;
    switch (a7) {
    case 1: case 4: case 9: case 10: case 11: case 34: case 36: case 93: call = (Syscall)a7; break;
    default: throw MemoryFault{pc, cat("ecall with unknown service number a7 = ", a7)};
    }
    switch (call) {
    case Syscall::PrintInt: output.append(std::to_string((int32_t)a0)); break;
    case Syscall::PrintUnsigned: output.append(std::to_string(a0)); break;
    case Syscall::PrintHex: output.append(Format::hex(a0)); break;
    case Syscall::PrintChar: output.append(std::vector<uint8_t>{(uint8_t)a0}); break;
    case Syscall::PrintString: output.append(mem.cString(a0, 100000)); break;
    case Syscall::Sbrk: {
        uint32_t old = brk;
        if (brk + a0 > MemoryMap::heapLimit || brk + a0 < brk) return 0u;
        brk += a0;
        return old;
    }
    case Syscall::Exit0:
        output.exitCode = 0;
        halted = true;
        break;
    case Syscall::Exit:
        output.exitCode = (int32_t)a0;
        halted = true;
        break;
    }
    if (log.size() < 10000) log.push_back({pc, syscallName(call)});
    return std::nullopt;
}

// MARK: - ISA simulator

static std::vector<std::optional<Decoded>> predecode(const std::vector<uint8_t>& text) {
    std::vector<std::optional<Decoded>> out;
    for (size_t k = 0; k + 3 < text.size(); k += 4) {
        uint32_t w = (uint32_t)text[k] | (uint32_t)text[k + 1] << 8 | (uint32_t)text[k + 2] << 16 | (uint32_t)text[k + 3] << 24;
        out.push_back(Decoder::decode(w));
    }
    return out;
}

ISASimulator::ISASimulator(ExecutableP e) : exe(std::move(e)), mem(exe->text, exe->data), pc_(exe->entry) {
    regs[Reg::sp] = MemoryMap::stackTop;
    regs[Reg::gp] = 0x10008000;
    decoded = predecode(exe->text);
}

bool ISASimulator::step() {
    if (halted()) return false;
    uint32_t off = pc_ - exe->textBase;
    if (pc_ < exe->textBase || off / 4 >= (uint32_t)decoded.size() || (pc_ & 3) != 0) {
        fault(cat("jumped to ", Format::hex(pc_), ", which is not code (pc out of the text segment)"));
        return false;
    }
    if (hierarchy) hierarchy->access(AccessKind::Fetch, pc_);
    const auto& od = decoded[off / 4];
    if (!od) {
        fault(cat("illegal instruction at ", Format::hex(pc_)));
        return false;
    }
    const Decoded& d = *od;
    uint32_t a = regs[d.rs1], b = regs[d.rs2];
    uint32_t next = pc_ + 4;
    std::optional<uint32_t> result;
    try {
        switch (d.op) {
        case Op::ECALL:
            if (auto v = sys.handle(regs[Reg::a0], regs[Reg::a7], mem, pc_)) regs[Reg::a0] = *v;
            break;
        case Op::EBREAK:
            fault(cat("ebreak at ", Format::hex(pc_)));
            return false;
        case Op::FENCE: break;
        case Op::BEQ: case Op::BNE: case Op::BLT: case Op::BGE: case Op::BLTU: case Op::BGEU:
            if (Exec::taken(d, a, b)) next = Exec::target(d, a, pc_);
            break;
        case Op::JAL: case Op::JALR:
            result = pc_ + 4;
            next = Exec::target(d, a, pc_);
            break;
        default: {
            uint32_t v = Exec::alu(d, a, b, pc_);
            if (opIsLoad(d.op) || opIsStore(d.op)) {
                if (hierarchy) hierarchy->access(opIsStore(d.op) ? AccessKind::Store : AccessKind::Load, v);
                uint32_t m = Exec::memory(d, v, b, mem);
                if (opIsLoad(d.op)) result = m;
            } else {
                result = v;
            }
        }
        }
    } catch (const MemoryFault& f) {
        auto sym = exe->symbolize(pc_);
        fault(cat(f.message, " — at ", Format::hex(pc_), " (", sym ? *sym : "?", ")"));
        return false;
    }
    if (result && d.rd != 0) regs[d.rd] = *result;
    pc_ = next;
    instret_ += 1;
    return !halted();
}

void ISASimulator::run(int limit) {
    while (instret_ < limit && step()) {}
    if (!halted() && instret_ >= limit) fault(cat("stopped after ", limit, " instructions (infinite loop?)"));
}

// MARK: - Caches

const char* accessKindName(AccessKind k) {
    switch (k) {
    case AccessKind::Fetch: return "fetch";
    case AccessKind::Load: return "load";
    case AccessKind::Store: return "store";
    }
    return "?";
}

const char* replacementName(Replacement r) {
    switch (r) {
    case Replacement::LRU: return "LRU";
    case Replacement::FIFO: return "FIFO";
    case Replacement::Random: return "Random";
    }
    return "?";
}

const char* writePolicyName(WritePolicy w) {
    return w == WritePolicy::WriteBack ? "Write-back, write-allocate" : "Write-through, no write-allocate";
}

const char* missKindName(MissKind k) {
    switch (k) {
    case MissKind::Compulsory: return "compulsory";
    case MissKind::Capacity: return "capacity";
    case MissKind::Conflict: return "conflict";
    }
    return "?";
}

std::string CacheConfig::associativityName() const {
    if (ways == 1) return "direct-mapped";
    if (ways >= blocks()) return "fully associative";
    return cat(ways, "-way set associative");
}

void LRUShadow::unlink(int k) {
    if (prev[k] >= 0) next[prev[k]] = next[k];
    else head = next[k];
    if (next[k] >= 0) prev[next[k]] = prev[k];
    else tail = prev[k];
}

void LRUShadow::pushFront(int k) {
    prev[k] = -1;
    next[k] = head;
    if (head >= 0) prev[head] = k;
    head = k;
    if (tail < 0) tail = k;
}

bool LRUShadow::access(uint32_t block) {
    auto it = index.find(block);
    if (it != index.end()) {
        unlink(it->second);
        pushFront(it->second);
        return true;
    }
    if ((int)keys.size() < capacity) {
        keys.push_back(block);
        prev.push_back(-1);
        next.push_back(-1);
        int k = (int)keys.size() - 1;
        index[block] = k;
        pushFront(k);
    } else {
        int k = tail;
        unlink(k);
        index.erase(keys[k]);
        keys[k] = block;
        index[block] = k;
        pushFront(k);
    }
    return false;
}

Cache::Cache(const CacheConfig& c) : config(c), shadow(c.blocks()) { lines_.assign(c.sets() * c.ways, Line()); }

Cache::Split Cache::split(uint32_t addr) const {
    int off = (int)(addr & (uint32_t)(config.blockSize - 1));
    int set = (int)((addr >> config.offsetBits()) & (uint32_t)(config.sets() - 1));
    uint32_t tag = config.indexBits() + config.offsetBits() >= 32 ? 0 : addr >> (config.offsetBits() + config.indexBits());
    return {tag, set, off};
}

uint32_t Cache::blockAddress(int set, uint32_t tag) const {
    int sh = config.offsetBits() + config.indexBits();
    uint32_t t = sh >= 32 ? 0 : tag << sh;
    return t | ((uint32_t)set << config.offsetBits());
}

CacheEvent Cache::access(uint32_t addr, AccessKind kind, bool allocate, int time) {
    clock += 1;
    Split sp = split(addr);
    uint32_t block = addr >> config.offsetBits();
    int base = sp.set * config.ways;
    stats_.accesses += 1;
    if (kind == AccessKind::Store) stats_.writes += 1;
    else stats_.reads += 1;
    bool shadowHit = shadow.access(block);
    CacheEvent ev;
    ev.address = addr;
    ev.kind = kind;
    ev.set = sp.set;
    ev.tag = sp.tag;
    ev.time = time;
    for (int w = 0; w < config.ways; w++) {
        Line& l = lines_[base + w];
        if (l.valid && l.tag == sp.tag) {
            stats_.hits += 1;
            l.lastUse = clock;
            l.lastTouch = time;
            if (kind == AccessKind::Store && config.write == WritePolicy::WriteBack) l.dirty = true;
            ev.hit = true;
            ev.way = w;
            record(ev);
            return ev;
        }
    }
    stats_.misses += 1;
    MissKind mk = !seen.count(block) ? MissKind::Compulsory : (!shadowHit ? MissKind::Capacity : MissKind::Conflict);
    seen.insert(block);
    switch (mk) {
    case MissKind::Compulsory: stats_.compulsory += 1; break;
    case MissKind::Capacity: stats_.capacity += 1; break;
    case MissKind::Conflict: stats_.conflict += 1; break;
    }
    ev.missKind = mk;
    if (!allocate || (kind == AccessKind::Store && config.write == WritePolicy::WriteThrough)) {
        record(ev);
        return ev;
    }
    // Choose a victim: an empty way first, otherwise by the replacement policy.
    int victim = -1;
    for (int w = 0; w < config.ways; w++)
        if (!lines_[base + w].valid) {
            victim = w;
            break;
        }
    if (victim < 0) {
        switch (config.replacement) {
        case Replacement::LRU:
            victim = 0;
            for (int w = 1; w < config.ways; w++)
                if (lines_[base + w].lastUse < lines_[base + victim].lastUse) victim = w;
            break;
        case Replacement::FIFO:
            victim = 0;
            for (int w = 1; w < config.ways; w++)
                if (lines_[base + w].inserted < lines_[base + victim].inserted) victim = w;
            break;
        case Replacement::Random:
            rng = rng * 1103515245u + 12345u;
            victim = (int)((rng >> 16) % (uint32_t)config.ways);
            break;
        }
        const Line& old = lines_[base + victim];
        ev.evicted = blockAddress(sp.set, old.tag);
        if (old.dirty) {
            ev.writeback = true;
            stats_.writebacks += 1;
        }
    }
    lines_[base + victim] = Line{true, kind == AccessKind::Store && config.write == WritePolicy::WriteBack, sp.tag, clock, clock, time};
    ev.way = victim;
    record(ev);
    return ev;
}

void Cache::record(const CacheEvent& e) {
    last_ = e;
    recent_.push_back(e);
    while (recent_.size() > 64) recent_.pop_front();
}

HierarchyConfig HierarchyConfig::teaching() {
    HierarchyConfig h;
    h.l1i = CacheConfig{"L1 I-cache", 1024, 32, 2, Replacement::LRU, WritePolicy::WriteBack, 1};
    h.l1d = CacheConfig{"L1 D-cache", 512, 16, 2, Replacement::LRU, WritePolicy::WriteBack, 1};
    h.l2 = CacheConfig{"L2 cache", 4096, 32, 4, Replacement::LRU, WritePolicy::WriteBack, 3};
    h.dramLatency = 10;
    h.enabled = true;
    return h;
}

HierarchyConfig HierarchyConfig::perfect() {
    HierarchyConfig t = teaching();
    HierarchyConfig h;
    h.l1i = t.l1i;
    h.l1d = t.l1d;
    h.l2 = std::nullopt;
    h.dramLatency = 0;
    h.enabled = false;
    return h;
}

MemoryHierarchy::MemoryHierarchy(const HierarchyConfig& c) : config(c), l1i(c.l1i), l1d(c.l1d) {
    if (c.l2) l2 = std::make_unique<Cache>(*c.l2);
}

MemoryAccess MemoryHierarchy::access(AccessKind kind, uint32_t addr) {
    if (!config.enabled) return MemoryAccess{kind, addr, 0, "memory", time};
    Cache& l1 = kind == AccessKind::Fetch ? l1i : l1d;
    CacheEvent e = l1.access(addr, kind, true, time);
    int penalty = 0;
    std::string served = "L1";
    bool writeThroughStore = kind == AccessKind::Store && l1.config.write == WritePolicy::WriteThrough;
    if (writeThroughStore) {
        // Every store also goes to the next level, through a write buffer (no stall).
        if (l2) l2->access(addr, AccessKind::Store, true, time);
        else dramAccesses += 1;
    }
    if (!e.hit && !writeThroughStore) {
        if (l2) {
            CacheEvent e2 = l2->access(addr, kind == AccessKind::Store ? AccessKind::Load : kind, true, time);
            penalty += l2->config.latency;
            served = "L2";
            if (!e2.hit) {
                penalty += config.dramLatency;
                dramAccesses += 1;
                served = "DRAM";
            }
            if (e.writeback && e.evicted) l2->access(*e.evicted, AccessKind::Store, true, time);
        } else {
            penalty += config.dramLatency;
            dramAccesses += 1;
            served = "DRAM";
        }
    }
    stallCycles[kind] += penalty;
    MemoryAccess a{kind, addr, penalty, served, time};
    recent.push_back(a);
    while (recent.size() > 200) recent.pop_front();
    return a;
}

double MemoryHierarchy::amat(const Cache& c) const {
    double l2Part = l2 ? l2->config.latency + l2->stats().missRate() * config.dramLatency : (double)config.dramLatency;
    return 1 + c.stats().missRate() * l2Part;
}

// MARK: - Pipeline

const std::vector<BranchStage>& allBranchStages() {
    static const std::vector<BranchStage> all = {BranchStage::ID, BranchStage::EX, BranchStage::MEM};
    return all;
}

const char* branchStageName(BranchStage s) { return s == BranchStage::ID ? "ID" : s == BranchStage::EX ? "EX" : "MEM"; }

const std::vector<PredictorKind>& allPredictors() {
    static const std::vector<PredictorKind> all = {PredictorKind::NotTaken, PredictorKind::OneBit, PredictorKind::TwoBit};
    return all;
}

const char* predictorName(PredictorKind p) {
    switch (p) {
    case PredictorKind::NotTaken: return "Static: predict not taken";
    case PredictorKind::OneBit: return "Dynamic: 1-bit history";
    case PredictorKind::TwoBit: return "Dynamic: 2-bit saturating counter";
    }
    return "?";
}

const char* PipeStage::name(int k) {
    static const char* n[] = {"IF", "ID", "EX", "MEM", "WB"};
    return n[k];
}

const char* PipeStage::longName(int k) {
    static const char* n[] = {"Instruction fetch", "Instruction decode & register read", "Execute (ALU)", "Memory access", "Write back"};
    return n[k];
}

const char* stallReasonName(StallReason r) {
    switch (r) {
    case StallReason::LoadUse: return "load-use hazard";
    case StallReason::DataHazard: return "data hazard (no forwarding)";
    case StallReason::BranchOperand: return "branch operand not ready";
    case StallReason::Ecall: return "system call waits for the pipeline to drain";
    case StallReason::DCache: return "D-cache miss";
    case StallReason::ICache: return "I-cache miss";
    }
    return "?";
}

const char* forwardSourceName(ForwardSource f) {
    switch (f) {
    case ForwardSource::None: return "none";
    case ForwardSource::ExMem: return "EX/MEM";
    case ForwardSource::MemWb: return "MEM/WB";
    }
    return "?";
}

Pipeline::Pipeline(ExecutableP e, const PipelineConfig& c)
    : exe(std::move(e)), config(c), mem(exe->text, exe->data), hierarchy(c.hierarchy), pc_(exe->entry) {
    regs[Reg::sp] = MemoryMap::stackTop;
    regs[Reg::gp] = 0x10008000;
    decoded = predecode(exe->text);
    counters.assign(config.btbEntries, 1);
    btbTag.assign(config.btbEntries, 0xFFFFFFFFu);
    btbTarget.assign(config.btbEntries, 0);
}

std::pair<bool, uint32_t> Pipeline::predict(uint32_t pc) const {
    if (config.predictor == PredictorKind::NotTaken) return {false, pc + 4};
    int k = btbIndex(pc);
    if (btbTag[k] != pc) return {false, pc + 4};
    bool t = config.predictor == PredictorKind::OneBit ? counters[k] >= 1 : counters[k] >= 2;
    return t ? std::pair<bool, uint32_t>{true, btbTarget[k]} : std::pair<bool, uint32_t>{false, pc + 4};
}

void Pipeline::train(uint32_t pc, bool taken, uint32_t target) {
    if (config.predictor == PredictorKind::NotTaken) return;
    int k = btbIndex(pc);
    if (btbTag[k] != pc) {
        if (!taken) return;
        btbTag[k] = pc;
        counters[k] = config.predictor == PredictorKind::OneBit ? 1 : 2;
        btbTarget[k] = target;
        return;
    }
    btbTarget[k] = target;
    if (config.predictor == PredictorKind::OneBit) {
        counters[k] = taken ? 1 : 0;
    } else if (taken) {
        counters[k] = std::min(3, counters[k] + 1);
    } else {
        counters[k] = counters[k] == 0 ? 0 : counters[k] - 1;
    }
}

Pipeline::PredictorState Pipeline::predictorState(uint32_t pc) const {
    int k = btbIndex(pc);
    return {btbTag[k] == pc, (int)counters[k], btbTarget[k]};
}

bool Pipeline::writes(const std::optional<InFlight>& x, int r) {
    if (!x || x->fault) return false;
    auto d = x->dest();
    return d && *d == r;
}

StageSlot Pipeline::slot(const std::optional<InFlight>& x, StageSlot::Status status) {
    if (!x) return StageSlot::bubble();
    return StageSlot{x->seq, x->pc, status};
}

std::optional<uint32_t> Pipeline::resolve(InFlight& x) {
    if (!x.d || !(opIsBranch(x.d->op) || opIsJump(x.d->op)) || x.resolved) return std::nullopt;
    const Decoded& d = *x.d;
    x.resolved = true;
    x.taken = Exec::taken(d, x.a, x.b);
    x.target = Exec::target(d, x.a, x.pc);
    if (opIsBranch(d.op)) stats_.branches += 1;
    uint32_t actualNext = x.taken ? x.target : x.pc + 4;
    uint32_t predictedNext = x.predictedTaken ? x.predictedTarget : x.pc + 4;
    train(x.pc, x.taken, x.target);
    if (actualNext != predictedNext) {
        if (opIsBranch(d.op)) stats_.mispredictions += 1;
        return actualNext;
    }
    return std::nullopt;
}

bool Pipeline::step() {
    using S = StageSlot::Status;
    if (halted_) return false;
    stats_.cycles += 1;
    hierarchy.time = stats_.cycles;
    CycleRecord rec;
    rec.cycle = stats_.cycles;
    rec.stages[0] = StageSlot::bubble();
    rec.stages[1] = slot(ifid_);
    rec.stages[2] = slot(idex_);
    rec.stages[3] = slot(exmem_);
    rec.stages[4] = slot(memwb_);
    if (fetchPending) rec.stages[0] = StageSlot{fetchPending->seq, fetchPending->pc, S::Normal};

    // ---- WB: write the result into the register file (first half of the cycle).
    if (memwb_) {
        const InFlight& w = *memwb_;
        rec.retired = w.seq;
        if (w.fault) {
            sys.output.fault = *w.fault;
            halted_ = true;
            finish(rec);
            return false;
        }
        if (auto r = w.dest()) {
            regs[*r] = w.result;
            rec.registerWrite = std::make_pair(*r, w.result);
        }
        stats_.retired += 1;
        if (w.exits) {
            halted_ = true;
            finish(rec);
            return false;
        }
    }

    std::optional<uint32_t> redirect;
    int flushStages = 0;
    std::string reason;

    // ---- MEM: loads and stores go to the data cache.
    std::optional<InFlight> toWB;
    if (exmem_) {
        InFlight m = *exmem_;
        if (!m.memDone) {
            m.memDone = true;
            if (m.d && !m.fault && (opIsLoad(m.d->op) || opIsStore(m.d->op))) {
                const Decoded& d = *m.d;
                MemoryAccess acc = hierarchy.access(opIsStore(d.op) ? AccessKind::Store : AccessKind::Load, m.alu);
                rec.data = acc;
                m.memWait = acc.penalty;
                try {
                    uint32_t v = Exec::memory(d, m.alu, m.b, mem);
                    m.result = opIsLoad(d.op) ? v : m.alu;
                } catch (const MemoryFault& f) {
                    auto sym = exe->symbolize(m.pc);
                    m.fault = cat(f.message, " — at ", Format::hex(m.pc), " (", sym ? *sym : "?", ")");
                }
            } else {
                m.result = m.alu;
            }
            if (config.branchStage == BranchStage::MEM && !m.fault) {
                if (auto target = resolve(m)) {
                    redirect = target;
                    flushStages = 3;
                    reason = "branch resolved in MEM was mispredicted";
                }
            }
        }
        if (m.memWait > 0) {
            // Data-cache miss: the whole pipeline behind MEM freezes; WB gets a bubble.
            m.memWait -= 1;
            exmem_ = m;
            // The instruction frozen in EX must not lose a value MEM/WB was about to forward.
            if (config.forwarding && idex_ && idex_->d && memwb_ && memwb_->dest() && !memwb_->fault) {
                InFlight e = *idex_;
                const Decoded& d = *e.d;
                int r = *memwb_->dest();
                int r1 = d.op == Op::ECALL ? Reg::a0 : d.rs1, r2 = d.op == Op::ECALL ? Reg::a7 : d.rs2;
                if (r1 == r) e.a = memwb_->result;
                if (r2 == r) e.b = memwb_->result;
                idex_ = e;
            }
            memwb_.reset();
            rec.stall = StallReason::DCache;
            rec.stages[PipeStage::MEM].status = S::Waiting;
            for (int k = 0; k <= 2; k++)
                if (rec.stages[k].seq >= 0) rec.stages[k].status = S::Stalled;
            stats_.stalls[StallReason::DCache] += 1;
            finish(rec);
            return true;
        }
        toWB = m;
    }

    // ---- EX: the ALU, with operands forwarded from later stages.
    std::optional<InFlight> toMEM;
    if (idex_) {
        InFlight e = *idex_;
        if (e.d && !e.fault && (!redirect || flushStages < 3)) {
            const Decoded& d = *e.d;
            int r1 = d.op == Op::ECALL ? Reg::a0 : d.rs1;
            int r2 = d.op == Op::ECALL ? Reg::a7 : d.rs2;
            if (config.forwarding) {
                // toWB is what EX/MEM held at the start of the cycle; memwb is the old MEM/WB.
                auto forward = [&](int r, uint32_t v) -> std::pair<uint32_t, ForwardSource> {
                    if (r == 0) return {0, ForwardSource::None};
                    if (toWB && writes(toWB, r) && !toWB->isLoad()) return {toWB->alu, ForwardSource::ExMem};
                    if (memwb_ && writes(memwb_, r)) return {memwb_->result, ForwardSource::MemWb};
                    return {v, ForwardSource::None};
                };
                bool uses1 = opReadsRs1(d.op) || d.op == Op::ECALL, uses2 = opReadsRs2(d.op) || d.op == Op::ECALL;
                auto [a, fa] = uses1 ? forward(r1, e.a) : std::pair<uint32_t, ForwardSource>{e.a, ForwardSource::None};
                auto [b, fb] = uses2 ? forward(r2, e.b) : std::pair<uint32_t, ForwardSource>{e.b, ForwardSource::None};
                e.a = a;
                e.b = b;
                rec.forwardA = fa;
                rec.forwardB = fb;
                stats_.forwards += (fa != ForwardSource::None ? 1 : 0) + (fb != ForwardSource::None ? 1 : 0);
            }
            if (d.op == Op::ECALL) {
                try {
                    auto newA0 = sys.handle(e.a, e.b, mem, e.pc);
                    e.alu = newA0 ? *newA0 : e.a;
                    if (sys.halted) {
                        e.exits = true;
                        draining = true;
                    }
                } catch (const MemoryFault& f) {
                    e.fault = f.message;
                }
            } else if (d.op == Op::EBREAK) {
                e.fault = cat("ebreak at ", Format::hex(e.pc));
            } else {
                e.alu = Exec::alu(d, e.a, e.b, e.pc);
            }
            if (config.branchStage == BranchStage::EX && !e.fault) {
                if (auto target = resolve(e)) {
                    redirect = target;
                    flushStages = 2;
                    reason = "branch resolved in EX was mispredicted";
                }
            }
        }
        toMEM = e;
    }

    // ---- ID: decode, read registers, detect hazards.
    bool stallID = false;
    std::optional<InFlight> toEX;
    if (ifid_ && !redirect) {
        InFlight i = *ifid_;
        std::vector<int> srcs = i.sources();
        std::optional<Op> op = i.d ? std::optional<Op>(i.d->op) : std::nullopt;
        bool comparesHere = config.branchStage == BranchStage::ID && op && (opIsBranch(*op) || *op == Op::JALR);
        auto contains = [&](int r) { return std::find(srcs.begin(), srcs.end(), r) != srcs.end(); };
        if (!i.fault) {
            if (config.forwarding) {
                if (idex_ && idex_->isLoad() && idex_->dest() && contains(*idex_->dest())) {
                    stallID = true;
                    rec.stall = StallReason::LoadUse;
                }
            } else if (std::any_of(srcs.begin(), srcs.end(), [&](int r) { return writes(idex_, r) || writes(exmem_, r); })) {
                stallID = true;
                rec.stall = StallReason::DataHazard;
            }
            if (!stallID && comparesHere) {
                // The comparator sits in ID, so its operands must already exist.
                bool waitingALU = std::any_of(srcs.begin(), srcs.end(), [&](int r) { return writes(idex_, r); });
                bool waitingLoad = std::any_of(srcs.begin(), srcs.end(), [&](int r) { return exmem_ && exmem_->isLoad() && writes(exmem_, r); });
                if (waitingALU || waitingLoad) {
                    stallID = true;
                    rec.stall = StallReason::BranchOperand;
                }
            }
            if (!stallID && op && *op == Op::ECALL && (idex_ || exmem_)) {
                stallID = true;
                rec.stall = StallReason::Ecall;
            }
        }
        if (stallID) {
            stats_.stalls[*rec.stall] += 1;
            rec.stages[PipeStage::ID].status = S::Stalled;
        } else {
            if (i.d) {
                const Decoded& d = *i.d;
                int r1 = d.op == Op::ECALL ? Reg::a0 : d.rs1;
                int r2 = d.op == Op::ECALL ? Reg::a7 : d.rs2;
                i.a = regs[r1];
                i.b = regs[r2];
                if (comparesHere) {
                    // Forward an ALU result sitting in EX/MEM straight into the comparator.
                    if (config.forwarding && exmem_ && !exmem_->isLoad()) {
                        if (d.rs1 != 0 && writes(exmem_, d.rs1)) {
                            i.a = exmem_->alu;
                            rec.forwardID = true;
                        }
                        if (d.rs2 != 0 && writes(exmem_, d.rs2)) {
                            i.b = exmem_->alu;
                            rec.forwardID = true;
                        }
                    }
                    if (auto target = resolve(i)) {
                        redirect = target;
                        flushStages = 1;
                        reason = "branch resolved in ID was mispredicted";
                    }
                } else if (d.op == Op::JAL) {
                    if (auto target = resolve(i)) {
                        // jal's target (pc + offset) is known as soon as it is decoded.
                        redirect = target;
                        flushStages = 1;
                        reason = "jump: target known in ID";
                    }
                }
            }
            toEX = i;
        }
    }

    // ---- IF: fetch from the I-cache. A miss keeps IF busy for several cycles.
    std::optional<InFlight> toID;
    if (!draining) {
        if (!fetchPending) {
            uint32_t fpc = pc_;
            std::optional<MemoryAccess> acc;
            if (fpc >= exe->textBase && fpc < exe->textEnd() && (fpc & 3) == 0) {
                acc = hierarchy.access(AccessKind::Fetch, fpc);
                rec.fetch = acc;
            }
            fetchPending = Fetch{nextSeq, fpc, acc ? acc->penalty : 0};
            nextSeq += 1;
            if (recordHistory) seqPC_[nextSeq - 1] = fpc;
        }
        Fetch f = *fetchPending;
        rec.stages[0] = StageSlot{f.seq, f.pc, S::Normal};
        if (f.wait > 0) {
            f.wait -= 1;
            fetchPending = f;
            rec.stages[0].status = S::Waiting;
            if (!rec.stall) rec.stall = StallReason::ICache;
            stats_.stalls[StallReason::ICache] += 1;
        } else if (stallID) {
            rec.stages[0].status = S::Stalled;
        } else {
            fetchPending.reset();
            InFlight x;
            x.seq = f.seq;
            x.pc = f.pc;
            uint32_t off = f.pc - exe->textBase;
            if (f.pc >= exe->textBase && (f.pc & 3) == 0 && off / 4 < (uint32_t)decoded.size()) {
                x.d = decoded[off / 4];
                if (!x.d) x.fault = cat("illegal instruction at ", Format::hex(f.pc));
            } else {
                x.fault = cat("jumped to ", Format::hex(f.pc), ", which is not code (pc out of the text segment)");
            }
            auto [t, target] = !x.fault ? predict(f.pc) : std::pair<bool, uint32_t>{false, f.pc + 4};
            x.predictedTaken = t;
            x.predictedTarget = target;
            pc_ = t ? target : f.pc + 4;
            toID = x;
        }
    }

    // ---- Clock edge: every pipeline register updates at once.
    memwb_ = toWB;
    exmem_ = toMEM;
    if (stallID) {
        idex_.reset();   // a bubble (nop) goes down the pipe
    } else {
        idex_ = toEX;
        ifid_ = toID;
    }
    if (draining) {
        // An exit ecall is on its way out: cancel everything younger.
        if ((exmem_ && exmem_->exits) || (memwb_ && memwb_->exits)) {
            if (idex_ && !idex_->exits) idex_.reset();
            ifid_.reset();
            fetchPending.reset();
            for (int k = 0; k <= 1; k++)
                if (rec.stages[k].seq >= 0) rec.stages[k].status = S::Flushed;
        }
    }
    if (redirect) applyRedirect(*redirect, flushStages, rec, reason);
    finish(rec);
    return true;
}

void Pipeline::applyRedirect(uint32_t target, int stages, CycleRecord& rec, const std::string& reason) {
    // The stages in front of the one that resolved the branch hold wrong-path instructions.
    int n = 0;
    for (int k = 0; k < stages; k++)
        if (rec.stages[k].seq >= 0) {
            rec.stages[k].status = StageSlot::Status::Flushed;
            n += 1;
        }
    ifid_.reset();
    if (stages >= 2) idex_.reset();
    if (stages >= 3) exmem_.reset();
    fetchPending.reset();
    pc_ = target;
    rec.redirect = target;
    rec.flushed = n;
    rec.flushReason = reason;
    if (n > 0) {
        stats_.flushes += 1;
        stats_.flushedInstructions += n;
    }
}

void Pipeline::finish(const CycleRecord& rec) {
    if (!recordHistory) return;
    history_.push_back(rec);
    if ((int)history_.size() > historyLimit + 512) {
        while ((int)history_.size() > historyLimit) history_.pop_front();
        int minSeq = INT32_MAX;
        for (auto& s : history_.front().stages)
            if (s.seq >= 0) minSeq = std::min(minSeq, s.seq);
        if (minSeq == INT32_MAX) minSeq = 0;
        for (auto it = seqPC_.begin(); it != seqPC_.end();) {
            if (it->first < minSeq - 8) it = seqPC_.erase(it);
            else ++it;
        }
    }
}

void Pipeline::run(int maxCycles) {
    while (stats_.cycles < maxCycles && step()) {}
    if (!halted_ && stats_.cycles >= maxCycles) {
        sys.output.fault = cat("stopped after ", maxCycles, " cycles (infinite loop?)");
        halted_ = true;
    }
}

}  // namespace sp
