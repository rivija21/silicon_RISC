import Foundation

public enum SectionKind: String, Sendable { case text = ".text", data = ".data" }

public enum RelocType: String, Sendable {
    case branch = "R_RISCV_BRANCH"
    case jal = "R_RISCV_JAL"
    case pcrelHi20 = "R_RISCV_PCREL_HI20"
    case pcrelLo12I = "R_RISCV_PCREL_LO12_I"
    case pcrelLo12S = "R_RISCV_PCREL_LO12_S"
    case hi20 = "R_RISCV_HI20"
    case lo12I = "R_RISCV_LO12_I"
    case lo12S = "R_RISCV_LO12_S"
    case abs32 = "R_RISCV_32"

    public var explanation: String {
        switch self {
        case .branch: return "12-bit PC-relative branch offset"
        case .jal: return "20-bit PC-relative jump offset"
        case .pcrelHi20: return "upper 20 bits of (symbol − PC), for auipc"
        case .pcrelLo12I, .pcrelLo12S: return "lower 12 bits of (symbol − PC of the matching auipc)"
        case .hi20: return "upper 20 bits of the absolute address, for lui"
        case .lo12I, .lo12S: return "lower 12 bits of the absolute address"
        case .abs32: return "a full 32-bit address stored as data"
        }
    }
}

public struct Relocation: Sendable {
    public var section: SectionKind
    public var offset: Int
    public var type: RelocType
    public var symbol: String
    public var addend: Int32
    /// For a %pcrel_lo: offset of the auipc it pairs with.
    public var hiOffset: Int?
    /// Index of the listing entry (text) it patches.
    public var listing: Int?
}

public struct ObjectSymbol: Sendable {
    public var name: String
    public var section: SectionKind
    public var offset: Int
    public var isGlobal: Bool
    public var line: Int
}

/// One machine instruction in an object file.
public struct ListingEntry: Sendable {
    public var offset: Int
    public var word: UInt32
    /// Assembly text of this one machine instruction.
    public var text: String
    /// The pseudo-instruction it was expanded from ("li t0, 100000"), if any.
    public var pseudo: String?
    /// Which part of the expansion, e.g. 1 of 2.
    public var part: Int
    public var parts: Int
    /// Index into the object's assembly lines.
    public var asmLine: Int
    public var origin: AsmOrigin?
    public var function: String?
}

public struct DataEntry: Sendable {
    public var offset: Int
    public var size: Int
    public var text: String
    public var asmLine: Int
    public var origin: AsmOrigin?
}

public final class ObjectFile: @unchecked Sendable {
    public let name: String
    public var text: [UInt8] = []
    public var data: [UInt8] = []
    public var symbols: [String: ObjectSymbol] = [:]
    public var relocations: [Relocation] = []
    public var listing: [ListingEntry] = []
    public var dataListing: [DataEntry] = []
    public var lines: [AsmLine]
    public var diagnostics: [Diagnostic] = []
    public var hasErrors: Bool { diagnostics.contains { $0.severity == .error } }
    /// Symbols this object uses but does not define.
    public var undefined: Set<String> = []
    /// A library member: only linked in when something needs one of its symbols.
    public var isLibrary = false
    public var summary = ""

    init(name: String, lines: [AsmLine]) {
        self.name = name
        self.lines = lines
    }

    public var globalSymbols: [String] { symbols.values.filter { $0.isGlobal }.map { $0.name }.sorted() }
}

/// A two-pass assembler for RV32IM, with the usual pseudo-instructions.
///
/// Pass 1 reads every line, works out how many bytes each one needs and
/// records where each label lands. Pass 2 encodes the instructions, now that
/// every label's offset is known. References that cannot be resolved inside
/// this file (another section, another file) become relocations for the linker.
public final class Assembler {
    private let obj: ObjectFile
    private var section: SectionKind = .text
    private var equ: [String: Int32] = [:]
    private var globals: Set<String> = []
    private var stmts: [Stmt] = []

    private struct Stmt {
        var line: Int
        var section: SectionKind
        var offset: Int
        var size: Int
        var mnemonic: String
        var operands: [String]
        var isDirective: Bool
        var rawArgs: String
    }

    public static func assemble(_ lines: [AsmLine], name: String) -> ObjectFile {
        let a = Assembler(ObjectFile(name: name, lines: lines))
        a.run()
        return a.obj
    }

    /// Assembles hand-written assembly text.
    public static func assemble(text: String, name: String) -> ObjectFile {
        let lines = text.components(separatedBy: "\n").map { l -> AsmLine in
            let t = l.trimmingCharacters(in: .whitespaces)
            let kind: AsmLine.Kind = t.isEmpty ? .blank : t.hasPrefix("#") ? .comment : t.hasSuffix(":") ? .label : t.hasPrefix(".") ? .directive : .instruction
            return AsmLine(kind, l)
        }
        return assemble(lines, name: name)
    }

    private init(_ obj: ObjectFile) { self.obj = obj }

    private struct AsmError: Error { let message: String }

    private func error(_ msg: String, _ line: Int) {
        obj.diagnostics.append(Diagnostic(.error, .assembler, msg, line: line + 1))
    }

    private var offsets: [SectionKind: Int] = [.text: 0, .data: 0]

    private func run() {
        pass1()
        guard !obj.hasErrors else { return }
        pass2()
        obj.undefined = Set(obj.relocations.map { $0.symbol }).filter { obj.symbols[$0] == nil }
        for g in globals {
            if obj.symbols[g] != nil { obj.symbols[g]!.isGlobal = true } else { obj.undefined.insert(g) }
        }
    }

    // MARK: Lexing a line

    static func stripComment(_ s: String) -> String {
        var inQuote = false, inChar = false, prev: Character = " "
        var out = ""
        for c in s {
            if c == "\"" && !inChar && prev != "\\" { inQuote.toggle() }
            if c == "'" && !inQuote && prev != "\\" { inChar.toggle() }
            if (c == "#" || c == ";") && !inQuote && !inChar { break }
            out.append(c)
            prev = c
        }
        return out
    }

    static func splitOperands(_ s: String) -> [String] {
        var parts: [String] = []
        var cur = ""
        var depth = 0, inQuote = false, prev: Character = " "
        for c in s {
            if c == "\"" && prev != "\\" { inQuote.toggle() }
            if !inQuote {
                if c == "(" { depth += 1 }
                if c == ")" { depth -= 1 }
                if c == "," && depth == 0 {
                    parts.append(cur.trimmingCharacters(in: .whitespaces))
                    cur = ""
                    prev = c
                    continue
                }
            }
            cur.append(c)
            prev = c
        }
        let last = cur.trimmingCharacters(in: .whitespaces)
        if !last.isEmpty || !parts.isEmpty { parts.append(last) }
        return parts
    }

    static func isSymbolStart(_ c: Character) -> Bool { c.isLetter || c == "_" || c == "." || c == "$" }
    static func isSymbolChar(_ c: Character) -> Bool { c.isLetter || c.isNumber || c == "_" || c == "." || c == "$" }

    // MARK: Pass 1

    private func pass1() {
        for (idx, l) in obj.lines.enumerated() {
            var s = Assembler.stripComment(l.text).trimmingCharacters(in: .whitespaces)
            // Labels.
            while let colon = s.firstIndex(of: ":") {
                let name = String(s[..<colon]).trimmingCharacters(in: .whitespaces)
                guard let first = name.first, Assembler.isSymbolStart(first), name.allSatisfy(Assembler.isSymbolChar) else { break }
                if obj.symbols[name] != nil {
                    error("label '\(name)' is defined twice", idx)
                } else {
                    obj.symbols[name] = ObjectSymbol(name: name, section: section, offset: offsets[section]!, isGlobal: false, line: idx + 1)
                }
                s = String(s[s.index(after: colon)...]).trimmingCharacters(in: .whitespaces)
            }
            if s.isEmpty { continue }
            let mnemonicEnd = s.firstIndex { $0 == " " || $0 == "\t" } ?? s.endIndex
            let mnemonic = String(s[..<mnemonicEnd]).lowercased()
            let rest = String(s[mnemonicEnd...]).trimmingCharacters(in: .whitespaces)
            if mnemonic.hasPrefix(".") {
                directive1(mnemonic, rest, idx)
                continue
            }
            if section != .text {
                error("instructions belong in .text, but this line is in .data", idx)
                continue
            }
            let ops = Assembler.splitOperands(rest)
            let n = instructionCount(mnemonic, ops, idx)
            stmts.append(Stmt(line: idx, section: .text, offset: offsets[.text]!, size: 4 * n, mnemonic: mnemonic, operands: ops,
                              isDirective: false, rawArgs: rest))
            offsets[.text]! += 4 * n
        }
    }

    private func align(_ to: Int) {
        let o = offsets[section]!
        let pad = (to - o % to) % to
        if pad > 0 {
            stmts.append(Stmt(line: -1, section: section, offset: o, size: pad, mnemonic: ".pad", operands: [], isDirective: true, rawArgs: ""))
            offsets[section]! += pad
        }
    }

    private func directive1(_ d: String, _ rest: String, _ idx: Int) {
        let args = Assembler.splitOperands(rest)
        func add(_ size: Int) {
            stmts.append(Stmt(line: idx, section: section, offset: offsets[section]!, size: size, mnemonic: d, operands: args,
                              isDirective: true, rawArgs: rest))
            offsets[section]! += size
        }
        switch d {
        case ".text": section = .text
        case ".data", ".rodata", ".bss", ".sdata": section = .data
        case ".section":
            let n = args.first ?? ""
            section = n.hasPrefix(".text") ? .text : .data
        case ".globl", ".global":
            for a in args { globals.insert(a) }
        case ".align", ".p2align":
            guard let n = Int(args.first ?? ""), n >= 0, n <= 12 else { error("\(d) needs a power of two exponent", idx); return }
            align(1 << n)
        case ".balign":
            guard let n = Int(args.first ?? ""), n > 0, n & (n - 1) == 0 else { error(".balign needs a power of two", idx); return }
            align(n)
        case ".word", ".long": add(4 * args.count)
        case ".half", ".short": add(2 * args.count)
        case ".byte": add(args.count)
        case ".space", ".zero", ".skip":
            guard let n = Int(args.first ?? ""), n >= 0 else { error("\(d) needs a size", idx); return }
            add(n)
        case ".string", ".asciz", ".ascii":
            guard let bytes = Assembler.parseString(rest) else { error("\(d) needs a \"quoted string\"", idx); return }
            add(bytes.count + (d == ".ascii" ? 0 : 1))
        case ".equ", ".set":
            guard args.count == 2, let v = try? constant(args[1]) else { error("\(d) needs a name and a constant", idx); return }
            equ[args[0]] = v
        case ".type", ".size", ".file", ".ident", ".option", ".attribute", ".local", ".comm", ".weak", ".cfi_startproc", ".cfi_endproc":
            break
        default:
            error("unknown directive \(d)", idx)
        }
    }

    static func parseString(_ s: String) -> [UInt8]? {
        let t = s.trimmingCharacters(in: .whitespaces)
        guard t.count >= 2, t.first == "\"", t.last == "\"" else { return nil }
        let inner = Array(t.dropFirst().dropLast().utf8)
        var out: [UInt8] = []
        var i = 0
        while i < inner.count {
            let c = inner[i]
            if c == 92, i + 1 < inner.count {
                i += 1
                switch inner[i] {
                case UInt8(ascii: "n"): out.append(10)
                case UInt8(ascii: "t"): out.append(9)
                case UInt8(ascii: "r"): out.append(13)
                case UInt8(ascii: "0"): out.append(0)
                case UInt8(ascii: "\\"): out.append(92)
                case UInt8(ascii: "\""): out.append(34)
                case UInt8(ascii: "'"): out.append(39)
                case UInt8(ascii: "x"):
                    var v = 0, n = 0
                    while n < 2, i + 1 < inner.count, let d = Int(String(UnicodeScalar(inner[i + 1])), radix: 16) {
                        v = v * 16 + d
                        i += 1
                        n += 1
                    }
                    out.append(UInt8(v))
                default: out.append(inner[i])
                }
            } else {
                out.append(c)
            }
            i += 1
        }
        return out
    }

    /// How many machine instructions a (pseudo-)instruction expands to.
    private func instructionCount(_ m: String, _ ops: [String], _ idx: Int) -> Int {
        switch m {
        case "la": return 2
        case "li":
            guard ops.count == 2 else { return 1 }
            if let v = try? constant(ops[1]) { return (v >= -2048 && v <= 2047) || (v & 0xFFF) == 0 ? 1 : 2 }
            return 2
        default: return 1
        }
    }

    // MARK: Values

    private enum Imm {
        case const(Int32)
        case symbol(String, Int32)
        case hi(String, Int32)
        case lo(String, Int32)
        case pcrelHi(String, Int32)
        case pcrelLo(String)
    }

    private func constant(_ s: String) throws -> Int32 {
        if case let .const(v) = try immediate(s) { return v }
        throw AsmError(message: "'\(s)' must be a constant number")
    }

    private func number(_ s: String) -> Int64? {
        var t = s.trimmingCharacters(in: .whitespaces)
        var neg = false
        if t.hasPrefix("-") { neg = true; t.removeFirst() } else if t.hasPrefix("+") { t.removeFirst() }
        var v: Int64?
        if t.hasPrefix("0x") || t.hasPrefix("0X") { v = Int64(t.dropFirst(2), radix: 16) }
        else if t.hasPrefix("0b") || t.hasPrefix("0B") { v = Int64(t.dropFirst(2), radix: 2) }
        else if t.count >= 3, t.first == "'", t.last == "'" {
            let inner = String(t.dropFirst().dropLast())
            if let b = Assembler.parseString("\"\(inner)\""), b.count == 1 { v = Int64(b[0]) }
        } else { v = Int64(t) }
        guard let x = v else { return nil }
        return neg ? -x : x
    }

    private func immediate(_ s: String) throws -> Imm {
        let t = s.trimmingCharacters(in: .whitespaces)
        if let v = number(t) {
            guard v >= -2_147_483_648, v <= 0xFFFF_FFFF else { throw AsmError(message: "\(t) does not fit in 32 bits") }
            return .const(Int32(truncatingIfNeeded: v))
        }
        for (prefix, kind) in [("%hi(", 0), ("%lo(", 1), ("%pcrel_hi(", 2), ("%pcrel_lo(", 3)] where t.hasPrefix(prefix) && t.hasSuffix(")") {
            let inner = String(t.dropFirst(prefix.count).dropLast())
            let (sym, add) = try symbolExpr(inner)
            switch kind {
            case 0: return .hi(sym, add)
            case 1: return .lo(sym, add)
            case 2: return .pcrelHi(sym, add)
            default: return .pcrelLo(sym)
            }
        }
        let (sym, add) = try symbolExpr(t)
        if let v = equ[sym] { return .const(v &+ add) }
        return .symbol(sym, add)
    }

    /// "label", "label+8", "label-4"
    private func symbolExpr(_ s: String) throws -> (String, Int32) {
        var name = s.trimmingCharacters(in: .whitespaces)
        var add: Int32 = 0
        if let k = name.lastIndex(where: { $0 == "+" || $0 == "-" }), k != name.startIndex {
            let tail = String(name[k...])
            if let v = number(tail) {
                add = Int32(truncatingIfNeeded: v)
                name = String(name[..<k]).trimmingCharacters(in: .whitespaces)
            }
        }
        guard let f = name.first, Assembler.isSymbolStart(f), name.allSatisfy(Assembler.isSymbolChar) else {
            throw AsmError(message: "'\(s)' is not a number or a label")
        }
        if Reg.parse(name) != nil { throw AsmError(message: "expected a number or label, but '\(name)' is a register") }
        return (name, add)
    }

    private func register(_ s: String) throws -> Int {
        let t = s.trimmingCharacters(in: .whitespaces)
        guard let r = Reg.parse(t) else {
            throw AsmError(message: t.isEmpty ? "missing register" : "'\(t)' is not a register (use x0–x31 or names like t0, a0, sp)")
        }
        return r
    }

    /// "8(sp)" → (8, sp). Also "(sp)" and "%lo(x)(t0)".
    private func memoryOperand(_ s: String) throws -> (Imm, Int) {
        let t = s.trimmingCharacters(in: .whitespaces)
        guard t.hasSuffix(")"), let open = t.lastIndex(of: "(") else {
            throw AsmError(message: "expected offset(register), like 8(sp), but found '\(t)'")
        }
        let reg = try register(String(t[t.index(after: open)..<t.index(before: t.endIndex)]))
        let off = String(t[..<open]).trimmingCharacters(in: .whitespaces)
        return (off.isEmpty ? .const(0) : try immediate(off), reg)
    }

    // MARK: Pass 2

    private struct Machine {
        var op: Op
        var rd = 0, rs1 = 0, rs2 = 0
        var imm: Imm = .const(0)
    }

    private func pass2() {
        for st in stmts {
            if st.isDirective {
                directive2(st)
                continue
            }
            do {
                let ms = try expand(st)
                if ms.count * 4 != st.size {
                    error("internal: size changed between passes for \(st.mnemonic)", st.line)
                    continue
                }
                for (k, m) in ms.enumerated() {
                    let off = st.offset + 4 * k
                    let word = try encode(m, at: off, part: k, stmt: st)
                    obj.text += [UInt8(word & 0xFF), UInt8((word >> 8) & 0xFF), UInt8((word >> 16) & 0xFF), UInt8(word >> 24)]
                    let d = Decoder.decode(word) ?? Decoded(op: m.op, raw: word)
                    let isPseudo = ms.count > 1 || m.op.rawValue != st.mnemonic
                    let src = obj.lines[st.line]
                    obj.listing.append(ListingEntry(offset: off, word: word, text: render(m, d),
                                                    pseudo: isPseudo ? "\(st.mnemonic) \(st.rawArgs)".trimmingCharacters(in: .whitespaces) : nil,
                                                    part: k + 1, parts: ms.count, asmLine: st.line, origin: src.origin, function: src.function))
                }
            } catch let e as AsmError {
                error(e.message, st.line)
                obj.text += [UInt8](repeating: 0, count: st.size)
            } catch {}
        }
        // Attach relocations to their listing entries.
        for k in obj.relocations.indices where obj.relocations[k].section == .text {
            obj.relocations[k].listing = obj.listing.firstIndex { $0.offset == obj.relocations[k].offset }
        }
    }

    private func render(_ m: Machine, _ d: Decoded) -> String {
        func sym() -> String? {
            switch m.imm {
            case let .symbol(s, a): return a == 0 ? s : "\(s)\(a > 0 ? "+" : "")\(a)"
            case let .hi(s, _): return "%hi(\(s))"
            case let .lo(s, _): return "%lo(\(s))"
            case let .pcrelHi(s, _): return "%pcrel_hi(\(s))"
            case let .pcrelLo(s): return "%pcrel_lo(\(s))"
            case .const: return nil
            }
        }
        let r = Reg.name
        guard let s = sym() else { return Decoder.disassemble(d) }
        switch m.op.spec.format {
        case .B: return "\(m.op.rawValue) \(r(m.rs1)), \(r(m.rs2)), \(s)"
        case .J: return "jal \(r(m.rd)), \(s)"
        case .U: return "\(m.op.rawValue) \(r(m.rd)), \(s)"
        case .S: return "\(m.op.rawValue) \(r(m.rs2)), \(s)(\(r(m.rs1)))"
        case .I where m.op.isLoad || m.op == .jalr: return "\(m.op.rawValue) \(r(m.rd)), \(s)(\(r(m.rs1)))"
        default: return "\(m.op.rawValue) \(r(m.rd)), \(r(m.rs1)), \(s)"
        }
    }

    private func directive2(_ st: Stmt) {
        func emit(_ bytes: [UInt8], _ text: String) {
            if st.section == .text {
                obj.text += bytes
            } else {
                obj.dataListing.append(DataEntry(offset: obj.data.count, size: bytes.count, text: text, asmLine: st.line,
                                                 origin: st.line >= 0 ? obj.lines[st.line].origin : nil))
                obj.data += bytes
            }
        }
        func le(_ v: UInt32, _ n: Int) -> [UInt8] { (0..<n).map { UInt8((v >> UInt32(8 * $0)) & 0xFF) } }
        switch st.mnemonic {
        case ".pad":
            emit([UInt8](repeating: 0, count: st.size), "(padding)")
        case ".word", ".long", ".half", ".short", ".byte":
            let size = st.mnemonic == ".byte" ? 1 : (st.mnemonic == ".half" || st.mnemonic == ".short") ? 2 : 4
            var bytes: [UInt8] = []
            for (k, a) in st.operands.enumerated() {
                do {
                    switch try immediate(a) {
                    case let .const(v):
                        if size == 1 && (v < -128 || v > 255) { error(".byte value \(v) does not fit in 8 bits", st.line) }
                        if size == 2 && (v < -32768 || v > 65535) { error(".half value \(v) does not fit in 16 bits", st.line) }
                        bytes += le(UInt32(bitPattern: v), size)
                    case let .symbol(s, add):
                        guard size == 4 else { error("only .word can hold an address", st.line); bytes += le(0, size); continue }
                        obj.relocations.append(Relocation(section: st.section, offset: (st.section == .text ? obj.text.count : obj.data.count) + bytes.count,
                                                          type: .abs32, symbol: s, addend: add, hiOffset: nil, listing: nil))
                        obj.undefined.insert(s)
                        bytes += le(0, 4)
                    default:
                        error("%hi/%lo cannot be used in data", st.line)
                        bytes += le(0, size)
                    }
                } catch let e as AsmError {
                    error(e.message, st.line)
                    bytes += le(0, size)
                } catch {}
                _ = k
            }
            emit(bytes, "\(st.mnemonic) \(st.rawArgs)")
        case ".space", ".zero", ".skip":
            emit([UInt8](repeating: 0, count: st.size), "\(st.mnemonic) \(st.rawArgs)")
        case ".string", ".asciz", ".ascii":
            var b = Assembler.parseString(st.rawArgs) ?? []
            if st.mnemonic != ".ascii" { b.append(0) }
            emit(b, "\(st.mnemonic) \(st.rawArgs)")
        default:
            break
        }
    }

    private func expand(_ st: Stmt) throws -> [Machine] {
        let o = st.operands
        func need(_ n: Int) throws {
            if o.count != n {
                throw AsmError(message: "'\(st.mnemonic)' takes \(n) operand\(n == 1 ? "" : "s"), but \(o.count) \(o.count == 1 ? "was" : "were") given")
            }
        }
        func reg(_ k: Int) throws -> Int { try register(o[k]) }
        func imm(_ k: Int) throws -> Imm { try immediate(o[k]) }
        switch st.mnemonic {
        case "nop": try need(0); return [Machine(op: .addi)]
        case "li":
            try need(2)
            let rd = try reg(0)
            switch try imm(1) {
            case let .const(v):
                if v >= -2048 && v <= 2047 { return [Machine(op: .addi, rd: rd, rs1: 0, imm: .const(v))] }
                let lo = (v << 20) >> 20
                let hi = (v &- lo) >> 12
                if lo == 0 { return [Machine(op: .lui, rd: rd, imm: .const(hi & 0xFFFFF))] }
                return [Machine(op: .lui, rd: rd, imm: .const(hi & 0xFFFFF)), Machine(op: .addi, rd: rd, rs1: rd, imm: .const(lo))]
            case let .symbol(s, a):
                return [Machine(op: .lui, rd: rd, imm: .hi(s, a)), Machine(op: .addi, rd: rd, rs1: rd, imm: .lo(s, a))]
            default: throw AsmError(message: "li needs a constant")
            }
        case "la":
            try need(2)
            let rd = try reg(0)
            guard case let .symbol(s, a) = try imm(1) else { throw AsmError(message: "la needs a label") }
            return [Machine(op: .auipc, rd: rd, imm: .pcrelHi(s, a)), Machine(op: .addi, rd: rd, rs1: rd, imm: .pcrelLo("."))]
        case "mv": try need(2); return [Machine(op: .addi, rd: try reg(0), rs1: try reg(1))]
        case "not": try need(2); return [Machine(op: .xori, rd: try reg(0), rs1: try reg(1), imm: .const(-1))]
        case "neg": try need(2); return [Machine(op: .sub, rd: try reg(0), rs1: 0, rs2: try reg(1))]
        case "seqz": try need(2); return [Machine(op: .sltiu, rd: try reg(0), rs1: try reg(1), imm: .const(1))]
        case "snez": try need(2); return [Machine(op: .sltu, rd: try reg(0), rs1: 0, rs2: try reg(1))]
        case "sltz": try need(2); return [Machine(op: .slt, rd: try reg(0), rs1: try reg(1), rs2: 0)]
        case "sgtz": try need(2); return [Machine(op: .slt, rd: try reg(0), rs1: 0, rs2: try reg(1))]
        case "beqz": try need(2); return [Machine(op: .beq, rs1: try reg(0), rs2: 0, imm: try imm(1))]
        case "bnez": try need(2); return [Machine(op: .bne, rs1: try reg(0), rs2: 0, imm: try imm(1))]
        case "blez": try need(2); return [Machine(op: .bge, rs1: 0, rs2: try reg(0), imm: try imm(1))]
        case "bgez": try need(2); return [Machine(op: .bge, rs1: try reg(0), rs2: 0, imm: try imm(1))]
        case "bltz": try need(2); return [Machine(op: .blt, rs1: try reg(0), rs2: 0, imm: try imm(1))]
        case "bgtz": try need(2); return [Machine(op: .blt, rs1: 0, rs2: try reg(0), imm: try imm(1))]
        case "bgt": try need(3); return [Machine(op: .blt, rs1: try reg(1), rs2: try reg(0), imm: try imm(2))]
        case "ble": try need(3); return [Machine(op: .bge, rs1: try reg(1), rs2: try reg(0), imm: try imm(2))]
        case "bgtu": try need(3); return [Machine(op: .bltu, rs1: try reg(1), rs2: try reg(0), imm: try imm(2))]
        case "bleu": try need(3); return [Machine(op: .bgeu, rs1: try reg(1), rs2: try reg(0), imm: try imm(2))]
        case "j": try need(1); return [Machine(op: .jal, rd: 0, imm: try imm(0))]
        case "call": try need(1); return [Machine(op: .jal, rd: Reg.ra, imm: try imm(0))]
        case "tail": try need(1); return [Machine(op: .jal, rd: 0, imm: try imm(0))]
        case "ret": try need(0); return [Machine(op: .jalr, rd: 0, rs1: Reg.ra)]
        case "jr": try need(1); return [Machine(op: .jalr, rd: 0, rs1: try reg(0))]
        case "jal":
            if o.count == 1 { return [Machine(op: .jal, rd: Reg.ra, imm: try imm(0))] }
            try need(2)
            return [Machine(op: .jal, rd: try reg(0), imm: try imm(1))]
        case "jalr":
            if o.count == 1 { return [Machine(op: .jalr, rd: Reg.ra, rs1: try reg(0))] }
            if o.count == 2 {
                let (i, b) = try memoryOperand(o[1])
                return [Machine(op: .jalr, rd: try reg(0), rs1: b, imm: i)]
            }
            try need(3)
            return [Machine(op: .jalr, rd: try reg(0), rs1: try reg(1), imm: try imm(2))]
        default:
            break
        }
        guard let op = Op(rawValue: st.mnemonic) else {
            throw AsmError(message: "unknown instruction '\(st.mnemonic)'")
        }
        switch op.spec.format {
        case .R:
            try need(3)
            return [Machine(op: op, rd: try reg(0), rs1: try reg(1), rs2: try reg(2))]
        case .I:
            switch op {
            case .ecall, .ebreak, .fence: return [Machine(op: op)]
            case .lb, .lh, .lw, .lbu, .lhu:
                try need(2)
                let (i, b) = try memoryOperand(o[1])
                return [Machine(op: op, rd: try reg(0), rs1: b, imm: i)]
            default:
                try need(3)
                return [Machine(op: op, rd: try reg(0), rs1: try reg(1), imm: try imm(2))]
            }
        case .S:
            try need(2)
            let (i, b) = try memoryOperand(o[1])
            return [Machine(op: op, rs1: b, rs2: try reg(0), imm: i)]
        case .B:
            try need(3)
            return [Machine(op: op, rs1: try reg(0), rs2: try reg(1), imm: try imm(2))]
        case .U:
            try need(2)
            return [Machine(op: op, rd: try reg(0), imm: try imm(1))]
        case .J:
            try need(2)
            return [Machine(op: op, rd: try reg(0), imm: try imm(1))]
        }
    }

    private func reloc(_ type: RelocType, _ sym: String, _ add: Int32, at off: Int, hi: Int? = nil) {
        obj.relocations.append(Relocation(section: .text, offset: off, type: type, symbol: sym, addend: add, hiOffset: hi, listing: nil))
        if obj.symbols[sym] == nil { obj.undefined.insert(sym) }
    }

    private var lastPcrelHi: Int = 0

    private func encode(_ m: Machine, at off: Int, part: Int, stmt: Stmt) throws -> UInt32 {
        var d = Decoded(op: m.op, rd: m.rd, rs1: m.rs1, rs2: m.rs2)
        let fmt = m.op.spec.format
        switch m.imm {
        case let .const(v):
            switch fmt {
            case .I:
                if m.op.isShiftImm {
                    guard v >= 0 && v <= 31 else { throw AsmError(message: "shift amount \(v) must be 0–31") }
                } else if !(m.op == .ecall || m.op == .ebreak || m.op == .fence) {
                    guard v >= -2048 && v <= 2047 else { throw AsmError(message: "immediate \(v) does not fit in 12 bits (−2048…2047); use li to load it into a register") }
                }
            case .S:
                guard v >= -2048 && v <= 2047 else { throw AsmError(message: "offset \(v) does not fit in 12 bits") }
            case .B:
                guard v >= -4096 && v <= 4095, v & 1 == 0 else { throw AsmError(message: "branch offset \(v) is out of range or odd") }
            case .J:
                guard v >= -(1 << 20) && v < (1 << 20), v & 1 == 0 else { throw AsmError(message: "jump offset \(v) is out of range or odd") }
            case .U:
                guard v >= -(1 << 19) && v <= 0xFFFFF else { throw AsmError(message: "upper immediate \(v) does not fit in 20 bits") }
            case .R: break
            }
            d.imm = fmt == .U ? v & 0xFFFFF : v
        case let .symbol(s, a):
            guard fmt == .B || fmt == .J else { throw AsmError(message: "a label can only be used here with %hi/%lo; use la to load an address") }
            if let sym = obj.symbols[s], sym.section == .text {
                // Same file, same section: the assembler can resolve it right away.
                let delta = Int32(sym.offset - off) &+ a
                if fmt == .B {
                    guard delta >= -4096 && delta <= 4095 else { throw AsmError(message: "branch to '\(s)' is too far (\(delta) bytes; the limit is ±4 KiB)") }
                } else {
                    guard delta >= -(1 << 20) && delta < (1 << 20) else { throw AsmError(message: "jump to '\(s)' is too far") }
                }
                d.imm = delta
            } else {
                reloc(fmt == .B ? .branch : .jal, s, a, at: off)
            }
        case let .hi(s, a):
            guard fmt == .U else { throw AsmError(message: "%hi only works with lui") }
            reloc(.hi20, s, a, at: off)
        case let .lo(s, a):
            guard fmt == .I || fmt == .S else { throw AsmError(message: "%lo needs an I- or S-type instruction") }
            reloc(fmt == .S ? .lo12S : .lo12I, s, a, at: off)
        case let .pcrelHi(s, a):
            guard fmt == .U else { throw AsmError(message: "%pcrel_hi only works with auipc") }
            reloc(.pcrelHi20, s, a, at: off)
            lastPcrelHi = off
        case let .pcrelLo(s):
            guard fmt == .I || fmt == .S else { throw AsmError(message: "%pcrel_lo needs an I- or S-type instruction") }
            // In `la`, the low part refers to the auipc just before it.
            let hi = s == "." ? lastPcrelHi : (obj.symbols[s]?.offset ?? lastPcrelHi)
            let target = obj.relocations.last { $0.offset == hi && $0.type == .pcrelHi20 }
            reloc(fmt == .S ? .pcrelLo12S : .pcrelLo12I, target?.symbol ?? s, target?.addend ?? 0, at: off, hi: hi)
        }
        return Encoder.encode(d)
    }
}
