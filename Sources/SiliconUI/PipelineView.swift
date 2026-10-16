import AppKit
import SiliconCore

func shortAsm(_ exe: Executable, _ pc: UInt32) -> String {
    guard let ins = exe.instruction(at: pc), let d = ins.decoded else { return "?" }
    return Decoder.disassemble(d, pc: pc) { exe.label(at: $0) ?? exe.symbolize($0) }
}

// MARK: - Datapath

/// Small values in decimal, addresses and big patterns in hex.
func regValueText(_ v: UInt32) -> String {
    let i = Int32(bitPattern: v)
    return abs(Int(i)) < 100_000 ? String(i) : "0x" + String(v, radix: 16)
}

func accessText(_ a: MemoryAccess, _ cache: String) -> (String, NSColor) {
    switch a.servedBy {
    case "memory": return ("ideal memory", Theme.secondary)
    case "L1": return ("\(cache) hit", Theme.green)
    default: return ("miss → \(a.servedBy), +\(a.penalty) cycles", Theme.crimson)
    }
}

/// A simplified version of the pipelined datapath in Patterson & Hennessy (Figure 4.x).
final class DatapathCanvas: FlippedView {
    weak var model: AppModel?

    private func inFlight(_ p: Pipeline, seq: Int) -> InFlight? {
        [p.ifid, p.idex, p.exmem, p.memwb].compactMap { $0 }.first { $0.seq == seq }
    }

    override func draw(_ dirtyRect: NSRect) {
        Theme.panel.setFill()
        bounds.fill()
        for gx in stride(from: CGFloat(0), to: bounds.width, by: 16) { line(NSPoint(x: gx, y: 0), NSPoint(x: gx, y: bounds.height), Theme.grid.alpha(0.7)) }
        for gy in stride(from: CGFloat(0), to: bounds.height, by: 16) { line(NSPoint(x: 0, y: gy), NSPoint(x: bounds.width, y: gy), Theme.grid.alpha(0.7)) }
        guard let m = model, let p = m.pipeline, let exe = m.good?.executable else {
            drawText("Compile a program to see it run.", at: NSPoint(x: 20, y: 20), Fonts.body, Theme.secondary)
            return
        }
        let rec = p.lastRecord
        let W = bounds.width, H = bounds.height
        let regW: CGFloat = 12
        let colW = (W - 20 - 4 * regW) / 5
        func colX(_ k: Int) -> CGFloat { 10 + CGFloat(k) * (colW + regW) }
        let top: CGFloat = 56, bottom = H - 30
        let midY = (top + bottom) / 2

        // Stage headers with the instruction in each stage.
        for k in 0..<5 {
            let x = colX(k)
            let slot = rec?.stages[k] ?? .bubble
            let head = NSRect(x: x + 2, y: 6, width: colW - 4, height: 44)
            var fill = Theme.stageFill(k)
            var text = "—"
            var sub = PipeStage(rawValue: k)!.longName
            var color = Theme.stage[k]
            switch slot.status {
            case .bubble:
                fill = NSColor(hex: 0xF1EDE4)
                text = "bubble"
                color = Theme.faint
            case .flushed:
                fill = Theme.errorFill
                text = shortAsm(exe, slot.pc)
                sub = "✕ flushed: wrong path"
                color = Theme.crimson
            case .stalled:
                text = shortAsm(exe, slot.pc)
                sub = "stalled: \(rec?.stall?.rawValue ?? "")"
                fill = NSColor(hex: 0xEDE9E0)
                color = Theme.secondary
            case .waiting:
                text = shortAsm(exe, slot.pc)
                sub = k == 0 ? "waiting for the I-cache miss" : "waiting for the D-cache miss"
                fill = NSColor(hex: 0xFFF1C9)
                color = Theme.amber
            case .normal:
                text = slot.seq >= 0 ? shortAsm(exe, slot.pc) : "—"
            }
            if slot.seq >= 0 && slot.status != .bubble && k == 0 && slot.pc >= exe.textEnd { text = "(past the end of code)" }
            fillRounded(head, 6, fill)
            strokeRounded(head, 6, color.alpha(0.6), 1.2)
            let tag = PipeStage(rawValue: k)!.name
            _ = drawPill(tag, at: NSPoint(x: head.minX + 6, y: head.minY + 5), font: Fonts.mono(9.5, .bold), fill: Theme.stage[k], text: .white)
            let tx = head.minX + 6 + textSize(tag, Fonts.mono(9.5, .bold)).width + 16
            drawText(text, in: NSRect(x: tx, y: head.minY + 3, width: head.maxX - tx - 4, height: 18),
                     Fonts.mono(colW < 190 ? 10.5 : 11.5, .semibold), slot.status == .bubble ? Theme.faint : Theme.text)
            drawText(sub, in: NSRect(x: head.minX + 8, y: head.minY + 24, width: head.width - 14, height: 16), Fonts.tiny, slot.status == .normal ? Theme.secondary : color)
        }
        // Pipeline registers.
        let names = ["IF/ID", "ID/EX", "EX/MEM", "MEM/WB"]
        for k in 0..<4 {
            let x = colX(k + 1) - regW
            let r = NSRect(x: x, y: top, width: regW, height: bottom - top)
            fillRounded(r, 3, NSColor(hex: 0xE9E1D1))
            strokeRounded(r, 3, Theme.borderStrong)
            NSGraphicsContext.saveGraphicsState()
            let t = NSAffineTransform()
            t.translateX(by: x + 2, yBy: bottom - 6)
            t.rotate(byDegrees: -90)
            t.concat()
            drawText(names[k], at: .zero, Fonts.mono(8.5, .bold), Theme.secondary)
            NSGraphicsContext.restoreGraphicsState()
        }

        func box(_ r: NSRect, _ title: String, active: Bool, _ k: Int, lit: NSColor? = nil) {
            fillRounded(r, 5, lit ?? (active ? Theme.stageFill(k) : Theme.panel))
            strokeRounded(r, 5, active ? Theme.stage[k] : Theme.borderStrong, active ? 1.4 : 1)
            drawText(title, in: r.insetBy(dx: 4, dy: 0), Fonts.ui(10.5, .semibold), active ? Theme.text : Theme.secondary, align: .center)
        }
        func wire(_ a: NSPoint, _ b: NSPoint, active: Bool, _ k: Int, arrow: Bool = true) {
            line(a, b, active ? Theme.stage[k] : Theme.borderStrong, active ? 2 : 1.2)
            if arrow { arrowHead(at: b, from: a, active ? Theme.stage[k] : Theme.borderStrong, size: 5) }
        }
        func value(_ s: String, at pt: NSPoint, _ color: NSColor = Theme.ink) {
            drawText(s, at: pt, Fonts.mono(9.5), color)
        }
        let active = (0..<5).map { (rec?.stages[$0].seq ?? -1) >= 0 && rec?.stages[$0].status != .flushed }

        // IF
        let x0 = colX(0)
        let pcBox = NSRect(x: x0 + 8, y: midY - 18, width: 34, height: 36)
        box(pcBox, "PC", active: active[0], 0)
        let imem = NSRect(x: x0 + 58, y: midY - 34, width: colW - 70, height: 68)
        let ifWait = rec?.stages[0].status == .waiting
        box(imem, "Instruction\nmemory", active: active[0], 0, lit: ifWait ? NSColor(hex: 0xFFF1C9) : nil)
        drawText("I-cache", in: NSRect(x: imem.minX, y: imem.maxY - 16, width: imem.width, height: 12), Fonts.tiny, Theme.secondary, align: .center)
        wire(NSPoint(x: pcBox.maxX, y: midY), NSPoint(x: imem.minX, y: midY), active: active[0], 0)
        wire(NSPoint(x: imem.maxX, y: midY), NSPoint(x: colX(1) - regW, y: midY), active: active[0], 0)
        let adder = NSRect(x: x0 + 8, y: top + 6, width: 34, height: 22)
        box(adder, "+4", active: active[0], 0)
        if let s = rec?.stages[0], s.seq >= 0 { value("pc = " + hexAddr(s.pc), at: NSPoint(x: pcBox.minX - 2, y: imem.maxY + 16)) }
        if let f = rec?.fetch {
            let (t, c) = accessText(f, "I-cache")
            drawText(t, in: NSRect(x: pcBox.minX - 2, y: imem.maxY + 2, width: colW - 4, height: 13), Fonts.mono(9.5), c)
        }

        // ID
        let x1 = colX(1)
        let regs = NSRect(x: x1 + 20, y: midY - 40, width: colW - 60, height: 80)
        box(regs, "Registers", active: active[1], 1)
        let imm = NSRect(x: x1 + 20, y: bottom - 34, width: colW - 60, height: 24)
        box(imm, "Imm gen", active: active[1], 1)
        let hazard = NSRect(x: x1 + 14, y: top + 4, width: colW - 28, height: 24)
        let stallNow = rec?.stall != nil && rec?.stall != .dcache && rec?.stall != .icache
        box(hazard, stallNow ? "Hazard unit: STALL" : "Hazard detection", active: stallNow, 1, lit: stallNow ? Theme.errorFill : nil)
        wire(NSPoint(x: x1, y: midY), NSPoint(x: regs.minX, y: midY), active: active[1], 1)
        wire(NSPoint(x: regs.maxX, y: midY - 18), NSPoint(x: colX(2) - regW, y: midY - 18), active: active[1], 1)
        wire(NSPoint(x: regs.maxX, y: midY + 18), NSPoint(x: colX(2) - regW, y: midY + 18), active: active[1], 1)
        wire(NSPoint(x: imm.maxX, y: imm.midY), NSPoint(x: colX(2) - regW, y: imm.midY), active: active[1], 1)
        if let s = rec?.stages[1], s.seq >= 0, let x = p.idex, x.seq == s.seq, let d = x.d {
            let srcs = d.op == .ecall ? [Reg.a0, Reg.a7] : [d.rs1, d.rs2].filter { $0 != 0 }
            var y = regs.maxY + 3
            for (j, r) in srcs.prefix(2).enumerated() where (j == 0 && (d.op.readsRs1 || d.op == .ecall)) || (j == 1 && (d.op.readsRs2 || d.op == .ecall)) {
                value("\(Reg.name(r)) = \(Int32(bitPattern: j == 0 ? x.a : x.b))", at: NSPoint(x: regs.minX, y: y))
                y += 12
            }
        }
        if rec?.forwardID == true {
            drawText("forwarded into the branch comparator", in: NSRect(x: x1 + 4, y: hazard.maxY + 2, width: colW, height: 12), Fonts.tiny, Theme.amber)
        }

        // EX
        let x2 = colX(2)
        let alu = NSBezierPath()
        let ax = x2 + colW * 0.42, ay = midY
        alu.move(to: NSPoint(x: ax, y: ay - 42))
        alu.line(to: NSPoint(x: ax + 44, y: ay - 18))
        alu.line(to: NSPoint(x: ax + 44, y: ay + 18))
        alu.line(to: NSPoint(x: ax, y: ay + 42))
        alu.line(to: NSPoint(x: ax, y: ay + 10))
        alu.line(to: NSPoint(x: ax + 10, y: ay))
        alu.line(to: NSPoint(x: ax, y: ay - 10))
        alu.close()
        (active[2] ? Theme.stageFill(2) : Theme.panel).setFill()
        alu.fill()
        (active[2] ? Theme.stage[2] : Theme.borderStrong).setStroke()
        alu.lineWidth = active[2] ? 1.5 : 1
        alu.stroke()
        drawText("ALU", at: NSPoint(x: ax + 16, y: ay - 7), Fonts.ui(11, .bold), active[2] ? Theme.text : Theme.secondary)
        let muxA = NSRect(x: x2 + 14, y: midY - 34, width: 16, height: 30)
        let muxB = NSRect(x: x2 + 14, y: midY + 4, width: 16, height: 30)
        for mx in [muxA, muxB] {
            fillRounded(mx, 7, Theme.panel)
            strokeRounded(mx, 7, Theme.borderStrong)
        }
        wire(NSPoint(x: x2, y: midY - 18), NSPoint(x: muxA.minX, y: midY - 18), active: active[2], 2, arrow: false)
        wire(NSPoint(x: x2, y: midY + 18), NSPoint(x: muxB.minX, y: midY + 18), active: active[2], 2, arrow: false)
        wire(NSPoint(x: muxA.maxX, y: muxA.midY), NSPoint(x: ax, y: ay - 26), active: active[2], 2)
        wire(NSPoint(x: muxB.maxX, y: muxB.midY), NSPoint(x: ax, y: ay + 26), active: active[2], 2)
        wire(NSPoint(x: ax + 44, y: ay), NSPoint(x: colX(3) - regW, y: ay), active: active[2], 2)
        let fwdUnit = NSRect(x: x2 + 10, y: bottom - 30, width: colW - 20, height: 22)
        let fwdActive = (rec?.forwardA ?? .none) != .none || (rec?.forwardB ?? .none) != .none
        box(fwdUnit, m.pipelineConfig.forwarding ? "Forwarding unit" : "Forwarding: off", active: fwdActive, 2,
            lit: fwdActive ? NSColor(hex: 0xFFF1C9) : nil)
        if let s = rec?.stages[2], s.seq >= 0, let x = p.exmem, x.seq == s.seq, let d = x.d {
            drawText("= \(Int32(bitPattern: x.alu))", in: NSRect(x: x2 + 4, y: ay + 46, width: colW - 8, height: 13), Fonts.mono(9.5), Theme.ink, align: .center)
            if d.op.isBranch || d.op.isJump, x.resolved {
                drawText(x.taken ? "taken → \(hexAddr(x.target))" : "not taken", in: NSRect(x: x2 + 6, y: top + 6, width: colW - 10, height: 14), Fonts.tiny, x.taken ? Theme.crimson : Theme.green)
            }
        }

        // MEM
        let x3 = colX(3)
        let dmem = NSRect(x: x3 + 22, y: midY - 34, width: colW - 50, height: 68)
        let memWait = rec?.stages[3].status == .waiting
        box(dmem, "Data\nmemory", active: active[3] && (p.exmem?.d?.op.isLoad == true || p.exmem?.d?.op.isStore == true || rec?.data != nil), 3,
            lit: memWait ? NSColor(hex: 0xFFF1C9) : nil)
        drawText("D-cache", in: NSRect(x: dmem.minX, y: dmem.maxY - 16, width: dmem.width, height: 12), Fonts.tiny, Theme.secondary, align: .center)
        wire(NSPoint(x: x3, y: midY), NSPoint(x: dmem.minX, y: midY), active: active[3], 3)
        wire(NSPoint(x: dmem.maxX, y: midY), NSPoint(x: colX(4) - regW, y: midY), active: active[3], 3)
        if let a = rec?.data {
            value("\(a.kind == .store ? "store" : "load") \(hexAddr(a.address))", at: NSPoint(x: dmem.minX - 4, y: dmem.maxY + 3))
            let (t, c) = accessText(a, "D-cache")
            drawText(t, in: NSRect(x: dmem.minX - 4, y: dmem.maxY + 15, width: colW - 10, height: 13), Fonts.mono(9.5), c)
        }

        // WB
        let x4 = colX(4)
        let wbMux = NSRect(x: x4 + colW * 0.35, y: midY - 22, width: 18, height: 44)
        fillRounded(wbMux, 8, active[4] ? Theme.stageFill(4) : Theme.panel)
        strokeRounded(wbMux, 8, active[4] ? Theme.stage[4] : Theme.borderStrong)
        wire(NSPoint(x: x4, y: midY), NSPoint(x: wbMux.minX, y: midY), active: active[4], 4)
        if let (r, v) = rec?.registerWrite {
            drawText("\(Reg.name(r)) ← \(regValueText(v))", in: NSRect(x: x4 + 4, y: midY + 28, width: colW - 8, height: 14), Fonts.mono(10.5, .semibold), Theme.stage[4], align: .center)
        }
        // Write-back path to the register file.
        let wbY = bottom - 4
        let back = NSBezierPath()
        back.move(to: NSPoint(x: wbMux.maxX, y: midY))
        back.line(to: NSPoint(x: wbMux.maxX + 14, y: midY))
        back.line(to: NSPoint(x: wbMux.maxX + 14, y: wbY))
        back.line(to: NSPoint(x: regs.midX, y: wbY))
        back.line(to: NSPoint(x: regs.midX, y: regs.maxY))
        back.lineWidth = rec?.registerWrite != nil ? 2 : 1
        (rec?.registerWrite != nil ? Theme.stage[4] : Theme.borderStrong).setStroke()
        back.stroke()

        // Forwarding paths into the ALU muxes.
        func forwardPath(_ src: ForwardSource, into mux: NSRect, offset: CGFloat) {
            guard src != .none else { return }
            let fromX = src == .exmem ? colX(3) - regW / 2 : colX(4) - regW / 2
            let c = src == .exmem ? Theme.amber : Theme.purple
            let p = NSBezierPath()
            let y = bottom - 40 - offset
            p.move(to: NSPoint(x: fromX, y: midY + 4))
            p.line(to: NSPoint(x: fromX, y: y))
            p.line(to: NSPoint(x: mux.minX - 6, y: y))
            p.line(to: NSPoint(x: mux.minX - 6, y: mux.midY + 6))
            p.line(to: NSPoint(x: mux.minX, y: mux.midY + 6))
            p.lineWidth = 2.4
            c.setStroke()
            p.setLineDash([6, 3], count: 2, phase: 0)
            p.stroke()
            arrowHead(at: NSPoint(x: mux.minX, y: mux.midY + 6), from: NSPoint(x: mux.minX - 6, y: mux.midY + 6), c)
            drawText("from \(src.rawValue)", at: NSPoint(x: mux.maxX + 6, y: y - 12), Fonts.mono(9, .semibold), c)
        }
        forwardPath(rec?.forwardA ?? .none, into: muxA, offset: 0)
        forwardPath(rec?.forwardB ?? .none, into: muxB, offset: 8)

        // Branch redirect back to the PC.
        if let r = rec, let target = r.redirect {
            let stageX = m.pipelineConfig.branchStage == .id ? colX(1) + colW / 2 : m.pipelineConfig.branchStage == .ex ? colX(2) + colW / 2 : colX(3) + colW / 2
            let y = top - 2
            let p = NSBezierPath()
            p.move(to: NSPoint(x: stageX, y: top + 30))
            p.line(to: NSPoint(x: stageX, y: y))
            p.line(to: NSPoint(x: pcBox.midX, y: y))
            p.line(to: NSPoint(x: pcBox.midX, y: pcBox.minY))
            p.lineWidth = 2.2
            Theme.crimson.setStroke()
            p.stroke()
            arrowHead(at: NSPoint(x: pcBox.midX, y: pcBox.minY), from: NSPoint(x: pcBox.midX, y: y), Theme.crimson)
            let msg = r.flushed > 0 ? "\(r.flushReason): flush \(r.flushed), restart at \(hexAddr(target))" : "jump to \(hexAddr(target))"
            let sz = textSize(msg, Fonts.ui(10.5, .semibold))
            let lr = NSRect(x: max(colX(0) + 50, stageX - sz.width - 14), y: y - 9, width: sz.width + 12, height: 17)
            fillRounded(lr, 5, Theme.errorFill)
            drawText(msg, at: NSPoint(x: lr.minX + 6, y: lr.minY + 1.5), Fonts.ui(10.5, .semibold), Theme.crimson)
        }
        if let s = rec?.stall, s == .dcache {
            let msg = "the whole pipeline is frozen until the data arrives"
            drawText(msg, in: NSRect(x: colX(0), y: bottom + 6, width: colX(3) - colX(0), height: 14), Fonts.small, Theme.amber, align: .center)
        }
        drawText("cycle \(p.cycle)", in: NSRect(x: W - 120, y: bottom + 8, width: 110, height: 14), Fonts.mono(10.5, .semibold), Theme.secondary, align: .right)
    }
}

// MARK: - Multi-cycle pipeline diagram

final class PipelineChart: FlippedView {
    weak var model: AppModel?
    var onClickPC: ((UInt32) -> Void)?
    private var rowPCs: [UInt32] = []
    let rowH: CGFloat = 21
    let colW: CGFloat = 34
    let labelW: CGFloat = 210

    override func draw(_ dirtyRect: NSRect) {
        Theme.panel.setFill()
        bounds.fill()
        guard let m = model, let p = m.pipeline, let exe = m.good?.executable else { return }
        let cols = max(4, Int((bounds.width - labelW - 10) / colW))
        let maxRows = max(3, Int((bounds.height - 52) / rowH))
        let hist = p.history.suffix(cols)
        guard let firstCycle = hist.first?.cycle else {
            drawText("Press Step or Run: each row will be an instruction, each column a clock cycle.", at: NSPoint(x: 14, y: 14), Fonts.body, Theme.secondary)
            return
        }
        struct C { var stage: Int; var status: StageSlot.Status }
        var cells: [Int: [Int: C]] = [:]
        for r in hist {
            for (k, s) in r.stages.enumerated() where s.seq >= 0 {
                cells[s.seq, default: [:]][r.cycle] = C(stage: k, status: s.status)
            }
        }
        let seqs = cells.keys.sorted().suffix(maxRows)
        rowPCs = []
        // Header: cycle numbers.
        for (j, r) in hist.enumerated() {
            let x = labelW + CGFloat(j) * colW
            if r.cycle == p.cycle {
                Theme.selection.alpha(0.6).setFill()
                NSRect(x: x, y: 0, width: colW, height: bounds.height).fill()
            }
            if j % 2 == 0 || cols < 16 {
                drawText("\(r.cycle)", in: NSRect(x: x, y: 4, width: colW, height: 14), Fonts.mono(9), Theme.faint, align: .center)
            }
        }
        line(NSPoint(x: 0, y: 22.5), NSPoint(x: bounds.width, y: 22.5), Theme.border)
        let names = ["IF", "ID", "EX", "MEM", "WB"]
        for (i, seq) in seqs.enumerated() {
            let y = 26 + CGFloat(i) * rowH
            let pc = p.seqPC[seq] ?? 0
            rowPCs.append(pc)
            let txt = pc >= exe.textBase && pc < exe.textEnd ? shortAsm(exe, pc) : "(fetch past the end)"
            let flushed = cells[seq]?.values.contains { $0.status == .flushed } ?? false
            let a = highlightAsm(txt, font: Fonts.mono(10.5), dim: flushed)
            drawAttributed(a, in: NSRect(x: 10, y: y + 3, width: labelW - 16, height: 16))
            if let sel = m.selection.pc, sel == pc {
                Theme.selectionStrong.setFill()
                NSRect(x: 0, y: y, width: 4, height: rowH - 1).fill()
            }
            for (j, r) in hist.enumerated() {
                guard let c = cells[seq]?[r.cycle] else { continue }
                let cr = NSRect(x: labelW + CGFloat(j) * colW + 1.5, y: y + 1.5, width: colW - 3, height: rowH - 3)
                switch c.status {
                case .flushed:
                    fillRounded(cr, 4, Theme.errorFill)
                    drawText("✕", in: cr, Fonts.ui(10, .bold), Theme.crimson, align: .center)
                case .stalled:
                    fillRounded(cr, 4, NSColor(hex: 0xEDE9E0))
                    strokeRounded(cr, 4, Theme.borderStrong)
                    drawText(names[c.stage], in: cr, Fonts.mono(9.5, .semibold), Theme.faint, align: .center)
                case .waiting:
                    fillRounded(cr, 4, NSColor(hex: 0xFFF1C9))
                    strokeRounded(cr, 4, Theme.amber.alpha(0.7))
                    drawText(names[c.stage], in: cr, Fonts.mono(9.5, .semibold), Theme.amber, align: .center)
                default:
                    fillRounded(cr, 4, Theme.stage[c.stage].alpha(0.85))
                    drawText(names[c.stage], in: cr, Fonts.mono(9.5, .bold), .white, align: .center)
                }
            }
        }
        // Legend.
        let ly = bounds.height - 16
        var lx: CGFloat = 10
        for (t, f, fg) in [("stall (bubble behind it)", NSColor(hex: 0xEDE9E0), Theme.secondary), ("cache miss: waiting", NSColor(hex: 0xFFF1C9), Theme.amber), ("✕ flushed (wrong path)", Theme.errorFill, Theme.crimson)] {
            let r = drawPill(t, at: NSPoint(x: lx, y: ly - 2), font: Fonts.tiny, fill: f, text: fg)
            lx = r.maxX + 8
        }
        _ = firstCycle
    }

    override func mouseDown(with event: NSEvent) {
        let p = convert(event.locationInWindow, from: nil)
        let i = Int((p.y - 26) / rowH)
        if i >= 0 && i < rowPCs.count { onClickPC?(rowPCs[i]) }
    }
}

// MARK: - Stats and registers

final class CPUStatsCanvas: FlippedView {
    weak var model: AppModel?

    override func draw(_ dirtyRect: NSRect) {
        Theme.panelAlt.setFill()
        bounds.fill()
        line(NSPoint(x: 0.5, y: 0), NSPoint(x: 0.5, y: bounds.height), Theme.border)
        guard let m = model, let p = m.pipeline else { return }
        let s = p.stats
        var y: CGFloat = 10
        func row(_ k: String, _ v: String, _ c: NSColor = Theme.text, bold: Bool = false) {
            drawText(k, at: NSPoint(x: 12, y: y), Fonts.small, Theme.secondary)
            drawText(v, in: NSRect(x: 12, y: y, width: bounds.width - 24, height: 15), Fonts.mono(11, bold ? .bold : .medium), c, align: .right)
            y += 16
        }
        drawText("Performance", at: NSPoint(x: 12, y: y), Fonts.subheading, Theme.heading)
        y += 22
        row("Clock cycles", formatCount(s.cycles), bold: true)
        row("Instructions completed", formatCount(s.retired))
        row("CPI (cycles per instruction)", s.retired > 0 ? String(format: "%.2f", s.cpi) : "—", s.cpi > 1.6 ? Theme.crimson : Theme.green, bold: true)
        y += 4
        let stallTotal = s.stalls.values.reduce(0, +)
        row("Stall cycles", formatCount(stallTotal))
        for r in [StallReason.loadUse, .dataHazard, .branchOperand, .ecall, .icache, .dcache] {
            if let n = s.stalls[r], n > 0 { row("   " + r.rawValue, formatCount(n), Theme.secondary) }
        }
        row("Instructions flushed", formatCount(s.flushedInstructions))
        if s.branches > 0 {
            let acc = 100 * Double(s.branches - s.mispredictions) / Double(s.branches)
            row("Branches (predicted right)", "\(formatCount(s.branches)) (\(String(format: "%.0f", acc))%)")
        }
        row("Values forwarded", formatCount(s.forwards))
        y += 4
        if s.retired > 0 {
            // P&H's comparison: a single-cycle design has CPI = 1 but a clock as slow as its slowest instruction.
            let pipelinedPS = Double(s.cycles) * 200
            let singlePS = Double(s.retired) * 800
            let txt = String(format: "Single-cycle CPU: %@ cycles × 800 ps.\nThis pipeline: %@ cycles × 200 ps.\nSpeed-up ≈ %.1f×", formatCount(s.retired), formatCount(s.cycles), singlePS / pipelinedPS)
            NSAttributedString(string: txt, attributes: [.font: Fonts.tiny, .foregroundColor: Theme.secondary]).draw(with: NSRect(x: 12, y: y, width: bounds.width - 20, height: 44), options: [.usesLineFragmentOrigin])
            y += 46
        }
        line(NSPoint(x: 10, y: y), NSPoint(x: bounds.width - 10, y: y), Theme.border)
        y += 8
        drawText("Register file", at: NSPoint(x: 12, y: y), Fonts.subheading, Theme.heading)
        y += 20
        let written = p.lastRecord?.registerWrite?.0
        let colW = (bounds.width - 16) / 2
        for r in 0..<32 {
            let col = r / 16, rw = r % 16
            let x = 8 + CGFloat(col) * colW
            let ry = y + CGFloat(rw) * 15
            if ry > bounds.height - 4 { continue }
            if r == written {
                Theme.stageFill(4).setFill()
                NSRect(x: x, y: ry, width: colW - 2, height: 15).fill()
            }
            let v = p.register(r)
            drawText(Reg.name(r), at: NSPoint(x: x + 4, y: ry + 1), Fonts.mono(10, .semibold), r == written ? Theme.stage[4] : Theme.register)
            let shown = (r == Reg.sp || r == Reg.ra || r == Reg.gp || v > 0xFFFF && v < 0xFFFF_0000) ? hex8(v) : String(Int32(bitPattern: v))
            drawText(shown, in: NSRect(x: x + 30, y: ry + 1, width: colW - 36, height: 13), Fonts.mono(10), v == 0 ? Theme.faint : Theme.text, align: .right)
        }
    }
}

final class PipelineView: NSView, StationView {
    let datapath = DatapathCanvas()
    let chart = PipelineChart()
    let stats = CPUStatsCanvas()
    let forwarding = Segmented(["Forwarding on", "off"])
    let branchStage = Segmented(["ID", "EX", "MEM"])
    let predictor = PopUp(PredictorKind.allCases.map { $0.rawValue })
    let branchLabel = makeLabel("Branches resolved in", Fonts.small, Theme.secondary)
    let caches = Check("Caches", isOn: true)
    let empty = EmptyNote()
    weak var model: AppModel?

    override init(frame: NSRect) {
        super.init(frame: frame)
        for v in [datapath, chart, stats, forwarding, branchStage, predictor, branchLabel, caches, empty] as [NSView] { addSubview(v) }
        caches.toolTip = "Off = ideal memory: every fetch and load takes one cycle, so only hazards stall the pipeline"
        caches.onChange = { [weak self] on in
            guard let m = self?.model else { return }
            var h = m.pipelineConfig.hierarchy
            h.enabled = on
            m.pipelineConfig.hierarchy = h
        }
        forwarding.onChange = { [weak self] k in self?.model?.pipelineConfig.forwarding = k == 0 }
        branchStage.onChange = { [weak self] k in self?.model?.pipelineConfig.branchStage = BranchStage.allCases[k] }
        predictor.onChange = { [weak self] k in self?.model?.pipelineConfig.predictor = PredictorKind.allCases[k] }
        chart.onClickPC = { [weak self] pc in
            guard let m = self?.model else { return }
            m.selection = Selection(line: m.line(ofPC: pc), pc: pc, origin: .pipeline)
        }
    }
    required init?(coder: NSCoder) { fatalError() }

    override var isFlipped: Bool { true }

    override func layout() {
        super.layout()
        let sw: CGFloat = 236
        let fs = forwarding.intrinsicContentSize
        forwarding.frame = NSRect(x: 10, y: 8, width: fs.width, height: 24)
        branchLabel.frame = NSRect(x: forwarding.frame.maxX + 16, y: 12, width: 124, height: 16)
        let bs = branchStage.intrinsicContentSize
        branchStage.frame = NSRect(x: branchLabel.frame.maxX + 2, y: 8, width: bs.width + 10, height: 24)
        predictor.frame = NSRect(x: branchStage.frame.maxX + 14, y: 7, width: 250, height: 26)
        caches.frame = NSRect(x: predictor.frame.maxX + 14, y: 10, width: caches.intrinsicContentSize.width, height: 20)
        let top: CGFloat = 40
        let dh = min(310, max(250, (bounds.height - top) * 0.52))
        datapath.frame = NSRect(x: 0, y: top, width: bounds.width - sw, height: dh)
        chart.frame = NSRect(x: 0, y: top + dh, width: bounds.width - sw, height: bounds.height - top - dh)
        stats.frame = NSRect(x: bounds.width - sw, y: top, width: sw, height: bounds.height - top)
        empty.frame = bounds
    }

    override func draw(_ dirtyRect: NSRect) {
        Theme.panel.setFill()
        bounds.fill()
        line(NSPoint(x: 0, y: 39.5), NSPoint(x: bounds.width, y: 39.5), Theme.border)
        line(NSPoint(x: 0, y: chart.frame.minY + 0.5), NSPoint(x: chart.frame.maxX, y: chart.frame.minY + 0.5), Theme.border)
    }

    func reload(_ model: AppModel, _ event: ModelEvent) {
        self.model = model
        datapath.model = model
        chart.model = model
        stats.model = model
        empty.isHidden = model.pipeline != nil
        if model.pipeline == nil {
            empty.title = "Nothing to run"
            empty.message = "Fix the errors in the program first."
            empty.needsDisplay = true
        }
        forwarding.selected = model.pipelineConfig.forwarding ? 0 : 1
        caches.isOn = model.pipelineConfig.hierarchy.enabled
        branchStage.selected = BranchStage.allCases.firstIndex(of: model.pipelineConfig.branchStage) ?? 1
        predictor.button.selectItem(at: PredictorKind.allCases.firstIndex(of: model.pipelineConfig.predictor) ?? 0)
        datapath.needsDisplay = true
        chart.needsDisplay = true
        stats.needsDisplay = true
    }
}
