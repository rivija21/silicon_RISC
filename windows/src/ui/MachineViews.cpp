#include "Views.h"

namespace ui {

using namespace sp;

/// Binary text with each encoding field on its own coloured background.
static AttrString coloredBinary(const Decoded& d, const Font& font = Fonts::mono(10.5f)) {
    AttrString out;
    auto fields = Decoder::fields(d);
    for (int k = 0; k < (int)fields.size(); k++) {
        const FieldInfo& f = fields[k];
        out.add(binaryString(f.value, f.hi - f.lo + 1), font, Theme::text, Theme::fieldColor(f.name));
        if (k < (int)fields.size() - 1) out.add(" ", font, Theme::text);
    }
    return out;
}

static const char* opcodeName(uint32_t opcode) {
    switch (opcode) {
    case 0b0110011: return "OP (register–register ALU)";
    case 0b0010011: return "OP-IMM (ALU with immediate)";
    case 0b0000011: return "LOAD";
    case 0b0100011: return "STORE";
    case 0b1100011: return "BRANCH";
    case 0b1101111: return "JAL";
    case 0b1100111: return "JALR";
    case 0b0110111: return "LUI";
    case 0b0010111: return "AUIPC";
    case 0b1110011: return "SYSTEM";
    case 0b0001111: return "MISC-MEM";
    default: return nullptr;
    }
}

// MARK: - Encoding

void EncodingCanvas::draw(Gfx& g, const Rect& dirty) {
    fillRect(bounds(), Theme::panel);
    if (!ins || !ins->decoded) {
        drawText("Click an instruction below to see how it is encoded.", Point(16, 16), Fonts::body, Theme::secondary);
        return;
    }
    const Decoded& d = *ins->decoded;
    AttrString title = highlightAsm(Decoder::disassemble(d, ins->address, symbolize), Fonts::mono(15, Weight::Medium));
    title.add(cat("    ", formatName(opSpec(d.op).format), "-type"), Fonts::serif(13, Weight::Semibold), Theme::heading);
    g.drawAttributed(title, {16, 8});
    std::string sub = cat(opSummary(d.op), "   ·   at ", hexAddr(ins->address), "   ·   word 0x", hex8(ins->word));
    if (ins->pseudo) {
        sub += "   ·   from the pseudo-instruction  " + *ins->pseudo;
        if (ins->parts > 1) sub += cat(" (part ", ins->part, " of ", ins->parts, ")");
    }
    drawText(sub, Rect(16, 34, width() - 32, 16), Fonts::small, Theme::secondary);
    auto fields = Decoder::fields(d);
    float x0 = 16, w = width() - 32;
    float bw = w / 32;
    float top = 62;
    // Bit numbers.
    for (int b = 0; b < 32; b++) {
        float x = x0 + (31 - b) * bw;
        bool edge = b == 31 || b == 0;
        for (auto& f : fields)
            if (f.hi == b || f.lo == b) edge = true;
        if (edge) drawText(std::to_string(b), Rect(x, top - 14, bw, 12), Fonts::mono(8.5f), Theme::faint, Align::Center);
    }
    for (auto& f : fields) {
        float fx = x0 + (31 - f.hi) * bw;
        float fw = (f.hi - f.lo + 1) * bw;
        Rect r{fx, top, fw, 26};
        fillRect(r, Theme::fieldColor(f.name));
        for (int b = f.lo; b <= f.hi; b++) {
            uint32_t bit = (ins->word >> b) & 1;
            float bx = x0 + (31 - b) * bw;
            drawText(bit ? "1" : "0", Rect(bx, top, bw, 26), Fonts::mono(12, bit ? Weight::Bold : Weight::Regular), bit ? Theme::text : Theme::secondary,
                     Align::Center);
            if (b != f.lo) line({bx, top + 4}, {bx, top + 22}, Theme::text.alpha(0.08f));
        }
        strokeRect(r, Theme::borderStrong);
        drawText(f.name, Rect(fx, top + 30, fw, 14), Fonts::mono(9.5f, Weight::Semibold), Theme::text, Align::Center);
        std::string meaning;
        if (f.name == "rd" || f.name == "rs1" || f.name == "rs2") meaning = cat("x", f.value, " ", Reg::name((int)f.value));
        else if (f.name == "opcode") {
            const char* n = opcodeName(f.value);
            meaning = n ? split(n, ' ')[0] : "?";
        } else if (f.name == "funct3" || f.name == "funct7" || f.name == "shamt") meaning = std::to_string(f.value);
        if (!meaning.empty()) drawText(meaning, Rect(fx, top + 44, fw, 14), Fonts::mono(9.5f), Theme::secondary, Align::Center);
    }
    // How the immediate is assembled.
    float y = top + 66;
    InstFormat fmt = opSpec(d.op).format;
    if (fmt != InstFormat::R && !(d.op == Op::ECALL || d.op == Op::EBREAK)) {
        std::string s = cat("immediate = ", d.imm);
        switch (fmt) {
        case InstFormat::B:
            s += cat("  — branch offset in bytes (bit 0 is always 0, so it is not stored); target = pc + ", d.imm, " = ",
                     hexAddr(ins->address + (uint32_t)d.imm));
            break;
        case InstFormat::J: s += cat("  — jump offset in bytes; target = pc + ", d.imm, " = ", hexAddr(ins->address + (uint32_t)d.imm)); break;
        case InstFormat::U:
            s = cat("upper immediate = 0x", hexString((uint32_t)d.imm & 0xFFFFF), " — placed in bits 31…12 of rd");
            break;
        case InstFormat::S: s += "  — split in two pieces so that rs1/rs2 stay in the same bit positions as in R-type"; break;
        default: s += opIsShiftImm(d.op) ? "" : "  — sign-extended from 12 bits"; break;
        }
        drawText(s, Rect(16, y, width() - 32, 16), Fonts::small, Theme::text);
        y += 18;
    }
    if (const char* n = opcodeName(d.raw & 0x7F)) {
        std::string s = cat("opcode ", binaryString(d.raw & 0x7F, 7), " = ", n);
        if (fmt == InstFormat::R || fmt == InstFormat::I || fmt == InstFormat::S || fmt == InstFormat::B)
            s += cat("; funct3", (fmt == InstFormat::R || opIsShiftImm(d.op)) ? "/funct7" : "", " choose the exact operation");
        drawText(s, Rect(16, y, width() - 32, 16), Fonts::small, Theme::secondary);
    }
}

// MARK: - Machine code view

MachineCodeView::MachineCodeView() {
    for (View* v : std::initializer_list<View*>{&encoding, &listing, &scope, &empty}) addSubview(v);
    scope.onChange = [this](int) {
        if (model) rebuild(*model);
    };
    listing.list.onClick = [this](int, const Row& r) {
        if (!model || !r.pc) return;
        selectedPC_ = r.pc;
        Selection s;
        s.line = r.line;
        s.pc = r.pc;
        s.origin = Station::Machine;
        model->setSelection(s);
    };
    listing.list.highlight = [this](const Row& r) -> std::optional<Color> {
        if (!model) return std::nullopt;
        const Selection& s = model->selection();
        if (s.pc && r.pc == s.pc) return Theme::selectionStrong.alpha(0.6f);
        if (s.line && r.line == s.line && r.pc) return Theme::selection;
        return std::nullopt;
    };
}

void MachineCodeView::layout() {
    encoding.setFrame({0, 0, width(), 170});
    Size s = scope.intrinsicSize();
    scope.setFrame({width() - s.w - 12, 176, s.w, 24});
    listing.setFrame({0, 206, width(), height() - 206});
    empty.setFrame(bounds());
}

void MachineCodeView::draw(Gfx& g, const Rect& dirty) {
    fillRect(bounds(), Theme::panel);
    line({0, 170.5f}, {width(), 170.5f}, Theme::border);
    Font f = Fonts::mono(9.5f, Weight::Semibold);
    drawText("Address", Point(12, 185), f, Theme::faint);
    drawText("Hex", Point(92, 185), f, Theme::faint);
    drawText("Binary, split into fields", Point(168, 185), f, Theme::faint);
    drawText("Instruction", Point(460, 185), f, Theme::faint);
    line({0, 205.5f}, {width(), 205.5f}, Theme::border);
}

void MachineCodeView::rebuild(AppModel& m) {
    CompilationP c = m.shown();
    if (!c || !c->executable) return;
    ExecutableP exe = c->executable;
    std::vector<Row> rows;
    std::string lastObj, lastFn;
    for (auto& ins : exe->instructions) {
        if (scope.selected() == 0 && ins.object != "main.o") continue;
        if (ins.object != lastObj) {
            lastObj = ins.object;
            std::string summary;
            for (auto& o : exe->objects)
                if (o.name == ins.object) summary = o.summary;
            rows.push_back(Row::header(ins.object, summary));
        }
        std::string fn = exe->function(ins.address).value_or("");
        if (fn != lastFn) {
            lastFn = fn;
            rows.push_back(Row({Cell(AttrString(fn + ":", Fonts::mono(11.5f, Weight::Semibold), Theme::label))}, Row::Kind::Section, 20));
        }
        if (!ins.decoded) continue;
        std::string asmText = Decoder::disassemble(*ins.decoded, ins.address, [exe](uint32_t a) -> std::optional<std::string> {
            if (auto l = exe->label(a)) return l;
            return exe->symbolize(a);
        });
        Row r({Cell(format("%08x", ins.address), Fonts::mono(10.5f), Theme::secondary, 80.0f), Cell(hex8(ins.word), Fonts::mono(10.5f), Theme::number, 76.0f),
               Cell(coloredBinary(*ins.decoded), 292.0f), Cell(highlightAsm(asmText, Fonts::mono(11.5f)), 230.0f),
               Cell(ins.pseudo ? "← " + *ins.pseudo : "", Fonts::mono(10.5f), Theme::purple)});
        r.pc = ins.address;
        r.line = m.sourceLine(ins);
        rows.push_back(r);
    }
    listing.list.minWidth = 900;
    listing.set(rows);
}

void MachineCodeView::reload(AppModel& m, ModelEvent e) {
    model = &m;
    CompilationP c = m.shown();
    if (!c || !c->executable) {
        empty.setHidden(false);
        empty.title = "No machine code";
        empty.message = "The program has to compile and link first.";
        empty.setNeedsDisplay();
        return;
    }
    empty.setHidden(true);
    ExecutableP exe = c->executable;
    if (e == ModelEvent::Compiled || listing.list.rows().empty()) rebuild(m);
    // Show the selected instruction, or the first one of the selected line.
    std::optional<uint32_t> pc = m.selection().pc;
    if (!pc && m.selection().line) {
        for (auto& ins : exe->instructions)
            if (m.sourceLine(ins) == m.selection().line) {
                pc = ins.address;
                break;
            }
    }
    if (!pc)
        if (Pipeline* p = m.pipeline(); p && p->lastRecord() && p->lastRecord()->stages[2].seq >= 0) pc = p->lastRecord()->stages[2].pc;
    const LinkedInstruction* ins = nullptr;
    if (pc) ins = exe->instruction(*pc);
    else if (selectedPC_) ins = exe->instruction(*selectedPC_);
    if (!ins)
        for (auto& i : exe->instructions)
            if (i.object == "main.o") {
                ins = &i;
                break;
            }
    encoding.ins = ins ? std::optional<LinkedInstruction>(*ins) : std::nullopt;
    encoding.symbolize = [exe](uint32_t a) -> std::optional<std::string> {
        if (auto l = exe->label(a)) return l;
        return exe->symbolize(a);
    };
    encoding.setNeedsDisplay();
    listing.list.setNeedsDisplay();
    if (e == ModelEvent::Selection && m.selection().origin != Station::Machine && pc) {
        auto& rs = listing.list.rows();
        for (int k = 0; k < (int)rs.size(); k++)
            if (rs[k].pc == pc) {
                listing.list.reveal(k);
                break;
            }
    }
}

// MARK: - Linking

void MemoryMapCanvas::draw(Gfx& g, const Rect& dirty) {
    fillRect(bounds(), Theme::panel);
    if (!exe) return;
    drawText("The process's memory", Point(14, 8), Fonts::subheading, Theme::heading);
    drawText("not to scale · addresses grow upwards", Point(14, 28), Fonts::tiny, Theme::secondary);
    float x = 104, w = width() - x - 14;
    struct Part {
        std::string name;
        uint32_t start;
        int size;
    };
    struct Seg {
        std::string name;
        uint32_t lo, hi;
        Color color;
        float h;
        std::vector<Part> parts;
    };
    uint32_t heapTop = std::max(brk.value_or(MemoryMap::heapBase), MemoryMap::heapBase);
    std::vector<Part> dataParts, textParts;
    for (auto& o : exe->objects) {
        if (o.dataSize > 0) dataParts.push_back({o.name, o.dataStart, o.dataSize});
        if (o.textSize > 0) textParts.push_back({o.name, o.textStart, o.textSize});
    }
    std::vector<Seg> segs = {
        {"stack ↓", stackPointer.value_or(MemoryMap::stackTop), MemoryMap::stackTop, Color::hex(0xE3ECF8), 70, {}},
        {"", 0, 0, Color::clear(), 34, {}},
        {"heap ↑ (malloc)", MemoryMap::heapBase, heapTop, Color::hex(0xDDEEE3), 50, {}},
        {"", 0, 0, Color::clear(), 20, {}},
        {".data", exe->dataBase, exe->dataEnd(), Color::hex(0xF6E2C8), 90, dataParts},
        {"", 0, 0, Color::clear(), 30, {}},
        {".text (code)", exe->textBase, exe->textEnd(), Color::hex(0xEDE3F3), 150, textParts},
        {"reserved", 0, exe->textBase, Color::hex(0xF1EDE4), 26, {}},
    };
    float y = 50;
    for (auto& s : segs) {
        if (s.name.empty()) {
            // A gap: a zig-zag break.
            g.strokePolyline({Point(x + w / 2 - 10, y + 6), Point(x + w / 2, y + s.h / 2), Point(x + w / 2 + 10, y + s.h - 6)}, Theme::faint, 1, {3, 3});
            y += s.h;
            continue;
        }
        Rect r{x, y, w, s.h};
        fillRect(r, s.color);
        strokeRect(r.insetBy(0.5f, 0.5f), Theme::borderStrong);
        if (!s.parts.empty()) {
            int total = 0;
            for (auto& p : s.parts) total += p.size;
            float inner = s.h - 20;
            float py = y + s.h;
            for (auto& p : s.parts) {
                float ph = std::max(14.0f, inner * p.size / std::max(1, total));
                py -= ph;
                Rect pr{x + 6, std::max(y + 20, py), w - 12, std::min(ph, py + ph - y - 20) - 2};
                if (pr.h > 8) {
                    fillRect(pr, Theme::panel.alpha(0.55f));
                    drawText(cat(p.name, "  ", p.size, " B"), pr.insetBy(6, 0), Fonts::mono(9.5f), Theme::text);
                }
            }
            drawText(s.name, Point(x + 8, y + 2), Fonts::mono(11, Weight::Semibold), Theme::text);
        } else {
            drawText(s.name, r.insetBy(10, 0), Fonts::mono(11, Weight::Semibold), Theme::text);
        }
        drawText(hexAddr(s.hi), Rect(4, y - 6, x - 10, 12), Fonts::mono(9.5f), Theme::secondary, Align::Right);
        if (s.name != "reserved") drawText(hexAddr(s.lo), Rect(4, y + s.h - 7, x - 10, 12), Fonts::mono(9.5f), Theme::secondary, Align::Right);
        y += s.h;
    }
    if (stackPointer) drawText("sp = " + hexAddr(*stackPointer), Point(x + 8, 50 + 50), Fonts::mono(10), Theme::ink);
}

LinkView::LinkView() {
    for (View* v : std::initializer_list<View*>{&listing, &map, &empty}) addSubview(v);
    listing.list.onClick = [this](int, const Row& r) {
        if (!model) return;
        Selection s;
        s.line = r.line;
        s.pc = r.pc;
        s.origin = Station::Link;
        model->setSelection(s);
    };
    listing.list.highlight = [this](const Row& r) -> std::optional<Color> {
        if (model && model->selection().pc && r.pc == model->selection().pc) return Theme::selection;
        return std::nullopt;
    };
}

void LinkView::layout() {
    float mw = std::min(330.0f, width() * 0.36f);
    listing.setFrame({0, 0, width() - mw, height()});
    map.setFrame({width() - mw, 0, mw, height()});
    empty.setFrame(bounds());
}

void LinkView::reload(AppModel& m, ModelEvent e) {
    model = &m;
    CompilationP c = m.shown();
    if (!c || !c->executable) {
        empty.setHidden(false);
        empty.title = "Nothing linked";
        empty.message = m.compilation() && m.compilation()->failedStage ? cat("Stopped at: ", stageName(*m.compilation()->failedStage), ".")
                                                                        : "Compile the program first.";
        empty.setNeedsDisplay();
        return;
    }
    empty.setHidden(true);
    ExecutableP exe = c->executable;
    map.exe = exe;
    if (Pipeline* p = m.pipeline()) {
        map.stackPointer = p->reg(Reg::sp);
        map.brk = p->sys.brk;
    } else {
        map.stackPointer.reset();
        map.brk.reset();
    }
    map.setNeedsDisplay();
    if (!(e == ModelEvent::Compiled || listing.list.rows().empty())) return;
    std::vector<Row> rows;
    rows.push_back(Row::header("Object files", "the linker starts with crt0.o and main.o, then pulls library members in as needed"));
    for (auto& o : exe->objects) {
        std::string what = o.isLibrary ? "library: " + o.reason
                           : o.name == "crt0.o" ? "start-up code: sets up the stack, calls main, then exit"
                                                : "your program";
        Row r({Cell(AttrString(o.name, Fonts::mono(12, Weight::Semibold), o.isLibrary ? Theme::purple : Theme::ink), 100.0f),
               Cell(cat(o.textSize, " B code", o.dataSize > 0 ? cat(", ", o.dataSize, " B data") : std::string("")), Fonts::mono(10.5f), Theme::secondary,
                    150.0f),
               Cell(what, Fonts::small, Theme::text)},
              Row::Kind::Normal, 22);
        r.tooltip = o.summary;
        rows.push_back(r);
    }
    rows.push_back(Row::spacer());
    rows.push_back(Row::header("Relocations", cat((int)exe->relocations.size(), " places the assembler could not finish: addresses only the linker knows")));
    rows.push_back(Row({Cell("AT", Fonts::mono(9.5f, Weight::Semibold), Theme::faint, 80.0f), Cell("TYPE", Fonts::mono(9.5f, Weight::Semibold), Theme::faint, 160.0f),
                        Cell("SYMBOL → ADDRESS", Fonts::mono(9.5f, Weight::Semibold), Theme::faint, 190.0f),
                        Cell("BEFORE → AFTER", Fonts::mono(9.5f, Weight::Semibold), Theme::faint)},
                       Row::Kind::Section, 18));
    int count = 0;
    for (auto& rel : exe->relocations) {
        if (count++ >= 400) break;
        std::string before = rel.type == RelocType::Abs32 ? "(data word)" : hex8(rel.before);
        Row r({Cell(format("%08x", rel.address), Fonts::mono(10.5f), Theme::secondary, 80.0f),
               Cell(replaceAll(relocName(rel.type), "R_RISCV_", ""), Fonts::mono(10.5f), Theme::purple, 160.0f),
               Cell(AttrString(cat(rel.symbol, " → ", format("%08x", rel.target)), Fonts::mono(10.5f), Theme::label), 190.0f),
               Cell(cat(before, " → ", hex8(rel.after)), Fonts::mono(10.5f), Theme::text)});
        r.tooltip = relocExplanation(rel.type);
        if (rel.type != RelocType::Abs32) r.pc = rel.address;
        if (const LinkedInstruction* ins = exe->instruction(rel.address)) r.line = m.sourceLine(*ins);
        rows.push_back(r);
    }
    rows.push_back(Row::spacer());
    rows.push_back(Row::header("Symbol table", "every label with its final address · .L labels are local to their file"));
    for (auto& s : exe->symbols) {
        if (hasPrefix(s.name, ".L") && !hasPrefix(s.name, ".LC")) continue;
        Row r({Cell(format("%08x", s.address), Fonts::mono(10.5f), Theme::secondary, 80.0f),
               Cell(AttrString(s.name, Fonts::mono(11.5f, Weight::Semibold), s.section == SectionKind::Text ? Theme::label : Theme::string), 160.0f),
               Cell(sectionName(s.section), Fonts::mono(10.5f), Theme::secondary, 60.0f), Cell(s.object, Fonts::mono(10.5f), Theme::secondary, 90.0f),
               Cell(s.isGlobal ? "global" : "local", Fonts::small, s.isGlobal ? Theme::ink : Theme::faint)});
        if (s.section == SectionKind::Text) r.pc = s.address;
        rows.push_back(r);
    }
    listing.set(rows);
}

}  // namespace ui
