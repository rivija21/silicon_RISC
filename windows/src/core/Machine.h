// The simulated machine: memory, the single-cycle ISA simulator, caches,
// and the five-stage pipeline.
#pragma once

#include <deque>

#include "Backend.h"

namespace sp {

// MARK: - Memory

struct MemoryFault {
    uint32_t address = 0;
    std::string message;
};

/// Byte-addressed little-endian memory with three regions:
/// text (code), data + heap, and the stack. Anything else faults,
/// which is how a wild pointer shows up as a "segmentation fault".
class Memory {
public:
    explicit Memory(const std::vector<uint8_t>& text = {}, const std::vector<uint8_t>& data = {});

    const uint32_t textBase;
    const uint32_t dataBase;
    const uint32_t stackBase;
    const std::vector<uint8_t>& text() const { return text_; }
    /// Highest address written in the data/heap region, for the memory view.
    uint32_t dataHigh() const { return dataHigh_; }
    /// Lowest stack address touched (the stack grows down).
    uint32_t stackLow() const { return stackLow_; }

    std::string describe(uint32_t addr) const;
    bool isMapped(uint32_t addr) const;

    uint8_t load8(uint32_t addr) const {
        int r;
        size_t o = locate(addr, 1, false, r);
        return r == 1 ? data_[o] : r == 2 ? stack_[o] : text_[o];
    }
    uint16_t load16(uint32_t addr) const;
    uint32_t load32(uint32_t addr) const;
    void store8(uint32_t addr, uint8_t v) {
        int r;
        size_t o = locate(addr, 1, true, r);
        if (r == 1) data_[o] = v;
        else stack_[o] = v;
        touched(r, addr);
    }
    void store16(uint32_t addr, uint16_t v);
    void store32(uint32_t addr, uint32_t v);
    /// Reads a zero-terminated string (for print_str), at most `limit` bytes.
    std::vector<uint8_t> cString(uint32_t addr, int limit = 4096) const;
    /// A snapshot of a small window of memory (nullopt = unmapped).
    std::vector<std::optional<uint8_t>> bytes(uint32_t addr, int count) const;

private:
    std::vector<uint8_t> text_;
    std::vector<uint8_t> data_;
    std::vector<uint8_t> stack_;
    uint32_t dataHigh_ = 0;
    uint32_t stackLow_ = 0;

    /// Region (0 text, 1 data, 2 stack) and offset, or throws MemoryFault.
    size_t locate(uint32_t addr, uint32_t size, bool write, int& region) const {
        if (addr >= dataBase && addr - dataBase < (uint32_t)data_.size() - size + 1) {
            region = 1;
            return addr - dataBase;
        }
        if (addr >= stackBase && addr - stackBase <= (uint32_t)stack_.size() - size) {
            region = 2;
            return addr - stackBase;
        }
        return locateSlow(addr, size, write, region);
    }
    size_t locateSlow(uint32_t addr, uint32_t size, bool write, int& region) const;
    void touched(int r, uint32_t addr) {
        if (r == 1) {
            if (addr >= dataHigh_) dataHigh_ = addr + 4;
        } else if (addr < stackLow_) {
            stackLow_ = addr;
        }
    }
};

/// What the simulated program printed, and how it ended.
struct ProgramOutput {
    /// Raw bytes as printed (UTF-8 for display).
    std::string text;
    std::optional<int32_t> exitCode;
    std::optional<std::string> fault;

    /// Output is kept to a sensible size so a runaway print loop cannot eat the machine.
    static constexpr size_t limit = 200000;
    void append(const std::vector<uint8_t>& bytes) {
        if (text.size() >= limit) return;
        text.append(bytes.begin(), bytes.end());
    }
    void append(const std::string& s) {
        if (text.size() >= limit) return;
        text += s;
    }
    bool operator==(const ProgramOutput&) const = default;
};

/// The system calls our programs use, numbered as in the RARS simulator.
enum class Syscall : uint32_t { PrintInt = 1, PrintString = 4, Sbrk = 9, Exit0 = 10, PrintChar = 11, PrintHex = 34, PrintUnsigned = 36, Exit = 93 };
const char* syscallName(Syscall s);

/// The parts of an instruction's behaviour that both CPU models share.
namespace Exec {
/// The ALU result: arithmetic, the address for loads/stores, or the link value pc+4 for jumps.
uint32_t alu(const Decoded& d, uint32_t a, uint32_t b, uint32_t pc);
bool taken(const Decoded& d, uint32_t a, uint32_t b);
/// Where control goes if the branch/jump is taken.
inline uint32_t target(const Decoded& d, uint32_t a, uint32_t pc) {
    if (d.op == Op::JALR) return (a + (uint32_t)d.imm) & ~1u;
    return pc + (uint32_t)d.imm;
}
/// Performs a load or store. Returns the loaded value for loads.
uint32_t memory(const Decoded& d, uint32_t addr, uint32_t storeValue, Memory& mem);
}  // namespace Exec

/// What the "operating system" does on ecall. Shared by both CPU models.
class SyscallHandler {
public:
    ProgramOutput output;
    uint32_t brk = MemoryMap::heapBase;
    bool halted = false;
    std::vector<std::pair<uint32_t, std::string>> log;

    /// Returns the new value of a0, or nullopt if a0 is unchanged. Throws MemoryFault.
    std::optional<uint32_t> handle(uint32_t a0, uint32_t a7, Memory& mem, uint32_t pc);
};

// MARK: - Caches

enum class AccessKind : uint8_t { Fetch, Load, Store };
const char* accessKindName(AccessKind k);

enum class Replacement : uint8_t { LRU, FIFO, Random };
const char* replacementName(Replacement r);
enum class WritePolicy : uint8_t { WriteBack, WriteThrough };
const char* writePolicyName(WritePolicy w);

struct CacheConfig {
    std::string name;
    int size = 0;
    int blockSize = 4;
    /// Number of ways. 1 = direct-mapped; size / blockSize = fully associative.
    int ways = 1;
    Replacement replacement = Replacement::LRU;
    WritePolicy write = WritePolicy::WriteBack;
    /// Extra cycles to get data from this level when the level above misses.
    int latency = 1;

    int blocks() const { return size / blockSize; }
    int sets() const { return std::max(1, blocks() / std::max(1, ways)); }
    int offsetBits() const { return trailingZeros((uint64_t)blockSize); }
    int indexBits() const { return trailingZeros((uint64_t)sets()); }
    int tagBits() const { return 32 - offsetBits() - indexBits(); }
    std::string associativityName() const;
    bool operator==(const CacheConfig&) const = default;
};

enum class MissKind : uint8_t { Compulsory, Capacity, Conflict };
const char* missKindName(MissKind k);

struct CacheEvent {
    uint32_t address = 0;
    AccessKind kind = AccessKind::Load;
    bool hit = false;
    int set = 0;
    int way = -1;
    uint32_t tag = 0;
    std::optional<MissKind> missKind;
    /// Address of the block that was thrown out, if any, and whether it had to be written back.
    std::optional<uint32_t> evicted;
    bool writeback = false;
    int time = 0;
};

struct CacheStats {
    int accesses = 0, hits = 0, misses = 0;
    int reads = 0, writes = 0;
    int compulsory = 0, capacity = 0, conflict = 0;
    int writebacks = 0;
    double hitRate() const { return accesses == 0 ? 0 : (double)hits / accesses; }
    double missRate() const { return accesses == 0 ? 0 : (double)misses / accesses; }
};

/// A fully associative LRU cache that only tracks which blocks are present.
/// It is the yardstick for the "3 Cs".
class LRUShadow {
public:
    explicit LRUShadow(int capacity) : capacity(std::max(1, capacity)) {}
    /// Returns true on a hit. Either way the block becomes most recently used.
    bool access(uint32_t block);

private:
    int capacity;
    std::unordered_map<uint32_t, int> index;
    std::vector<uint32_t> keys;
    std::vector<int> prev, next;
    int head = -1, tail = -1;
    void unlink(int k);
    void pushFront(int k);
};

class Cache {
public:
    struct Line {
        bool valid = false;
        bool dirty = false;
        uint32_t tag = 0;
        int lastUse = 0;
        int inserted = 0;
        /// When it was last touched, for the "recently used" glow in the view.
        int lastTouch = -1;
    };

    explicit Cache(const CacheConfig& c);
    const CacheConfig config;
    const std::vector<Line>& lines() const { return lines_; }
    const CacheStats& stats() const { return stats_; }
    const std::deque<CacheEvent>& recent() const { return recent_; }
    const std::optional<CacheEvent>& last() const { return last_; }

    struct Split {
        uint32_t tag;
        int set;
        int offset;
    };
    Split split(uint32_t addr) const;
    const Line& line(int set, int way) const { return lines_[set * config.ways + way]; }
    /// Address of the first byte of the block held in a line.
    uint32_t blockAddress(int set, uint32_t tag) const;
    /// Looks up an address. `allocate` = bring the block in on a miss.
    CacheEvent access(uint32_t addr, AccessKind kind, bool allocate = true, int time = 0);

private:
    std::vector<Line> lines_;
    CacheStats stats_;
    std::deque<CacheEvent> recent_;
    std::optional<CacheEvent> last_;
    std::unordered_set<uint32_t> seen;
    LRUShadow shadow;
    int clock = 0;
    uint32_t rng = 12345;
    void record(const CacheEvent& e);
};

struct HierarchyConfig {
    CacheConfig l1i;
    CacheConfig l1d;
    std::optional<CacheConfig> l2;
    int dramLatency = 10;
    bool enabled = true;

    /// Small on purpose, so that cache behaviour shows up with small programs.
    static HierarchyConfig teaching();
    /// Ideal memory: every access takes one cycle.
    static HierarchyConfig perfect();
    bool operator==(const HierarchyConfig&) const = default;
};

struct MemoryAccess {
    AccessKind kind = AccessKind::Load;
    uint32_t address = 0;
    /// Extra stall cycles beyond a 1-cycle L1 hit.
    int penalty = 0;
    /// "L1", "L2", "DRAM" or "memory".
    std::string servedBy;
    int time = 0;
};

/// L1 instruction and data caches, an optional unified L2, then main memory (DRAM).
class MemoryHierarchy {
public:
    explicit MemoryHierarchy(const HierarchyConfig& c);
    const HierarchyConfig config;
    Cache l1i;
    Cache l1d;
    std::unique_ptr<Cache> l2;
    int dramAccesses = 0;
    std::map<AccessKind, int> stallCycles;
    std::deque<MemoryAccess> recent;
    int time = 0;

    /// Simulates one access and returns how many cycles the CPU must stall.
    MemoryAccess access(AccessKind kind, uint32_t addr);
    /// Average memory access time for data, in cycles.
    double amat(const Cache& c) const;
};

// MARK: - The single-cycle model

/// The instruction-set simulator: one instruction per step, no timing.
class ISASimulator {
public:
    explicit ISASimulator(ExecutableP exe);
    ExecutableP exe;
    Memory mem;
    SyscallHandler sys;
    MemoryHierarchy* hierarchy = nullptr;

    const ProgramOutput& output() const { return sys.output; }
    bool halted() const { return sys.halted || sys.output.fault.has_value(); }
    int instret() const { return instret_; }
    uint32_t pc() const { return pc_; }
    uint32_t reg(int r) const { return regs[r]; }

    /// Executes one instruction. Returns false once the program has stopped.
    bool step();
    /// Runs until the program exits, faults, or `limit` instructions have run.
    void run(int limit = 50000000);

private:
    uint32_t regs[32] = {};
    uint32_t pc_;
    int instret_ = 0;
    std::vector<std::optional<Decoded>> decoded;
    void fault(const std::string& m) { sys.output.fault = m; }
};

// MARK: - The pipeline

enum class BranchStage : uint8_t { ID, EX, MEM };
const std::vector<BranchStage>& allBranchStages();
const char* branchStageName(BranchStage s);
inline int branchPenalty(BranchStage s) { return s == BranchStage::ID ? 1 : s == BranchStage::EX ? 2 : 3; }

enum class PredictorKind : uint8_t { NotTaken, OneBit, TwoBit };
const std::vector<PredictorKind>& allPredictors();
const char* predictorName(PredictorKind p);

struct PipelineConfig {
    bool forwarding = true;
    BranchStage branchStage = BranchStage::EX;
    PredictorKind predictor = PredictorKind::NotTaken;
    HierarchyConfig hierarchy = HierarchyConfig::teaching();
    int btbEntries = 64;
    bool operator==(const PipelineConfig&) const = default;
};

namespace PipeStage {
enum { IF = 0, ID, EX, MEM, WB };
const char* name(int k);
const char* longName(int k);
}  // namespace PipeStage

enum class StallReason : uint8_t { LoadUse, DataHazard, BranchOperand, Ecall, DCache, ICache };
const char* stallReasonName(StallReason r);

enum class ForwardSource : uint8_t { None, ExMem, MemWb };
const char* forwardSourceName(ForwardSource f);

/// What one stage held during one cycle.
struct StageSlot {
    enum class Status : uint8_t { Normal, Bubble, Stalled, Flushed, Waiting };
    int seq = -1;
    uint32_t pc = 0;
    Status status = Status::Bubble;
    static StageSlot bubble() { return StageSlot{-1, 0, Status::Bubble}; }
};

struct CycleRecord {
    int cycle = 0;
    StageSlot stages[5];
    ForwardSource forwardA = ForwardSource::None;
    ForwardSource forwardB = ForwardSource::None;
    /// Forwarding into ID for a branch resolved there.
    bool forwardID = false;
    std::optional<StallReason> stall;
    int flushed = 0;
    std::string flushReason;
    std::optional<int> retired;
    std::optional<MemoryAccess> fetch;
    std::optional<MemoryAccess> data;
    std::optional<uint32_t> redirect;
    std::optional<std::pair<int, uint32_t>> registerWrite;
};

/// One instruction travelling down the pipeline.
struct InFlight {
    int seq = 0;
    uint32_t pc = 0;
    std::optional<Decoded> d;
    std::optional<std::string> fault;
    bool predictedTaken = false;
    uint32_t predictedTarget = 0;
    uint32_t a = 0;
    uint32_t b = 0;
    uint32_t alu = 0;
    uint32_t result = 0;
    bool taken = false;
    uint32_t target = 0;
    bool resolved = false;
    bool memDone = false;
    int memWait = 0;
    bool exits = false;

    std::optional<int> dest() const { return d ? d->dest() : std::nullopt; }
    bool isLoad() const { return d && opIsLoad(d->op); }
    std::vector<int> sources() const { return d ? d->sources() : std::vector<int>{}; }
};

struct PipelineStats {
    int cycles = 0;
    int retired = 0;
    std::map<StallReason, int> stalls;
    int flushes = 0;
    int flushedInstructions = 0;
    int branches = 0;
    int mispredictions = 0;
    int forwards = 0;
    double cpi() const { return retired == 0 ? 0 : (double)cycles / retired; }
};

/// The classic five-stage RISC-V pipeline of Patterson & Hennessy, chapter 4.
///
/// Every call to `step()` is one clock cycle. The stages are evaluated from
/// WB back to IF, each reading the pipeline register in front of it as it was
/// at the start of the cycle — just like flip-flops that all update on the
/// clock edge. The register file is written in the first half of the cycle
/// and read in the second half, so WB → ID needs no forwarding.
class Pipeline {
public:
    Pipeline(ExecutableP exe, const PipelineConfig& config = PipelineConfig());
    ExecutableP exe;
    const PipelineConfig config;
    Memory mem;
    SyscallHandler sys;
    MemoryHierarchy hierarchy;

    const PipelineStats& stats() const { return stats_; }
    const ProgramOutput& output() const { return sys.output; }
    int cycle() const { return stats_.cycles; }
    uint32_t reg(int r) const { return regs[r]; }
    bool halted() const { return halted_; }
    const CycleRecord* lastRecord() const { return history_.empty() ? nullptr : &history_.back(); }
    const std::deque<CycleRecord>& history() const { return history_; }
    std::optional<uint32_t> seqPC(int seq) const {
        auto it = seqPC_.find(seq);
        return it == seqPC_.end() ? std::nullopt : std::optional<uint32_t>(it->second);
    }

    const std::optional<InFlight>& ifid() const { return ifid_; }
    const std::optional<InFlight>& idex() const { return idex_; }
    const std::optional<InFlight>& exmem() const { return exmem_; }
    const std::optional<InFlight>& memwb() const { return memwb_; }
    uint32_t pc() const { return pc_; }
    uint32_t fetchPC() const { return fetchPending ? fetchPending->pc : pc_; }

    struct PredictorState {
        bool inBTB;
        int counter;
        uint32_t target;
    };
    PredictorState predictorState(uint32_t pc) const;

    int historyLimit = 4096;
    bool recordHistory = true;

    /// Advances one clock cycle. Returns false once the program has finished.
    bool step();
    /// Runs until the program stops or `maxCycles` cycles have passed.
    void run(int maxCycles = 100000000);

private:
    uint32_t regs[32] = {};
    PipelineStats stats_;
    std::optional<InFlight> ifid_, idex_, exmem_, memwb_;
    uint32_t pc_;
    struct Fetch {
        int seq;
        uint32_t pc;
        int wait;
    };
    std::optional<Fetch> fetchPending;
    bool draining = false;
    bool halted_ = false;
    int nextSeq = 0;
    std::vector<std::optional<Decoded>> decoded;
    std::vector<uint8_t> counters;
    std::vector<uint32_t> btbTag;
    std::vector<uint32_t> btbTarget;
    std::deque<CycleRecord> history_;
    std::unordered_map<int, uint32_t> seqPC_;

    int btbIndex(uint32_t pc) const { return (int)((pc >> 2) % (uint32_t)config.btbEntries); }
    std::pair<bool, uint32_t> predict(uint32_t pc) const;
    void train(uint32_t pc, bool taken, uint32_t target);
    static bool writes(const std::optional<InFlight>& x, int r);
    static StageSlot slot(const std::optional<InFlight>& x, StageSlot::Status status = StageSlot::Status::Normal);
    std::optional<uint32_t> resolve(InFlight& x);
    void applyRedirect(uint32_t target, int stages, CycleRecord& rec, const std::string& reason);
    void finish(const CycleRecord& rec);
};

}  // namespace sp
