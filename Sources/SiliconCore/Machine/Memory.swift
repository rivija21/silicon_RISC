import Foundation

public struct MemoryFault: Error, CustomStringConvertible, Sendable {
    public var address: UInt32
    public var message: String
    public var description: String { message }
}

/// Byte-addressed little-endian memory with three regions:
/// text (code), data + heap, and the stack. Anything else faults,
/// which is how a wild pointer shows up as a "segmentation fault".
public final class Memory {
    public let textBase: UInt32
    public private(set) var text: [UInt8]
    public let dataBase: UInt32
    public private(set) var data: [UInt8]
    public let stackBase: UInt32
    public private(set) var stack: [UInt8]
    /// Highest address written in the data/heap region, for the memory view.
    public private(set) var dataHigh: UInt32 = 0
    /// Lowest stack address touched (the stack grows down).
    public private(set) var stackLow: UInt32 = 0

    public init(text: [UInt8] = [], data: [UInt8] = []) {
        textBase = MemoryMap.textBase
        self.text = text
        dataBase = MemoryMap.dataBase
        self.data = [UInt8](repeating: 0, count: Int(MemoryMap.heapLimit - MemoryMap.dataBase))
        for (i, b) in data.enumerated() { self.data[i] = b }
        stackBase = MemoryMap.stackBottom
        stack = [UInt8](repeating: 0, count: Int(MemoryMap.stackTop &+ 4 &- MemoryMap.stackBottom) + 16)
        dataHigh = dataBase &+ UInt32(data.count)
        stackLow = MemoryMap.stackTop
    }

    public func copy() -> Memory {
        let m = Memory(text: text, data: [])
        m.data = data
        m.stack = stack
        m.dataHigh = dataHigh
        m.stackLow = stackLow
        return m
    }

    public func describe(_ addr: UInt32) -> String {
        if addr < 4096 { return "address \(Format.hex(addr)) (looks like a NULL pointer)" }
        return "address \(Format.hex(addr))"
    }

    @inline(__always)
    private func locate(_ addr: UInt32, _ size: UInt32, write: Bool) throws -> (Int, Int) {
        if addr &- dataBase < UInt32(data.count) &- size &+ 1, addr >= dataBase {
            return (1, Int(addr &- dataBase))
        }
        if addr >= stackBase, addr &- stackBase <= UInt32(stack.count) &- size {
            return (2, Int(addr &- stackBase))
        }
        if addr >= textBase, addr &- textBase <= UInt32(text.count) &- size, text.count >= Int(size) {
            if write { throw MemoryFault(address: addr, message: "write to the code (text) segment at \(Format.hex(addr))") }
            return (0, Int(addr &- textBase))
        }
        throw MemoryFault(address: addr, message: "\(write ? "store to" : "load from") unmapped \(describe(addr)) — segmentation fault")
    }

    public func isMapped(_ addr: UInt32) -> Bool { (try? locate(addr, 1, write: false)) != nil }

    @inline(__always)
    public func load8(_ addr: UInt32) throws -> UInt8 {
        let (r, o) = try locate(addr, 1, write: false)
        switch r {
        case 1: return data[o]
        case 2: return stack[o]
        default: return text[o]
        }
    }

    @inline(__always)
    public func load16(_ addr: UInt32) throws -> UInt16 {
        if addr & 1 != 0 { throw MemoryFault(address: addr, message: "misaligned halfword load at \(Format.hex(addr))") }
        return UInt16(try load8(addr)) | UInt16(try load8(addr &+ 1)) << 8
    }

    @inline(__always)
    public func load32(_ addr: UInt32) throws -> UInt32 {
        if addr & 3 != 0 { throw MemoryFault(address: addr, message: "misaligned word load at \(Format.hex(addr)) (words must be at multiples of 4)") }
        let (r, o) = try locate(addr, 4, write: false)
        switch r {
        case 1: return UInt32(data[o]) | UInt32(data[o + 1]) << 8 | UInt32(data[o + 2]) << 16 | UInt32(data[o + 3]) << 24
        case 2: return UInt32(stack[o]) | UInt32(stack[o + 1]) << 8 | UInt32(stack[o + 2]) << 16 | UInt32(stack[o + 3]) << 24
        default: return UInt32(text[o]) | UInt32(text[o + 1]) << 8 | UInt32(text[o + 2]) << 16 | UInt32(text[o + 3]) << 24
        }
    }

    @inline(__always)
    private func touched(_ r: Int, _ addr: UInt32) {
        if r == 1 { if addr >= dataHigh { dataHigh = addr &+ 4 } } else if addr < stackLow { stackLow = addr }
    }

    @inline(__always)
    public func store8(_ addr: UInt32, _ v: UInt8) throws {
        let (r, o) = try locate(addr, 1, write: true)
        if r == 1 { data[o] = v } else { stack[o] = v }
        touched(r, addr)
    }

    @inline(__always)
    public func store16(_ addr: UInt32, _ v: UInt16) throws {
        if addr & 1 != 0 { throw MemoryFault(address: addr, message: "misaligned halfword store at \(Format.hex(addr))") }
        try store8(addr, UInt8(v & 0xFF))
        try store8(addr &+ 1, UInt8(v >> 8))
    }

    @inline(__always)
    public func store32(_ addr: UInt32, _ v: UInt32) throws {
        if addr & 3 != 0 { throw MemoryFault(address: addr, message: "misaligned word store at \(Format.hex(addr)) (words must be at multiples of 4)") }
        let (r, o) = try locate(addr, 4, write: true)
        if r == 1 {
            data[o] = UInt8(v & 0xFF); data[o + 1] = UInt8((v >> 8) & 0xFF)
            data[o + 2] = UInt8((v >> 16) & 0xFF); data[o + 3] = UInt8(v >> 24)
        } else {
            stack[o] = UInt8(v & 0xFF); stack[o + 1] = UInt8((v >> 8) & 0xFF)
            stack[o + 2] = UInt8((v >> 16) & 0xFF); stack[o + 3] = UInt8(v >> 24)
        }
        touched(r, addr)
    }

    /// Reads a zero-terminated string (for print_str), at most `limit` bytes.
    public func cString(_ addr: UInt32, limit: Int = 4096) throws -> [UInt8] {
        var out: [UInt8] = []
        var a = addr
        while out.count < limit {
            let b = try load8(a)
            if b == 0 { break }
            out.append(b)
            a &+= 1
        }
        return out
    }

    /// A snapshot of a small window of memory for the memory view.
    public func bytes(_ addr: UInt32, count: Int) -> [UInt8?] {
        (0..<count).map { try? load8(addr &+ UInt32($0)) }
    }
}

/// What the simulated program printed, and how it ended.
public struct ProgramOutput: Equatable, Sendable {
    public var text: String = ""
    public var exitCode: Int32? = nil
    public var fault: String? = nil

    public init() {}

    /// Output is kept to a sensible size so a runaway print loop cannot eat the machine.
    public static let limit = 200_000
    public mutating func append(_ bytes: [UInt8]) {
        guard text.utf8.count < ProgramOutput.limit else { return }
        text += String(decoding: bytes, as: UTF8.self)
    }
    public mutating func append(_ s: String) {
        guard text.utf8.count < ProgramOutput.limit else { return }
        text += s
    }
}

/// The system calls our programs use, numbered as in the RARS simulator.
public enum Syscall: UInt32 {
    case printInt = 1
    case printString = 4
    case sbrk = 9
    case exit0 = 10
    case printChar = 11
    case printHex = 34
    case printUnsigned = 36
    case exit = 93

    public var name: String {
        switch self {
        case .printInt: return "print_int"
        case .printString: return "print_string"
        case .sbrk: return "sbrk"
        case .exit0: return "exit"
        case .printChar: return "print_char"
        case .printHex: return "print_hex"
        case .printUnsigned: return "print_unsigned"
        case .exit: return "exit"
        }
    }
}
