import Foundation

public enum AccessKind: String, Sendable { case fetch, load, store }

public enum Replacement: String, CaseIterable, Sendable {
    case lru = "LRU", fifo = "FIFO", random = "Random"
}

public enum WritePolicy: String, CaseIterable, Sendable {
    case writeBack = "Write-back, write-allocate"
    case writeThrough = "Write-through, no write-allocate"
}

public struct CacheConfig: Equatable, Sendable {
    public var name: String
    public var size: Int
    public var blockSize: Int
    /// Number of ways. 1 = direct-mapped; size / blockSize = fully associative.
    public var ways: Int
    public var replacement: Replacement
    public var write: WritePolicy
    /// Extra cycles to get data from this level when the level above misses.
    public var latency: Int

    public init(name: String, size: Int, blockSize: Int, ways: Int, replacement: Replacement = .lru,
                write: WritePolicy = .writeBack, latency: Int) {
        self.name = name
        self.size = size
        self.blockSize = blockSize
        self.ways = ways
        self.replacement = replacement
        self.write = write
        self.latency = latency
    }

    public var blocks: Int { size / blockSize }
    public var sets: Int { max(1, blocks / max(1, ways)) }
    public var offsetBits: Int { blockSize.trailingZeroBitCount }
    public var indexBits: Int { sets.trailingZeroBitCount }
    public var tagBits: Int { 32 - offsetBits - indexBits }
    public var isValid: Bool {
        size > 0 && blockSize >= 4 && size & (size - 1) == 0 && blockSize & (blockSize - 1) == 0
            && ways >= 1 && blocks % ways == 0 && blockSize <= size && sets & (sets - 1) == 0
    }
    public var associativityName: String {
        if ways == 1 { return "direct-mapped" }
        if ways >= blocks { return "fully associative" }
        return "\(ways)-way set associative"
    }
}

public enum MissKind: String, Sendable { case compulsory, capacity, conflict }

public struct CacheEvent: Sendable {
    public var address: UInt32
    public var kind: AccessKind
    public var hit: Bool
    public var set: Int
    public var way: Int
    public var tag: UInt32
    public var missKind: MissKind?
    /// Address of the block that was thrown out, if any, and whether it had to be written back.
    public var evicted: UInt32?
    public var writeback: Bool
    public var time: Int
}

public struct CacheStats: Sendable {
    public var accesses = 0, hits = 0, misses = 0
    public var reads = 0, writes = 0
    public var compulsory = 0, capacity = 0, conflict = 0
    public var writebacks = 0
    public var hitRate: Double { accesses == 0 ? 0 : Double(hits) / Double(accesses) }
    public var missRate: Double { accesses == 0 ? 0 : Double(misses) / Double(accesses) }
}

/// A fully associative LRU cache that only tracks which blocks are present.
/// It is the yardstick for the "3 Cs": a miss that this ideal cache would
/// also have is a capacity miss; one it would not have is a conflict miss.
final class LRUShadow {
    private let capacity: Int
    private var index: [UInt32: Int] = [:]
    private var keys: [UInt32] = []
    private var prev: [Int] = [], next: [Int] = []
    private var head = -1, tail = -1

    init(capacity: Int) { self.capacity = max(1, capacity) }

    private func unlink(_ k: Int) {
        if prev[k] >= 0 { next[prev[k]] = next[k] } else { head = next[k] }
        if next[k] >= 0 { prev[next[k]] = prev[k] } else { tail = prev[k] }
    }
    private func pushFront(_ k: Int) {
        prev[k] = -1
        next[k] = head
        if head >= 0 { prev[head] = k }
        head = k
        if tail < 0 { tail = k }
    }

    /// Returns true on a hit. Either way the block becomes most recently used.
    func access(_ block: UInt32) -> Bool {
        if let k = index[block] {
            unlink(k)
            pushFront(k)
            return true
        }
        if keys.count < capacity {
            keys.append(block)
            prev.append(-1)
            next.append(-1)
            let k = keys.count - 1
            index[block] = k
            pushFront(k)
        } else {
            let k = tail
            unlink(k)
            index[keys[k]] = nil
            keys[k] = block
            index[block] = k
            pushFront(k)
        }
        return false
    }
}

public final class Cache {
    public struct Line: Sendable {
        public var valid = false
        public var dirty = false
        public var tag: UInt32 = 0
        public var lastUse = 0
        public var inserted = 0
        /// When it was last touched, for the "recently used" glow in the view.
        public var lastTouch = -1
    }

    public let config: CacheConfig
    public private(set) var lines: [Line]
    public private(set) var stats = CacheStats()
    public private(set) var recent: [CacheEvent] = []
    public private(set) var last: CacheEvent?
    private var seen: Set<UInt32> = []
    private let shadow: LRUShadow
    private var clock = 0
    private var rng: UInt32 = 12345

    public init(_ c: CacheConfig) {
        config = c
        lines = [Line](repeating: Line(), count: c.sets * c.ways)
        shadow = LRUShadow(capacity: c.blocks)
    }

    public func split(_ addr: UInt32) -> (tag: UInt32, set: Int, offset: Int) {
        let off = Int(addr & UInt32(config.blockSize - 1))
        let set = Int((addr >> UInt32(config.offsetBits)) & UInt32(config.sets - 1))
        let tag = config.indexBits + config.offsetBits >= 32 ? 0 : addr >> UInt32(config.offsetBits + config.indexBits)
        return (tag, set, off)
    }

    public func line(set: Int, way: Int) -> Line { lines[set * config.ways + way] }

    /// Address of the first byte of the block held in a line.
    public func blockAddress(set: Int, tag: UInt32) -> UInt32 {
        (tag << UInt32(config.offsetBits + config.indexBits)) | (UInt32(set) << UInt32(config.offsetBits))
    }

    /// Looks up an address. `allocate` = bring the block in on a miss.
    @discardableResult
    public func access(_ addr: UInt32, _ kind: AccessKind, allocate: Bool = true, time: Int = 0) -> CacheEvent {
        clock += 1
        let (tag, set, _) = split(addr)
        let block = addr >> UInt32(config.offsetBits)
        let base = set * config.ways
        stats.accesses += 1
        if kind == .store { stats.writes += 1 } else { stats.reads += 1 }
        let shadowHit = shadow.access(block)
        var ev = CacheEvent(address: addr, kind: kind, hit: false, set: set, way: -1, tag: tag, missKind: nil,
                            evicted: nil, writeback: false, time: time)
        for w in 0..<config.ways where lines[base + w].valid && lines[base + w].tag == tag {
            stats.hits += 1
            lines[base + w].lastUse = clock
            lines[base + w].lastTouch = time
            if kind == .store && config.write == .writeBack { lines[base + w].dirty = true }
            ev.hit = true
            ev.way = w
            record(ev)
            return ev
        }
        stats.misses += 1
        let mk: MissKind = !seen.contains(block) ? .compulsory : (!shadowHit ? .capacity : .conflict)
        seen.insert(block)
        switch mk {
        case .compulsory: stats.compulsory += 1
        case .capacity: stats.capacity += 1
        case .conflict: stats.conflict += 1
        }
        ev.missKind = mk
        if !allocate || (kind == .store && config.write == .writeThrough) {
            record(ev)
            return ev
        }
        // Choose a victim: an empty way first, otherwise by the replacement policy.
        var victim = -1
        for w in 0..<config.ways where !lines[base + w].valid { victim = w; break }
        if victim < 0 {
            switch config.replacement {
            case .lru:
                victim = (0..<config.ways).min { lines[base + $0].lastUse < lines[base + $1].lastUse }!
            case .fifo:
                victim = (0..<config.ways).min { lines[base + $0].inserted < lines[base + $1].inserted }!
            case .random:
                rng = rng &* 1_103_515_245 &+ 12345
                victim = Int((rng >> 16) % UInt32(config.ways))
            }
            let old = lines[base + victim]
            ev.evicted = blockAddress(set: set, tag: old.tag)
            if old.dirty {
                ev.writeback = true
                stats.writebacks += 1
            }
        }
        lines[base + victim] = Line(valid: true, dirty: kind == .store && config.write == .writeBack, tag: tag,
                                    lastUse: clock, inserted: clock, lastTouch: time)
        ev.way = victim
        record(ev)
        return ev
    }

    private func record(_ e: CacheEvent) {
        last = e
        recent.append(e)
        if recent.count > 64 { recent.removeFirst(recent.count - 64) }
    }
}

public struct HierarchyConfig: Equatable, Sendable {
    public var l1i: CacheConfig
    public var l1d: CacheConfig
    public var l2: CacheConfig?
    public var dramLatency: Int
    public var enabled: Bool

    public init(l1i: CacheConfig, l1d: CacheConfig, l2: CacheConfig?, dramLatency: Int, enabled: Bool = true) {
        self.l1i = l1i
        self.l1d = l1d
        self.l2 = l2
        self.dramLatency = dramLatency
        self.enabled = enabled
    }

    /// Small on purpose, so that cache behaviour shows up with small programs.
    public static let teaching = HierarchyConfig(
        l1i: CacheConfig(name: "L1 I-cache", size: 1024, blockSize: 32, ways: 2, latency: 1),
        l1d: CacheConfig(name: "L1 D-cache", size: 512, blockSize: 16, ways: 2, latency: 1),
        l2: CacheConfig(name: "L2 cache", size: 4096, blockSize: 32, ways: 4, latency: 3),
        dramLatency: 10)

    /// Ideal memory: every access takes one cycle.
    public static let perfect = HierarchyConfig(l1i: teaching.l1i, l1d: teaching.l1d, l2: nil, dramLatency: 0, enabled: false)
}

public struct MemoryAccess: Sendable {
    public var kind: AccessKind
    public var address: UInt32
    /// Extra stall cycles beyond a 1-cycle L1 hit.
    public var penalty: Int
    /// "L1", "L2" or "DRAM".
    public var servedBy: String
    public var time: Int
}

/// L1 instruction and data caches, an optional unified L2, then main memory (DRAM).
public final class MemoryHierarchy {
    public let config: HierarchyConfig
    public let l1i: Cache
    public let l1d: Cache
    public let l2: Cache?
    public private(set) var dramAccesses = 0
    public private(set) var stallCycles = [AccessKind: Int]()
    public private(set) var recent: [MemoryAccess] = []
    public var time = 0

    public init(_ c: HierarchyConfig) {
        config = c
        l1i = Cache(c.l1i)
        l1d = Cache(c.l1d)
        l2 = c.l2.map(Cache.init)
    }

    /// Simulates one access and returns how many cycles the CPU must stall.
    @discardableResult
    public func access(_ kind: AccessKind, _ addr: UInt32) -> MemoryAccess {
        guard config.enabled else { return MemoryAccess(kind: kind, address: addr, penalty: 0, servedBy: "memory", time: time) }
        let l1 = kind == .fetch ? l1i : l1d
        let e = l1.access(addr, kind, time: time)
        var penalty = 0
        var served = "L1"
        if kind == .store && l1.config.write == .writeThrough {
            // Every store also goes to the next level, through a write buffer (no stall).
            if let l2 { l2.access(addr, .store, time: time) } else { dramAccesses += 1 }
        }
        if !e.hit && !(kind == .store && l1.config.write == .writeThrough) {
            if let l2 {
                let e2 = l2.access(addr, kind == .store ? .load : kind, time: time)
                penalty += l2.config.latency
                served = "L2"
                if !e2.hit {
                    penalty += config.dramLatency
                    dramAccesses += 1
                    served = "DRAM"
                }
                if e.writeback, let victim = e.evicted { l2.access(victim, .store, time: time) }
            } else {
                penalty += config.dramLatency
                dramAccesses += 1
                served = "DRAM"
            }
        }
        stallCycles[kind, default: 0] += penalty
        let a = MemoryAccess(kind: kind, address: addr, penalty: penalty, servedBy: served, time: time)
        recent.append(a)
        if recent.count > 200 { recent.removeFirst(recent.count - 200) }
        return a
    }

    /// Average memory access time for data, in cycles (P&H: hit time + miss rate × miss penalty).
    public func amat(_ c: Cache) -> Double {
        let l2Part: Double
        if let l2 {
            l2Part = Double(l2.config.latency) + l2.stats.missRate * Double(config.dramLatency)
        } else {
            l2Part = Double(config.dramLatency)
        }
        return 1 + c.stats.missRate * l2Part
    }
}
