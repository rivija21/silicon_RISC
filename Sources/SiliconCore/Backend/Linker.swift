import Foundation

public struct LinkedInstruction: Sendable {
    public var address: UInt32
    public var word: UInt32
    public var decoded: Decoded?
    /// Assembly text with symbols (as written by the compiler or assembler).
    public var text: String
    public var pseudo: String?
    public var part: Int
    public var parts: Int
    public var object: String
    public var asmLine: Int
    public var origin: AsmOrigin?
    public var function: String?
    /// The word before the linker patched it (equal to `word` if no relocation applied).
    public var unlinkedWord: UInt32
}

public struct RelocationRecord: Sendable {
    public var object: String
    public var type: RelocType
    public var symbol: String
    public var address: UInt32
    public var target: UInt32
    public var before: UInt32
    public var after: UInt32
    public var value: Int32
}

public struct LinkedObject: Sendable {
    public var name: String
    public var textStart: UInt32
    public var textSize: Int
    public var dataStart: UInt32
    public var dataSize: Int
    public var isLibrary: Bool
    public var reason: String
    public var summary: String
}

public struct LinkedSymbol: Sendable {
    public var name: String
    public var address: UInt32
    public var section: SectionKind
    public var object: String
    public var isGlobal: Bool
}

public struct LinkedData: Sendable {
    public var address: UInt32
    public var size: Int
    public var text: String
    public var object: String
    public var origin: AsmOrigin?
}

/// A linked program, ready to load: code at 0x00400000, data at 0x10010000.
public final class Executable: @unchecked Sendable {
    public var text: [UInt8] = []
    public var data: [UInt8] = []
    public let textBase = MemoryMap.textBase
    public let dataBase = MemoryMap.dataBase
    public var entry: UInt32 = MemoryMap.textBase
    public var symbols: [LinkedSymbol] = []
    public var instructions: [LinkedInstruction] = []
    public var relocations: [RelocationRecord] = []
    public var objects: [LinkedObject] = []
    public var dataEntries: [LinkedData] = []
    private var functionStarts: [(UInt32, String)] = []

    public var textEnd: UInt32 { textBase + UInt32(text.count) }
    public var dataEnd: UInt32 { dataBase + UInt32(data.count) }

    public func instruction(at pc: UInt32) -> LinkedInstruction? {
        guard pc >= textBase, pc & 3 == 0 else { return nil }
        let k = Int((pc - textBase) / 4)
        return k < instructions.count ? instructions[k] : nil
    }

    func finish() {
        functionStarts = symbols.filter { $0.section == .text && !$0.name.hasPrefix(".L") }.map { ($0.address, $0.name) }.sorted { $0.0 < $1.0 }
    }

    /// "main+12" for an address inside main.
    public func symbolize(_ addr: UInt32) -> String? {
        if let exact = symbols.first(where: { $0.address == addr && !$0.name.hasPrefix(".") }) { return exact.name }
        if addr >= textBase && addr < textEnd {
            if let f = functionStarts.last(where: { $0.0 <= addr }) {
                return addr == f.0 ? f.1 : "\(f.1)+\(addr - f.0)"
            }
        }
        if addr >= dataBase && addr < dataEnd {
            if let s = symbols.filter({ $0.section == .data && $0.address <= addr }).max(by: { $0.address < $1.address }) {
                return addr == s.address ? s.name : "\(s.name)+\(addr - s.address)"
            }
        }
        return nil
    }

    /// The label at exactly this code address, local labels (.L…) included. For branch targets.
    public func label(at addr: UInt32) -> String? {
        let here = symbols.filter { $0.address == addr && $0.section == .text }
        return here.first { $0.name.hasPrefix(".L") && !$0.name.hasSuffix("_return") }?.name ?? here.first?.name
    }

    public func function(at pc: UInt32) -> String? {
        functionStarts.last { $0.0 <= pc }?.1
    }

    public func address(of name: String) -> UInt32? {
        symbols.first { $0.name == name && $0.isGlobal }?.address ?? symbols.first { $0.name == name }?.address
    }
}

/// The linker: picks the library members the program needs, places every
/// section in memory, gives each symbol its final address and patches the
/// relocations the assembler left behind.
public enum Linker {
    public static func link(_ inputs: [ObjectFile], archive: [ObjectFile], entry: String = "_start") -> (Executable?, [Diagnostic]) {
        var diags: [Diagnostic] = []
        func err(_ m: String) { diags.append(Diagnostic(.error, .linker, m)) }

        // 1. Resolve undefined symbols by pulling members out of the library archive.
        var objs = inputs
        var reasons: [String: String] = [:]
        for o in inputs { reasons[o.name] = "named on the command line" }
        var defined: [String: String] = [:]
        func addDefs(_ o: ObjectFile) {
            for g in o.globalSymbols where defined[g] == nil { defined[g] = o.name }
        }
        for o in objs { addDefs(o) }
        var changed = true
        while changed {
            changed = false
            let needed = objs.flatMap { $0.undefined }.filter { defined[$0] == nil }
            for sym in needed.sorted() where defined[sym] == nil {
                if let member = archive.first(where: { m in m.globalSymbols.contains(sym) && !objs.contains { $0 === m } }) {
                    let who = objs.first { $0.undefined.contains(sym) }?.name ?? "?"
                    reasons[member.name] = "\(who) needs \(sym)"
                    objs.append(member)
                    addDefs(member)
                    changed = true
                }
            }
        }
        // Duplicate definitions.
        var seen: [String: String] = [:]
        for o in objs {
            for g in o.globalSymbols {
                if let first = seen[g] { err("multiple definition of '\(g)' (in \(first) and \(o.name))") }
                seen[g] = o.name
            }
        }
        for o in objs {
            for u in o.undefined where defined[u] == nil {
                err("undefined reference to '\(u)' (used in \(o.name))")
            }
        }
        guard diags.isEmpty else { return (nil, diags) }

        // 2. Lay out the sections.
        let exe = Executable()
        var textAt: [ObjectIdentifier: UInt32] = [:], dataAt: [ObjectIdentifier: UInt32] = [:]
        var tpos = exe.textBase, dpos = exe.dataBase
        for o in objs {
            textAt[ObjectIdentifier(o)] = tpos
            dpos = (dpos + 7) & ~7
            dataAt[ObjectIdentifier(o)] = dpos
            exe.objects.append(LinkedObject(name: o.name, textStart: tpos, textSize: o.text.count, dataStart: dpos, dataSize: o.data.count,
                                            isLibrary: o.isLibrary, reason: reasons[o.name] ?? "", summary: o.summary))
            tpos += UInt32(o.text.count)
            dpos += UInt32(o.data.count)
        }
        exe.text = []
        for o in objs { exe.text += o.text }
        exe.data = [UInt8](repeating: 0, count: Int(dpos - exe.dataBase))
        for o in objs {
            let base = Int(dataAt[ObjectIdentifier(o)]! - exe.dataBase)
            for (k, b) in o.data.enumerated() { exe.data[base + k] = b }
        }

        // 3. Symbol addresses.
        var globalAddr: [String: UInt32] = [:]
        for o in objs {
            for s in o.symbols.values {
                let base = s.section == .text ? textAt[ObjectIdentifier(o)]! : dataAt[ObjectIdentifier(o)]!
                let a = base + UInt32(s.offset)
                exe.symbols.append(LinkedSymbol(name: s.name, address: a, section: s.section, object: o.name, isGlobal: s.isGlobal))
                if s.isGlobal { globalAddr[s.name] = a }
            }
        }
        exe.symbols.sort { ($0.address, $0.name) < ($1.address, $1.name) }
        func resolve(_ name: String, in o: ObjectFile) -> UInt32? {
            if let s = o.symbols[name] {
                let base = s.section == .text ? textAt[ObjectIdentifier(o)]! : dataAt[ObjectIdentifier(o)]!
                return base + UInt32(s.offset)
            }
            return globalAddr[name]
        }

        // 4. Listing with final addresses.
        for o in objs {
            let base = textAt[ObjectIdentifier(o)]!
            for e in o.listing {
                exe.instructions.append(LinkedInstruction(address: base + UInt32(e.offset), word: e.word, decoded: Decoder.decode(e.word),
                                                          text: e.text, pseudo: e.pseudo, part: e.part, parts: e.parts, object: o.name,
                                                          asmLine: e.asmLine, origin: e.origin, function: e.function, unlinkedWord: e.word))
            }
            let dbase = dataAt[ObjectIdentifier(o)]!
            for d in o.dataListing {
                exe.dataEntries.append(LinkedData(address: dbase + UInt32(d.offset), size: d.size, text: d.text, object: o.name, origin: d.origin))
            }
        }
        exe.instructions.sort { $0.address < $1.address }

        // 5. Apply relocations.
        func word(_ a: UInt32) -> UInt32 {
            let k = Int(a - exe.textBase)
            return UInt32(exe.text[k]) | UInt32(exe.text[k + 1]) << 8 | UInt32(exe.text[k + 2]) << 16 | UInt32(exe.text[k + 3]) << 24
        }
        func setWord(_ a: UInt32, _ w: UInt32, data: Bool) {
            if data {
                let k = Int(a - exe.dataBase)
                for i in 0..<4 { exe.data[k + i] = UInt8((w >> UInt32(8 * i)) & 0xFF) }
            } else {
                let k = Int(a - exe.textBase)
                for i in 0..<4 { exe.text[k + i] = UInt8((w >> UInt32(8 * i)) & 0xFF) }
            }
        }
        for o in objs {
            let tbase = textAt[ObjectIdentifier(o)]!, dbase = dataAt[ObjectIdentifier(o)]!
            for r in o.relocations {
                guard let s = resolve(r.symbol, in: o) else {
                    err("undefined reference to '\(r.symbol)' (used in \(o.name))")
                    continue
                }
                let target = s &+ UInt32(bitPattern: r.addend)
                let p = (r.section == .text ? tbase : dbase) + UInt32(r.offset)
                if r.type == .abs32 {
                    setWord(p, target, data: r.section == .data)
                    exe.relocations.append(RelocationRecord(object: o.name, type: r.type, symbol: r.symbol, address: p, target: target,
                                                            before: 0, after: target, value: Int32(bitPattern: target)))
                    continue
                }
                let before = word(p)
                var imm: Int32
                switch r.type {
                case .branch, .jal:
                    imm = Int32(bitPattern: target &- p)
                    let limit: Int32 = r.type == .branch ? 4096 : 1 << 20
                    if imm < -limit || imm >= limit { err("\(o.name): relocation \(r.type.rawValue) to '\(r.symbol)' is out of range") }
                case .pcrelHi20:
                    let v = Int32(bitPattern: target &- p)
                    imm = ((v &+ 0x800) >> 12) & 0xFFFFF
                case .pcrelLo12I, .pcrelLo12S:
                    let hiP = tbase + UInt32(r.hiOffset ?? r.offset)
                    let v = Int32(bitPattern: target &- hiP)
                    imm = v &- (((v &+ 0x800) >> 12) << 12)
                case .hi20:
                    imm = ((Int32(bitPattern: target) &+ 0x800) >> 12) & 0xFFFFF
                case .lo12I, .lo12S:
                    let v = Int32(bitPattern: target)
                    imm = (v << 20) >> 20
                case .abs32:
                    imm = 0
                }
                guard let after = Encoder.patch(before, imm: imm) else {
                    err("\(o.name): cannot patch the instruction at \(Format.hex(p))")
                    continue
                }
                setWord(p, after, data: false)
                exe.relocations.append(RelocationRecord(object: o.name, type: r.type, symbol: r.symbol, address: p, target: target,
                                                        before: before, after: after, value: imm))
                if let k = exe.instructions.firstIndex(where: { $0.address == p }) {
                    exe.instructions[k].word = after
                    exe.instructions[k].decoded = Decoder.decode(after)
                }
            }
        }
        guard let e = globalAddr[entry] else {
            err("no entry point '\(entry)'")
            return (nil, diags)
        }
        exe.entry = e
        exe.finish()
        return (diags.contains { $0.severity == .error } ? nil : exe, diags)
    }
}
