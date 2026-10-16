import AppKit
import SiliconCore

/// Binary text with each encoding field on its own coloured background.
func coloredBinary(_ d: Decoded, font: NSFont = Fonts.mono(10.5)) -> NSAttributedString {
    let out = NSMutableAttributedString()
    for (k, f) in Decoder.fields(d).enumerated() {
        let bits = binaryString(f.3, bits: f.1 - f.2 + 1)
        out.append(NSAttributedString(string: bits, attributes: [.font: font, .foregroundColor: Theme.text, .backgroundColor: Theme.fieldColor(f.0)]))
        if k < Decoder.fields(d).count - 1 { out.add(" ", font) }
    }
    return out
}

let opcodeNames: [UInt32: String] = [
    0b0110011: "OP (register–register ALU)", 0b0010011: "OP-IMM (ALU with immediate)", 0b0000011: "LOAD", 0b0100011: "STORE",
    0b1100011: "BRANCH", 0b1101111: "JAL", 0b1100111: "JALR", 0b0110111: "LUI", 0b0010111: "AUIPC", 0b1110011: "SYSTEM", 0b0001111: "MISC-MEM",
]

/// The "green card" view of one instruction word: 32 bits split into fields.
final class EncodingCanvas: FlippedView {
    var ins: LinkedInstruction?
    var symbolize: ((UInt32) -> String?)?

    override func draw(_ dirtyRect: NSRect) {
        Theme.panel.setFill()
        bounds.fill()
        guard let ins, let d = ins.decoded else {
            drawText("Click an instruction below to see how it is encoded.", at: NSPoint(x: 16, y: 16), Fonts.body, Theme.secondary)
            return
        }
        let title = NSMutableAttributedString()
        title.append(highlightAsm(Decoder.disassemble(d, pc: ins.address, symbol: symbolize), font: Fonts.mono(15, .medium)))
        title.add("    \(d.op.spec.format.rawValue)-type", Fonts.serif(13, .semibold), Theme.heading)
        title.draw(at: NSPoint(x: 16, y: 10))
        let sub = "\(d.op.summary)   ·   at \(hexAddr(ins.address))   ·   word 0x\(hex8(ins.word))" + (ins.pseudo.map { "   ·   from the pseudo-instruction  \($0)" + (ins.parts > 1 ? " (part \(ins.part) of \(ins.parts))" : "") } ?? "")
        drawText(sub, in: NSRect(x: 16, y: 34, width: bounds.width - 32, height: 16), Fonts.small, Theme.secondary)
        let fields = Decoder.fields(d)
        let x0: CGFloat = 16, w = bounds.width - 32
        let bw = w / 32
        let top: CGFloat = 62
        // Bit numbers.
        for b in 0..<32 {
            let x = x0 + CGFloat(31 - b) * bw
            if b == 31 || b == 0 || fields.contains(where: { $0.1 == b || $0.2 == b }) {
                drawText("\(b)", in: NSRect(x: x, y: top - 14, width: bw, height: 12), Fonts.mono(8.5), Theme.faint, align: .center)
            }
        }
        for f in fields {
            let fx = x0 + CGFloat(31 - f.1) * bw
            let fw = CGFloat(f.1 - f.2 + 1) * bw
            let r = NSRect(x: fx, y: top, width: fw, height: 26)
            Theme.fieldColor(f.0).setFill()
            r.fill()
            for b in f.2...f.1 {
                let bit = (ins.word >> UInt32(b)) & 1
                let bx = x0 + CGFloat(31 - b) * bw
                drawText(bit == 1 ? "1" : "0", in: NSRect(x: bx, y: top, width: bw, height: 26), Fonts.mono(12, bit == 1 ? .bold : .regular), bit == 1 ? Theme.text : Theme.secondary, align: .center)
                if b != f.2 { line(NSPoint(x: bx, y: top + 4), NSPoint(x: bx, y: top + 22), Theme.text.alpha(0.08)) }
            }
            Theme.borderStrong.setStroke()
            NSBezierPath(rect: r).stroke()
            drawText(f.0, in: NSRect(x: fx, y: top + 30, width: fw, height: 14), Fonts.mono(9.5, .semibold), Theme.text, align: .center)
            var meaning = ""
            switch f.0 {
            case "rd", "rs1", "rs2": meaning = "x\(f.3) \(Reg.name(Int(f.3)))"
            case "opcode": meaning = opcodeNames[f.3].map { $0.components(separatedBy: " ").first! } ?? "?"
            case "funct3", "funct7": meaning = "\(f.3)"
            case "shamt": meaning = "\(f.3)"
            default: meaning = ""
            }
            if !meaning.isEmpty { drawText(meaning, in: NSRect(x: fx, y: top + 44, width: fw, height: 14), Fonts.mono(9.5), Theme.secondary, align: .center) }
        }
        // How the immediate is assembled.
        var y = top + 66
        if [.I, .S, .B, .U, .J].contains(d.op.spec.format), !(d.op == .ecall || d.op == .ebreak) {
            var s = "immediate = \(d.imm)"
            switch d.op.spec.format {
            case .B: s += "  — branch offset in bytes (bit 0 is always 0, so it is not stored); target = pc + \(d.imm) = \(hexAddr(ins.address &+ UInt32(bitPattern: d.imm)))"
            case .J: s += "  — jump offset in bytes; target = pc + \(d.imm) = \(hexAddr(ins.address &+ UInt32(bitPattern: d.imm)))"
            case .U: s = "upper immediate = 0x\(String(UInt32(bitPattern: d.imm) & 0xFFFFF, radix: 16)) — placed in bits 31…12 of rd"
            case .S: s += "  — split in two pieces so that rs1/rs2 stay in the same bit positions as in R-type"
            default: s += d.op.isShiftImm ? "" : "  — sign-extended from 12 bits"
            }
            drawText(s, in: NSRect(x: 16, y: y, width: bounds.width - 32, height: 16), Fonts.small, Theme.text)
            y += 18
        }
        if let n = opcodeNames[d.raw & 0x7F] {
            drawText("opcode \(binaryString(d.raw & 0x7F, bits: 7)) = \(n)" + (d.op.spec.format == .R || d.op.spec.format == .I || d.op.spec.format == .S || d.op.spec.format == .B ? "; funct3\(d.op.spec.format == .R || d.op.isShiftImm ? "/funct7" : "") choose the exact operation" : ""),
                     in: NSRect(x: 16, y: y, width: bounds.width - 32, height: 16), Fonts.small, Theme.secondary)
        }
    }
}

final class MachineCodeView: NSView, StationView {
    let encoding = EncodingCanvas()
    let listing = ListPanel()
    let scope = Segmented(["Your program", "Everything linked"])
    let empty = EmptyNote()
    weak var model: AppModel?
    private var selectedPC: UInt32?

    override init(frame: NSRect) {
        super.init(frame: frame)
        for v in [encoding, listing, scope, empty] as [NSView] { addSubview(v) }
        scope.onChange = { [weak self] _ in
            guard let m = self?.model else { return }
            self?.rebuild(m)
        }
        listing.list.onClick = { [weak self] _, r in
            guard let self, let m = self.model, let pc = r.pc else { return }
            self.selectedPC = pc
            m.selection = Selection(line: r.line, pc: pc, origin: .machine)
        }
        listing.list.highlight = { [weak self] r in
            guard let m = self?.model else { return nil }
            if let pc = m.selection.pc, r.pc == pc { return Theme.selectionStrong.alpha(0.6) }
            if let l = m.selection.line, r.line == l, r.pc != nil { return Theme.selection }
            return nil
        }
    }
    required init?(coder: NSCoder) { fatalError() }

    override var isFlipped: Bool { true }

    override func layout() {
        super.layout()
        encoding.frame = NSRect(x: 0, y: 0, width: bounds.width, height: 170)
        let s = scope.intrinsicContentSize
        scope.frame = NSRect(x: bounds.width - s.width - 12, y: 176, width: s.width, height: 24)
        listing.frame = NSRect(x: 0, y: 206, width: bounds.width, height: bounds.height - 206)
        empty.frame = bounds
    }

    override func draw(_ dirtyRect: NSRect) {
        Theme.panel.setFill()
        bounds.fill()
        line(NSPoint(x: 0, y: 170.5), NSPoint(x: bounds.width, y: 170.5), Theme.border)
        drawText("Address", at: NSPoint(x: 12, y: 186), Fonts.mono(9.5, .semibold), Theme.faint)
        drawText("Hex", at: NSPoint(x: 92, y: 186), Fonts.mono(9.5, .semibold), Theme.faint)
        drawText("Binary, split into fields", at: NSPoint(x: 168, y: 186), Fonts.mono(9.5, .semibold), Theme.faint)
        drawText("Instruction", at: NSPoint(x: 460, y: 186), Fonts.mono(9.5, .semibold), Theme.faint)
        line(NSPoint(x: 0, y: 205.5), NSPoint(x: bounds.width, y: 205.5), Theme.border)
    }

    private func rebuild(_ model: AppModel) {
        guard let exe = model.shown?.executable else { return }
        var rows: [Row] = []
        var lastObj = "", lastFn = ""
        for ins in exe.instructions {
            if scope.selected == 0 && ins.object != "main.o" { continue }
            if ins.object != lastObj {
                lastObj = ins.object
                let o = exe.objects.first { $0.name == ins.object }
                rows.append(.header(ins.object, detail: o?.summary ?? ""))
            }
            let fn = exe.function(at: ins.address) ?? ""
            if fn != lastFn {
                lastFn = fn
                var r = Row([Cell(NSAttributedString(fn + ":", Fonts.mono(11.5, .semibold), Theme.label))], height: 20)
                r.kind = .section
                rows.append(r)
            }
            guard let d = ins.decoded else { continue }
            let asmText = Decoder.disassemble(d, pc: ins.address) { exe.label(at: $0) ?? exe.symbolize($0) }
            var r = Row([Cell(String(format: "%08x", ins.address), Fonts.mono(10.5), Theme.secondary, width: 80),
                         Cell(hex8(ins.word), Fonts.mono(10.5), Theme.number, width: 76),
                         Cell(coloredBinary(d), width: 292),
                         Cell(highlightAsm(asmText, font: Fonts.mono(11.5)), width: 230),
                         Cell(ins.pseudo.map { "← " + $0 } ?? "", Fonts.mono(10.5), Theme.purple)])
            r.pc = ins.address
            r.line = model.sourceLine(ins)
            rows.append(r)
        }
        listing.list.minWidth = 900
        listing.set(rows)
    }

    func reload(_ model: AppModel, _ event: ModelEvent) {
        self.model = model
        guard let c = model.shown, let exe = c.executable else {
            empty.isHidden = false
            empty.title = "No machine code"
            empty.message = "The program has to compile and link first."
            empty.needsDisplay = true
            return
        }
        empty.isHidden = true
        if event == .compiled || listing.list.rows.isEmpty { rebuild(model) }
        // Show the selected instruction, or the first one of the selected line.
        var pc = model.selection.pc
        if pc == nil, let l = model.selection.line {
            pc = exe.instructions.first { model.sourceLine($0) == l }?.address
        }
        if pc == nil, let p = model.pipeline, let rec = p.lastRecord, rec.stages[2].seq >= 0 { pc = rec.stages[2].pc }
        encoding.ins = (pc ?? selectedPC).flatMap { exe.instruction(at: $0) } ?? exe.instructions.first { $0.object == "main.o" }
        encoding.symbolize = { exe.label(at: $0) ?? exe.symbolize($0) }
        encoding.needsDisplay = true
        listing.list.needsDisplay = true
        if event == .selection, model.selection.origin != .machine, let p = pc, let k = listing.list.rows.firstIndex(where: { $0.pc == p }) {
            listing.list.reveal(k)
        }
    }
}

// MARK: - Linking

final class MemoryMapCanvas: FlippedView {
    var exe: Executable?
    var sp: UInt32?
    var brk: UInt32?

    override func draw(_ dirtyRect: NSRect) {
        Theme.panel.setFill()
        bounds.fill()
        guard let exe else { return }
        drawText("The process's memory", at: NSPoint(x: 14, y: 10), Fonts.subheading, Theme.heading)
        drawText("not to scale · addresses grow upwards", at: NSPoint(x: 14, y: 28), Fonts.tiny, Theme.secondary)
        let x: CGFloat = 104, w = bounds.width - x - 14
        struct Seg { var name: String; var lo: UInt32; var hi: UInt32; var color: NSColor; var h: CGFloat; var parts: [(String, UInt32, Int)] }
        let heapTop = max(brk ?? MemoryMap.heapBase, MemoryMap.heapBase)
        let segs: [Seg] = [
            Seg(name: "stack ↓", lo: sp ?? MemoryMap.stackTop, hi: MemoryMap.stackTop, color: NSColor(hex: 0xE3ECF8), h: 70, parts: []),
            Seg(name: "", lo: 0, hi: 0, color: .clear, h: 34, parts: []),
            Seg(name: "heap ↑ (malloc)", lo: MemoryMap.heapBase, hi: heapTop, color: NSColor(hex: 0xDDEEE3), h: 50, parts: []),
            Seg(name: "", lo: 0, hi: 0, color: .clear, h: 20, parts: []),
            Seg(name: ".data", lo: exe.dataBase, hi: exe.dataEnd, color: NSColor(hex: 0xF6E2C8), h: 90,
                parts: exe.objects.filter { $0.dataSize > 0 }.map { ($0.name, $0.dataStart, $0.dataSize) }),
            Seg(name: "", lo: 0, hi: 0, color: .clear, h: 30, parts: []),
            Seg(name: ".text (code)", lo: exe.textBase, hi: exe.textEnd, color: NSColor(hex: 0xEDE3F3), h: 150,
                parts: exe.objects.filter { $0.textSize > 0 }.map { ($0.name, $0.textStart, $0.textSize) }),
            Seg(name: "reserved", lo: 0, hi: exe.textBase, color: NSColor(hex: 0xF1EDE4), h: 26, parts: []),
        ]
        var y: CGFloat = 50
        for s in segs {
            if s.name.isEmpty {
                // A gap: a zig-zag break.
                let p = NSBezierPath()
                p.move(to: NSPoint(x: x + w / 2 - 10, y: y + 6))
                p.line(to: NSPoint(x: x + w / 2, y: y + s.h / 2))
                p.line(to: NSPoint(x: x + w / 2 + 10, y: y + s.h - 6))
                Theme.faint.setStroke()
                p.setLineDash([3, 3], count: 2, phase: 0)
                p.stroke()
                y += s.h
                continue
            }
            let r = NSRect(x: x, y: y, width: w, height: s.h)
            s.color.setFill()
            r.fill()
            Theme.borderStrong.setStroke()
            NSBezierPath(rect: r).stroke()
            if !s.parts.isEmpty {
                let total = s.parts.reduce(0) { $0 + $1.2 }
                let inner = s.h - 20
                var py = y + s.h
                for (name, _, size) in s.parts {
                    let ph = max(14, inner * CGFloat(size) / CGFloat(max(1, total)))
                    py -= ph
                    let pr = NSRect(x: x + 6, y: max(y + 20, py), width: w - 12, height: min(ph, py + ph - y - 20) - 2)
                    if pr.height > 8 {
                        Theme.panel.alpha(0.55).setFill()
                        pr.fill()
                        drawText("\(name)  \(size) B", in: pr.insetBy(dx: 6, dy: 0), Fonts.mono(9.5), Theme.text)
                    }
                }
                drawText(s.name, at: NSPoint(x: x + 8, y: y + 3), Fonts.mono(11, .semibold), Theme.text)
            } else {
                drawText(s.name, in: r.insetBy(dx: 10, dy: 0), Fonts.mono(11, .semibold), Theme.text)
            }
            drawText(hexAddr(s.hi), in: NSRect(x: 4, y: y - 6, width: x - 10, height: 12), Fonts.mono(9.5), Theme.secondary, align: .right)
            if s.name != "reserved" {
                drawText(hexAddr(s.lo), in: NSRect(x: 4, y: y + s.h - 7, width: x - 10, height: 12), Fonts.mono(9.5), Theme.secondary, align: .right)
            }
            y += s.h
        }
        if let sp {
            drawText("sp = \(hexAddr(sp))", at: NSPoint(x: x + 8, y: 50 + 52), Fonts.mono(10), Theme.ink)
        }
    }
}

final class LinkView: NSView, StationView {
    let listing = ListPanel()
    let map = MemoryMapCanvas()
    let empty = EmptyNote()
    weak var model: AppModel?

    override init(frame: NSRect) {
        super.init(frame: frame)
        for v in [listing, map, empty] as [NSView] { addSubview(v) }
        listing.list.onClick = { [weak self] _, r in
            guard let m = self?.model else { return }
            m.selection = Selection(line: r.line, pc: r.pc, origin: .link)
        }
        listing.list.highlight = { [weak self] r in
            guard let m = self?.model, let pc = m.selection.pc, r.pc == pc else { return nil }
            return Theme.selection
        }
    }
    required init?(coder: NSCoder) { fatalError() }

    override func layout() {
        super.layout()
        let mw: CGFloat = min(330, bounds.width * 0.36)
        listing.frame = NSRect(x: 0, y: 0, width: bounds.width - mw, height: bounds.height)
        map.frame = NSRect(x: bounds.width - mw, y: 0, width: mw, height: bounds.height)
        empty.frame = bounds
    }

    func reload(_ model: AppModel, _ event: ModelEvent) {
        self.model = model
        guard let c = model.shown, let exe = c.executable else {
            empty.isHidden = false
            empty.title = "Nothing linked"
            empty.message = c(model)
            empty.needsDisplay = true
            return
        }
        empty.isHidden = true
        map.exe = exe
        map.sp = model.pipeline.map { $0.register(Reg.sp) }
        map.brk = model.pipeline?.sys.brk
        map.needsDisplay = true
        guard event == .compiled || listing.list.rows.isEmpty else { return }
        var rows: [Row] = []
        rows.append(.header("Object files", detail: "the linker starts with crt0.o and main.o, then pulls library members in as needed"))
        for o in exe.objects {
            var r = Row([Cell(NSAttributedString(o.name, Fonts.mono(12, .semibold), o.isLibrary ? Theme.purple : Theme.ink), width: 100),
                         Cell("\(o.textSize) B code" + (o.dataSize > 0 ? ", \(o.dataSize) B data" : ""), Fonts.mono(10.5), Theme.secondary, width: 150),
                         Cell(o.isLibrary ? "library: \(o.reason)" : o.name == "crt0.o" ? "start-up code: sets up the stack, calls main, then exit" : "your program", Fonts.small, Theme.text)], height: 22)
            r.tooltip = o.summary
            rows.append(r)
        }
        rows.append(.spacer())
        let interesting = exe.relocations
        rows.append(.header("Relocations", detail: "\(interesting.count) places the assembler could not finish: addresses only the linker knows"))
        rows.append({ var r = Row([Cell("AT", Fonts.mono(9.5, .semibold), Theme.faint, width: 80), Cell("TYPE", Fonts.mono(9.5, .semibold), Theme.faint, width: 160),
                                   Cell("SYMBOL → ADDRESS", Fonts.mono(9.5, .semibold), Theme.faint, width: 190), Cell("BEFORE → AFTER", Fonts.mono(9.5, .semibold), Theme.faint)], height: 18); r.kind = .section; return r }())
        for rel in interesting.prefix(400) {
            let before = rel.type == .abs32 ? "(data word)" : hex8(rel.before)
            var r = Row([Cell(String(format: "%08x", rel.address), Fonts.mono(10.5), Theme.secondary, width: 80),
                         Cell(rel.type.rawValue.replacingOccurrences(of: "R_RISCV_", with: ""), Fonts.mono(10.5), Theme.purple, width: 160),
                         Cell(NSAttributedString("\(rel.symbol) → \(String(format: "%08x", rel.target))", Fonts.mono(10.5), Theme.label), width: 190),
                         Cell("\(before) → \(hex8(rel.after))", Fonts.mono(10.5), Theme.text)])
            r.tooltip = rel.type.explanation
            r.pc = rel.type == .abs32 ? nil : rel.address
            r.line = exe.instruction(at: rel.address).flatMap { model.sourceLine($0) }
            rows.append(r)
        }
        rows.append(.spacer())
        rows.append(.header("Symbol table", detail: "every label with its final address · .L labels are local to their file"))
        for s in exe.symbols where !s.name.hasPrefix(".L") || s.name.hasPrefix(".LC") {
            var r = Row([Cell(String(format: "%08x", s.address), Fonts.mono(10.5), Theme.secondary, width: 80),
                         Cell(NSAttributedString(s.name, Fonts.mono(11.5, .semibold), s.section == .text ? Theme.label : Theme.string), width: 160),
                         Cell(s.section.rawValue, Fonts.mono(10.5), Theme.secondary, width: 60),
                         Cell(s.object, Fonts.mono(10.5), Theme.secondary, width: 90),
                         Cell(s.isGlobal ? "global" : "local", Fonts.small, s.isGlobal ? Theme.ink : Theme.faint)])
            r.pc = s.section == .text ? s.address : nil
            rows.append(r)
        }
        listing.set(rows)
    }

    private func c(_ m: AppModel) -> String {
        if let s = m.compilation?.failedStage { return "Stopped at: \(s.rawValue)." }
        return "Compile the program first."
    }
}
