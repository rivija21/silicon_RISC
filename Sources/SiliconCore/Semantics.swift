import Foundation

/// Integer arithmetic exactly as RV32IM does it. The constant folder, the
/// reference interpreter and the CPU simulators all use these, so every
/// layer agrees even on corner cases C leaves undefined:
///  - x / 0 = -1 and x % 0 = x (RISC-V does not trap on division by zero)
///  - INT_MIN / -1 = INT_MIN and INT_MIN % -1 = 0
///  - shift amounts use only their low 5 bits
///  - overflow wraps around (two's complement)
public enum RV {
    @inline(__always) public static func div(_ a: Int32, _ b: Int32) -> Int32 {
        if b == 0 { return -1 }
        if a == Int32.min && b == -1 { return Int32.min }
        return a / b
    }
    @inline(__always) public static func rem(_ a: Int32, _ b: Int32) -> Int32 {
        if b == 0 { return a }
        if a == Int32.min && b == -1 { return 0 }
        return a % b
    }
    @inline(__always) public static func divu(_ a: UInt32, _ b: UInt32) -> UInt32 { b == 0 ? 0xFFFF_FFFF : a / b }
    @inline(__always) public static func remu(_ a: UInt32, _ b: UInt32) -> UInt32 { b == 0 ? a : a % b }

    /// A C binary operator on 32-bit values. `unsigned` selects unsigned
    /// division, right shift and comparison.
    public static func binary(_ op: BinaryOp, _ a: Int32, _ b: Int32, unsigned: Bool) -> Int32 {
        let ua = UInt32(bitPattern: a), ub = UInt32(bitPattern: b)
        switch op {
        case .add: return a &+ b
        case .sub: return a &- b
        case .mul: return a &* b
        case .div: return unsigned ? Int32(bitPattern: divu(ua, ub)) : div(a, b)
        case .rem: return unsigned ? Int32(bitPattern: remu(ua, ub)) : rem(a, b)
        case .shl: return Int32(bitPattern: ua << (ub & 31))
        case .shr: return unsigned ? Int32(bitPattern: ua >> (ub & 31)) : a >> Int32(ub & 31)
        case .bitAnd: return a & b
        case .bitOr: return a | b
        case .bitXor: return a ^ b
        case .lt: return (unsigned ? ua < ub : a < b) ? 1 : 0
        case .gt: return (unsigned ? ua > ub : a > b) ? 1 : 0
        case .le: return (unsigned ? ua <= ub : a <= b) ? 1 : 0
        case .ge: return (unsigned ? ua >= ub : a >= b) ? 1 : 0
        case .eq: return a == b ? 1 : 0
        case .ne: return a != b ? 1 : 0
        case .logAnd: return (a != 0 && b != 0) ? 1 : 0
        case .logOr: return (a != 0 || b != 0) ? 1 : 0
        }
    }
}

/// Where things live in the simulated machine (the same layout as the RARS simulator).
public enum MemoryMap {
    public static let textBase: UInt32 = 0x0040_0000
    public static let dataBase: UInt32 = 0x1001_0000
    public static let heapBase: UInt32 = 0x1004_0000
    public static let heapLimit: UInt32 = 0x1044_0000
    public static let stackTop: UInt32 = 0x7FFF_EFFC
    public static let stackSize: UInt32 = 0x0010_0000
    public static var stackBottom: UInt32 { (stackTop & ~0xF) - stackSize + 16 }
}

/// Text output as the simulated program prints it.
public enum Format {
    public static func hex(_ v: UInt32, digits: Int = 8) -> String {
        let s = String(v, radix: 16)
        return "0x" + String(repeating: "0", count: max(0, digits - s.count)) + s
    }
}
