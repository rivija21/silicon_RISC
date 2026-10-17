#include "Views.h"

namespace ui {

using namespace sp;

/// Colours one IR instruction's text.
static AttrString irAttributed(const std::string& text, const Font& font = Fonts::code) {
    AttrString out;
    std::string tok;
    static const std::set<std::string> mnemonics = {"load.w", "load.b", "store.w", "store.b", "call", "goto", "if", "else", "return", "(char)"};
    static const std::set<std::string> bold = {"call", "goto", "if", "else", "return", "load.w", "load.b", "store.w", "store.b"};
    auto flush = [&] {
        if (tok.empty()) return;
        Color c;
        bool isTemp = hasPrefix(tok, "%t") && tok.size() > 2 && std::all_of(tok.begin() + 2, tok.end(), [](char ch) { return ch >= '0' && ch <= '9'; });
        if (hasPrefix(tok, "%")) c = isTemp ? Theme::secondary : Theme::green;
        else if (hasPrefix(tok, "[$") || hasPrefix(tok, "$")) c = Theme::purple;
        else if (hasPrefix(tok, "[@") || hasPrefix(tok, "@")) c = Theme::label;
        else if (mnemonics.count(tok)) c = Theme::mnemonic;
        else if ((tok[0] >= '0' && tok[0] <= '9') || (tok[0] == '-' && tok.size() > 1)) c = Theme::number;
        else if (contains(tok, ".") || std::isalpha((unsigned char)tok[0])) c = Theme::label;
        else c = Theme::punct;
        Font f = bold.count(tok) ? Fonts::mono(font.size, Weight::Semibold) : font;
        out.add(tok, f, c);
        tok.clear();
    };
    std::u16string u = toUTF16(text);
    for (char16_t ch : u) {
        if (ch == u' ' || ch == u',' || ch == u'(' || ch == u')' || ch == u']' || ch == 0x2190) {
            flush();
            out.add(toUTF8(std::u16string(1, ch)), font, Theme::punct);
        } else if (ch == u'[') {
            flush();
            out.add("[", font, Theme::punct);
        } else {
            tok += toUTF8(std::u16string(1, ch));
        }
    }
    flush();
    return out;
}

// MARK: - Pass list

void PassList::draw(Gfx& g, const Rect& dirty) {
    fillRect(bounds(), Theme::panelAlt);
    line({width() - 0.5f, 0}, {width() - 0.5f, height()}, Theme::border);
    drawText("Optimiser passes", Point(12, 8), Fonts::subheading, Theme::heading);
    drawText("click a step to see the IR after it", Point(12, 28), Fonts::tiny, Theme::secondary);
    presetRects_.clear();
    float px = 12;
    for (const char* t : {"-O0  none", "-O1  all"}) {
        float w = textSize(t, Fonts::small).w + 16;
        Rect r{px, 46, w, 20};
        fillRounded(r, 5, Theme::panel);
        strokeRounded(r, 5, Theme::borderStrong);
        drawText(t, r, Fonts::ui(11, Weight::Medium), Theme::ink, Align::Center);
        presetRects_.push_back(r);
        px += w + 6;
    }
    rects_.clear();
    checkRects_.clear();
    float y = 76;
    for (int k = 0; k < (int)entries.size(); k++) {
        const Entry& e = entries[k];
        float h = e.pass ? 40 : 36;
        Rect r{8, y, width() - 16, h};
        rects_.push_back(r);
        if (k == selected) {
            drawCard(r, 6, Color::hex(0xE8EFF9));
            strokeRounded(r, 6, Theme::ink.alpha(0.5f));
        } else if (e.snapshot) {
            fillRounded(r, 6, Theme::panel.alpha(0.6f));
        }
        float tx = r.minX() + 10;
        if (e.pass) {
            Rect cb{r.minX() + 8, r.minY() + 8, 14, 14};
            checkRects_.push_back(cb);
            fillRounded(cb, 3.5f, e.enabled ? Theme::ink : Theme::panel);
            strokeRounded(cb, 3.5f, e.enabled ? Theme::ink : Theme::borderStrong);
            if (e.enabled)
                g.strokePolyline({Point(cb.minX() + 3.2f, cb.midY()), Point(cb.minX() + 6, cb.maxY() - 3.5f), Point(cb.maxX() - 3, cb.minY() + 3.5f)},
                                 Color::white(), 1.8f);
            tx = cb.maxX() + 8;
        } else {
            checkRects_.push_back(Rect());
        }
        Color fg = e.enabled || !e.pass ? Theme::text : Theme::faint;
        drawText(e.title, Rect(tx, r.minY() + 5, r.maxX() - tx - 8, 16), Fonts::ui(11.5f, Weight::Semibold), fg);
        std::string note;
        if (!e.snapshot) note = e.pass ? "off" : "";
        else if (!e.pass && !e.isFinal) note = cat(e.delta, " instructions");
        else if (!e.changed) note = "no change";
        else note = e.delta == 0 ? "rewrote instructions" : (e.delta < 0 ? cat(e.delta, " instructions") : cat("+", e.delta, " instructions"));
        Color nc = e.delta < 0 ? Theme::green : (e.changed ? Theme::amber : Theme::faint);
        drawText(note, Rect(tx, r.minY() + 21, r.maxX() - tx - 8, 14), Fonts::tiny, e.snapshot ? nc : Theme::faint);
        y += h + 4;
    }
}

void PassList::mouseDown(const MouseEvent& ev) {
    Point p = localPoint(ev);
    for (int k = 0; k < (int)presetRects_.size(); k++)
        if (presetRects_[k].contains(p)) {
            if (onPreset) onPreset(k == 1);
            return;
        }
    for (int k = 0; k < (int)rects_.size(); k++) {
        if (!rects_[k].contains(p)) continue;
        if (entries[k].pass && checkRects_[k].insetBy(-4, -4).contains(p)) {
            if (onToggle) onToggle(*entries[k].pass, !entries[k].enabled);
            return;
        }
        if (entries[k].snapshot) {
            selected = k;
            if (onSelect) onSelect(k);
            setNeedsDisplay();
        }
        return;
    }
}

// MARK: - Control-flow graph

void CFGCanvas::recompute() {
    boxes_.clear();
    order_.clear();
    if (!function) return;
    float y = 20;
    float lineH = 15;
    float x = 70;
    for (auto& b : function->blocks) {
        int n = std::min((int)b->insts.size(), 8) + ((int)b->insts.size() > 8 ? 1 : 0) + 1;
        float h = 24 + n * lineH + 6;
        boxes_[b->id] = Rect(x, y, boxW, h);
        order_.push_back(b->id);
        y += h + 34;
    }
    size_ = Size(x + boxW + 150, y + 10);
    setNeedsDisplay();
}

void CFGCanvas::draw(Gfx& g, const Rect& dirty) {
    fillRect(dirty, Theme::panel);
    // Graph-paper background.
    for (float gx = 0; gx < width(); gx += 20) line({gx + 0.5f, 0}, {gx + 0.5f, height()}, Theme::grid);
    for (float gy = 0; gy < height(); gy += 20) line({0, gy + 0.5f}, {width(), gy + 0.5f}, Theme::grid);
    if (!function) return;
    const IRFunction& f = *function;
    // Edges first.
    float rightLane = 0, leftLane = 0;
    for (int k = 0; k < (int)order_.size(); k++) {
        int id = order_[k];
        Block* b = f.block(id);
        if (!b) continue;
        Rect r = boxes_[id];
        auto succ = b->term.successors();
        for (int si = 0; si < (int)succ.size(); si++) {
            int s = succ[si];
            auto tb = boxes_.find(s);
            if (tb == boxes_.end()) continue;
            Rect t = tb->second;
            std::string label = b->term.kind == Terminator::Branch ? (si == 0 ? "true" : "false") : "";
            Color color = label == "true" ? Theme::green : label == "false" ? Theme::crimson : Theme::secondary;
            bool isNext = k + 1 < (int)order_.size() && order_[k + 1] == s;
            if (isNext) {
                Point a{r.midX() + (succ.size() > 1 ? -40 : 0), r.maxY()};
                Point e{t.midX() + (succ.size() > 1 ? -40 : 0), t.minY()};
                line(a, e, color, 1.6f);
                arrowHead(e, a, color);
                if (!label.empty()) drawText(label, Point(a.x + 5, a.y + 6), Fonts::tiny, color);
            } else if (t.minY() > r.minY()) {
                rightLane += 14;
                float lx = r.maxX() + 12 + rightLane;
                g.strokePolyline({Point(r.maxX(), r.maxY() - 10), Point(lx, r.maxY() - 10), Point(lx, t.minY() + 12), Point(t.maxX(), t.minY() + 12)}, color,
                                 1.6f);
                arrowHead({t.maxX(), t.minY() + 12}, {lx, t.minY() + 12}, color);
                if (!label.empty()) drawText(label, Point(r.maxX() + 4, r.maxY() - 26), Fonts::tiny, color);
            } else {
                // A back edge: this is a loop.
                leftLane += 12;
                float lx = r.minX() - 12 - leftLane;
                Color loopColor = label.empty() ? Theme::ink : color;
                g.strokePolyline({Point(r.minX(), r.maxY() - 10), Point(lx, r.maxY() - 10), Point(lx, t.minY() + 12), Point(t.minX(), t.minY() + 12)},
                                 loopColor, 1.8f, {5, 3});
                arrowHead({t.minX(), t.minY() + 12}, {lx, t.minY() + 12}, loopColor);
                drawText(label.empty() ? "loop" : label, Point(lx - 2, (r.maxY() + t.minY()) / 2), Fonts::tiny, loopColor);
            }
        }
    }
    for (int id : order_) {
        Block* b = f.block(id);
        if (!b) continue;
        Rect r = boxes_[id];
        bool hasSel = false;
        if (selectedLine) {
            for (auto& i : b->insts)
                if (i.line == *selectedLine) hasSel = true;
            if (b->termLine == *selectedLine) hasSel = true;
        }
        drawCard(r, 6, hasSel ? Color::hex(0xFFF8E3) : Theme::panel);
        if (hasSel) strokeRounded(r, 6, Theme::amber, 1.5f);
        Rect head{r.minX(), r.minY(), r.w, 22};
        fillRounded(head.insetBy(0.5f, 0.5f), 6, Color::hex(0xEFE7D8));
        fillRect({r.minX() + 0.5f, r.minY() + 12, r.w - 1, 10}, Color::hex(0xEFE7D8));
        drawText(b->name + ":", Point(r.minX() + 8, r.minY() + 3), Fonts::mono(11, Weight::Semibold), Theme::label);
        float y = r.minY() + 26;
        int shown = 0;
        for (auto& i : b->insts) {
            if (shown++ >= 8) break;
            AttrString a = irAttributed(IRPrinter::inst(i, f), Fonts::mono(10));
            if (changed.count(i.id)) fillRect({r.minX() + 3, y, r.w - 6, 15}, Theme::okFill);
            TextBox box;
            box.truncate = true;
            g.drawAttributed(a, {r.minX() + 10, y + 1, r.w - 16, 14}, box);
            y += 15;
        }
        if (b->insts.size() > 8) {
            drawText(cat("… ", (int)b->insts.size() - 8, " more"), Point(r.minX() + 10, y + 1), Fonts::mono(10), Theme::faint);
            y += 15;
        }
        TextBox box;
        box.truncate = true;
        g.drawAttributed(irAttributed(IRPrinter::term(b->term, f), Fonts::mono(10, Weight::Semibold)), {r.minX() + 10, y + 1, r.w - 16, 14}, box);
    }
}

// MARK: - IR view

IRView::IRView() {
    for (View* v : std::initializer_list<View*>{&passes, &listing, &cfgPanel, &mode, &functionPicker, &explanation, &empty}) addSubview(v);
    explanation.wraps = true;
    cfgPanel.setHidden(true);
    mode.onChange = [this](int k) {
        listing.setHidden(k == 1);
        cfgPanel.setHidden(k == 0);
        functionPicker.setHidden(k == 0);
        setNeedsLayout();
    };
    functionPicker.setHidden(true);
    functionPicker.onChange = [this](int k) {
        if (model && k < (int)functions_.size()) model->setFocusFunction(functions_[k]);
    };
    passes.onSelect = [this](int k) {
        if (model && passes.entries[k].snapshot) model->setIRSnapshot(passes.entries[k].snapshot);
    };
    passes.onToggle = [this](PassID p, bool on) {
        if (!model) return;
        CompileOptions o = model->options();
        if (on) o.passes.insert(p);
        else o.passes.erase(p);
        model->setIRSnapshot(std::nullopt);
        model->setOptions(o);
    };
    passes.onPreset = [this](bool all) {
        if (!model) return;
        CompileOptions o = model->options();
        o.passes = all ? std::set<PassID>(allPasses().begin(), allPasses().end()) : std::set<PassID>{};
        model->setIRSnapshot(std::nullopt);
        model->setOptions(o);
    };
    listing.list.onClick = [this](int, const Row& r) {
        if (!model) return;
        Selection s;
        s.line = r.line;
        s.irInst = r.irInst;
        s.origin = Station::IR;
        model->setSelection(s);
    };
    listing.list.highlight = [this](const Row& r) -> std::optional<Color> {
        if (model && model->selection().line && r.line == model->selection().line) return Theme::selection;
        return std::nullopt;
    };
}

void IRView::layout() {
    float pw = 250;
    passes.setFrame({0, 0, pw, height()});
    float x = pw + 10;
    Size ms = mode.intrinsicSize();
    mode.setFrame({x, 8, ms.w, 24});
    functionPicker.setFrame({mode.frame().maxX() + 10, 7, 170, 26});
    explanation.setFrame({x, 38, width() - x - 12, 44});
    float top = 86;
    listing.setFrame({pw, top, width() - pw, height() - top});
    cfgPanel.setFrame(listing.frame());
    empty.setFrame(bounds());
}

void IRView::draw(Gfx& g, const Rect& dirty) {
    fillRect(bounds(), Theme::panel);
    line({250, 85.5f}, {width(), 85.5f}, Theme::border);
}

void IRView::reload(AppModel& m, ModelEvent e) {
    model = &m;
    CompilationP c = m.shown();
    if (m.isAssembly() || !c || c->snapshots.empty()) {
        empty.setHidden(false);
        empty.title = "No IR";
        empty.message = m.isAssembly() ? "Hand-written assembly skips the compiler." : "Fix the errors in the program first.";
        empty.setNeedsDisplay();
        return;
    }
    empty.setHidden(true);
    const auto& snaps = c->snapshots;
    int shownIndex = std::min(m.irSnapshot().value_or((int)snaps.size() - 1), (int)snaps.size() - 1);
    // Pass list entries: all passes, with the ones that ran pointing at their snapshot.
    std::vector<PassList::Entry> entries;
    entries.push_back({"Unoptimised IR", std::nullopt, 0, snaps[0].instructions, false, true, false});
    int selectedEntry = 0;
    for (PassID p : allPasses()) {
        std::optional<int> k;
        for (int i = 0; i < (int)snaps.size(); i++)
            if (snaps[i].pass == p) {
                k = i;
                break;
            }
        int delta = k ? snaps[*k].instructions - snaps[*k - 1].instructions : 0;
        std::string title = passTitle(p);
        size_t paren = title.find(" (");
        if (paren != std::string::npos) title = title.substr(0, paren);
        title = replaceAll(title, "Promote locals to registers", "locals → registers");
        entries.push_back({cat(passShortTitle(p), " — ", title), p, k, delta, k ? snaps[*k].changed : false, c->options.passes.count(p) > 0, false});
        if (k && *k == shownIndex) selectedEntry = (int)entries.size() - 1;
    }
    if (!snaps.back().pass && snaps.size() > 1) {
        int k = (int)snaps.size() - 1;
        entries.push_back({"Repeat until nothing changes", std::nullopt, k, snaps.back().instructions - snaps[k - 1].instructions, snaps.back().changed,
                           true, true});
        if (k == shownIndex) selectedEntry = (int)entries.size() - 1;
    }
    if (shownIndex == 0) selectedEntry = 0;
    passes.entries = entries;
    passes.selected = selectedEntry;
    passes.setNeedsDisplay();

    const PassSnapshot& snap = snaps[shownIndex];
    IRModuleP prev = shownIndex > 0 ? snaps[shownIndex - 1].module : nullptr;
    if (snap.pass) {
        explanation.setText(passExplanation(*snap.pass));
    } else if (shownIndex == 0) {
        explanation.setText(
            "Three-address code straight from the syntax tree, as -O0 would make it: every variable has a stack slot ($x), every read is a load and "
            "every write a store. %t registers are compiler temporaries; there are as many as needed.");
    } else {
        explanation.setText(
            "Passes feed each other — folding a constant makes code dead, removing code empties blocks — so the cheap passes repeat until "
            "the IR stops changing. This is what the back end receives.");
    }
    // Instructions that are new or rewritten compared with the previous step.
    std::set<int> changed;
    int removed = 0;
    if (prev) {
        std::map<int, InstKind> before;
        for (auto& f : prev->functions)
            for (auto& b : f->blocks)
                for (auto& i : b->insts) before[i.id] = i.kind;
        std::set<int> after;
        for (auto& f : snap.module->functions)
            for (auto& b : f->blocks)
                for (auto& i : b->insts) {
                    after.insert(i.id);
                    auto it = before.find(i.id);
                    if (it == before.end() || !(it->second == i.kind)) changed.insert(i.id);
                }
        for (auto& [id, k] : before)
            if (!after.count(id)) removed++;
    }
    functions_.clear();
    for (auto& f : snap.module->functions) functions_.push_back(f->name);
    std::string focus;
    if (m.focusFunction() && std::find(functions_.begin(), functions_.end(), *m.focusFunction()) != functions_.end()) focus = *m.focusFunction();
    else if (std::find(functions_.begin(), functions_.end(), "main") != functions_.end()) focus = "main";
    else if (!functions_.empty()) focus = functions_[0];
    int fi = 0;
    for (int k = 0; k < (int)functions_.size(); k++)
        if (functions_[k] == focus) fi = k;
    functionPicker.set(functions_, fi);
    cfg.module = snap.module;
    cfg.function = nullptr;
    for (auto& f : snap.module->functions)
        if (f->name == focus) cfg.function = f.get();
    cfg.changed = changed;
    cfg.selectedLine = m.selection().line;
    cfg.recompute();
    cfgPanel.setNeedsLayout();

    std::vector<Row> rows;
    std::string summary = cat(snap.instructions, " instructions");
    if (prev) summary += cat(" · ", (int)changed.size(), " new or rewritten (green) · ", removed, " removed");
    rows.push_back(Row({Cell(summary, Fonts::small, Theme::secondary)}, Row::Kind::Normal, 20));
    for (auto& fp : snap.module->functions) {
        const IRFunction& f = *fp;
        rows.push_back(Row::spacer(4));
        std::vector<std::string> ps;
        for (int p : f.params) ps.push_back(f.vregs[p].name);
        std::vector<std::string> sl;
        for (auto& s : f.slots) sl.push_back(cat(s.name, " ", s.size, "B"));
        Row hdr = Row::header(cat("function ", f.name, "(", join(ps, ", "), ")"), f.slots.empty() ? "" : "stack slots: " + join(sl, ", "));
        hdr.line = f.line;
        rows.push_back(hdr);
        auto preds = f.predecessors();
        for (auto& bp : f.blocks) {
            const Block& b = *bp;
            std::vector<std::string> from;
            for (int pid : preds[b.id])
                if (Block* pb = f.block(pid)) from.push_back(pb->name);
            Row br({Cell(AttrString(b.name + ":", Fonts::mono(12, Weight::Semibold), Theme::label), 200.0f),
                    Cell(from.empty() ? "" : "← from " + join(from, ", "), Fonts::small, Theme::faint)},
                   Row::Kind::Section, 22);
            rows.push_back(br);
            for (auto& i : b.insts) {
                Row r({Cell(irAttributed(IRPrinter::inst(i, f)), std::nullopt, Align::Left, std::nullopt, 18),
                       Cell(i.line > 0 ? cat("line ", i.line) : "", Fonts::mono(10), Theme::faint, 60.0f, Align::Right)});
                r.line = i.line;
                r.irInst = i.id;
                if (changed.count(i.id)) r.background = Theme::okFill;
                rows.push_back(r);
            }
            Row tr({Cell(irAttributed(IRPrinter::term(b.term, f)), std::nullopt, Align::Left, std::nullopt, 18),
                    Cell(b.termLine > 0 ? cat("line ", b.termLine) : "", Fonts::mono(10), Theme::faint, 60.0f, Align::Right)});
            tr.line = b.termLine;
            tr.irInst = b.termID;
            rows.push_back(tr);
        }
    }
    if (e != ModelEvent::Selection || listing.list.rows().empty()) listing.set(rows);
    else listing.list.setNeedsDisplay();
    if (e == ModelEvent::Selection && m.selection().origin != Station::IR && m.selection().line) {
        auto& rs = listing.list.rows();
        for (int k = 0; k < (int)rs.size(); k++)
            if (rs[k].line == m.selection().line && rs[k].irInst) {
                listing.list.reveal(k);
                break;
            }
    }
}

}  // namespace ui
