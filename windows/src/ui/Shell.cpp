#include "Views.h"

namespace ui {

using namespace sp;

// MARK: - Empty note

void EmptyNote::draw(Gfx& g, const Rect& dirty) {
    fillRect(bounds(), Theme::panel);
    Rect r{30, height() / 2 - 40, width() - 60, 80};
    drawText(title, Rect(r.minX(), r.minY(), r.w, 24), Fonts::heading, Theme::heading, Align::Center);
    TextBox box;
    box.wrap = true;
    box.align = Align::Center;
    g.drawTextBox(message, {r.minX(), r.minY() + 30, r.w, 50}, Fonts::body, Theme::secondary, box);
}

// MARK: - Lessons

std::pair<std::string, std::string> Lessons::text(Station s) {
    switch (s) {
    case Station::Source:
        return {"The whole journey",
                "A compiler is a pipeline of translations. The front end checks what the program means, the middle end improves it, and the back "
                "end turns it into RISC-V instructions. The assembler and linker produce bytes, and the CPU (a 5-stage pipeline with caches) runs "
                "them. Click any station above to look inside it."};
    case Station::Tokens:
        return {"Lexical analysis — characters become tokens",
                "The lexer reads characters and groups them into tokens: keywords, names, numbers, strings and punctuation. Spaces and comments "
                "are thrown away. The tiny preprocessor here replaces #define names with their text as they are read; that is why N in your code "
                "may show up as a number."};
    case Station::Tree:
        return {"Parsing — tokens become a syntax tree",
                "A recursive-descent parser has one function per grammar rule (statement, expression, term…). Operator precedence decides the "
                "shape: a + b * c becomes +(a, *(b, c)). After semantic analysis every expression node also carries its C type."};
    case Station::Symbols:
        return {"Semantic analysis — names and types",
                "Every name is looked up in nested scopes, every expression gets a type, and C's implicit conversions are worked out (char → "
                "int, int → unsigned, array → pointer). Mistakes like calling with the wrong number of arguments are caught here. The last "
                "column shows where the back end finally put each variable."};
    case Station::IR:
        return {"Intermediate representation and the optimiser",
                "The tree is lowered into three-address code inside basic blocks, which form a control-flow graph. Each optimisation pass rewrites "
                "the IR without changing what the program computes. Switch passes off to see what each one contributes. Then compare the Assembly "
                "and the Pipeline cycle count at -O0 and -O1."};
    case Station::Asm:
        return {"Code generation — RISC-V assembly (P&H chapter 2)",
                "Instruction selection maps each IR operation onto RV32IM instructions. The register allocator then maps the unlimited virtual "
                "registers onto real ones: values alive across a call go into callee-saved s-registers, and the rest go into t-registers. The "
                "calling convention puts arguments in a0–a7 and the return address in ra, and every function builds a stack frame."};
    case Station::Machine:
        return {"Machine code — instruction formats (P&H §2.5)",
                "The assembler encodes every instruction as a 32-bit word in one of six formats (R, I, S, B, U, J). The opcode and register fields "
                "always sit at the same bit positions, so the decoder can start reading registers before it knows the instruction. "
                "Pseudo-instructions such as li, la, mv and call expand into real ones."};
    case Station::Link:
        return {"Linking and loading",
                "Each source file becomes an object file with holes: addresses it cannot know yet. The linker places the code at 0x00400000 and "
                "the data at 0x10010000 (the RARS layout). It pulls in the library members your program needs and patches every relocation with "
                "the final address. crt0 is the real entry point: it calls main."};
    case Station::Pipeline:
        return {"The pipelined datapath (P&H §4.5–4.9)",
                "Five instructions are in flight at once, one per stage. Hazards break the rhythm. A value needed before it is written is "
                "forwarded from EX/MEM or MEM/WB, except right after a load, where the hazard unit inserts a one-cycle bubble. A taken branch "
                "flushes the instructions fetched behind it, and a better predictor flushes less often. Watch the CPI."};
    case Station::Memory:
        return {"The memory hierarchy (P&H chapter 5)",
                "Caches exploit locality: recently used data is likely to be used again (temporal), and so is its neighbour (spatial). Each "
                "address splits into tag | set index | block offset. A miss is compulsory (first touch), capacity (the cache is too small) or "
                "conflict (too many blocks map to one set). Change the size, block size and associativity, then re-run."};
    }
    return {"", ""};
}

// MARK: - Journey bar

int JourneyBar::status(Station s) const {
    // 0 ok, 1 failed here, 2 not reached / skipped
    if (!model || !model->compilation()) return 2;
    const Compilation& c = *model->compilation();
    if (c.isAssembly && stationNeedsC(s)) return 2;
    if (!c.failedStage) return 0;
    static const std::vector<Stage> stagesInOrder = {Stage::Lexer, Stage::Preprocessor, Stage::Parser, Stage::Semantic,
                                                     Stage::IR, Stage::Codegen, Stage::Assembler, Stage::Linker};
    auto indexOf = [&](Stage st) {
        auto it = std::find(stagesInOrder.begin(), stagesInOrder.end(), st);
        return it == stagesInOrder.end() ? 0 : (int)(it - stagesInOrder.begin());
    };
    int fi = indexOf(*c.failedStage);
    std::optional<Stage> st;
    switch (s) {
    case Station::Tokens: st = Stage::Lexer; break;
    case Station::Tree: st = Stage::Parser; break;
    case Station::Symbols: st = Stage::Semantic; break;
    case Station::IR: st = Stage::IR; break;
    case Station::Asm: st = Stage::Codegen; break;
    case Station::Machine: st = Stage::Assembler; break;
    case Station::Link: st = Stage::Linker; break;
    default: break;
    }
    if (st) {
        int si = indexOf(*st);
        if (si == fi) return 1;
        if (si > fi) return 2;
        return 0;
    }
    if (s == Station::Source) return 0;
    return model->good() ? 0 : 2;
}

void JourneyBar::draw(Gfx& g, const Rect& dirty) {
    fillRect(bounds(), Theme::background);
    if (!model) return;
    float pad = 14;
    float w = (width() - pad * 2) / sp::stationCount;
    rects_.clear();
    float cy = 20;
    // The rail.
    line({pad + w / 2, cy}, {width() - pad - w / 2, cy}, Theme::borderStrong, 2);
    // Layer brackets.
    struct Group {
        const char* name;
        int lo, hi;
    };
    static const Group groups[] = {{"front end", 0, 3}, {"middle end", 4, 4}, {"back end", 5, 7}, {"hardware", 8, 9}};
    for (auto& gr : groups) {
        float x0 = pad + gr.lo * w + 6, x1 = pad + (gr.hi + 1) * w - 6;
        float y = height() - 9;
        line({x0, y - 4}, {x0, y}, Theme::borderStrong);
        line({x0, y}, {x1, y}, Theme::borderStrong);
        line({x1, y - 4}, {x1, y}, Theme::borderStrong);
        Size sz = textSize(gr.name, Fonts::tiny);
        Rect lr{(x0 + x1) / 2 - sz.w / 2 - 5, y - 6, sz.w + 10, 12};
        fillRect(lr, Theme::background);
        drawText(gr.name, Point(lr.minX() + 5, y - sz.h / 2 - 0.5f), Fonts::tiny, Theme::faint);
    }
    for (int k = 0; k < sp::stationCount; k++) {
        Station s = (Station)k;
        float x = pad + k * w;
        Rect r{x, 2, w, height() - 16};
        rects_.push_back(r);
        int st = status(s);
        bool isSel = model->station() == s;
        Point c{r.midX(), cy};
        float rad = isSel ? 13 : 11;
        Rect circle{c.x - rad, c.y - rad, rad * 2, rad * 2};
        Color fill = isSel ? Theme::ink : st == 1 ? Theme::crimson : st == 2 ? Color::hex(0xE7E0D2) : (hovered_ == k ? Theme::hover : Theme::panel);
        fillRounded(circle, rad, fill);
        strokeRounded(circle, rad, isSel ? Theme::ink : st == 1 ? Theme::crimson : Theme::borderStrong, 1.5f);
        Color numColor = isSel || st == 1 ? Color::white() : st == 2 ? Theme::faint : Theme::ink;
        drawText(std::to_string(k + 1), circle, Fonts::ui(isSel ? 12 : 11, Weight::Bold), numColor, Align::Center);
        drawText(stationTitle(s), Rect(r.minX(), cy + 15, r.w, 15), Fonts::ui(11, isSel ? Weight::Bold : Weight::Medium),
                 isSel ? Theme::ink : st == 2 ? Theme::faint : st == 1 ? Theme::crimson : Theme::text, Align::Center);
    }
}

void JourneyBar::mouseMoved(const MouseEvent& e) {
    Point p = localPoint(e);
    std::optional<int> h;
    for (int k = 0; k < (int)rects_.size(); k++)
        if (rects_[k].contains(p)) h = k;
    if (h != hovered_) {
        hovered_ = h;
        setNeedsDisplay();
    }
}

void JourneyBar::mouseExited(const MouseEvent&) {
    if (hovered_) {
        hovered_.reset();
        setNeedsDisplay();
    }
}

void JourneyBar::mouseDown(const MouseEvent& e) {
    Point p = localPoint(e);
    for (int k = 0; k < (int)rects_.size(); k++)
        if (rects_[k].contains(p) && model) {
            model->setStation((Station)k);
            return;
        }
}

// MARK: - Lesson strip

void LessonStrip::setStation(Station s) {
    station_ = s;
    setNeedsDisplay();
}

void LessonStrip::draw(Gfx& g, const Rect& dirty) {
    Rect r = bounds().insetBy(0.5f, 0.5f);
    drawCard(r, 8, Color::hex(0xFBF6EA));
    auto [title, body] = Lessons::text(station_);
    g.drawIcon(Icon::Book, {12, 9, 16, 16}, Theme::heading);
    drawText(title, Point(34, 7), Fonts::serif(13, Weight::Semibold), Theme::heading);
    TextBox box;
    box.wrap = true;
    box.truncate = true;
    box.lineSpacing = 2;
    g.drawTextBox(body, {14, 28, width() - 28, height() - 32}, Fonts::ui(11.5f), Theme::text, box);
}

// MARK: - Console

ConsoleView::ConsoleView() {
    outputView.editable = false;
    outputView.font = Fonts::mono(11.5f);
    outputView.background = Color::hex(0xFFFEFA);
    outputView.textColor = Theme::text;
    outputView.inset = {8, 6};
    outputScroll.setDocumentView(&outputView);
    outputScroll.hasVerticalScroller = true;
    outputScroll.hasHorizontalScroller = true;
    outputScroll.background = Color::hex(0xFFFEFA);
    addSubview(&tabs);
    addSubview(&outputScroll);
    addSubview(&problems);
    problems.setHidden(true);
    tabs.onChange = [this](int k) {
        outputScroll.setHidden(k != 0);
        problems.setHidden(k != 1);
    };
    problems.list.onClick = [this](int, const Row& r) {
        if (r.line && onSelectLine) onSelectLine(*r.line);
    };
}

void ConsoleView::layout() {
    Size s = tabs.intrinsicSize();
    tabs.setFrame({8, 6, s.w, 24});
    outputScroll.setFrame({1, 36, width() - 2, height() - 37});
    problems.setFrame(outputScroll.frame());
    outputView.updateSize();
}

void ConsoleView::draw(Gfx& g, const Rect& dirty) {
    drawCard(bounds().insetBy(0.5f, 0.5f));
    if (!model) return;
    std::string status;
    Color color = Theme::secondary;
    if (Pipeline* p = model->pipeline()) {
        if (p->output().fault) {
            status = "stopped: " + *p->output().fault;
            color = Theme::crimson;
        } else if (p->halted()) {
            status = cat("exited with code ", p->output().exitCode.value_or(0), " after ", formatCount(p->stats().cycles), " cycles");
            color = Theme::green;
        } else if (p->cycle() > 0) {
            status = "running… cycle " + formatCount(p->cycle());
        } else {
            status = "ready";
        }
    } else if (model->hasErrors()) {
        status = "fix the problems to run";
        color = Theme::crimson;
    }
    float tx = tabs.frame().maxX();
    drawText(status, Rect(tx + 10, 10, width() - tx - 20, 16), Fonts::small, color, Align::Right);
    line({1, 35.5f}, {width() - 1, 35.5f}, Theme::border);
}

void ConsoleView::reload(AppModel& m) {
    model = &m;
    Pipeline* p = m.pipeline();
    std::string text = p ? p->output().text : "";
    if (text != lastText_ || (text.empty() && showingPlaceholder_ != (p != nullptr))) {
        lastText_ = text;
        if (text.empty()) {
            outputView.setText(p ? L"(nothing printed yet — press Run)" : L"");
            outputView.textColor = Theme::faint;
            showingPlaceholder_ = p != nullptr;
        } else {
            outputView.setText(widen(text));
            outputView.textColor = Theme::text;
            showingPlaceholder_ = false;
            outputView.scrollToEnd();
        }
    }
    std::vector<Diagnostic> diags = m.compilation() ? m.compilation()->diagnostics : std::vector<Diagnostic>{};
    int problemsCount = 0;
    for (auto& d : diags)
        if (d.severity != Diagnostic::Severity::Note) problemsCount++;
    tabs.setOptions({"Output", problemsCount > 0 ? cat("Problems (", problemsCount, ")") : std::string("Problems")});
    std::vector<Row> rows;
    for (auto& d : diags) {
        const char* label;
        Color fill, fg;
        if (d.severity == Diagnostic::Severity::Error) {
            label = "error";
            fill = Theme::errorFill;
            fg = Theme::crimson;
        } else if (d.severity == Diagnostic::Severity::Warning) {
            label = "warning";
            fill = Color::hex(0xFFF1C9);
            fg = Theme::amber;
        } else {
            label = "note";
            fill = Color::hex(0xE6EEF9);
            fg = Theme::ink;
        }
        Row r({Cell(AttrString(label, Fonts::ui(10, Weight::Bold), fg), 60.0f, Align::Left, fill),
               Cell(d.line > 0 ? std::to_string(d.line) : "", Fonts::mono(10.5f), Theme::secondary, 34.0f, Align::Right),
               Cell(AttrString(d.message, Fonts::small, Theme::text), std::nullopt, Align::Left, std::nullopt, 8)},
              Row::Kind::Normal, 24);
        if (d.line > 0) r.line = d.line;
        r.tooltip = d.message;
        rows.push_back(r);
    }
    if (rows.empty()) rows.push_back(Row({Cell("No problems.", Fonts::small, Theme::faint)}));
    problems.set(rows);
    if (m.hasErrors() && tabs.selected() == 0 && m.compilation() && !m.compilation()->errors().empty()) {
        tabs.setSelected(1);
        outputScroll.setHidden(true);
        problems.setHidden(false);
    }
    setNeedsDisplay();
}

// MARK: - Top bar

const double TopBar::speeds[6] = {2, 6, 20, 80, 400, 5000};

static std::vector<std::string> exampleNames() {
    std::vector<std::string> n;
    for (auto& e : Examples::all()) n.push_back(e.name);
    return n;
}

TopBar::TopBar() : examples(exampleNames()) {
    for (View* v : std::initializer_list<View*>{&examples, &lang, &opt, &reset, &back, &stepCycle, &stepInst, &stepLine, &play, &toEnd, &speed,
                                                &speedLabel})
        addSubview(v);
    reset.tooltip = "Reset the machine to cycle 0";
    back.tooltip = "One cycle back";
    stepCycle.tooltip = "Advance one clock cycle (Ctrl+J)";
    stepInst.tooltip = "Run until the next instruction completes (Ctrl+K)";
    stepLine.tooltip = "Run until an instruction from another C line completes (Ctrl+L)";
    toEnd.tooltip = "Run to the end of the program";
    play.tooltip = "Run or pause the clock (F5)";
    speedLabel.align = Align::Center;
    speed.onChange = [this](int k) {
        if (!model) return;
        model->speed = speeds[k];
        refresh();
    };
    examples.onChange = [this](int k) {
        if (model) model->load(Examples::all()[k]);
    };
    lang.onChange = [this](int k) {
        if (model) model->setAssemblyMode(k == 1);
    };
    opt.onChange = [this](int k) {
        if (!model) return;
        CompileOptions o = model->options();
        o.passes = k == 0 ? std::set<PassID>{} : std::set<PassID>(allPasses().begin(), allPasses().end());
        model->setIRSnapshot(std::nullopt);
        model->setOptions(o);
    };
    reset.action = [this] {
        if (model) model->resetMachine();
    };
    back.action = [this] {
        if (model) model->stepBack();
    };
    stepCycle.action = [this] {
        if (model) model->stepCycle();
    };
    stepInst.action = [this] {
        if (model) model->stepInstruction();
    };
    stepLine.action = [this] {
        if (model) model->stepLine();
    };
    play.action = [this] {
        if (model) model->togglePlay();
    };
    toEnd.action = [this] {
        if (model) model->runToEnd();
    };
}

void TopBar::layout() {
    float y = 14;
    float x = 196;
    examples.setFrame({x, y - 1, 220, 26});
    x += 228;
    Size ls = lang.intrinsicSize();
    lang.setFrame({x, y, ls.w, 24});
    x += ls.w + 8;
    Size os = opt.intrinsicSize();
    opt.setFrame({x, y, os.w, 24});
    // Clock controls on the right.
    float rx = width() - 12;
    auto place = [&](View& v, float w) {
        rx -= w;
        v.setFrame({rx, y - 1, w, 26});
        rx -= 5;
    };
    speedLabel.setFrame({width() - 92, y + 23, 80, 12});
    place(speed, 80);
    place(toEnd, 32);
    place(play, play.intrinsicSize().w + 6);
    place(stepLine, stepLine.intrinsicSize().w);
    place(stepInst, stepInst.intrinsicSize().w);
    place(stepCycle, stepCycle.intrinsicSize().w);
    place(back, 32);
    place(reset, 32);
}

void TopBar::draw(Gfx& g, const Rect& dirty) {
    fillRect(bounds(), Theme::panel);
    line({0, height() - 0.5f}, {width(), height() - 0.5f}, Theme::border);
    // A small chip-like emblem.
    Rect em{14, 12, 28, 28};
    fillRounded(em, 6, Theme::ink);
    for (int k = 0; k < 4; k++) {
        float o = k * 6.0f + 6;
        line({em.minX() + o, em.minY() - 3}, {em.minX() + o, em.minY()}, Theme::ink, 1.5f);
        line({em.minX() + o, em.maxY()}, {em.minX() + o, em.maxY() + 3}, Theme::ink, 1.5f);
    }
    drawText("C", em, Fonts::serif(15, Weight::Bold), Color::white(), Align::Center);
    drawText("Silicon Path", Point(50, 7), Fonts::serif(18, Weight::Semibold), Theme::text);
    drawText("from C to RISC-V hardware", Point(51, 31), Fonts::tiny, Theme::secondary);
}

void TopBar::refresh() {
    if (!model) return;
    AppModel& m = *model;
    auto& all = Examples::all();
    for (int k = 0; k < (int)all.size(); k++)
        if (all[k].name == m.exampleName) examples.select(k);
    lang.setSelected(m.isAssembly() ? 1 : 0);
    opt.setEnabledMask(m.isAssembly() ? std::optional<std::vector<bool>>(std::vector<bool>{false, false}) : std::nullopt);
    bool all7 = m.options().passes.size() == allPasses().size(), none = m.options().passes.empty();
    opt.setOptions(all7 || none ? std::vector<std::string>{"-O0", "-O1"} : std::vector<std::string>{"-O0", "-O1", "custom"});
    opt.setSelected(none ? 0 : all7 ? 1 : 2);
    bool canRun = m.pipeline() && !m.pipeline()->halted();
    for (TButton* b : {&stepCycle, &stepInst, &stepLine, &play, &toEnd}) b->setEnabled(canRun);
    back.setEnabled(m.pipeline() && m.pipeline()->cycle() > 0);
    reset.setEnabled(m.pipeline() != nullptr);
    play.setTitle(m.running() ? "Pause" : "Run");
    play.setIcon(m.running() ? Icon::Pause : Icon::Play);
    int k = 1;
    for (int i = 0; i < 6; i++)
        if (speeds[i] == m.speed) k = i;
    speed.setValue(k);
    speedLabel.setText(m.speed >= 1000 ? "full speed" : cat((int)m.speed, " cycles/s"));
    setNeedsLayout();
}

// MARK: - Stale banner

void StaleBanner::draw(Gfx& g, const Rect& dirty) {
    fillRounded(bounds().insetBy(0.5f, 0.5f), 7, Color::hex(0xFBE3DE));
    strokeRounded(bounds().insetBy(0.5f, 0.5f), 7, Theme::crimson.alpha(0.4f));
    g.drawIcon(Icon::Warning, {12, (height() - 13) / 2, 13, 13}, Theme::crimson);
    drawText(text, Rect(32, 0, width() - 44, height()), Fonts::ui(11.5f, Weight::Medium), Theme::crimson);
}

}  // namespace ui
