#include "Views.h"

namespace ui {

using namespace sp;
using Status = StageSlot::Status;

static std::string shortAsm(const Executable& exe, uint32_t pc) {
    const LinkedInstruction* ins = exe.instruction(pc);
    if (!ins || !ins->decoded) return "?";
    return Decoder::disassemble(*ins->decoded, pc, [&exe](uint32_t a) -> std::optional<std::string> {
        if (auto l = exe.label(a)) return l;
        return exe.symbolize(a);
    });
}

/// Small values in decimal, addresses and big patterns in hex.
static std::string regValueText(uint32_t v) {
    int32_t i = (int32_t)v;
    return std::abs((long long)i) < 100000 ? std::to_string(i) : "0x" + hexString(v);
}

static std::pair<std::string, Color> accessText(const MemoryAccess& a, const std::string& cache) {
    if (a.servedBy == "memory") return {"ideal memory", Theme::secondary};
    if (a.servedBy == "L1") return {cache + " hit", Theme::green};
    return {cat("miss → ", a.servedBy, ", +", a.penalty, " cycles"), Theme::crimson};
}

// MARK: - Datapath

void DatapathCanvas::draw(Gfx& g, const Rect& dirty) {
    fillRect(bounds(), Theme::panel);
    for (float gx = 0; gx < width(); gx += 16) line({gx + 0.5f, 0}, {gx + 0.5f, height()}, Theme::grid.alpha(0.7f));
    for (float gy = 0; gy < height(); gy += 16) line({0, gy + 0.5f}, {width(), gy + 0.5f}, Theme::grid.alpha(0.7f));
    Pipeline* p = model ? model->pipeline() : nullptr;
    if (!p || !model->good() || !model->good()->executable) {
        drawText("Compile a program to see it run.", Point(20, 20), Fonts::body, Theme::secondary);
        return;
    }
    const Executable& exe = *model->good()->executable;
    const CycleRecord* rec = p->lastRecord();
    float W = width(), H = height();
    float regW = 12;
    float colW = (W - 20 - 4 * regW) / 5;
    auto colX = [&](int k) { return 10 + k * (colW + regW); };
    float top = 56, bottom = H - 30;
    float midY = (top + bottom) / 2;

    // Stage headers with the instruction in each stage.
    for (int k = 0; k < 5; k++) {
        float x = colX(k);
        StageSlot slot = rec ? rec->stages[k] : StageSlot::bubble();
        Rect head{x + 2, 6, colW - 4, 44};
        Color fill = Theme::stageFill(k);
        std::string text = "—";
        std::string sub = PipeStage::longName(k);
        Color color = Theme::stage[k];
        switch (slot.status) {
        case Status::Bubble:
            fill = Color::hex(0xF1EDE4);
            text = "bubble";
            color = Theme::faint;
            break;
        case Status::Flushed:
            fill = Theme::errorFill;
            text = shortAsm(exe, slot.pc);
            sub = "✕ flushed: wrong path";
            color = Theme::crimson;
            break;
        case Status::Stalled:
            text = shortAsm(exe, slot.pc);
            sub = cat("stalled: ", rec && rec->stall ? stallReasonName(*rec->stall) : "");
            fill = Color::hex(0xEDE9E0);
            color = Theme::secondary;
            break;
        case Status::Waiting:
            text = shortAsm(exe, slot.pc);
            sub = k == 0 ? "waiting for the I-cache miss" : "waiting for the D-cache miss";
            fill = Color::hex(0xFFF1C9);
            color = Theme::amber;
            break;
        case Status::Normal: text = slot.seq >= 0 ? shortAsm(exe, slot.pc) : "—"; break;
        }
        if (slot.seq >= 0 && slot.status != Status::Bubble && k == 0 && slot.pc >= exe.textEnd()) text = "(past the end of code)";
        fillRounded(head, 6, fill);
        strokeRounded(head, 6, color.alpha(0.6f), 1.2f);
        std::string tag = PipeStage::name(k);
        drawPill(tag, Point(head.minX() + 6, head.minY() + 5), Fonts::mono(9.5f, Weight::Bold), Theme::stage[k], Color::white());
        float tx = head.minX() + 6 + textSize(tag, Fonts::mono(9.5f, Weight::Bold)).w + 16;
        drawText(text, Rect(tx, head.minY() + 3, head.maxX() - tx - 4, 18), Fonts::mono(colW < 190 ? 10.5f : 11.5f, Weight::Semibold),
                 slot.status == Status::Bubble ? Theme::faint : Theme::text);
        drawText(sub, Rect(head.minX() + 8, head.minY() + 24, head.w - 14, 16), Fonts::tiny, slot.status == Status::Normal ? Theme::secondary : color);
    }
    // Pipeline registers.
    const char* names[] = {"IF/ID", "ID/EX", "EX/MEM", "MEM/WB"};
    for (int k = 0; k < 4; k++) {
        float x = colX(k + 1) - regW;
        Rect r{x, top, regW, bottom - top};
        fillRounded(r, 3, Color::hex(0xE9E1D1));
        strokeRounded(r, 3, Theme::borderStrong);
        auto saved = g.transform();
        g.setTransform(D2D1::Matrix3x2F::Rotation(-90) * D2D1::Matrix3x2F::Translation(x + 0.5f, bottom - 6) * saved);
        drawText(names[k], Point(0, 0), Fonts::mono(8.5f, Weight::Bold), Theme::secondary);
        g.setTransform(saved);
    }

    auto box = [&](const Rect& r, const std::string& title, bool active, int k, std::optional<Color> lit = std::nullopt) {
        fillRounded(r, 5, lit ? *lit : (active ? Theme::stageFill(k) : Theme::panel));
        strokeRounded(r, 5, active ? Theme::stage[k] : Theme::borderStrong, active ? 1.4f : 1);
        TextBox tb;
        tb.align = Align::Center;
        Font f = Fonts::ui(10.5f, Weight::Semibold);
        // Shrink the title rather than break a word when the box is narrow.
        float widest = 0;
        for (auto& part : split(title, '\n')) widest = std::max(widest, textSize(part, f).w);
        if (widest > r.w - 6) f.size = std::max(7.5f, f.size * (r.w - 6) / widest);
        float lines = contains(title, "\n") ? 2.0f : 1.0f;
        float th = TextEngine::get().lineHeight(f) * lines;
        g.drawTextBox(title, {r.minX() + 2, r.midY() - th / 2, r.w - 4, th + 2}, f, active ? Theme::text : Theme::secondary, tb);
    };
    auto wire = [&](Point a, Point b, bool active, int k, bool arrow = true) {
        line(a, b, active ? Theme::stage[k] : Theme::borderStrong, active ? 2 : 1.2f);
        if (arrow) arrowHead(b, a, active ? Theme::stage[k] : Theme::borderStrong, 5);
    };
    auto value = [&](const std::string& s, Point pt, Color c = Theme::ink) { drawText(s, pt, Fonts::mono(9.5f), c); };
    bool active[5];
    for (int k = 0; k < 5; k++) active[k] = rec && rec->stages[k].seq >= 0 && rec->stages[k].status != Status::Flushed;

    // IF
    float x0 = colX(0);
    Rect pcBox{x0 + 8, midY - 18, 34, 36};
    box(pcBox, "PC", active[0], 0);
    Rect imem{x0 + 58, midY - 34, colW - 70, 68};
    bool ifWait = rec && rec->stages[0].status == Status::Waiting;
    box(imem, "Instruction\nmemory", active[0], 0, ifWait ? std::optional<Color>(Color::hex(0xFFF1C9)) : std::nullopt);
    drawText("I-cache", Rect(imem.minX(), imem.maxY() - 16, imem.w, 12), Fonts::tiny, Theme::secondary, Align::Center);
    wire({pcBox.maxX(), midY}, {imem.minX(), midY}, active[0], 0);
    wire({imem.maxX(), midY}, {colX(1) - regW, midY}, active[0], 0);
    Rect adder{x0 + 8, top + 6, 34, 22};
    box(adder, "+4", active[0], 0);
    if (rec && rec->stages[0].seq >= 0) value("pc = " + hexAddr(rec->stages[0].pc), {pcBox.minX() - 2, imem.maxY() + 16});
    if (rec && rec->fetch) {
        auto [t, c] = accessText(*rec->fetch, "I-cache");
        drawText(t, Rect(pcBox.minX() - 2, imem.maxY() + 2, colW - 4, 13), Fonts::mono(9.5f), c);
    }

    // ID
    float x1 = colX(1);
    Rect regs{x1 + 20, midY - 40, colW - 60, 80};
    box(regs, "Registers", active[1], 1);
    Rect imm{x1 + 20, bottom - 34, colW - 60, 24};
    box(imm, "Imm gen", active[1], 1);
    Rect hazard{x1 + 14, top + 4, colW - 28, 24};
    bool stallNow = rec && rec->stall && *rec->stall != StallReason::DCache && *rec->stall != StallReason::ICache;
    box(hazard, stallNow ? "Hazard unit: STALL" : "Hazard detection", stallNow, 1, stallNow ? std::optional<Color>(Theme::errorFill) : std::nullopt);
    wire({x1, midY}, {regs.minX(), midY}, active[1], 1);
    wire({regs.maxX(), midY - 18}, {colX(2) - regW, midY - 18}, active[1], 1);
    wire({regs.maxX(), midY + 18}, {colX(2) - regW, midY + 18}, active[1], 1);
    wire({imm.maxX(), imm.midY()}, {colX(2) - regW, imm.midY()}, active[1], 1);
    if (rec && rec->stages[1].seq >= 0 && p->idex() && p->idex()->seq == rec->stages[1].seq && p->idex()->d) {
        const InFlight& x = *p->idex();
        const Decoded& d = *x.d;
        std::vector<int> srcs;
        if (d.op == Op::ECALL) srcs = {Reg::a0, Reg::a7};
        else {
            if (d.rs1 != 0) srcs.push_back(d.rs1);
            if (d.rs2 != 0) srcs.push_back(d.rs2);
        }
        float y = regs.maxY() + 3;
        for (int j = 0; j < (int)srcs.size() && j < 2; j++) {
            bool show = (j == 0 && (opReadsRs1(d.op) || d.op == Op::ECALL)) || (j == 1 && (opReadsRs2(d.op) || d.op == Op::ECALL));
            if (!show) continue;
            value(cat(Reg::name(srcs[j]), " = ", (int32_t)(j == 0 ? x.a : x.b)), {regs.minX(), y});
            y += 12;
        }
    }
    if (rec && rec->forwardID)
        drawText("forwarded into the branch comparator", Rect(x1 + 4, hazard.maxY() + 2, colW, 12), Fonts::tiny, Theme::amber);

    // EX
    float x2 = colX(2);
    float ax = x2 + colW * 0.42f, ay = midY;
    std::vector<Point> alu = {Point(ax, ay - 42), Point(ax + 44, ay - 18), Point(ax + 44, ay + 18), Point(ax, ay + 42),
                              Point(ax, ay + 10), Point(ax + 10, ay), Point(ax, ay - 10)};
    g.fillPolygon(alu, active[2] ? Theme::stageFill(2) : Theme::panel);
    g.strokePolyline(alu, active[2] ? Theme::stage[2] : Theme::borderStrong, active[2] ? 1.5f : 1, {}, true);
    drawText("ALU", Point(ax + 16, ay - 8), Fonts::ui(11, Weight::Bold), active[2] ? Theme::text : Theme::secondary);
    Rect muxA{x2 + 14, midY - 34, 16, 30};
    Rect muxB{x2 + 14, midY + 4, 16, 30};
    for (const Rect& mx : {muxA, muxB}) {
        fillRounded(mx, 7, Theme::panel);
        strokeRounded(mx, 7, Theme::borderStrong);
    }
    wire({x2, midY - 18}, {muxA.minX(), midY - 18}, active[2], 2, false);
    wire({x2, midY + 18}, {muxB.minX(), midY + 18}, active[2], 2, false);
    wire({muxA.maxX(), muxA.midY()}, {ax, ay - 26}, active[2], 2);
    wire({muxB.maxX(), muxB.midY()}, {ax, ay + 26}, active[2], 2);
    wire({ax + 44, ay}, {colX(3) - regW, ay}, active[2], 2);
    Rect fwdUnit{x2 + 10, bottom - 30, colW - 20, 22};
    bool fwdActive = rec && (rec->forwardA != ForwardSource::None || rec->forwardB != ForwardSource::None);
    box(fwdUnit, model->pipelineConfig().forwarding ? "Forwarding unit" : "Forwarding: off", fwdActive, 2,
        fwdActive ? std::optional<Color>(Color::hex(0xFFF1C9)) : std::nullopt);
    if (rec && rec->stages[2].seq >= 0 && p->exmem() && p->exmem()->seq == rec->stages[2].seq && p->exmem()->d) {
        const InFlight& x = *p->exmem();
        drawText(cat("= ", (int32_t)x.alu), Rect(x2 + 4, ay + 46, colW - 8, 13), Fonts::mono(9.5f), Theme::ink, Align::Center);
        if ((opIsBranch(x.d->op) || opIsJump(x.d->op)) && x.resolved)
            drawText(x.taken ? "taken → " + hexAddr(x.target) : "not taken", Rect(x2 + 6, top + 6, colW - 10, 14), Fonts::tiny,
                     x.taken ? Theme::crimson : Theme::green);
    }

    // MEM
    float x3 = colX(3);
    Rect dmem{x3 + 22, midY - 34, colW - 50, 68};
    bool memWait = rec && rec->stages[3].status == Status::Waiting;
    bool memActive = active[3] && ((p->exmem() && p->exmem()->d && (opIsLoad(p->exmem()->d->op) || opIsStore(p->exmem()->d->op))) || (rec && rec->data));
    box(dmem, "Data\nmemory", memActive, 3, memWait ? std::optional<Color>(Color::hex(0xFFF1C9)) : std::nullopt);
    drawText("D-cache", Rect(dmem.minX(), dmem.maxY() - 16, dmem.w, 12), Fonts::tiny, Theme::secondary, Align::Center);
    wire({x3, midY}, {dmem.minX(), midY}, active[3], 3);
    wire({dmem.maxX(), midY}, {colX(4) - regW, midY}, active[3], 3);
    if (rec && rec->data) {
        const MemoryAccess& a = *rec->data;
        value(cat(a.kind == AccessKind::Store ? "store " : "load ", hexAddr(a.address)), {dmem.minX() - 4, dmem.maxY() + 3});
        auto [t, c] = accessText(a, "D-cache");
        drawText(t, Rect(dmem.minX() - 4, dmem.maxY() + 15, colW - 10, 13), Fonts::mono(9.5f), c);
    }

    // WB
    float x4 = colX(4);
    Rect wbMux{x4 + colW * 0.35f, midY - 22, 18, 44};
    fillRounded(wbMux, 8, active[4] ? Theme::stageFill(4) : Theme::panel);
    strokeRounded(wbMux, 8, active[4] ? Theme::stage[4] : Theme::borderStrong);
    wire({x4, midY}, {wbMux.minX(), midY}, active[4], 4);
    if (rec && rec->registerWrite) {
        auto [r, v] = *rec->registerWrite;
        drawText(cat(Reg::name(r), " ← ", regValueText(v)), Rect(x4 + 4, midY + 28, colW - 8, 14), Fonts::mono(10.5f, Weight::Semibold), Theme::stage[4],
                 Align::Center);
    }
    // Write-back path to the register file.
    float wbY = bottom - 4;
    bool wrote = rec && rec->registerWrite;
    g.strokePolyline({Point(wbMux.maxX(), midY), Point(wbMux.maxX() + 14, midY), Point(wbMux.maxX() + 14, wbY), Point(regs.midX(), wbY),
                      Point(regs.midX(), regs.maxY())},
                     wrote ? Theme::stage[4] : Theme::borderStrong, wrote ? 2 : 1);

    // Forwarding paths into the ALU muxes.
    auto forwardPath = [&](ForwardSource src, const Rect& mux, float offset) {
        if (src == ForwardSource::None) return;
        float fromX = src == ForwardSource::ExMem ? colX(3) - regW / 2 : colX(4) - regW / 2;
        Color c = src == ForwardSource::ExMem ? Theme::amber : Theme::purple;
        float y = bottom - 40 - offset;
        g.strokePolyline({Point(fromX, midY + 4), Point(fromX, y), Point(mux.minX() - 6, y), Point(mux.minX() - 6, mux.midY() + 6), Point(mux.minX(), mux.midY() + 6)},
                         c, 2.4f, {6, 3});
        arrowHead({mux.minX(), mux.midY() + 6}, {mux.minX() - 6, mux.midY() + 6}, c);
        drawText(cat("from ", forwardSourceName(src)), Point(mux.maxX() + 6, y - 13), Fonts::mono(9, Weight::Semibold), c);
    };
    if (rec) {
        forwardPath(rec->forwardA, muxA, 0);
        forwardPath(rec->forwardB, muxB, 8);
    }

    // Branch redirect back to the PC.
    if (rec && rec->redirect) {
        uint32_t target = *rec->redirect;
        BranchStage bs = model->pipelineConfig().branchStage;
        float stageX = bs == BranchStage::ID ? colX(1) + colW / 2 : bs == BranchStage::EX ? colX(2) + colW / 2 : colX(3) + colW / 2;
        float y = top - 2;
        g.strokePolyline({Point(stageX, top + 30), Point(stageX, y), Point(pcBox.midX(), y), Point(pcBox.midX(), pcBox.minY())}, Theme::crimson, 2.2f);
        arrowHead({pcBox.midX(), pcBox.minY()}, {pcBox.midX(), y}, Theme::crimson);
        std::string msg = rec->flushed > 0 ? cat(rec->flushReason, ": flush ", rec->flushed, ", restart at ", hexAddr(target)) : "jump to " + hexAddr(target);
        Size sz = textSize(msg, Fonts::ui(10.5f, Weight::Semibold));
        Rect lr{std::max(colX(0) + 50, stageX - sz.w - 14), y - 9, sz.w + 12, 17};
        fillRounded(lr, 5, Theme::errorFill);
        drawText(msg, Rect(lr.minX() + 6, lr.minY(), sz.w + 2, lr.h), Fonts::ui(10.5f, Weight::Semibold), Theme::crimson);
    }
    if (rec && rec->stall && *rec->stall == StallReason::DCache) {
        drawText("the whole pipeline is frozen until the data arrives", Rect(colX(0), bottom + 6, colX(3) - colX(0), 14), Fonts::small, Theme::amber,
                 Align::Center);
    }
    drawText(cat("cycle ", p->cycle()), Rect(W - 120, bottom + 8, 110, 14), Fonts::mono(10.5f, Weight::Semibold), Theme::secondary, Align::Right);
}

// MARK: - Multi-cycle pipeline diagram

void PipelineChart::draw(Gfx& g, const Rect& dirty) {
    fillRect(bounds(), Theme::panel);
    Pipeline* p = model ? model->pipeline() : nullptr;
    if (!p || !model->good() || !model->good()->executable) return;
    const Executable& exe = *model->good()->executable;
    int cols = std::max(4, (int)((width() - labelW - 10) / colW));
    int maxRows = std::max(3, (int)((height() - 52) / rowH));
    const auto& history = p->history();
    if (history.empty()) {
        drawText("Press Step or Run: each row will be an instruction, each column a clock cycle.", Point(14, 14), Fonts::body, Theme::secondary);
        rowPCs_.clear();
        return;
    }
    size_t firstIdx = history.size() > (size_t)cols ? history.size() - cols : 0;
    struct C {
        int stage;
        Status status;
    };
    std::map<int, std::map<int, C>> cells;
    for (size_t h = firstIdx; h < history.size(); h++) {
        const CycleRecord& r = history[h];
        for (int k = 0; k < 5; k++)
            if (r.stages[k].seq >= 0) cells[r.stages[k].seq][r.cycle] = C{k, r.stages[k].status};
    }
    std::vector<int> seqs;
    for (auto& [s, m] : cells) seqs.push_back(s);
    if ((int)seqs.size() > maxRows) seqs.erase(seqs.begin(), seqs.end() - maxRows);
    rowPCs_.clear();
    // Header: cycle numbers.
    for (size_t h = firstIdx, j = 0; h < history.size(); h++, j++) {
        const CycleRecord& r = history[h];
        float x = labelW + j * colW;
        if (r.cycle == p->cycle()) fillRect({x, 0, colW, height()}, Theme::selection.alpha(0.6f));
        if (j % 2 == 0 || cols < 16) drawText(std::to_string(r.cycle), Rect(x, 4, colW, 14), Fonts::mono(9), Theme::faint, Align::Center);
    }
    line({0, 22.5f}, {width(), 22.5f}, Theme::border);
    const char* names[] = {"IF", "ID", "EX", "MEM", "WB"};
    for (int i = 0; i < (int)seqs.size(); i++) {
        int seq = seqs[i];
        float y = 26 + i * rowH;
        uint32_t pc = p->seqPC(seq).value_or(0);
        rowPCs_.push_back(pc);
        std::string txt = pc >= exe.textBase && pc < exe.textEnd() ? shortAsm(exe, pc) : "(fetch past the end)";
        bool flushed = false;
        for (auto& [cy, c] : cells[seq])
            if (c.status == Status::Flushed) flushed = true;
        TextBox box;
        box.truncate = true;
        g.drawAttributed(highlightAsm(txt, Fonts::mono(10.5f), flushed), {10, y + 3, labelW - 16, 16}, box);
        if (model->selection().pc && *model->selection().pc == pc) fillRect({0, y, 4, rowH - 1}, Theme::selectionStrong);
        for (size_t h = firstIdx, j = 0; h < history.size(); h++, j++) {
            auto it = cells[seq].find(history[h].cycle);
            if (it == cells[seq].end()) continue;
            const C& c = it->second;
            Rect cr{labelW + j * colW + 1.5f, y + 1.5f, colW - 3, rowH - 3};
            switch (c.status) {
            case Status::Flushed:
                fillRounded(cr, 4, Theme::errorFill);
                drawText("✕", cr, Fonts::ui(10, Weight::Bold), Theme::crimson, Align::Center);
                break;
            case Status::Stalled:
                fillRounded(cr, 4, Color::hex(0xEDE9E0));
                strokeRounded(cr, 4, Theme::borderStrong);
                drawText(names[c.stage], cr, Fonts::mono(9.5f, Weight::Semibold), Theme::faint, Align::Center);
                break;
            case Status::Waiting:
                fillRounded(cr, 4, Color::hex(0xFFF1C9));
                strokeRounded(cr, 4, Theme::amber.alpha(0.7f));
                drawText(names[c.stage], cr, Fonts::mono(9.5f, Weight::Semibold), Theme::amber, Align::Center);
                break;
            default:
                fillRounded(cr, 4, Theme::stage[c.stage].alpha(0.85f));
                drawText(names[c.stage], cr, Fonts::mono(9.5f, Weight::Bold), Color::white(), Align::Center);
            }
        }
    }
    // Legend.
    float ly = height() - 16;
    float lx = 10;
    std::tuple<const char*, Color, Color> legend[] = {{"stall (bubble behind it)", Color::hex(0xEDE9E0), Theme::secondary},
                                                      {"cache miss: waiting", Color::hex(0xFFF1C9), Theme::amber},
                                                      {"✕ flushed (wrong path)", Theme::errorFill, Theme::crimson}};
    for (auto& [t, f, fg] : legend) {
        Rect r = drawPill(t, Point(lx, ly - 2), Fonts::tiny, f, fg);
        lx = r.maxX() + 8;
    }
}

void PipelineChart::mouseDown(const MouseEvent& e) {
    Point p = localPoint(e);
    int i = (int)std::floor((p.y - 26) / rowH);
    if (i >= 0 && i < (int)rowPCs_.size() && onClickPC) onClickPC(rowPCs_[i]);
}

// MARK: - Stats and registers

void CPUStatsCanvas::draw(Gfx& g, const Rect& dirty) {
    fillRect(bounds(), Theme::panelAlt);
    line({0.5f, 0}, {0.5f, height()}, Theme::border);
    Pipeline* p = model ? model->pipeline() : nullptr;
    if (!p) return;
    const PipelineStats& s = p->stats();
    float y = 10;
    auto row = [&](const std::string& k, const std::string& v, Color c = Theme::text, bool bold = false) {
        drawText(k, Point(12, y), Fonts::small, Theme::secondary);
        drawText(v, Rect(12, y, width() - 24, 15), Fonts::mono(11, bold ? Weight::Bold : Weight::Medium), c, Align::Right);
        y += 16;
    };
    drawText("Performance", Point(12, y - 2), Fonts::subheading, Theme::heading);
    y += 22;
    row("Clock cycles", formatCount(s.cycles), Theme::text, true);
    row("Instructions completed", formatCount(s.retired));
    row("CPI (cycles per instruction)", s.retired > 0 ? format("%.2f", s.cpi()) : "—", s.cpi() > 1.6 ? Theme::crimson : Theme::green, true);
    y += 4;
    int stallTotal = 0;
    for (auto& [r, n] : s.stalls) stallTotal += n;
    row("Stall cycles", formatCount(stallTotal));
    for (StallReason r : {StallReason::LoadUse, StallReason::DataHazard, StallReason::BranchOperand, StallReason::Ecall, StallReason::ICache, StallReason::DCache}) {
        auto it = s.stalls.find(r);
        if (it != s.stalls.end() && it->second > 0) row(std::string("   ") + stallReasonName(r), formatCount(it->second), Theme::secondary);
    }
    row("Instructions flushed", formatCount(s.flushedInstructions));
    if (s.branches > 0) {
        double acc = 100.0 * (s.branches - s.mispredictions) / s.branches;
        row("Branches (predicted right)", cat(formatCount(s.branches), " (", format("%.0f", acc), "%)"));
    }
    row("Values forwarded", formatCount(s.forwards));
    y += 4;
    if (s.retired > 0) {
        // P&H's comparison: a single-cycle design has CPI = 1 but a clock as slow as its slowest instruction.
        double pipelinedPS = s.cycles * 200.0, singlePS = s.retired * 800.0;
        std::string txt = cat("Single-cycle CPU: ", formatCount(s.retired), " cycles × 800 ps.\nThis pipeline: ", formatCount(s.cycles),
                              " cycles × 200 ps.\n", format("Speed-up ≈ %.1f×", singlePS / pipelinedPS));
        TextBox box;
        box.wrap = true;
        g.drawTextBox(txt, {12, y, width() - 20, 46}, Fonts::tiny, Theme::secondary, box);
        y += 46;
    }
    line({10, y}, {width() - 10, y}, Theme::border);
    y += 8;
    drawText("Register file", Point(12, y - 2), Fonts::subheading, Theme::heading);
    y += 20;
    std::optional<int> written;
    if (p->lastRecord() && p->lastRecord()->registerWrite) written = p->lastRecord()->registerWrite->first;
    float colW = (width() - 16) / 2;
    for (int r = 0; r < 32; r++) {
        int col = r / 16, rw = r % 16;
        float x = 8 + col * colW;
        float ry = y + rw * 15;
        if (ry > height() - 4) continue;
        if (written == r) fillRect({x, ry, colW - 2, 15}, Theme::stageFill(4));
        uint32_t v = p->reg(r);
        drawText(Reg::name(r), Rect(x + 4, ry, 30, 15), Fonts::mono(10, Weight::Semibold), written == r ? Theme::stage[4] : Theme::reg);
        std::string shown = (r == Reg::sp || r == Reg::ra || r == Reg::gp || (v > 0xFFFF && v < 0xFFFF0000u)) ? hex8(v) : std::to_string((int32_t)v);
        drawText(shown, Rect(x + 30, ry, colW - 36, 15), Fonts::mono(10), v == 0 ? Theme::faint : Theme::text, Align::Right);
    }
}

// MARK: - Pipeline view

static std::vector<std::string> predictorNames() {
    std::vector<std::string> n;
    for (auto k : allPredictors()) n.push_back(predictorName(k));
    return n;
}

PipelineView::PipelineView() : predictor(predictorNames()) {
    for (View* v : std::initializer_list<View*>{&datapath, &chart, &stats, &forwarding, &branchStage, &predictor, &branchLabel, &caches, &empty})
        addSubview(v);
    caches.tooltip = "Off = ideal memory: every fetch and load takes one cycle, so only hazards stall the pipeline";
    caches.onChange = [this](bool on) {
        if (!model) return;
        PipelineConfig c = model->pipelineConfig();
        c.hierarchy.enabled = on;
        model->setPipelineConfig(c);
    };
    forwarding.onChange = [this](int k) {
        if (!model) return;
        PipelineConfig c = model->pipelineConfig();
        c.forwarding = k == 0;
        model->setPipelineConfig(c);
    };
    branchStage.onChange = [this](int k) {
        if (!model) return;
        PipelineConfig c = model->pipelineConfig();
        c.branchStage = allBranchStages()[k];
        model->setPipelineConfig(c);
    };
    predictor.onChange = [this](int k) {
        if (!model) return;
        PipelineConfig c = model->pipelineConfig();
        c.predictor = allPredictors()[k];
        model->setPipelineConfig(c);
    };
    chart.onClickPC = [this](uint32_t pc) {
        if (!model) return;
        Selection s;
        s.line = model->lineOfPC(pc);
        s.pc = pc;
        s.origin = Station::Pipeline;
        model->setSelection(s);
    };
}

void PipelineView::layout() {
    float sw = 236;
    Size fs = forwarding.intrinsicSize();
    forwarding.setFrame({10, 8, fs.w, 24});
    branchLabel.setFrame({forwarding.frame().maxX() + 16, 12, 124, 16});
    Size bs = branchStage.intrinsicSize();
    branchStage.setFrame({branchLabel.frame().maxX() + 2, 8, bs.w + 10, 24});
    predictor.setFrame({branchStage.frame().maxX() + 14, 7, 250, 26});
    caches.setFrame({predictor.frame().maxX() + 14, 10, caches.intrinsicSize().w, 20});
    float top = 40;
    float dh = std::min(310.0f, std::max(250.0f, (height() - top) * 0.52f));
    datapath.setFrame({0, top, width() - sw, dh});
    chart.setFrame({0, top + dh, width() - sw, height() - top - dh});
    stats.setFrame({width() - sw, top, sw, height() - top});
    empty.setFrame(bounds());
}

void PipelineView::draw(Gfx& g, const Rect& dirty) {
    fillRect(bounds(), Theme::panel);
    line({0, 39.5f}, {width(), 39.5f}, Theme::border);
    line({0, chart.frame().minY() + 0.5f}, {chart.frame().maxX(), chart.frame().minY() + 0.5f}, Theme::border);
}

void PipelineView::reload(AppModel& m, ModelEvent e) {
    model = &m;
    datapath.model = &m;
    chart.model = &m;
    stats.model = &m;
    empty.setHidden(m.pipeline() != nullptr);
    if (!m.pipeline()) {
        empty.title = "Nothing to run";
        empty.message = "Fix the errors in the program first.";
        empty.setNeedsDisplay();
    }
    forwarding.setSelected(m.pipelineConfig().forwarding ? 0 : 1);
    caches.setOn(m.pipelineConfig().hierarchy.enabled);
    for (int k = 0; k < 3; k++)
        if (allBranchStages()[k] == m.pipelineConfig().branchStage) branchStage.setSelected(k);
    for (int k = 0; k < 3; k++)
        if (allPredictors()[k] == m.pipelineConfig().predictor) predictor.select(k);
    datapath.setNeedsDisplay();
    chart.setNeedsDisplay();
    stats.setNeedsDisplay();
}

}  // namespace ui
