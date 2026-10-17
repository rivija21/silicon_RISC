#include "Views.h"

namespace ui {

using namespace sp;

static Color registerColor(const Location& loc) {
    if (loc.kind == Location::Spill) return Theme::crimson;
    auto has = [](const std::vector<int>& v, int r) { return std::find(v.begin(), v.end(), r) != v.end(); };
    if (has(Reg::callerSaved, loc.n)) return Color::hex(0x4A86D0);
    if (has(Reg::argumentTemps, loc.n)) return Color::hex(0x8A6BC4);
    return Color::hex(0x2E9467);
}

// MARK: - Live intervals

void IntervalCanvas::recompute(float w) {
    int n = alloc ? (int)alloc->intervals.size() : 0;
    size_ = Size(std::max(260.0f, w), 44 + n * rowH + 30);
    setNeedsDisplay();
}

void IntervalCanvas::draw(Gfx& g, const Rect& dirty) {
    fillRect(dirty, Theme::panel);
    if (!alloc || !function || alloc->maxPosition <= 0) {
        drawText("No virtual registers.", Point(12, 12), Fonts::small, Theme::faint);
        return;
    }
    const Allocation& a = *alloc;
    const IRFunction& f = *function;
    float x0 = labelW, x1 = width() - 12;
    auto xp = [&](int p) { return x0 + (x1 - x0) * p / (float)a.maxPosition; };
    // Block boundaries.
    for (auto& b : f.blocks) {
        auto s = a.blockStart.find(b->id);
        if (s == a.blockStart.end()) continue;
        float x = xp(s->second - 1);
        line({x, 26}, {x, height() - 20}, Theme::grid, 1);
    }
    float lx = 8;
    std::pair<const char*, Color> legend[] = {{"t-reg", Color::hex(0x4A86D0)},
                                              {"a-reg (as temp)", Color::hex(0x8A6BC4)},
                                              {"s-reg (callee-saved)", Color::hex(0x2E9467)},
                                              {"spilled", Theme::crimson}};
    for (auto& [name, c] : legend) {
        fillRounded({lx, 8, 10, 8}, 2, c);
        drawText(name, Point(lx + 13, 4), Fonts::tiny, Theme::secondary);
        lx += textSize(name, Fonts::tiny).w + 26;
    }
    for (int c : a.callPositions) {
        float x = xp(c);
        line({x, 24}, {x, height() - 18}, Theme::amber.alpha(0.7f), 1, {3, 2});
        drawText("call", Point(x - 9, height() - 17), Fonts::tiny, Theme::amber);
    }
    float y = 30;
    for (auto& iv : a.intervals) {
        drawText(iv.name, Rect(6, y, labelW - 10, rowH), Fonts::mono(10.5f), f.vregs[iv.vreg].isVariable ? Theme::green : Theme::secondary, Align::Right);
        float bx = xp(iv.start), bw = std::max(4.0f, xp(iv.end) - bx);
        Rect r{bx, y + 2.5f, bw, rowH - 5};
        Color c = registerColor(iv.location);
        bool isSpill = iv.location.kind == Location::Spill;
        if (isSpill) {
            fillRounded(r, 3, c.alpha(0.18f));
            strokeRounded(r, 3, c, 1);
            g.pushClip(r);
            for (float hx = r.minX() - r.h; hx < r.maxX(); hx += 5) line({hx, r.maxY()}, {hx + r.h, r.minY()}, c.alpha(0.5f), 1);
            g.popClip();
        } else {
            fillRounded(r, 3, c.alpha(0.85f));
        }
        std::string label = isSpill ? cat("spill ", iv.location.n) : Reg::name(iv.location.n);
        float lw = textSize(label, Fonts::mono(9.5f, Weight::Semibold)).w;
        if (bw > lw + 6) drawText(label, Rect(bx + 4, y, bw - 4, rowH), Fonts::mono(9.5f, Weight::Semibold), isSpill ? c : Color::white());
        else drawText(label, Rect(bx + bw + 3, y, lw + 10, rowH), Fonts::mono(9.5f, Weight::Semibold), c);
        y += rowH;
    }
}

// MARK: - Stack frame

float FrameCanvas::entryHeight(const FrameLayout::Entry& e) { return std::min(80.0f, std::max(20.0f, e.size / 4.0f * 9)); }

void FrameCanvas::recompute(float w) {
    float total = 0;
    if (frameLayout)
        for (auto& e : frameLayout->entries) total += entryHeight(e);
    size_ = Size(std::max(260.0f, w), total + 110);
    setNeedsDisplay();
}

void FrameCanvas::draw(Gfx& g, const Rect& dirty) {
    fillRect(dirty, Theme::panel);
    if (!frameLayout) return;
    const FrameLayout& fr = *frameLayout;
    if (!fr.hasFrame) {
        drawText("Leaf function: no stack frame at all.", Point(14, 14), Fonts::body, Theme::text);
        drawText("It makes no calls and keeps every value in registers,", Point(14, 34), Fonts::small, Theme::secondary);
        drawText("so ra and sp are never touched.", Point(14, 50), Fonts::small, Theme::secondary);
        return;
    }
    float x = 96, w = std::min(220.0f, width() - 110);
    float y = 30;
    drawText("higher addresses — the caller's frame", Point(x, 8), Fonts::tiny, Theme::faint);
    line({x, y}, {x + w, y}, Theme::text, 1.5f);
    drawText("s0 →", Point(52, y - 10), Fonts::mono(11, Weight::Semibold), Theme::ink);
    std::map<std::string, Color> colors = {{"ra", Color::hex(0xF6E2C8)},
                                           {"fp", Color::hex(0xF3E9D6)},
                                           {"saved", Color::hex(0xDDEEE3)},
                                           {"spill", Color::hex(0xF7DCD8)},
                                           {"local", Color::hex(0xE3ECF8)}};
    int deepest = 0;
    for (auto& e : fr.entries) {
        float eh = entryHeight(e);
        Rect r{x, y, w, eh};
        auto c = colors.find(e.kind);
        fillRect(r, c == colors.end() ? Theme::panelAlt : c->second);
        strokeRect(r.insetBy(0.5f, 0.5f), Theme::borderStrong);
        drawText(e.what, r.insetBy(8, 0), Fonts::small, Theme::text);
        drawText(cat(e.offset, "(s0)"), Rect(8, y, x - 14, std::min(eh, 20.0f)), Fonts::mono(10), Theme::secondary, Align::Right);
        y += eh;
        deepest = std::max(deepest, -e.offset);
    }
    int pad = fr.size - deepest;
    if (pad > 0) {
        Rect r{x, y, w, 14};
        fillRect(r, Color::hex(0xF4F1EA));
        drawText("padding to a multiple of 16", r.insetBy(8, 0), Fonts::tiny, Theme::faint);
        y += 14;
    }
    line({x, y}, {x + w, y}, Theme::text, 1.5f);
    drawText("sp →", Point(52, y - 3), Fonts::mono(11, Weight::Semibold), Theme::ink);
    drawText(cat("frame size ", fr.size, " bytes (sp = s0 − ", fr.size, ")"), Point(x, y + 7), Fonts::small, Theme::secondary);
    drawText("lower addresses — the stack grows down", Point(x, y + 24), Fonts::tiny, Theme::faint);
}

// MARK: - Assembly view

AsmView::AsmView() {
    for (View* v : std::initializer_list<View*>{&listing, &intervalPanel, &framePanel, &functionPicker, &regs, &empty}) addSubview(v);
    functionPicker.onChange = [this](int k) {
        if (model && k < (int)functions_.size()) model->setFocusFunction(functions_[k]);
    };
    regs.onChange = [this](int k) {
        if (!model) return;
        CompileOptions o = model->options();
        static const std::optional<int> limits[] = {std::nullopt, 6, 3};
        o.registerLimit = limits[k];
        model->setOptions(o);
    };
    listing.list.onClick = [this](int, const Row& r) {
        if (!model) return;
        Selection s;
        s.line = r.line;
        s.pc = r.pc;
        s.origin = Station::Asm;
        model->setSelection(s);
        CompilationP c = model->shown();
        if (c && r.tag >= 0 && r.tag < (int)c->asmLines.size() && c->asmLines[r.tag].function) model->setFocusFunction(c->asmLines[r.tag].function);
    };
    listing.list.highlight = [this](const Row& r) -> std::optional<Color> {
        if (!model) return std::nullopt;
        const Selection& s = model->selection();
        if (s.pc && r.pc == s.pc) return Theme::selectionStrong.alpha(0.6f);
        if (s.line && r.line == s.line) return Theme::selection;
        return std::nullopt;
    };
}

void AsmView::layout() {
    float rw = std::min(420.0f, std::max(320.0f, width() * 0.42f));
    float lw = width() - rw;
    listing.setFrame({0, 0, lw, height()});
    functionPicker.setFrame({lw + 12, 34, 150, 26});
    Size rs = regs.intrinsicSize();
    regs.setFrame({width() - rs.w - 12, 35, rs.w, 24});
    float half = (height() - 66) * 0.55f;
    intervalPanel.setFrame({lw, 66, rw, half});
    framePanel.setFrame({lw, 66 + half + 30, rw, height() - 66 - half - 30});
    intervals.recompute(rw - 4);
    frameCanvas.recompute(rw - 4);
    empty.setFrame(bounds());
}

void AsmView::draw(Gfx& g, const Rect& dirty) {
    fillRect(bounds(), Theme::panel);
    float lw = listing.frame().w;
    line({lw + 0.5f, 0}, {lw + 0.5f, height()}, Theme::border);
    drawText("Register allocation", Point(lw + 12, 8), Fonts::subheading, Theme::heading);
    drawText("linear scan over live intervals", Point(lw + 150, 11), Fonts::tiny, Theme::secondary);
    float fy = framePanel.frame().minY() - 24;
    line({lw, fy - 4.5f}, {width(), fy - 4.5f}, Theme::border);
    drawText("Stack frame", Point(lw + 12, fy - 1), Fonts::subheading, Theme::heading);
    drawText("what the prologue builds", Point(lw + 104, fy + 2), Fonts::tiny, Theme::secondary);
}

void AsmView::reload(AppModel& m, ModelEvent e) {
    model = &m;
    CompilationP c = m.shown();
    if (!c || c->asmLines.empty()) {
        empty.setHidden(false);
        empty.title = "No assembly yet";
        empty.message = "Fix the errors in the program first.";
        empty.setNeedsDisplay();
        return;
    }
    empty.setHidden(true);
    shownCompilation_ = c;
    static const std::optional<int> limits[] = {std::nullopt, 6, 3};
    int ri = 0;
    for (int k = 0; k < 3; k++)
        if (limits[k] == c->options.registerLimit) ri = k;
    regs.setSelected(ri);
    regs.setEnabledMask(c->isAssembly ? std::optional<std::vector<bool>>(std::vector<bool>{false, false, false}) : std::nullopt);
    if (e == ModelEvent::Compiled || e == ModelEvent::Config || listing.list.rows().empty()) {
        pcsByAsmLine_.clear();
        if (c->executable)
            for (auto& ins : c->executable->instructions)
                if (ins.object == "main.o") pcsByAsmLine_[ins.asmLine].push_back(ins.address);
        std::vector<Row> rows;
        for (int k = 0; k < (int)c->asmLines.size(); k++) {
            const AsmLine& l = c->asmLines[k];
            Row r;
            switch (l.kind) {
            case AsmLine::Kind::Blank: r = Row({}, Row::Kind::Normal, 8); break;
            case AsmLine::Kind::Comment:
                r = Row({Cell(AttrString(trimmed(l.text), Fonts::mono(11), Theme::comment), std::nullopt, Align::Left, std::nullopt, c->isAssembly ? 0.0f : 6.0f)},
                        Row::Kind::Normal, 18);
                break;
            case AsmLine::Kind::Label:
                r = Row({Cell(highlightAsm(trimmed(l.text))), Cell(l.note, Fonts::small, Theme::faint, 200.0f)}, Row::Kind::Normal, 21);
                break;
            default: {
                auto it = pcsByAsmLine_.find(k);
                std::optional<uint32_t> pc;
                if (it != pcsByAsmLine_.end() && !it->second.empty()) pc = it->second.front();
                std::string addr = pc ? format("%06x", *pc & 0xFFFFFF) : "";
                r = Row({Cell(addr, Fonts::mono(10), Theme::faint, 54.0f), Cell(highlightAsm(trimmed(l.text)), std::nullopt, Align::Left, std::nullopt, 14),
                         Cell(l.note, Fonts::small, Theme::secondary, 210.0f)});
                r.pc = pc;
                r.tooltip = l.note;
            }
            }
            r.tag = k;
            if (c->isAssembly) r.line = k + 1;
            else if (l.origin && l.origin->line > 0) r.line = l.origin->line;
            rows.push_back(r);
        }
        listing.set(rows);
    }
    // Mark rows whose instructions are in the pipeline now.
    if (Pipeline* p = m.pipeline(); p && p->lastRecord()) {
        const CycleRecord& rec = *p->lastRecord();
        std::map<uint32_t, int> stageOf;
        for (int k = 0; k < 5; k++)
            if (rec.stages[k].seq >= 0 && rec.stages[k].status != StageSlot::Status::Flushed) stageOf[rec.stages[k].pc] = k;
        for (auto& row : listing.list.mutableRows()) {
            row.marker.reset();
            auto it = pcsByAsmLine_.find(row.tag);
            if (it == pcsByAsmLine_.end()) continue;
            int best = -1;
            for (uint32_t pc : it->second) {
                auto s = stageOf.find(pc);
                if (s != stageOf.end()) best = std::max(best, s->second);
            }
            if (best >= 0) row.marker = Theme::stage[best];
        }
    }
    listing.list.setNeedsDisplay();

    functions_.clear();
    if (c->ir)
        for (auto& f : c->ir->functions) functions_.push_back(f->name);
    std::optional<std::string> focus;
    if (m.focusFunction() && std::find(functions_.begin(), functions_.end(), *m.focusFunction()) != functions_.end()) focus = m.focusFunction();
    else if (std::find(functions_.begin(), functions_.end(), "main") != functions_.end()) focus = "main";
    else if (!functions_.empty()) focus = functions_[0];
    functionPicker.setHidden(functions_.empty());
    int fi = 0;
    for (int k = 0; k < (int)functions_.size(); k++)
        if (focus && functions_[k] == *focus) fi = k;
    functionPicker.set(functions_, fi);
    intervals.alloc = nullptr;
    intervals.function = nullptr;
    frameCanvas.frameLayout = nullptr;
    if (focus && !c->isAssembly) {
        auto a = c->allocations.find(*focus);
        if (a != c->allocations.end()) intervals.alloc = &a->second;
        for (auto& f : c->ir->functions)
            if (f->name == *focus) intervals.function = f.get();
        for (auto& fr : c->frames)
            if (fr.function == *focus) frameCanvas.frameLayout = &fr;
    }
    intervals.recompute(intervalPanel.width() - 4);
    frameCanvas.recompute(framePanel.width() - 4);
    intervalPanel.setNeedsLayout();
    framePanel.setNeedsLayout();

    auto& rs = listing.list.rows();
    if (e == ModelEvent::Selection && m.selection().origin != Station::Asm) {
        bool done = false;
        if (m.selection().pc)
            for (int k = 0; k < (int)rs.size() && !done; k++)
                if (rs[k].pc == m.selection().pc) {
                    listing.list.reveal(k);
                    done = true;
                }
        if (!done && m.selection().line)
            for (int k = 0; k < (int)rs.size(); k++)
                if (rs[k].line == m.selection().line && rs[k].pc) {
                    listing.list.reveal(k);
                    break;
                }
    }
    if (e == ModelEvent::Stepped && !m.running() && m.station() == Station::Asm) {
        if (Pipeline* p = m.pipeline(); p && p->lastRecord()) {
            const CycleRecord& rec = *p->lastRecord();
            for (int k = 4; k >= 0; k--) {
                if (rec.stages[k].seq < 0) continue;
                for (int i = 0; i < (int)rs.size(); i++)
                    if (rs[i].pc == rec.stages[k].pc) {
                        listing.list.reveal(i);
                        break;
                    }
                break;
            }
        }
    }
}

}  // namespace ui
