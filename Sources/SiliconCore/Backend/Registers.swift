import Foundation

/// The 32 RISC-V integer registers and their ABI roles.
public enum Reg {
    public static let names = [
        "zero", "ra", "sp", "gp", "tp", "t0", "t1", "t2",
        "s0", "s1", "a0", "a1", "a2", "a3", "a4", "a5",
        "a6", "a7", "s2", "s3", "s4", "s5", "s6", "s7",
        "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6",
    ]

    public static let zero = 0, ra = 1, sp = 2, gp = 3, tp = 4
    public static let s0 = 8, fp = 8
    public static let a0 = 10, a7 = 17
    public static let t4 = 29, t5 = 30, t6 = 31

    public static func a(_ k: Int) -> Int { 10 + k }

    /// Temporaries the allocator may use (caller-saved: a call may destroy them).
    public static let callerSaved = [5, 6, 7, 28]
    /// Argument registers double as temporaries for values that never meet a call
    /// (otherwise setting up the call's arguments could overwrite them).
    public static let argumentTemps = [15, 14, 13, 12, 16, 17, 11, 10]
    /// Saved registers the allocator may use (callee-saved: a function must restore them).
    public static let calleeSaved = [9, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27]
    /// t4, t5 and t6 are kept free for reloading spilled values, big constants and addresses.
    public static let scratch = [29, 30, 31]

    public static func name(_ r: Int) -> String { names[r & 31] }

    /// Accepts ABI names (a0, sp, fp…) and numeric names (x10).
    public static func parse(_ s: String) -> Int? {
        let l = s.lowercased()
        if let i = names.firstIndex(of: l) { return i }
        if l == "fp" { return 8 }
        if l.hasPrefix("x"), let n = Int(l.dropFirst()), n >= 0, n < 32, String(n) == l.dropFirst() { return n }
        return nil
    }

    public static func role(_ r: Int) -> String {
        switch r {
        case 0: return "hard-wired zero"
        case 1: return "return address"
        case 2: return "stack pointer"
        case 3: return "global pointer"
        case 4: return "thread pointer"
        case 5, 6, 7, 28, 29, 30, 31: return "temporary (caller-saved)"
        case 8: return "saved / frame pointer"
        case 9, 18...27: return "saved (callee-saved)"
        case 10, 11: return "argument / return value"
        default: return "argument"
        }
    }
}
